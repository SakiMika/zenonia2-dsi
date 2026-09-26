#include <nds.h>
#include <filesystem.h>
#include <stdio.h>
#include <string.h>
#include "lgt_loader.h"
#include "wipi_shim.h"
#include "embedded_assets.h"

#pragma pack(push,1)
struct InitParam1 {
    uint8_t bootstrap[0x214];
    uint32_t ptr_init_struct;
};
struct InitParam2 {
    uint32_t fn_get_import_table;
    uint32_t fn_get_import_function;
    uint32_t fn_unk3;
    uint32_t fn_unk4;
};
struct InitStruct {
    uint32_t unknown;
    uint32_t fn_init;
    uint32_t init_name;
};
#pragma pack(pop)

extern "C" uint32_t resolver_table(uint32_t table) { return wipi_get_import_table(table); }
extern "C" uint32_t resolver_function(uint32_t table, uint32_t index) {
    return (uint32_t)(uintptr_t)wipi_get_import_function(table,index);
}

static PrintConsole bottom;
static LgtLoadedImage image;

typedef int (*Zen2SoundRequestFn)(void*,int,int,int);
static Zen2SoundRequestFn g_zen2_sound_request_orig=nullptr;

extern "C" int zen2_sound_request_hook(void *self,int id,int arg2,int flag) {
    wipi_note_sound_request(id,arg2,flag);
    if(!g_zen2_sound_request_orig) return 0;
    return g_zen2_sound_request_orig(self,id,arg2,flag);
}

static unsigned patch_zen2_sound_request(LgtLoadedImage *img) {
    if(!img || !img->text) return 0;
    // Zenonia 2 Thumb function 0x15301 formats sound/%03d.mmf and is the
    // central request method used by gameplay SFX/BGM. Relocations turn its
    // literal function pointers into the relocated runtime address; replace
    // those literals with our ARM hook while retaining the original target.
    const uint32_t old_entry=0x00015301u;
    void *translated=lgt_translate_ptr(img,old_entry);
    if(!translated) { printf("[SNDHOOK!] target missing\n"); return 0; }
    const uint32_t target=(uint32_t)(uintptr_t)translated;
    const uint32_t hook=(uint32_t)(uintptr_t)&zen2_sound_request_hook;
    g_zen2_sound_request_orig=(Zen2SoundRequestFn)(uintptr_t)target;
    unsigned patched=0;
    for(uint32_t off=0; off+4<=img->text_size; off+=4) {
        uint32_t *w=(uint32_t*)(img->text+off);
        if(*w==target) { *w=hook; patched++; }
    }
    if(patched) {
        DC_FlushRange(img->text,img->text_size);
        IC_InvalidateRange(img->text,img->text_size);
    }
    printf("[SNDHOOK] %08lx -> %08lx ptrs=%u\n",
           (unsigned long)target,(unsigned long)hook,patched);
    return patched;
}

// Real NDS/DSi hardware has separate D/I caches. The LGT binary's lazy import
// resolver self-modifies executable import stubs, then only invalidates I-cache.
// Emulators tend to tolerate that, but real ARM9 can refetch the stale resolver
// stub while its +0x0c word has already been replaced with the returned function
// pointer. The next lazy resolve then mistakes that pointer for an import index,
// producing logs like [RES?] 1fb:02249xxx and CletRegister never runs.
//
// Resolve and patch all static 16-byte LGT import veneers up front while we own
// cache maintenance. Original veneer layout:
//   e52de004   push {lr}
//   eb......   bl lazy_resolver
//   table
//   index
// Direct veneer layout:
//   e59fc004   ldr ip,[pc,#4]
//   e12fff1c   bx  ip
//   table      (kept for diagnostics)
//   fnptr
static unsigned prepatch_import_region(uint8_t *base, uint32_t size, const char *name) {
    if(!base || size < 16) return 0;
    unsigned patched=0;
    for(uint32_t off=0; off+16<=size; off+=4) {
        uint32_t *w=(uint32_t*)(base+off);
        const uint32_t op0=w[0], op1=w[1], table=w[2], index=w[3];
        if(op0!=0xE52DE004u || (op1 & 0xFF000000u)!=0xEB000000u) continue;
        if(!(table==1u || table==0x1f8u || table==0x1fbu)) continue;
        if(index>=0x2000u) continue;
        void *fn=wipi_get_import_function(table,index);
        if(!fn) continue;
        w[0]=0xE59FC004u; // ldr ip,[pc,#4]
        w[1]=0xE12FFF1Cu; // bx ip
        w[2]=table;
        w[3]=(uint32_t)(uintptr_t)fn;
        patched++;
    }
    if(patched) {
        DC_FlushRange(base,size);
        IC_InvalidateRange(base,size);
    }
    printf("[HW] prepatch %s imports=%u\n",name?name:"?",patched);
    return patched;
}

