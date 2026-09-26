#include "zen_sav_backend.h"

#include <nds.h>
#include <nds/card.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

namespace {
// ---------------------------------------------------------------------------
// ZLOM native .sav layout (128 KiB EEPROM)
// ---------------------------------------------------------------------------
// 00000-00FFF : global header / commit area (4 KiB)
// 01000-0AFFF : Save0.dat (40 KiB block)
// 0B000-14FFF : Save1.dat (40 KiB block)
// 15000-1EFFF : Save2.dat (40 KiB block)
// 1F000-1FFFF : option.sav (4 KiB block)
//
// Each record starts with a 32-byte header. The payload is the exact byte stream
// supplied by Zenonia's WIPI DB API; no translation or recompression is done.
constexpr uint32_t SAV_TOTAL_SIZE      = 128u * 1024u;
constexpr uint32_t SAV_META_SIZE       = 0x1000u;
constexpr uint32_t SAV_SLOT_BLOCK      = 0xA000u;
constexpr uint32_t SAV_OPTION_BLOCK    = 0x1000u;
constexpr uint32_t SAV_RECORD_HEADER   = 32u;
constexpr uint32_t SAV_VERSION         = 1u;
constexpr uint32_t SAV_RECORD_COUNT    = 4u;
constexpr uint32_t SAV_PAGE_SIZE       = 128u;
constexpr uint32_t SAV_COMMIT_OFFSET   = 0x100u;
constexpr uint32_t SAV_COMMIT_SIZE     = 16u;

const unsigned char GLOBAL_MAGIC[8] = {'Z','L','O','M','S','V','3','3'};
const unsigned char RECORD_MAGIC[8] = {'Z','L','D','B','R','E','C','1'};

bool g_native_probe_allowed = false;
bool g_native_opened = false;
bool g_native_available = false;
int  g_native_attempts = 0;

bool g_fat_available = false;
char g_fat_prefix[16] = "";
char g_fat_path[96] = "";

static const char *basename_of(const char *name) {
    if(!name) return "";
    const char *base=name;
    for(const char *p=name; *p; ++p) if(*p=='/' || *p=='\\') base=p+1;
    return base;
}

static int record_id_for_name(const char *name) {
    const char *base=basename_of(name);
    if(!strcmp(base,"option.sav")) return 3;
    if(strncmp(base,"Save",4)!=0) return -1;
    if(base[4]<'0' || base[4]>'2') return -1;
    if(strcmp(base+5,".dat")!=0) return -1;
    return base[4]-'0';
}

static uint32_t record_offset(int id) {
    if(id>=0 && id<=2) return SAV_META_SIZE + (uint32_t)id*SAV_SLOT_BLOCK;
    if(id==3) return SAV_META_SIZE + 3u*SAV_SLOT_BLOCK;
    return SAV_TOTAL_SIZE;
}

static uint32_t record_block_size(int id) {
    if(id>=0 && id<=2) return SAV_SLOT_BLOCK;
    if(id==3) return SAV_OPTION_BLOCK;
    return 0;
}

static uint32_t record_payload_max(int id) {
    const uint32_t block=record_block_size(id);
    return block>SAV_RECORD_HEADER ? block-SAV_RECORD_HEADER : 0;
}

static void put32(unsigned char *p,uint32_t v) {
    p[0]=(unsigned char)(v); p[1]=(unsigned char)(v>>8);
    p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24);
}
static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

// Small table-less CRC32. Saves are at most ~40 KiB, so the compact version is
// preferable to adding another library dependency just for integrity checks.
static uint32_t crc32_bytes(const void *data,uint32_t len) {
    const unsigned char *p=(const unsigned char*)data;
    uint32_t crc=0xffffffffu;
    for(uint32_t i=0;i<len;i++) {
        crc^=p[i];
        for(int b=0;b<8;b++) crc=(crc>>1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc&1u));
    }
    return crc^0xffffffffu;
}

