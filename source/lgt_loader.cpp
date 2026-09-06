#include "lgt_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

namespace {
#pragma pack(push,1)
struct Elf32_Ehdr {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};
struct Elf32_Shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
};
struct Elf32_Rel { uint32_t r_offset; uint32_t r_info; };
#pragma pack(pop)

static const uint32_t SHT_REL = 9;
static const uint32_t R_ARM_PC24 = 1;
static const uint32_t R_ARM_ABS32 = 2;

struct MemFile { const uint8_t *data; size_t size; };
static bool read_at(const MemFile &f, size_t off, void *dst, size_t n) {
    if (!dst || off>f.size || n>f.size-off) return false;
    memcpy(dst,f.data+off,n); return true;
}
static uint8_t *alloc_section(uint32_t size) {
    size_t rounded=(size+31u)&~31u;
    uint8_t *p=static_cast<uint8_t*>(memalign(32,rounded?rounded:32));
    if(p) memset(p,0,rounded?rounded:32);
    return p;
}
static uintptr_t translate_addr(const LgtLoadedImage *img,uint32_t v) {
    if(v>=img->text_old && v<img->text_old+img->text_size)
        return reinterpret_cast<uintptr_t>(img->text)+(v-img->text_old);
    if(v>=img->data_old && v<img->data_old+img->data_size)
        return reinterpret_cast<uintptr_t>(img->data)+(v-img->data_old);
    if(v>=img->bss_old && v<img->bss_old+img->bss_size)
        return reinterpret_cast<uintptr_t>(img->bss)+(v-img->bss_old);
    return 0;
}
static uint32_t *location_for(const LgtLoadedImage *img,uint32_t old_addr) {
    uintptr_t p=translate_addr(img,old_addr); return p?reinterpret_cast<uint32_t*>(p):nullptr;
}
static bool patch_abs32(const LgtLoadedImage *img,uint32_t site_old) {
    uint32_t *site=location_for(img,site_old); if(!site)return false;
    uintptr_t translated=translate_addr(img,*site); if(translated)*site=(uint32_t)translated;
    return true;
}
static bool patch_arm_pc24(const LgtLoadedImage *img,uint32_t site_old) {
    uint32_t *site=location_for(img,site_old); if(!site)return false;
    uint32_t insn=*site; int32_t imm24=(int32_t)(insn&0x00ffffffu);
    if(imm24&0x00800000)imm24|=~0x00ffffff;
    uint32_t old_target=site_old+8u+(uint32_t)(imm24<<2);
    uintptr_t new_target=translate_addr(img,old_target), new_site=(uintptr_t)site;
    if(!new_target)return true;
    intptr_t delta=(intptr_t)new_target-(intptr_t)(new_site+8u);
    if((delta&3)!=0 || delta<-(1<<25) || delta>=(1<<25))return false;
    *site=(insn&0xff000000u)|(((uint32_t)(delta>>2))&0x00ffffffu);
    return true;
}
}

void *lgt_translate_ptr(const LgtLoadedImage *img,uint32_t old_addr) {
    uintptr_t p=translate_addr(img,old_addr); return p?reinterpret_cast<void*>(p):nullptr;
}