static void prepatch_lgt_imports(LgtLoadedImage *img) {
    if(!img) return;
    unsigned n=0;
    n += prepatch_import_region(img->text,img->text_size,"text");
    n += prepatch_import_region(img->data,img->data_size,"data");
    DC_FlushAll();
    IC_InvalidateAll();
    printf("[HW] import veneers ready=%u\n",n);
}

static void fatal_wait(const char *msg) {
    wipi_set_log_visible(true);
    printf("\n[FATAL] %s\nSTART = exit\n", msg);
    while(1){scanKeys();if(keysDown()&KEY_START)break;swiDelay(20000);}
}

static uint32_t read_cpsr(void) {
    uint32_t v=0;
    __asm__ volatile("mrs %0, cpsr" : "=r"(v));
    return v;
}

static void dump_irq_state(const char *tag) {
    printf("[IRQ] %s IME=%lu IE=%08lx IF=%08lx DISP=%04x CPSR=%08lx\n",
           tag?tag:"-",
           (unsigned long)REG_IME,
           (unsigned long)REG_IE,
           (unsigned long)REG_IF,
           (unsigned)REG_DISPSTAT,
           (unsigned long)read_cpsr());
}

int main(void) {
    powerOn(POWER_ALL_2D);
    soundEnable();
    soundSetMixerVolume(127);
    wipi_init_video(&bottom);
    consoleSelect(&bottom);
    consoleClear();
    printf("Zenonia Lost Of Memories - DSi port v033\n");
    printf("TOP: fit split 0..159 -> 256x192\n");
    printf("BOTTOM: fit split 160..319 + touch quickslots\n");
    printf("LOG: touch LOG on lower LCD\n");
    printf("[AUD] DS master=127; PCM normalize + forced channel volume\n\n");
    wipi_set_log_visible(false);

    if(!nitroFSInit(NULL)) { fatal_wait("NitroFS init failed"); return 1; }
    wipi_init_storage();

    printf("[ROM] NitroFS payload: %lu bytes / %lu files\n",
           (unsigned long)embedded_asset_pack_size(),
           (unsigned long)embedded_asset_count());
    EmbeddedAssetView binary{};
    if(!embedded_asset_find("binary.mod",&binary)) { fatal_wait("NitroFS binary.mod missing"); return 1; }
    printf("[ROM] binary.mod %lu bytes\n",(unsigned long)binary.size);
    printf("[VID] WIPI framebuffer 240x320 RGB565\n");
    printf("[VID] TOP source rows 0..159 scaled to 256x192\n");
    printf("[VID] BOTTOM source rows 160..319 scaled to 256x192 + touch quickslots\n");
    printf("[SPD] guest clock = 1.20x\n");

    if(!lgt_load_binary_memory(binary.data, binary.size, &image)) {
        fatal_wait("binary.mod load failed"); return 2;
    }
    // binary.mod has already been relocated into guest sections; free the
    // temporary NitroFS read cache before the game begins allocating memory.
    embedded_asset_release_cache();

    // Hardware-safe eager import binding. This avoids the LGT lazy resolver's
    // self-modifying-cache hazard on a physical ARM9.
    prepatch_lgt_imports(&image);
    patch_zen2_sound_request(&image);

    InitParam1 p1{};
    InitParam2 p2{};
    p2.fn_get_import_table=(uint32_t)(uintptr_t)&resolver_table;
    p2.fn_get_import_function=(uint32_t)(uintptr_t)&resolver_function;

    typedef void (*EntryFn)(InitParam1*, InitParam2*, uint32_t);
    EntryFn entry=(EntryFn)(image.entry_new | 1u);
    DC_FlushAll(); IC_InvalidateAll();
    printf("[BOOT] calling ELF entry...\n");
    entry(&p1,&p2,0);
    printf("[BOOT] entry returned, desc=%08lx\n",(unsigned long)p1.ptr_init_struct);

    InitStruct *desc=(InitStruct*)lgt_translate_ptr(&image,p1.ptr_init_struct);
    if(!desc) {
        printf("[BOOT] desc is direct RAM ptr\n");
        desc=(InitStruct*)(uintptr_t)p1.ptr_init_struct;
    }
    if(!desc || !desc->fn_init) { fatal_wait("invalid init descriptor"); return 3; }

    uint32_t init_old=desc->fn_init;
    void *init_new=lgt_translate_ptr(&image,init_old);
    if(!init_new) init_new=(void*)(uintptr_t)init_old;
    printf("[BOOT] init %08lx -> %p\n",(unsigned long)init_old,init_new);
    typedef void (*InitFn)(void);
    InitFn init=(InitFn)((uintptr_t)init_new | (init_old&1u));
    DC_FlushAll(); IC_InvalidateAll();
    init();
    printf("[BOOT] initializer returned\n");
    dump_irq_state("after init");

    const WipiCletState *cs=wipi_clet_state();
    printf("[STATE] registered=%s\n",cs->registered?"YES":"NO");
    if(!cs->registered) { fatal_wait("CletRegister missing"); return 4; }

    // Zenonia 2 diagnostic: CletRegister may hand us pointers expressed in the
    // original ELF virtual address space. Normalize each callback against the
    // relocated image before calling start. Direct RAM pointers are unchanged.
    WipiCletState *csrw=const_cast<WipiCletState*>(cs);
    printf("[CB] normalize callbacks\n");
    for(int i=0;i<6;i++) {
        const uint32_t raw=csrw->callbacks[i];
        const uint32_t thumb=raw&1u;
        void *mapped=raw ? lgt_translate_ptr(&image,raw&~1u) : nullptr;
        if(mapped) csrw->callbacks[i]=(uint32_t)(uintptr_t)mapped|thumb;
        const uint32_t fixed=csrw->callbacks[i];
        const bool ram=((fixed&~1u)>=0x02000000u && (fixed&~1u)<0x04000000u);
        printf("[CB%d] %08lx -> %08lx %s%s\n",i,
               (unsigned long)raw,(unsigned long)fixed,
               mapped?"REBASE ":"DIRECT ",ram?"RAM":"BAD");
    }
    printf("[CLET] main before startClet\n");
    // v005 melonDS diagnostic: do NOT wait for VBlank here. Zenonia 2
    // returns from its initializer with an IRQ/VBlank state that can make
    // swiWaitForVBlank() block forever before CB0 is even entered.
    dump_irq_state("before start");
    printf("[CLET] helper ptr=%p cb0=%08lx\n",(void*)&wipi_start_clet,(unsigned long)csrw->callbacks[0]);
    if(!wipi_start_clet()) { fatal_wait("startClet unavailable"); return 5; }

    printf("[RUN] startClet completed\n");
    wipi_debug_dump("after start");
    printf("[RUN] Zenonia Lost Of Memories v033 single .sav + audio + speed\n");
    printf("[RUN] input/timers enabled\n");
    printf("[RUN] bottom screen = runtime log\n");
    printf("L+R+START = quit diagnostic build\n");

    // Initial paint request; timers and events can trigger subsequent redraws.
    wipi_paint_clet();
    wipi_present();
    wipi_debug_dump("after paint");

    while(1) {
        scanKeys();
        uint32_t down=keysDown();
        uint32_t up=keysUp();
        if((down&KEY_START) && (keysHeld()&KEY_L) && (keysHeld()&KEY_R)) break;
        if(down&KEY_TOUCH) {
            touchPosition t{};
            touchRead(&t);
            wipi_handle_touch(down,(int)t.px,(int)t.py);
        }
        wipi_dispatch_keys(down,up);
        wipi_poll();
    }
    wipi_flush_storage();
    return 0;
}