// ---------------------------------------------------------------------------
// Native NTR cartridge EEPROM backend. Adapted from the supplied EasyRPG DSi
// source: regular 128 KiB EEPROM, 3-byte address, final-byte CS release.
// ---------------------------------------------------------------------------
static bool wait_aux_spi_idle(uint32_t budget=200000u) {
    while((REG_AUXSPICNT & CARD_SPI_BUSY) && budget) --budget;
    return (REG_AUXSPICNT & CARD_SPI_BUSY)==0;
}

static bool spi_byte_mode(unsigned char value,unsigned char *result,bool hold) {
    REG_AUXSPICNT = CARD_ENABLE | CARD_SPI_ENABLE | (hold ? CARD_SPI_HOLD : 0);
    REG_AUXSPIDATA = value;
    if(!wait_aux_spi_idle()) return false;
    if(result) *result=REG_AUXSPIDATA;
    return true;
}
static bool spi_hold(unsigned char value,unsigned char *result=nullptr) {
    return spi_byte_mode(value,result,true);
}
static bool spi_release(unsigned char value,unsigned char *result=nullptr) {
    const bool ok=spi_byte_mode(value,result,false);
    REG_AUXSPICNT=CARD_ENABLE;
    return ok;
}
static bool eeprom_write_enable() { return spi_release(0x06); }
static bool eeprom_wait_ready() {
    for(uint32_t poll=0;poll<8192;poll++) {
        unsigned char status=0xff;
        if(!spi_hold(0x05) || !spi_release(0,&status)) return false;
        if((status&1)==0) return true;
    }
    return false;
}
static bool eeprom_read(uint32_t address,unsigned char *data,uint32_t len) {
    if(!data || address>SAV_TOTAL_SIZE || len>SAV_TOTAL_SIZE-address) return false;
    while(len) {
        uint32_t chunk=len>1024u?1024u:len;
        bool ok=spi_hold(0x03) &&
                spi_hold((unsigned char)(address>>16)) &&
                spi_hold((unsigned char)(address>>8)) &&
                spi_hold((unsigned char)address);
        for(uint32_t i=0;ok && i<chunk;i++) {
            ok=(i+1==chunk)?spi_release(0,&data[i]):spi_hold(0,&data[i]);
        }
        if(!ok) return false;
        address+=chunk; data+=chunk; len-=chunk;
    }
    return true;
}
static bool eeprom_write_page(uint32_t address,const unsigned char *data,uint32_t len) {
    if(!data || !len || len>SAV_PAGE_SIZE) return false;
    if(address>SAV_TOTAL_SIZE || len>SAV_TOTAL_SIZE-address) return false;
    if(((address&(SAV_PAGE_SIZE-1u))+len)>SAV_PAGE_SIZE) return false;
    if(!eeprom_write_enable()) return false;
    bool ok=spi_hold(0x02) &&
            spi_hold((unsigned char)(address>>16)) &&
            spi_hold((unsigned char)(address>>8)) &&
            spi_hold((unsigned char)address);
    for(uint32_t i=0;ok && i<len;i++) {
        ok=(i+1==len)?spi_release(data[i]):spi_hold(data[i]);
    }
    return ok && eeprom_wait_ready();
}
static bool native_read(uint32_t address,void *data,uint32_t len) {
    return eeprom_read(address,(unsigned char*)data,len);
}
static bool native_write(uint32_t address,const void *data,uint32_t len) {
    const unsigned char *p=(const unsigned char*)data;
    while(len) {
        const uint32_t room=SAV_PAGE_SIZE-(address&(SAV_PAGE_SIZE-1u));
        const uint32_t chunk=len<room?len:room;
        if(!eeprom_write_page(address,p,chunk)) return false;
        address+=chunk; p+=chunk; len-=chunk;
    }
    return true;
}
static bool native_verify(uint32_t address,const void *expected,uint32_t len) {
    if(!len) return true;
    unsigned char temp[128];
    const unsigned char *src=(const unsigned char*)expected;
    while(len) {
        const uint32_t chunk=len>sizeof(temp)?sizeof(temp):len;
        if(!native_read(address,temp,chunk) || memcmp(temp,src,chunk)!=0) return false;
        address+=chunk; src+=chunk; len-=chunk;
    }
    return true;
}