bool lgt_load_binary_memory(const void *data,size_t size,LgtLoadedImage *out) {
    memset(out,0,sizeof(*out));
    if(!data || size<sizeof(Elf32_Ehdr)){printf("[ELF] embedded binary missing\n");return false;}
    MemFile f{(const uint8_t*)data,size};
    Elf32_Ehdr eh{};
    if(!read_at(f,0,&eh,sizeof(eh)) || memcmp(eh.e_ident,"\x7f" "ELF",4)!=0 ||
       eh.e_ident[4]!=1 || eh.e_ident[5]!=1 || eh.e_machine!=40 || eh.e_type!=2) {
        printf("[ELF] invalid ARM ELF32 EXEC\n"); return false;
    }
    if(eh.e_shentsize!=sizeof(Elf32_Shdr)||eh.e_shnum==0||eh.e_shnum>64){printf("[ELF] bad section table\n");return false;}
    size_t sht_bytes=(size_t)eh.e_shnum*sizeof(Elf32_Shdr);
    Elf32_Shdr *sh=(Elf32_Shdr*)malloc(sht_bytes);
    if(!sh||!read_at(f,eh.e_shoff,sh,sht_bytes)){printf("[ELF] cannot read sections\n");free(sh);return false;}
    if(eh.e_shstrndx>=eh.e_shnum){free(sh);return false;}
    uint32_t names_sz=sh[eh.e_shstrndx].sh_size;
    char *names=(char*)malloc(names_sz?names_sz:1);
    if(!names||!read_at(f,sh[eh.e_shstrndx].sh_offset,names,names_sz)){free(names);free(sh);return false;}

    int text_i=-1,data_i=-1,bss_i=-1;
    for(unsigned i=0;i<eh.e_shnum;i++){
        const char *name=(sh[i].sh_name<names_sz)?names+sh[i].sh_name:"?";
        if(!strcmp(name,".text"))text_i=(int)i;
        else if(!strcmp(name,".data"))data_i=(int)i;
        else if(!strcmp(name,".bss"))bss_i=(int)i;
    }
    if(text_i<0||data_i<0||bss_i<0){printf("[ELF] missing text/data/bss\n");free(names);free(sh);return false;}

    out->text_old=sh[text_i].sh_addr; out->text_size=sh[text_i].sh_size;
    out->data_old=sh[data_i].sh_addr; out->data_size=sh[data_i].sh_size;
    out->bss_old=sh[bss_i].sh_addr; out->bss_size=sh[bss_i].sh_size;
    out->entry_old=eh.e_entry;
    out->text=alloc_section(out->text_size); out->data=alloc_section(out->data_size); out->bss=alloc_section(out->bss_size);
    if(!out->text||!out->data||!out->bss||
       !read_at(f,sh[text_i].sh_offset,out->text,out->text_size)||
       !read_at(f,sh[data_i].sh_offset,out->data,out->data_size)){
        printf("[ELF] allocation/read failed\n");free(names);free(sh);return false;
    }

    unsigned abs_patched=0,pc24_patched=0,skipped=0;
    for(unsigned si=0;si<eh.e_shnum;si++){
        if(sh[si].sh_type!=SHT_REL||sh[si].sh_entsize!=sizeof(Elf32_Rel))continue;
        unsigned count=sh[si].sh_size/sizeof(Elf32_Rel);
        for(unsigned ri=0;ri<count;ri++){
            Elf32_Rel rel{};
            if(!read_at(f,sh[si].sh_offset+(size_t)ri*sizeof(rel),&rel,sizeof(rel))){skipped++;continue;}
            uint32_t type=rel.r_info&0xffu;
            if(type==R_ARM_ABS32){if(patch_abs32(out,rel.r_offset))abs_patched++;else skipped++;}
            else if(type==R_ARM_PC24){if(patch_arm_pc24(out,rel.r_offset))pc24_patched++;else skipped++;}
            else skipped++;
        }
    }
    uintptr_t entry=translate_addr(out,eh.e_entry);
    if(!entry){printf("[ELF] entry not in loaded sections\n");free(names);free(sh);return false;}
    out->entry_new=entry;
    DC_FlushRange(out->text,out->text_size); DC_FlushRange(out->data,out->data_size); DC_FlushRange(out->bss,out->bss_size);
    IC_InvalidateRange(out->text,out->text_size); IC_InvalidateRange(out->data,out->data_size);
    printf("[ELF] text %08lx -> %p (%lu)\n",(unsigned long)out->text_old,out->text,(unsigned long)out->text_size);
    printf("[ELF] data %08lx -> %p (%lu)\n",(unsigned long)out->data_old,out->data,(unsigned long)out->data_size);
    printf("[ELF] bss  %08lx -> %p (%lu)\n",(unsigned long)out->bss_old,out->bss,(unsigned long)out->bss_size);
    printf("[ELF] reloc ABS=%u PC24=%u skip=%u\n",abs_patched,pc24_patched,skipped);
    printf("[ELF] entry %08lx -> %p (Thumb)\n",(unsigned long)eh.e_entry,(void*)entry);
    free(names);free(sh);return true;
}