static bool native_probe() {
    if(g_native_available) return true;
    if(!g_native_probe_allowed || g_native_attempts>=2) return false;
    ++g_native_attempts;
    printf("[SAV] native probe %d\n",g_native_attempts);
    if(!g_native_opened) {
        if(!ntrcardOpen()) {
            printf("[SAV] ntrcardOpen failed\n");
            return false;
        }
        g_native_opened=true;
    }
    // Match the EasyRPG reference profile: this port intentionally uses a
    // fixed regular EEPROM128 protocol instead of probing/guessing save type.
    g_native_available=true;
    unsigned char test[8];
    if(!native_read(0,test,sizeof(test))) {
        g_native_available=false;
        printf("[SAV] EEPROM128 read failed\n");
        return false;
    }
    printf("[SAV] native EEPROM128 ready\n");
    return true;
}

static bool global_header_valid_native(uint32_t *seq=nullptr) {
    unsigned char h[32];
    if(!native_read(0,h,sizeof(h))) return false;
    if(memcmp(h,GLOBAL_MAGIC,8)!=0) return false;
    if(get32(h+8)!=SAV_VERSION || get32(h+12)!=SAV_TOTAL_SIZE || get32(h+16)!=SAV_RECORD_COUNT) return false;
    if(get32(h+28)!=crc32_bytes(h,28)) return false;
    if(seq) *seq=get32(h+20);
    return true;
}
static bool write_global_header_native(uint32_t seq) {
    unsigned char h[32]; memset(h,0xff,sizeof(h));
    memcpy(h,GLOBAL_MAGIC,8);
    put32(h+8,SAV_VERSION); put32(h+12,SAV_TOTAL_SIZE); put32(h+16,SAV_RECORD_COUNT);
    put32(h+20,seq); put32(h+24,0x33564153u); // "SAV3"
    put32(h+28,crc32_bytes(h,28));
    return native_write(0,h,sizeof(h)) && native_verify(0,h,sizeof(h));
}
static uint32_t native_sequence() {
    uint32_t seq=0; return global_header_valid_native(&seq)?seq:0;
}
static bool ensure_global_native() {
    if(global_header_valid_native()) return true;
    return write_global_header_native(0);
}
static bool native_commit_pulse(uint32_t seq) {
    unsigned char p[SAV_COMMIT_SIZE]; memset(p,0xff,sizeof(p));
    p[0]='Z';p[1]='L';p[2]='3';p[3]='3'; put32(p+4,seq); put32(p+8,0x45564153u);
    put32(p+12,crc32_bytes(p,12));
    return native_write(SAV_COMMIT_OFFSET,p,sizeof(p)) && native_verify(SAV_COMMIT_OFFSET,p,sizeof(p));
}

static bool make_record_header(int id,const unsigned char *data,uint32_t len,uint32_t seq,unsigned char h[SAV_RECORD_HEADER]) {
    if(id<0 || id>=4 || len>record_payload_max(id)) return false;
    memset(h,0xff,SAV_RECORD_HEADER); memcpy(h,RECORD_MAGIC,8);
    put32(h+8,SAV_VERSION); put32(h+12,(uint32_t)id); put32(h+16,len);
    put32(h+20,crc32_bytes(data,len)); put32(h+24,seq); put32(h+28,crc32_bytes(h,28));
    return true;
}
static bool validate_record_header(int id,const unsigned char h[SAV_RECORD_HEADER],uint32_t *len=nullptr) {
    if(id<0 || id>=4 || memcmp(h,RECORD_MAGIC,8)!=0) return false;
    if(get32(h+8)!=SAV_VERSION || get32(h+12)!=(uint32_t)id) return false;
    const uint32_t n=get32(h+16);
    if(n>record_payload_max(id) || get32(h+28)!=crc32_bytes(h,28)) return false;
    if(len) *len=n;
    return true;
}
static bool native_record_info(int id,uint32_t *len) {
    if(!native_probe()) return false;
    unsigned char h[SAV_RECORD_HEADER];
    if(!native_read(record_offset(id),h,sizeof(h))) return false;
    return validate_record_header(id,h,len);
}
static int native_load_record(int id,unsigned char **out,uint32_t *out_len) {
    if(!out || !out_len || !native_probe()) return -12;
    *out=nullptr; *out_len=0;
    unsigned char h[SAV_RECORD_HEADER]; uint32_t len=0;
    if(!native_read(record_offset(id),h,sizeof(h)) || !validate_record_header(id,h,&len)) return -12;
    unsigned char *mem=(unsigned char*)malloc(len?len:1);
    if(!mem) return -3;
    if(len && !native_read(record_offset(id)+SAV_RECORD_HEADER,mem,len)) { free(mem); return -12; }
    if(get32(h+20)!=crc32_bytes(mem,len)) { free(mem); return -12; }
    *out=mem; *out_len=len;
    return 0;
}
static int native_store_record(int id,const unsigned char *data,uint32_t len) {
    if(!native_probe()) return -12;
    if(len>record_payload_max(id)) {
        printf("[SAV!] record %d too large %lu > %lu\n",id,(unsigned long)len,(unsigned long)record_payload_max(id));
        return -18;
    }
    if(!ensure_global_native()) return -3;
    const uint32_t seq=native_sequence()+1u;
    unsigned char h[SAV_RECORD_HEADER];
    if(!make_record_header(id,data,len,seq,h)) return -18;
    const uint32_t off=record_offset(id);
    // Payload first, header second. A power loss cannot make a partially-written
    // payload look valid because the old/new CRC will not match.
    if(len && !native_write(off+SAV_RECORD_HEADER,data,len)) return -3;
    if(!native_write(off,h,sizeof(h))) return -3;
    if(!native_verify(off,h,sizeof(h))) return -3;
    if(len && !native_verify(off+SAV_RECORD_HEADER,data,len)) return -3;
    if(!write_global_header_native(seq) || !native_commit_pulse(seq)) return -3;
    printf("[SAV] EEPROM write rec=%d len=%lu seq=%lu\n",id,(unsigned long)len,(unsigned long)seq);
    return 0;
}
static int native_remove_record(int id) {
    if(!native_probe()) return -12;
    unsigned char blank[SAV_RECORD_HEADER]; memset(blank,0xff,sizeof(blank));
    if(!native_write(record_offset(id),blank,sizeof(blank))) return -3;
    const uint32_t seq=native_sequence()+1u;
    if(!write_global_header_native(seq) || !native_commit_pulse(seq)) return -3;
    return 0;
}

// ---------------------------------------------------------------------------
// Single ordinary FAT .sav fallback, using the exact same 128 KiB layout.
// ---------------------------------------------------------------------------
static bool fat_probe_prefix(const char *prefix) {
    char path[96];
    snprintf(path,sizeof(path),"%sZLOM_probe.tmp",prefix?prefix:"");
    FILE *f=fopen(path,"wb");
    if(!f) return false;
    const unsigned char m[4]={'Z','L','O','M'};
    bool ok=fwrite(m,1,sizeof(m),f)==sizeof(m);
    if(ok) ok=fflush(f)==0;
    if(fclose(f)!=0) ok=false;
    if(ok) remove(path);
    return ok;
}
static bool fat_file_header_valid(FILE *f,uint32_t *seq=nullptr) {
    if(!f || fseek(f,0,SEEK_SET)!=0) return false;
    unsigned char h[32]; if(fread(h,1,sizeof(h),f)!=sizeof(h)) return false;
    if(memcmp(h,GLOBAL_MAGIC,8)!=0 || get32(h+8)!=SAV_VERSION || get32(h+12)!=SAV_TOTAL_SIZE || get32(h+16)!=SAV_RECORD_COUNT) return false;
    if(get32(h+28)!=crc32_bytes(h,28)) return false;
    if(seq) *seq=get32(h+20);
    return true;
}
static bool fat_write_global(FILE *f,uint32_t seq) {
    if(!f) return false;
    unsigned char h[32]; memset(h,0xff,sizeof(h)); memcpy(h,GLOBAL_MAGIC,8);
    put32(h+8,SAV_VERSION); put32(h+12,SAV_TOTAL_SIZE); put32(h+16,SAV_RECORD_COUNT);
    put32(h+20,seq); put32(h+24,0x33564153u); put32(h+28,crc32_bytes(h,28));
    return fseek(f,0,SEEK_SET)==0 && fwrite(h,1,sizeof(h),f)==sizeof(h);
}
static bool fat_init_file(FILE *f) {
    if(!f) return false;
    if(fseek(f,(long)SAV_TOTAL_SIZE-1,SEEK_SET)!=0) return false;
    const unsigned char ff=0xff; if(fwrite(&ff,1,1,f)!=1) return false;
    return fat_write_global(f,0);
}
static FILE *fat_open_rw() {
    if(!g_fat_available || !g_fat_path[0]) return nullptr;
    FILE *f=fopen(g_fat_path,"r+b");
    if(!f) f=fopen(g_fat_path,"w+b");
    if(!f) return nullptr;
    if(!fat_file_header_valid(f)) {
        fclose(f); f=fopen(g_fat_path,"w+b");
        if(!f || !fat_init_file(f)) { if(f) fclose(f); return nullptr; }
    }
    return f;
}
static FILE *fat_open_ro() {
    if(!g_fat_available || !g_fat_path[0]) return nullptr;
    FILE *f=fopen(g_fat_path,"rb");
    if(!f) return nullptr;
    if(!fat_file_header_valid(f)) { fclose(f); return nullptr; }
    return f;
}
static bool fat_record_info(int id,uint32_t *len) {
    FILE *f=fat_open_ro(); if(!f) return false;
    unsigned char h[SAV_RECORD_HEADER];
    const bool ok=fseek(f,(long)record_offset(id),SEEK_SET)==0 && fread(h,1,sizeof(h),f)==sizeof(h) && validate_record_header(id,h,len);
    fclose(f); return ok;
}
static int fat_load_record(int id,unsigned char **out,uint32_t *out_len) {
    if(!out || !out_len) return -9;
    *out=nullptr; *out_len=0;
    FILE *f=fat_open_ro(); if(!f) return -12;
    unsigned char h[SAV_RECORD_HEADER]; uint32_t len=0;
    if(fseek(f,(long)record_offset(id),SEEK_SET)!=0 || fread(h,1,sizeof(h),f)!=sizeof(h) || !validate_record_header(id,h,&len)) { fclose(f); return -12; }
    unsigned char *mem=(unsigned char*)malloc(len?len:1); if(!mem) { fclose(f); return -3; }
    if(len && fread(mem,1,len,f)!=len) { free(mem); fclose(f); return -12; }
    fclose(f);
    if(get32(h+20)!=crc32_bytes(mem,len)) { free(mem); return -12; }
    *out=mem; *out_len=len; return 0;
}
static int fat_store_record(int id,const unsigned char *data,uint32_t len) {
    if(!g_fat_available) return -12;
    if(len>record_payload_max(id)) return -18;
    FILE *f=fat_open_rw(); if(!f) return -3;
    uint32_t seq=0; fat_file_header_valid(f,&seq); ++seq;
    unsigned char h[SAV_RECORD_HEADER]; if(!make_record_header(id,data,len,seq,h)) { fclose(f); return -18; }
    const long off=(long)record_offset(id);
    bool ok=true;
    if(len) ok=fseek(f,off+(long)SAV_RECORD_HEADER,SEEK_SET)==0 && fwrite(data,1,len,f)==len;
    if(ok) ok=fseek(f,off,SEEK_SET)==0 && fwrite(h,1,sizeof(h),f)==sizeof(h);
    if(ok) ok=fat_write_global(f,seq);
    if(ok) ok=fflush(f)==0;
    if(ok) { int fd=fileno(f); if(fd>=0) fsync(fd); }
    const int close_rc=fclose(f); if(close_rc!=0) ok=false;
    if(ok) printf("[SAV] FAT write rec=%d len=%lu -> %s\n",id,(unsigned long)len,g_fat_path);
    return ok?0:-3;
}
static int fat_remove_record(int id) {
    FILE *f=fat_open_rw(); if(!f) return -12;
    unsigned char blank[SAV_RECORD_HEADER]; memset(blank,0xff,sizeof(blank));
    uint32_t seq=0; fat_file_header_valid(f,&seq); ++seq;
    bool ok=fseek(f,(long)record_offset(id),SEEK_SET)==0 && fwrite(blank,1,sizeof(blank),f)==sizeof(blank) && fat_write_global(f,seq) && fflush(f)==0;
    if(ok) { int fd=fileno(f); if(fd>=0) fsync(fd); }
    if(fclose(f)!=0) ok=false;
    return ok?0:-3;
}
}

namespace ZenSav {
void InitFatFallback() {
    g_fat_available=false; g_fat_prefix[0]=0; g_fat_path[0]=0;
    const bool fat_ok=fatInitDefault();
    if(!fat_ok) {
        printf("[SAV] FAT fallback unavailable\n");
        return;
    }
    static const char *prefixes[]={"sd:/","fat:/","fat:","./",""};
    for(unsigned i=0;i<sizeof(prefixes)/sizeof(prefixes[0]);i++) {
        if(fat_probe_prefix(prefixes[i])) {
            snprintf(g_fat_prefix,sizeof(g_fat_prefix),"%s",prefixes[i]);
            snprintf(g_fat_path,sizeof(g_fat_path),"%sZLOM.sav",prefixes[i]);
            g_fat_available=true;
            printf("[SAV] FAT fallback ready: %s\n",g_fat_path);
            return;
        }
    }
    printf("[SAV] FAT mounted, no writable path\n");
}

void EnableNativeProbe() {
    g_native_probe_allowed=true;
    // Probe now rather than during early NitroFS startup. This mirrors the
    // supplied EasyRPG source's deferred Slot-1 policy.
    native_probe();
}

void Flush() {
    // Native EEPROM writes are synchronous at the WIPI DB layer and FAT writes
    // fflush/fsync/close each record. Nothing is intentionally buffered here.
}

bool IsPersistentName(const char *name) { return record_id_for_name(name)>=0; }
int RecordIdForName(const char *name) { return record_id_for_name(name); }

bool Exists(const char *name,uint32_t *out_len) {
    if(out_len) *out_len=0;
    const int id=record_id_for_name(name); if(id<0) return false;
    uint32_t len=0;
    if(native_record_info(id,&len)) { if(out_len) *out_len=len; return true; }
    if(fat_record_info(id,&len)) { if(out_len) *out_len=len; return true; }
    return false;
}

int Load(const char *name,uint8_t **out_data,uint32_t *out_len) {
    if(!out_data || !out_len) return -9;
    const int id=record_id_for_name(name); if(id<0) return -12;
    unsigned char *mem=nullptr; uint32_t len=0;
    int rc=native_load_record(id,&mem,&len);
    bool used_native=(rc==0);
    if(rc!=0) { rc=fat_load_record(id,&mem,&len); used_native=false; }
    if(rc!=0) return rc;
    *out_data=(uint8_t*)mem; *out_len=len;
    printf("[SAV] load %s rec=%d len=%lu via %s\n",basename_of(name),id,(unsigned long)len,
           used_native?"EEPROM":"FAT");
    return 0;
}

int Store(const char *name,const uint8_t *data,uint32_t len) {
    const int id=record_id_for_name(name); if(id<0) return -12;
    if(len && !data) return -9;
    int rc=native_store_record(id,(const unsigned char*)data,len);
    if(rc==0) return 0;
    const int frc=fat_store_record(id,(const unsigned char*)data,len);
    if(frc==0) return 0;
    printf("[SAV!] store %s failed native=%d fat=%d\n",basename_of(name),rc,frc);
    return rc!=-12?rc:frc;
}

int Remove(const char *name) {
    const int id=record_id_for_name(name); if(id<0) return -12;
    int nrc=native_remove_record(id);
    int frc=fat_remove_record(id);
    if(nrc==0 || frc==0) return 0;
    return nrc!=-12?nrc:frc;
}

const char *BackendName() {
    if(g_native_available) return "EEPROM128 .sav";
    if(g_fat_available) return "FAT ZLOM.sav";
    return "RAM only";
}
}
