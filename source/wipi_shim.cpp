#include "wipi_shim.h"
#include "embedded_assets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <time.h>
#include <sys/stat.h>

namespace {
constexpr int PHONE_W=240;
constexpr int PHONE_H=320;
constexpr int DS_W=256;
constexpr int DS_H=192;
constexpr int TOP_SRC_Y=32;
constexpr int UI_HUD_SRC_X=0;
constexpr int UI_HUD_SRC_Y=0;
constexpr int UI_HUD_W=72;
constexpr int UI_HUD_H=48;
constexpr int UI_DIALOG_INFO_SRC_Y=96;
constexpr int UI_DIALOG_CUTSCENE_SRC_Y=128;
constexpr int UI_DIALOG_CAPTURE_H=192; // capture a full lower-screen page so avatar + text fit together
constexpr int MINIMAP_SRC_X=176;
constexpr int MINIMAP_SRC_Y=8;
constexpr int MINIMAP_SRC_W=56;
constexpr int MINIMAP_SRC_H=56;
constexpr int MINIMAP_DST_W=112;
constexpr int MINIMAP_DST_H=112;
constexpr int UI_QUICK_SRC_X=48;
constexpr int UI_QUICK_SRC_Y=278;
constexpr int UI_QUICK_W=144;
constexpr int UI_QUICK_H=42;
constexpr int UI_QUICK_DST_X=56;
constexpr int UI_QUICK_DST_Y=146;
constexpr int LOG_BTN_X=210;
constexpr int LOG_BTN_Y=2;
constexpr int LOG_BTN_W=38;
constexpr int LOG_BTN_H=20;
// Existing on-screen quick slots 4..9 occupy the lower strip of Layout B.
// These are DS touch coordinates (not handset framebuffer coordinates).
constexpr int QUICK_SLOT_X0=51;
constexpr int QUICK_SLOT_W=26;
constexpr int QUICK_SLOT_COUNT=6;
constexpr int QUICK_SLOT_Y0=141;
constexpr int QUICK_SLOT_Y1=191;
constexpr uint8_t SUB_BG_COL=0;
constexpr uint8_t SUB_PANEL_COL=18;
constexpr uint8_t SUB_FRAME_COL=15;
// v029: guest-visible timing at 1.20x. Audio uses a separate unscaled real
// clock so increasing gameplay speed does not shorten SFX/BGM lifetime.
constexpr uint32_t GUEST_TIME_NUM=6;
constexpr uint32_t GUEST_TIME_DEN=5;
constexpr int TIMER_SLOTS=32;
constexpr int DB_SLOTS=24;
constexpr int UNKNOWN_THUNK_SLOTS=192;

// WIPI Runner lower-screen settings UI, adapted from the supplied NDS runner.
// The old minimap/map tab is intentionally NOT included: LOG opens only
// SINGLE, COMBO and LOG tabs.
constexpr int UI_TAB_SINGLE=0;
constexpr int UI_TAB_COMBO=1;
constexpr int UI_TAB_LOG=2;
constexpr int UI_TARGET_COUNT=10;
constexpr int UI_LOG_CAP=24;
constexpr int UI_LOG_LINES=18;

struct RunnerTargetDef { const char *name; int wipi_key; };
static const RunnerTargetDef g_runner_targets[UI_TARGET_COUNT] = {
    {"LS", -5}, {"RS", -16},
    {"1", '1'}, {"3", '3'}, {"5", '5'}, {"7", '7'}, {"9", '9'},
    {"*", '*'}, {"0", '0'}, {"#", '#'}
};
static const uint32_t g_runner_physical_keys[] = {
    KEY_L, KEY_R, KEY_A, KEY_B, KEY_X, KEY_Y,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_START, KEY_SELECT
};
static const char *g_runner_physical_names[] = {
    "L", "R", "A", "B", "X", "Y", "UP", "DOWN", "LEFT", "RIGHT", "START", "SELECT"
};
constexpr int UI_PHYSICAL_COUNT=(int)(sizeof(g_runner_physical_keys)/sizeof(g_runner_physical_keys[0]));

#pragma pack(push,4)
struct WipiFramebuffer {
    uint32_t width;
    uint32_t height;
    uint32_t bpl;
    uint32_t bpp;
    uint32_t buf;
};
struct WipiDisplayInfo {
    uint32_t bpp;
    uint32_t depth;
    uint32_t width;
    uint32_t height;
    uint32_t bpl;
    uint32_t color_type;
    uint32_t red_mask;
    uint32_t blue_mask;
    uint32_t green_mask;
};
// LGT WIPI graphics context ABI is 56 bytes.  Zenonia 2 passes this record
// between its native wrappers and the WIPI service table, so the field offsets
// must match the handset ABI (not the older compact Inotia approximation).
struct WipiGraphicsContext {
    int32_t clip_x1;
    int32_t clip_y1;
    int32_t clip_x2;
    int32_t clip_y2;
    uint32_t foreground;
    uint32_t background;
    uint32_t alpha;
    uint32_t pixel_op;
    uint32_t pixel_param;
    uint32_t font;
    uint32_t style;
    uint32_t xor_enabled;
    int32_t offset_x;
    int32_t offset_y;
};
static_assert(sizeof(WipiGraphicsContext)==56, "LGT graphics context ABI must be 56 bytes");
struct WipiImage {
    WipiFramebuffer img;
    WipiFramebuffer mask;
    uint32_t loop_count;
    uint32_t delay;
    uint32_t animated;
    uint32_t buf;
    uint32_t offset;
    uint32_t current;
    uint32_t len;
};
#pragma pack(pop)

struct TimerSlot {
    void *timer;
    void (*callback)(void*,void*);
    void *param;
    uint64_t due_ms;
    bool armed;
};

static uint16_t *g_phone_fb=nullptr;
static uint16_t *g_ds_fb=nullptr;
static uint16_t *g_top_comp=nullptr;
static uint16_t *g_sub_fb=nullptr;
static uint16_t *g_sub_comp=nullptr;
static int g_main_bg=-1;
static int g_sub_bg=-1;
static int g_log_bg=-1;
static PrintConsole *g_log_console=nullptr;
static bool g_log_visible=false;
static bool g_log_button_drawn=false;
static uint16_t *g_dialogue_snapshot=nullptr;
static bool g_dialogue_snapshot_valid=false;
static int g_dialogue_hold_frames=0;
static bool g_dialogue_last_visible=false;
static int g_dialogue_capture_y=UI_DIALOG_CUTSCENE_SRC_Y;
static WipiFramebuffer g_screen_fb{};
static WipiCletState g_clet{};
static TimerSlot g_timers[TIMER_SLOTS]{};
struct UnknownThunkEntry {
    uint32_t table;
    uint32_t index;
    uint32_t *code;
    bool called;
};
static UnknownThunkEntry g_unknown[UNKNOWN_THUNK_SLOTS]{};
static uint32_t *g_thunk_pool=nullptr;
static int g_thunk_count=0;
static uint32_t g_fallback_stub_count=0;
static uint64_t g_timer_clock_ms=0;
// v009: use Calico's 64-bit monotonic tick counter instead of accumulating
// cpuGetTiming() deltas. This keeps WIPI timers independent of VBlank/guest IRQ state.
static uint64_t g_tick_base=0;
static bool g_clock_started=false;
static uint64_t g_timer_wait_next_ms=0;
static uint64_t g_loop_report_next_ms=0;
static uint32_t g_loop_count=0;
static bool g_trace_next_timer=false;
static bool g_trace_repaint_once=false;
static uint32_t g_db_trace_budget=0;
static uint32_t g_unknown_trace_budget=0;
static uint32_t g_mem_trace_budget=0;
static uint32_t g_timer_trace_budget=0;
static uint32_t g_gfx_trace_budget=0;
static bool g_repaint_pending=false;
static bool g_present_pending=false;
static bool g_in_paint=false;

static uint32_t g_ui_single_map[UI_TARGET_COUNT]{};
static uint32_t g_ui_combo_map1[UI_TARGET_COUNT]{};
static uint32_t g_ui_combo_map2[UI_TARGET_COUNT]{};
static uint32_t g_ui_prev_logical=0;
static int g_ui_tab=UI_TAB_LOG;
static int g_ui_capture_single=-1;
static int g_ui_capture_combo=-1;
static int g_ui_selected_single=0;
static int g_ui_selected_combo=0;
static bool g_ui_dirty=true;
static char g_ui_log_lines[UI_LOG_CAP][32]{};
static int g_ui_log_count=0;
static int g_ui_log_head=0;

// v020: persistent, low-noise audio diagnostics shown in the LOG header.
static uint32_t g_audio_create_count=0;
static uint32_t g_audio_prepare_count=0;
static uint32_t g_audio_play_count=0;
static uint32_t g_audio_ok_count=0;
static uint32_t g_audio_fail_count=0;
static uint32_t g_audio_bytes_count=0;
static uint32_t g_audio_stop_count=0;
static uint32_t g_audio_free_count=0;
static uint32_t g_audio_last_peak=0;
static uint32_t g_audio_last_rate=0;
static uint32_t g_audio_last_samples=0;
static int32_t g_audio_last_channel=-1;
static int32_t g_audio_last_requested_volume=100;
static uint8_t g_audio_last_kind=0;
static uint32_t g_audio_end_count=0;
struct DummyMediaClip;
constexpr int MEDIA_CLIP_SLOTS=16;
static DummyMediaClip *g_media_clips[MEDIA_CLIP_SLOTS]{};
static char g_audio_last_asset[32]{};
static int g_media_profile_volume=100;
static int g_media_device_enabled=1;
static uint32_t g_audio_start_count=0;
// v025 Zenonia-specific BGM recovery. The original game sometimes opens a
// 1xx.mmf BGM resource but never reaches the Play call on this partial WIPI
// implementation. Track the last genuine BGM asset and start it as a loop only
// if the game does not do so itself shortly afterwards.
static char g_audio_last_bgm_asset[32]{};
static bool g_audio_bgm_pending=false;
static uint64_t g_audio_bgm_seen_ms=0;
static bool g_audio_game_bgm_started=false;
static DummyMediaClip *g_audio_fallback_bgm=nullptr;
static DummyMediaClip *g_audio_active_bgm=nullptr;
static uint32_t g_audio_fallback_bgm_count=0;
// v028: exact sound-request SFX recovery. Zenonia 2 formats sound/%03d.mmf at the point
// an effect is requested. If the native media manager does not reach Player.play
// shortly afterwards, play that same MMF once on a free DS channel.
static char g_audio_last_sfx_asset[32]{};
static bool g_audio_sfx_pending=false;
static uint64_t g_audio_sfx_seen_ms=0;
static uint64_t g_audio_clet_started_ms=0;
static uint32_t g_audio_sfx_play_snapshot=0;
static uint32_t g_audio_fallback_sfx_count=0;
static uint64_t g_audio_last_sfx_fire_ms=0;

static uint64_t monotonic_ms(void);
static uint64_t real_monotonic_ms(void);

static int audio_asset_numeric_id(const char *name) {
    if(!name) return -1;
    const char *base=strrchr(name,'/');
    if(!base) base=strrchr(name,'\\');
    base=base?base+1:name;
    if(base[0]<'0' || base[0]>'9') return -1;
    int id=0, digits=0;
    while(base[digits]>='0' && base[digits]<='9' && digits<4) {
        id=id*10+(base[digits]-'0');
        digits++;
    }
    if(digits!=3) return -1;
    return id;
}

static bool audio_asset_is_bgm(const char *name) {
    const int id=audio_asset_numeric_id(name);
    return id>=100 && id<=199;
}

static bool audio_asset_is_sfx(const char *name) {
    const int id=audio_asset_numeric_id(name);
    return id>=0 && id<100;
}

static void audio_note_asset(const char *name) {
    if(!name) return;
    const char *ext=strstr(name,".mmf");
    if(!ext) ext=strstr(name,".MMF");
    if(!ext) return;
    strncpy(g_audio_last_asset,name,sizeof(g_audio_last_asset)-1);
    g_audio_last_asset[sizeof(g_audio_last_asset)-1]='\0';
    if(audio_asset_is_bgm(name)) {
        // Only arm a transition when the requested BGM actually changes.
        if(strcmp(g_audio_last_bgm_asset,name)!=0) {
            strncpy(g_audio_last_bgm_asset,name,sizeof(g_audio_last_bgm_asset)-1);
            g_audio_last_bgm_asset[sizeof(g_audio_last_bgm_asset)-1]='\0';
            g_audio_bgm_seen_ms=real_monotonic_ms();
            g_audio_bgm_pending=true;
            g_audio_game_bgm_started=false;
        }
    } else if(audio_asset_is_sfx(name) && g_clet.started && g_audio_clet_started_ms) {
        const uint64_t now=real_monotonic_ms();
        // Ignore boot/preload traffic. Only recover effect requests once the
        // title has had time to enter normal interactive runtime.
        if(now-g_audio_clet_started_ms >= 4000) {
            strncpy(g_audio_last_sfx_asset,name,sizeof(g_audio_last_sfx_asset)-1);
            g_audio_last_sfx_asset[sizeof(g_audio_last_sfx_asset)-1]='\0';
            g_audio_sfx_seen_ms=now;
            g_audio_sfx_play_snapshot=g_audio_play_count;
            g_audio_sfx_pending=true;
        }
    }
}


static void audio_note_sound_request_internal(int id,int arg2,int flag) {
    if(id<0 || id>999) return;
    char path[32];
    snprintf(path,sizeof(path),"sound/%03d.mmf",id);
    static unsigned trace_budget=8;
    if(trace_budget) {
        printf("[SREQ] id=%03d a2=%d f=%d\n",id,arg2,flag);
        trace_budget--;
    }
    const uint64_t now=real_monotonic_ms();
    if(id>=100 && id<=199) {
        if(strcmp(g_audio_last_bgm_asset,path)!=0) {
            strncpy(g_audio_last_bgm_asset,path,sizeof(g_audio_last_bgm_asset)-1);
            g_audio_last_bgm_asset[sizeof(g_audio_last_bgm_asset)-1]='\0';
            g_audio_bgm_seen_ms=now;
            g_audio_bgm_pending=true;
            g_audio_game_bgm_started=false;
        }
        return;
    }
    if(id>=0 && id<100 && g_clet.started && g_audio_clet_started_ms) {
        // Exact runtime request from Zenonia's sound manager. This is stronger
        // than observing file opens because most effect MMFs are preloaded.
        if(now-g_audio_clet_started_ms >= 2500) {
            const bool same_pending=g_audio_sfx_pending && strcmp(g_audio_last_sfx_asset,path)==0;
            // Do not move the deadline forward every frame for the same effect.
            if(!same_pending) {
                strncpy(g_audio_last_sfx_asset,path,sizeof(g_audio_last_sfx_asset)-1);
                g_audio_last_sfx_asset[sizeof(g_audio_last_sfx_asset)-1]='\0';
                g_audio_sfx_seen_ms=now;
                g_audio_sfx_play_snapshot=g_audio_play_count;
                g_audio_sfx_pending=true;
            }
        }
    }
}

static void media_register_clip(DummyMediaClip *c) {
    if(!c) return;
    for(int i=0;i<MEDIA_CLIP_SLOTS;i++) if(!g_media_clips[i]) { g_media_clips[i]=c; return; }
}
static void media_unregister_clip(DummyMediaClip *c) {
    for(int i=0;i<MEDIA_CLIP_SLOTS;i++) if(g_media_clips[i]==c) { g_media_clips[i]=nullptr; return; }
}

static const char *runner_physical_name(uint32_t bit) {
    for(int i=0;i<UI_PHYSICAL_COUNT;i++) if(g_runner_physical_keys[i]==bit) return g_runner_physical_names[i];
    return "-";
}

static void runner_ui_log_push_line(const char *line) {
    if(!line || !*line) return;
    strncpy(g_ui_log_lines[g_ui_log_head],line,31);
    g_ui_log_lines[g_ui_log_head][31]='\0';
    g_ui_log_head=(g_ui_log_head+1)%UI_LOG_CAP;
    if(g_ui_log_count<UI_LOG_CAP) g_ui_log_count++;
    if(g_log_visible && g_ui_tab==UI_TAB_LOG) g_ui_dirty=true;
}

static void runner_ui_draw(void);

static void runner_ui_log_push_text(const char *text) {
    if(!text) return;
    char line[32]; int n=0;
    for(const char *s=text;;s++) {
        const char ch=*s;
        if(ch=='\n' || ch=='\0') {
            if(n>0) { line[n]='\0'; runner_ui_log_push_line(line); n=0; }
            if(ch=='\0') break;
        } else if(ch!='\r') {
            if(n<31) line[n++]=ch;
        }
    }
}

static int runner_printf(const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap,fmt);
    int n=vsnprintf(buf,sizeof(buf),fmt,ap);
    va_end(ap);
    buf[sizeof(buf)-1]='\0';
    // Runtime logs live in the in-RAM ring buffer. Do not write to the hidden
    // libnds console while gameplay is visible: the 16-bit SUB bitmap consumes
    // the full 128 KiB VRAM C address range and text writes would corrupt it.
    runner_ui_log_push_text(buf);
    if(g_log_visible) {
        g_ui_dirty=true;
        // v006 melonDS bring-up: redraw immediately.  If the guest hangs inside
        // CB0 before wipi_poll(), the last WIPI/import trace must still be visible.
        runner_ui_draw();
    }
    return n;
}
#define printf runner_printf

static void runner_ui_defaults(void) {
    memset(g_ui_single_map,0,sizeof(g_ui_single_map));
    memset(g_ui_combo_map1,0,sizeof(g_ui_combo_map1));
    memset(g_ui_combo_map2,0,sizeof(g_ui_combo_map2));
    // Preserve the v024 WIPI defaults while exposing the full handset keypad.
    g_ui_single_map[0]=KEY_A;       // LS / primary soft key
    g_ui_single_map[1]=KEY_B;       // RS / secondary soft key
    g_ui_single_map[2]=KEY_L;       // 1
    g_ui_single_map[3]=KEY_R;       // 3
    g_ui_single_map[7]=KEY_X;       // *
    g_ui_single_map[8]=KEY_SELECT;  // 0
    g_ui_single_map[9]=KEY_Y;       // #
    g_ui_prev_logical=0;
}

static uint32_t runner_first_physical(uint32_t mask) {
    for(int i=0;i<UI_PHYSICAL_COUNT;i++) if(mask&g_runner_physical_keys[i]) return g_runner_physical_keys[i];
    return 0;
}

static void runner_first_two_physical(uint32_t mask,uint32_t *a,uint32_t *b) {
    *a=0; *b=0;
    for(int i=0;i<UI_PHYSICAL_COUNT;i++) if(mask&g_runner_physical_keys[i]) {
        if(!*a) *a=g_runner_physical_keys[i];
        else if(!*b) { *b=g_runner_physical_keys[i]; return; }
    }
}

static void runner_format_mapping(char *out,size_t cap,int idx,bool combo) {
    if(!out||cap==0) return;
    if(combo) {
        if(g_ui_combo_map1[idx]&&g_ui_combo_map2[idx])
            snprintf(out,cap,"%s+%s",runner_physical_name(g_ui_combo_map1[idx]),runner_physical_name(g_ui_combo_map2[idx]));
        else snprintf(out,cap,"-");
    } else {
        if(g_ui_single_map[idx]) snprintf(out,cap,"%s",runner_physical_name(g_ui_single_map[idx]));
        else snprintf(out,cap,"-");
    }
    out[cap-1]='\0';
}

static void runner_ui_set_row(int row,const char *text) {
    char tmp[32];
    snprintf(tmp,sizeof(tmp),"%-31.31s",text?text:"");
    iprintf("\x1b[%d;0H%s",row,tmp);
}

static void runner_ui_draw(void) {
    if(!g_log_visible || !g_log_console || !g_ui_dirty) return;
    consoleSelect(g_log_console);
    iprintf("\x1b[2J\x1b[0;0H");
    char tabs[32];
    snprintf(tabs,sizeof(tabs),"%s %s %s [close]",
             g_ui_tab==UI_TAB_SINGLE?"[single]":" single ",
             g_ui_tab==UI_TAB_COMBO?"[combo]":" combo ",
             g_ui_tab==UI_TAB_LOG?"[log]":" log ");
    runner_ui_set_row(0,tabs);
    runner_ui_set_row(1,"D-pad logical keys stay fixed");

    if(g_ui_tab==UI_TAB_LOG) {
        char aud[32];
        const int bgm_id=audio_asset_numeric_id(g_audio_last_bgm_asset);
        snprintf(aud,sizeof(aud),"AUD p%lu e%lu B%03d f%lu s%lu c%d",
                 (unsigned long)g_audio_play_count,(unsigned long)g_audio_end_count,
                 bgm_id<0?0:bgm_id,(unsigned long)g_audio_fallback_bgm_count,
                 (unsigned long)g_audio_fallback_sfx_count,(int)g_audio_last_channel);
        runner_ui_set_row(2,aud);
        int start=g_ui_log_head-g_ui_log_count; if(start<0) start+=UI_LOG_CAP;
        for(int i=0;i<UI_LOG_LINES;i++) {
            int logical=g_ui_log_count-UI_LOG_LINES+i;
            if(logical<0 || logical>=g_ui_log_count) runner_ui_set_row(3+i,"");
            else runner_ui_set_row(3+i,g_ui_log_lines[(start+logical)%UI_LOG_CAP]);
        }
        runner_ui_set_row(22,"tap SINGLE/COMBO to remap");
        runner_ui_set_row(23,"settings are RAM-only for now");
    } else {
        runner_ui_set_row(2,g_ui_tab==UI_TAB_SINGLE ?
            (g_ui_capture_single>=0?"press one physical key":"tap a target row to remap") :
            (g_ui_capture_combo>=0?"hold two physical keys":"tap a target row to set combo"));
        for(int i=0;i<UI_TARGET_COUNT;i++) {
            char map[20],row[32];
            runner_format_mapping(map,sizeof(map),i,g_ui_tab==UI_TAB_COMBO);
            bool selected=(g_ui_tab==UI_TAB_SINGLE)?(i==g_ui_selected_single):(i==g_ui_selected_combo);
            snprintf(row,sizeof(row),"%c %-3s -> %-17s",selected?'>':' ',g_runner_targets[i].name,map);
            runner_ui_set_row(3+i*2,row);
            runner_ui_set_row(4+i*2,"");
        }
        runner_ui_set_row(23,"no MAP tab / no minimap");
    }
    g_ui_dirty=false;
}

struct DummyMediaClip {
    uint32_t magic;
    uint32_t type;
    uint8_t *data;
    uint32_t data_len;
    uint32_t data_cap;
    int16_t *pcm;
    uint32_t pcm_samples;
    uint32_t sample_rate;
    int32_t volume;
    int32_t playing;
    int32_t sound_id;
    uint32_t callback;
    uint8_t decoded_kind; // 1=SMAF ATR/Yamaha ADPCM, 2=RIFF PCM, 3=SMAF MTR synth
    uint8_t repeat;
    uint8_t end_notified;
    uint8_t runner_owned;
    uint64_t end_due_ms;
    char source_name[20];
};

// v030: small persistent PCM cache for frequently repeated effect MMFs.
// MTR synthesis is CPU-heavy on ARM9; caching means an attack/menu sound is
// rendered once, then replayed directly without re-decoding on every request.
struct SfxCacheEntry {
    char name[20];
    int16_t *pcm;
    uint32_t samples;
    uint32_t rate;
    uint64_t last_play_ms;
    uint64_t busy_until_ms;
};
constexpr int SFX_CACHE_SLOTS=12;
static SfxCacheEntry g_sfx_cache[SFX_CACHE_SLOTS]{};
static uint32_t g_sfx_cache_hits=0;
static uint32_t g_sfx_cache_misses=0;

constexpr uint32_t MEDIA_MAGIC=0x4d444131u; // MDA1
constexpr uint32_t MEDIA_MAX_ENCODED = 2u*1024u*1024u;
constexpr uint32_t MEDIA_MAX_PCM_BYTES = 6u*1024u*1024u;
constexpr uint32_t MEDIA_SYNTH_RATE = 8000u;
constexpr uint32_t MEDIA_MAX_MIX_BYTES = 0u; // v031 mixes directly into PCM

struct TraceStats {
    uint32_t db_opens=0, db_reads=0, db_read_bytes=0, db_open_fail=0;
    uint32_t get_fb=0, init_ctx=0, set_ctx=0, put_pixel=0, draw_rect=0, fill_rect=0;
    uint32_t draw_image=0, draw_string=0, flush=0, repaint=0, create_image=0;
    uint32_t offscreen_create=0, offscreen_destroy=0, copy_fb=0, copy_area=0;
    uint32_t get_rgb=0, set_rgb=0, timer_set=0, timer_fire=0;
};
static TraceStats g_trace{};

struct DbSlot {
    char name[48];
    uint8_t *data;
    uint32_t len;
    uint32_t capacity;
    uint32_t read_cursor;
    uint32_t write_cursor;
    bool used;
    bool packaged;
};
static DbSlot g_db[DB_SLOTS]{};

// WIPI time is millisecond-resolution. v009 uses Calico's system tick counter:
// it is 64-bit, monotonic, and intended for timed events in libnds 2.x.
// This avoids dependence on the VBlank wait path that Zenonia 2 disrupts.
static uint64_t monotonic_ms(void) {
    if(!g_clock_started) return g_timer_clock_ms;
    const uint64_t ticks=tickGetCount()-g_tick_base;
    return (ticks*1000ULL*(uint64_t)GUEST_TIME_NUM) /
           ((uint64_t)TICK_FREQ*(uint64_t)GUEST_TIME_DEN);
}
static uint64_t real_monotonic_ms(void) {
    if(!g_clock_started) return g_timer_clock_ms;
    const uint64_t ticks=tickGetCount()-g_tick_base;
    return (ticks*1000ULL)/(uint64_t)TICK_FREQ;
}
static uint64_t now_ms(void) { return monotonic_ms(); }
static uint64_t timer_now_ms(void) { return monotonic_ms(); }

static bool valid_ptr(uint32_t p) {
    return p>=0x02000000u && p<0x04000000u;
}

static WipiFramebuffer *as_fb(uint32_t p) {
    if (!valid_ptr(p)) return nullptr;
    WipiFramebuffer *fb=(WipiFramebuffer*)(uintptr_t)p;
    if (!fb->buf || fb->width==0 || fb->height==0 || fb->bpp==0) return nullptr;
    return fb;
}

static uint16_t *fb_pixels16(WipiFramebuffer *fb) {
    if (!fb || fb->bpp!=16 || !valid_ptr(fb->buf)) return nullptr;
    return (uint16_t*)(uintptr_t)fb->buf;
}

static UnknownThunkEntry *find_unknown(uint32_t table, uint32_t index) {
    for (int i=0;i<g_thunk_count;i++) {
        if (g_unknown[i].table==table && g_unknown[i].index==index) return &g_unknown[i];
    }
    return nullptr;
}


// ---------------------------------------------------------------------------
// WIPI Input Method (IM) API, table 0x1FB indexes 0x12C..0x130.
// Zenonia 2 queries these during CB0 before it creates the first gameplay
// screen.  Returning the generic stub's zero for 0x12D makes the game
// dereference a NULL supported-mode table, so provide the standard WIPI IM
// surface even though the DSi port does not need a full Korean automata yet.
//
// Standard API order confirmed from WIPI C API examples:
//   0x12C MC_imGetSurpportModeCount(void)
//   0x12D MC_imGetSupportedModes(void)
//   0x12E MC_imSetCurrentMode(int mode)
//   0x12F MC_imGetCurrentMode(void)
//   0x130 MC_imHandleInput(char key,int type,char *buf1,int *size1,
//                          char *buf2,int *size2)
// ---------------------------------------------------------------------------
static char g_im_mode_en_l[] = "EN/L";
static char g_im_mode_en_s[] = "EN/S";
static char g_im_mode_n123[] = "N123";
static char g_im_mode_ko[]   = "KO";
static char *g_im_modes[] = {
    g_im_mode_en_l, g_im_mode_en_s, g_im_mode_n123, g_im_mode_ko
};
static int32_t g_im_current_mode=1; // EN/S is the least surprising default.
static uint32_t g_im_trace_budget=24;

extern "C" int32_t im_get_support_mode_count(void) {
    if(g_im_trace_budget) {
        printf("[IM] mode count=4\n");
        g_im_trace_budget--;
    }
    return 4;
}

extern "C" char **im_get_supported_modes(void) {
    if(g_im_trace_budget) {
        printf("[IM] modes=%p EN/L EN/S N123 KO\n",(void*)g_im_modes);
        g_im_trace_budget--;
    }
    return g_im_modes;
}

extern "C" int32_t im_set_current_mode(int32_t mode) {
    const int32_t old=g_im_current_mode;
    if(mode>=0 && mode<4) g_im_current_mode=mode;
    if(g_im_trace_budget) {
        printf("[IM] set mode %ld -> %ld\n",(long)old,(long)g_im_current_mode);
        g_im_trace_budget--;
    }
    return 0;
}

extern "C" int32_t im_get_current_mode(void) {
    if(g_im_trace_budget) {
        printf("[IM] get mode=%ld\n",(long)g_im_current_mode);
        g_im_trace_budget--;
    }
    return g_im_current_mode;
}

static inline bool im_writeable_ptr(const void *p) {
    const uintptr_t v=(uintptr_t)p;
    return v>=0x02000000u && v<0x04000000u;
}

extern "C" int32_t im_handle_input(int32_t key, int32_t type,
                                     char *buf1, int32_t *size1,
                                     char *buf2, int32_t *size2) {
    int32_t cap1=0, cap2=0;
    if(im_writeable_ptr(size1)) cap1=*size1;
    if(im_writeable_ptr(size2)) cap2=*size2;

    // WIPI expects size1/size2 to become the number of bytes actually emitted.
    // For ordinary gameplay keys the IME should not synthesize text, so start
    // with empty complete/incomplete buffers.  Printable keypad ASCII is passed
    // through as a minimal fallback for later name-entry screens.
    if(im_writeable_ptr(buf1) && cap1>0) buf1[0]='\0';
    if(im_writeable_ptr(buf2) && cap2>0) buf2[0]='\0';
    if(im_writeable_ptr(size1)) *size1=0;
    if(im_writeable_ptr(size2)) *size2=0;

    if(key>=0x20 && key<=0x7e && im_writeable_ptr(buf1) && cap1>0) {
        char ch=(char)key;
        if(g_im_current_mode==0 && ch>='a' && ch<='z') ch=(char)(ch-'a'+'A');
        if(g_im_current_mode==1 && ch>='A' && ch<='Z') ch=(char)(ch-'A'+'a');
        if(g_im_current_mode==2 && !(ch>='0' && ch<='9')) ch='\0';
        if(ch) {
            buf1[0]=ch;
            if(cap1>1) buf1[1]='\0';
            if(im_writeable_ptr(size1)) *size1=1;
        }
    }

    if(g_im_trace_budget) {
        printf("[IM] input k=%ld t=%ld out=%ld/%ld\n",
               (long)key,(long)type,
               (long)(im_writeable_ptr(size1)?*size1:0),
               (long)(im_writeable_ptr(size2)?*size2:0));
        g_im_trace_budget--;
    }
    return 0;
}

extern "C" uint32_t wipi_dynamic_stub(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t key) {
    uint32_t table=key>>16, index=key&0xffffu;
    UnknownThunkEntry *e=find_unknown(table,index);
    const bool first=(!e || !e->called);
    const bool timer_trace=(g_trace_next_timer && g_unknown_trace_budget>0);
    if (first || timer_trace) {
        printf("[CALL?] %03lx:%03lx a=%08lx,%08lx,%08lx%s\n",
               (unsigned long)table,(unsigned long)index,
               (unsigned long)a0,(unsigned long)a1,(unsigned long)a2,
               timer_trace?" T":"");
        if(e) e->called=true;
        if(timer_trace) g_unknown_trace_budget--;
    }
    return 0;
}

// Make a tiny ARM interworking trampoline for every unresolved table:index.
// r0-r2 stay untouched; r3 becomes a packed import key, then BX reaches the
// common C handler. This fixes v002's misleading "last resolved import" log.
static void *make_unknown_thunk(uint32_t table, uint32_t index) {
    if (UnknownThunkEntry *e=find_unknown(table,index)) return (void*)e->code;
    if (!g_thunk_pool || g_thunk_count>=UNKNOWN_THUNK_SLOTS) return nullptr;
    UnknownThunkEntry &e=g_unknown[g_thunk_count];
    e.table=table; e.index=index; e.called=false;
    e.code=g_thunk_pool + g_thunk_count*5;
    uint32_t *c=e.code;
    c[0]=0xE59F3004u; // ldr r3,[pc,#4]  -> key at +12
    c[1]=0xE59FC004u; // ldr ip,[pc,#4]  -> handler at +16
    c[2]=0xE12FFF1Cu; // bx ip
    c[3]=(table<<16)|(index&0xffffu);
    c[4]=(uint32_t)(uintptr_t)&wipi_dynamic_stub;
    DC_FlushRange(c,5*sizeof(uint32_t));
    IC_InvalidateAll();
    g_thunk_count++;
    printf("[RES?] %03lx:%03lx\n",(unsigned long)table,(unsigned long)index);
    return (void*)c;
}

extern "C" uint32_t wipi_stub(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    if (g_fallback_stub_count++<8)
        printf("[STUB!] no thunk a=%08lx,%08lx\n",(unsigned long)a0,(unsigned long)a1);
    (void)a2;(void)a3;
    return 0;
}

extern "C" uint32_t clet_register(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    (void)a2;(void)a3;
    g_clet.register_arg0=a0; g_clet.register_arg1=a1; g_clet.registered=true;
    const uint32_t *p=reinterpret_cast<const uint32_t *>(a0);
    if (p && valid_ptr(a0)) {
        for (int i=0;i<6;i++) g_clet.callbacks[i]=p[i];
    }
    printf("[CLET] register a0=%08lx a1=%08lx\n", (unsigned long)a0,(unsigned long)a1);
    for (int i=0;i<6;i++) printf(" %d:%08lx%s",i,(unsigned long)g_clet.callbacks[i],(i&1)?"\n":"");
    return 0;
}

// LGT WIPIC 0x68 is still named Unk13 by current WIE and intentionally
// returns success. Keep it explicit so it cannot be confused with another import.
extern "C" int knl_unk68(uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3) {
    static bool once=false;
    if(!once) { printf("[WIPI] 1fb:068 no-op\n"); once=true; }
    (void)a0;(void)a1;(void)a2;(void)a3;
    return 0;
}

// This title also resolves module 0x1f8:0x16 during bootstrap. It is separate
// from WIPIC table 0x1fb and is currently a safe success-only vendor hook.
extern "C" int vendor_1f8_016(uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3) {
    static bool once=false;
    if(!once) { printf("[WIPI] 1f8:016 no-op\n"); once=true; }
    (void)a0;(void)a1;(void)a2;(void)a3;
    return 0;
}

extern "C" void *knl_alloc(uint32_t size) {
    void *p=size ? malloc(size) : nullptr;
    if(g_mem_trace_budget) { printf("[MEM] alloc %lu -> %p\n",(unsigned long)size,p); g_mem_trace_budget--; }
    return p;
}
extern "C" void *knl_calloc(uint32_t size) {
    void *p=size ? calloc(1,size) : nullptr;
    if(g_mem_trace_budget) { printf("[MEM] calloc %lu -> %p\n",(unsigned long)size,p); g_mem_trace_budget--; }
    return p;
}
extern "C" uint32_t knl_free(void *p) {
    uint32_t v=(uint32_t)(uintptr_t)p;
    if(g_mem_trace_budget) { printf("[MEM] free %p\n",p); g_mem_trace_budget--; }
    free(p); return v;
}
extern "C" int knl_total_mem(void) { return 12*1024*1024; }
extern "C" int knl_free_mem(void) { return 6*1024*1024; }
extern "C" uint64_t knl_current_time(void) { return now_ms(); }
extern "C" int knl_get_program_name(char *out, int cap) {
    const char *s="0002C004"; int n=(int)strlen(s)+1; if (!out || cap<n) return -18; memcpy(out,s,n); return 0;
}
extern "C" int knl_get_system_property(const char *id, char *out, uint32_t cap) {
    if (!id || !out || cap==0) return -9;
    const char *v=nullptr;
    if (!strcmp(id,"RSSILEVEL")) v="30";
    else if (!strcmp(id,"BATTERYLEVEL")) v="100";
    else if (!strcmp(id,"PHONEMODEL")) v="Nintendo DSi";
    else if (!strcmp(id,"VOLUMELEVEL")) v="100";
    else if (!strcmp(id,"MEDIADEVICES")) v="audio/x-smaf,audio/mmf,audio/wav";
    else if (!strcmp(id,"PHONENUMBER")) v="";
    else if (!strcmp(id,"MIN")) v="01000000000";
    else if (!strcmp(id,"ANNUN_CALL")||!strcmp(id,"ANNUN_SMS")||!strcmp(id,"ANNUN_SILENT")||!strcmp(id,"ANNUN_ALARM")||!strcmp(id,"ANNUN_SECURITY")||!strcmp(id,"CURRENTCH")||!strcmp(id,"AIRPLANE_MODE")||!strcmp(id,"ROAMING_AREA")||!strcmp(id,"DS_LOCK")) v="0";
    else return -9;
    size_t n=strlen(v)+1; if (n>cap) return -18; memcpy(out,v,n); return 0;
}
extern "C" int knl_set_system_property(const char *id, const char *value) {
    (void)id; (void)value;
    return 0;
}
extern "C" int knl_get_resource_id(const char *name, uint32_t *out_size) {
    if (out_size) *out_size=0;
    if (!name) return -12;
    audio_note_asset(name);
    EmbeddedAssetView a{};
    if(!embedded_asset_find(name,&a)) {
        static unsigned miss_log_count=0;
        const bool audio_miss = strstr(name,".mmf") || strstr(name,".MMF");
        if(audio_miss || miss_log_count<4) {
            printf("[RES!] miss %s\n",name);
            if(!audio_miss) miss_log_count++;
        }
        return -12;
    }
    if (out_size) *out_size=a.size;
    size_t n=strlen(name)+1; char *h=(char*)malloc(n); if (!h) return -3; memcpy(h,name,n);
    return (int)(uintptr_t)h;
}
extern "C" int knl_get_resource(int id, void *buf, uint32_t cap) {
    if (id<0 || !buf) return -9;
    const char *name=(const char*)(uintptr_t)(uint32_t)id;
    EmbeddedAssetView a{}; if(!embedded_asset_find(name,&a)) { printf("[RES!] read miss\n"); return -12; }
    if(a.size>cap) return -18;
    if(a.size) memcpy(buf,a.data,a.size);
    embedded_asset_release_cache();
    return 0;
}
extern "C" int knl_printk(const char *fmt, uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3) {
    if (!fmt) return 0;
    return printf(fmt,a0,a1,a2,a3);
}
extern "C" int knl_sprintk(char *dst,const char *fmt,uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3,uint32_t a4,uint32_t a5) {
    if(!dst||!fmt) return 0;
    return sprintf(dst,fmt,a0,a1,a2,a3,a4,a5);
}
extern "C" uint32_t knl_exit(int code) { printf("[WIPI] Exit(%d)\n",code); return 0; }

extern "C" void knl_def_timer(void *timer, void (*callback)(void*,void*)) {
    // MCTimer is guest-owned opaque storage.  Do not write our callback into
    // it: v014/v015 could overwrite game state next to an embedded timer.
    for (int i=0;i<TIMER_SLOTS;i++) {
        if (g_timers[i].timer==timer) {
            g_timers[i].callback=callback;
            g_timers[i].param=nullptr;
            g_timers[i].armed=false;
            return;
        }
    }
    for (int i=0;i<TIMER_SLOTS;i++) {
        if (g_timers[i].timer==nullptr) {
            g_timers[i].timer=timer;
            g_timers[i].callback=callback;
            g_timers[i].param=nullptr;
            g_timers[i].due_ms=0;
            g_timers[i].armed=false;
            return;
        }
    }
    printf("[TMR!] no slot (%d)\n",TIMER_SLOTS);
}

// MC_knlSetTimer compatibility note (v010): standard WIPI callers use three
// arguments, while this LGT Zenonia 2 binary has a wrapper that also places its
// owner object in r3.  knl_set_timer validates owner+0x0c == timer before using
// that fourth register, so ordinary 3-argument callers remain compatible.
extern "C" void knl_set_timer(void *timer, uint32_t delay, void *param, void *lgt_owner) {
    // Standard WIPI code uses three arguments: (timer, delay, param).  Zenonia 2's
    // LGT wrapper, however, deliberately leaves r2 == NULL and places the owner
    // object in r3.  Its timer is embedded at owner+0x0c and the callback wrapper
    // immediately dereferences the second callback argument as that owner object.
    // Preserve normal 3-argument behaviour, but recognize this strong structural
    // signature so an arbitrary r3 from ordinary callers can never be mistaken for
    // a callback parameter.
    void *effective_param=param;
    bool used_lgt_owner=false;
    const uintptr_t owner=(uintptr_t)lgt_owner;
    const uintptr_t tm=(uintptr_t)timer;
    if(param==nullptr && owner>=0x02000000u && owner<0x04000000u && owner+0x0cu==tm) {
        effective_param=lgt_owner;
        used_lgt_owner=true;
    }
    if (delay>600000u) delay=600000u;
    for (int i=0;i<TIMER_SLOTS;i++) if (g_timers[i].timer==timer) {
        g_timers[i].param=effective_param;
        g_timers[i].due_ms=timer_now_ms()+(uint64_t)delay;
        g_timers[i].armed=true; g_trace.timer_set++;
        return;
    }
    // Never infer a callback by reading opaque guest memory.  If SetTimer is
    // seen without DefTimer, keep the failure explicit so the next screenshot
    // points to the real lifecycle mismatch rather than calling random code.
    printf("[TMR!] set unknown %p d=%lu p=%p\n",timer,(unsigned long)delay,param);
}
extern "C" void knl_unset_timer(void *timer) {
    for (int i=0;i<TIMER_SLOTS;i++) if (g_timers[i].timer==timer) g_timers[i].armed=false;
}

extern "C" void *fb_ptr(void) { return g_phone_fb; }
extern "C" int fb_w(void) { return PHONE_W; }
extern "C" int fb_h(void) { return PHONE_H; }
extern "C" int fb_bpl(void) { return PHONE_W*2; }
extern "C" int fb_bpp(void) { return 16; }

extern "C" uint32_t gfx_get_screen_fb(int kind) {
    g_trace.get_fb++;
    if(g_gfx_trace_budget) { printf("[GFX] screen fb kind=%d -> %p\n",kind,(void*)&g_screen_fb); g_gfx_trace_budget--; }
    return kind==0 ? (uint32_t)(uintptr_t)&g_screen_fb : 0;
}

extern "C" uint32_t gfx_get_image_framebuffer(uint32_t imgp) {
    if(!valid_ptr(imgp)) return 0;
    WipiImage *im=(WipiImage*)(uintptr_t)imgp;
    return (uint32_t)(uintptr_t)&im->img;
}

extern "C" int gfx_get_image_property(uint32_t imgp,int property) {
    if(!valid_ptr(imgp)) return 0;
    WipiImage *im=(WipiImage*)(uintptr_t)imgp;
    if(property==4) return (int)im->img.width;
    if(property==5) return (int)im->img.height;
    return 0;
}

extern "C" uint32_t gfx_create_offscreen_framebuffer(int width,int height) {
    if(width<=0 || height<=0 || width>1024 || height>1024) return 0;
    WipiFramebuffer *fb=(WipiFramebuffer*)calloc(1,sizeof(WipiFramebuffer));
    if(!fb) return 0;
    const size_t bytes=(size_t)width*(size_t)height*2u;
    uint16_t *pixels=(uint16_t*)memalign(32,bytes);
    if(!pixels) { free(fb); return 0; }
    memset(pixels,0,bytes);
    fb->width=(uint32_t)width; fb->height=(uint32_t)height;
    fb->bpl=(uint32_t)width*2u; fb->bpp=16; fb->buf=(uint32_t)(uintptr_t)pixels;
    g_trace.offscreen_create++;
    if(g_gfx_trace_budget) { printf("[GFX] offscreen + %dx%d -> %p\n",width,height,(void*)fb); g_gfx_trace_budget--; }
    return (uint32_t)(uintptr_t)fb;
}

extern "C" void gfx_destroy_offscreen_framebuffer(uint32_t fbp) {
    if(!valid_ptr(fbp) || fbp==(uint32_t)(uintptr_t)&g_screen_fb) return;
    WipiFramebuffer *fb=(WipiFramebuffer*)(uintptr_t)fbp;
    if(fb->buf && valid_ptr(fb->buf)) free((void*)(uintptr_t)fb->buf);
    fb->buf=0; g_trace.offscreen_destroy++;
    if(g_gfx_trace_budget) { printf("[GFX] offscreen - %08lx\n",(unsigned long)fbp); g_gfx_trace_budget--; }
    free(fb);
}

extern "C" void gfx_init_context(WipiGraphicsContext *ctx) {
    g_trace.init_ctx++; if(!ctx) return;
    memset(ctx,0,sizeof(*ctx));
    ctx->clip_x1=0; ctx->clip_y1=0; ctx->clip_x2=0x7fff; ctx->clip_y2=0x7fff;
    ctx->foreground=0; ctx->background=0x00ffffffu; ctx->alpha=255; ctx->pixel_param=255;
}

extern "C" int gfx_set_context(WipiGraphicsContext *ctx,uint32_t op,uint32_t pv) {
    g_trace.set_ctx++; if(!ctx) return -9;
    switch(op) {
        case 0:
            if(valid_ptr(pv)) {
                const int32_t *v=(const int32_t*)(uintptr_t)pv;
                ctx->clip_x1=v[0]; ctx->clip_y1=v[1];
                ctx->clip_x2=v[0] + v[2] - 1; ctx->clip_y2=v[1] + v[3] - 1;
            }
            break;
        case 1: ctx->foreground=pv; break;
        case 2: ctx->background=pv; break;
        case 3: break;
        case 4: if(pv<=255) ctx->alpha=pv; break;
        case 5: ctx->pixel_op=pv; break;
        case 6: ctx->pixel_param=pv; break;
        case 7: ctx->font=pv; break;
        case 8: ctx->style=pv; break;
        case 9:
            if(pv==0) { ctx->pixel_op=0; ctx->xor_enabled=0; }
            else { ctx->alpha=0; ctx->pixel_op=1; ctx->xor_enabled=1; }
            break;
        case 10:
            if(valid_ptr(pv)) {
                const int32_t *v=(const int32_t*)(uintptr_t)pv;
                ctx->offset_x=v[0]; ctx->offset_y=v[1];
            }
            break;
        default: break;
    }
    return 0;
}

static inline bool gfx_clip_point(const WipiFramebuffer *fb,const WipiGraphicsContext *ctx,int x,int y) {
    if(!fb || !ctx || x<0 || y<0 || x>=(int)fb->width || y>=(int)fb->height) return false;
    if(x<ctx->clip_x1 || y<ctx->clip_y1 || x>ctx->clip_x2 || y>ctx->clip_y2) return false;
    return true;
}
static inline void gfx_store_pixel(WipiFramebuffer *fb,int x,int y,const WipiGraphicsContext *ctx,uint16_t color) {
    uint16_t *pix=fb_pixels16(fb); if(!pix || !gfx_clip_point(fb,ctx,x,y)) return;
    pix[y*(fb->bpl/2)+x]=color;
}

extern "C" int gfx_put_pixel(uint32_t fbp,int x,int y,const WipiGraphicsContext *ctx) {
    g_trace.put_pixel++; WipiFramebuffer *fb=as_fb(fbp); if(!fb||!ctx) return 0;
    x+=ctx->offset_x; y+=ctx->offset_y;
    gfx_store_pixel(fb,x,y,ctx,(uint16_t)ctx->foreground);
    return 0;
}

extern "C" int gfx_get_display_info(uint32_t reserved,WipiDisplayInfo *out) {
    (void)reserved; if(!out) return -9;
    out->bpp=16; out->depth=16; out->width=PHONE_W; out->height=PHONE_H; out->bpl=PHONE_W*2;
    out->color_type=1; out->red_mask=0xf800; out->green_mask=0x07e0; out->blue_mask=0x001f;
    return 1;
}
extern "C" int gfx_get_font(int face,int style,int size) { (void)face; (void)style; return size?size:12; }
extern "C" int gfx_get_font_height(int font) { return font>0 && font<64 ? font : 12; }
extern "C" int gfx_get_font_ascent(int font) { int h=gfx_get_font_height(font); return (h*3)/4; }
extern "C" int gfx_get_font_descent(int font) { int h=gfx_get_font_height(font); return h-gfx_get_font_ascent(font); }
extern "C" int gfx_get_string_width(int font,const uint8_t *str,int len) {
    if(!str) return 0; if(len<0) len=(int)strlen((const char*)str);
    int h=gfx_get_font_height(font); return len*((h+1)/2);
}
extern "C" int gfx_flush(int, uint32_t, int, int, uint32_t, uint32_t) {
    g_trace.flush++; g_present_pending=true; return 0;
}
extern "C" int gfx_repaint(int, int, int, int, int) {
    g_trace.repaint++; g_repaint_pending=true;
    if(g_trace_repaint_once) printf("[RPT] queued\n");
    return 0;
}
extern "C" uint16_t gfx_pixel_from_rgb(int r,int g,int b) {
    r&=255; g&=255; b&=255;
    return (uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));
}
extern "C" int gfx_rgb_from_pixel(uint16_t p,int *r,int *g,int *b) {
    if(r) *r=((p>>11)&31)*255/31;
    if(g) *g=((p>>5)&63)*255/63;
    if(b) *b=(p&31)*255/31;
    return p;
}

extern "C" void gfx_fill_rect(uint32_t fbp,int x,int y,int w,int h,const WipiGraphicsContext *ctx) {
    g_trace.fill_rect++; WipiFramebuffer *fb=as_fb(fbp); uint16_t *pix=fb_pixels16(fb); if(!pix||!ctx||w<=0||h<=0) return;
    x+=ctx->offset_x; y+=ctx->offset_y;
    int l=x, t=y, r=x+w, b=y+h;
    if(l<0)l=0; if(t<0)t=0; if(r>(int)fb->width)r=(int)fb->width; if(b>(int)fb->height)b=(int)fb->height;
    if(l<ctx->clip_x1)l=ctx->clip_x1; if(t<ctx->clip_y1)t=ctx->clip_y1;
    if(r>ctx->clip_x2+1)r=ctx->clip_x2+1; if(b>ctx->clip_y2+1)b=ctx->clip_y2+1;
    uint16_t c=(uint16_t)ctx->foreground; int stride=(int)(fb->bpl/2);
    for(int yy=t;yy<b;yy++) for(int xx=l;xx<r;xx++) pix[yy*stride+xx]=c;
}
extern "C" void gfx_draw_rect(uint32_t fbp,int x,int y,int w,int h,const WipiGraphicsContext *ctx) {
    g_trace.draw_rect++; if(w<=0||h<=0||!ctx)return;
    WipiFramebuffer *fb=as_fb(fbp); if(!fb)return; x+=ctx->offset_x; y+=ctx->offset_y;
    const uint16_t c=(uint16_t)ctx->foreground;
    for(int xx=0;xx<w;xx++){ gfx_store_pixel(fb,x+xx,y,ctx,c); gfx_store_pixel(fb,x+xx,y+h-1,ctx,c); }
    for(int yy=0;yy<h;yy++){ gfx_store_pixel(fb,x,y+yy,ctx,c); gfx_store_pixel(fb,x+w-1,y+yy,ctx,c); }
}
extern "C" void gfx_draw_line(uint32_t fbp,int x0,int y0,int x1,int y1,const WipiGraphicsContext *ctx) {
    WipiFramebuffer *fb=as_fb(fbp); if(!fb||!ctx)return; x0+=ctx->offset_x; y0+=ctx->offset_y; x1+=ctx->offset_x; y1+=ctx->offset_y;
    int dx=abs(x1-x0), sx=x0<x1?1:-1, dy=-abs(y1-y0), sy=y0<y1?1:-1, err=dx+dy; uint16_t c=(uint16_t)ctx->foreground;
    for(;;){ gfx_store_pixel(fb,x0,y0,ctx,c); if(x0==x1&&y0==y1)break; int e2=err*2; if(e2>=dy){err+=dy;x0+=sx;} if(e2<=dx){err+=dx;y0+=sy;} }
}

static void gfx_blit_fb(WipiFramebuffer *dst,int dx,int dy,int w,int h,WipiFramebuffer *src,int sx,int sy,const WipiGraphicsContext *ctx) {
    uint16_t *dp=fb_pixels16(dst), *sp=fb_pixels16(src); if(!dp||!sp||!ctx||w<=0||h<=0)return;
    dx+=ctx->offset_x; dy+=ctx->offset_y; int ds=dst->bpl/2, ss=src->bpl/2;
    // Use a temporary line when source and destination are the same framebuffer.
    uint16_t *tmp=nullptr; if(dst==src) tmp=(uint16_t*)malloc((size_t)w*2u);
    for(int yy=0;yy<h;yy++) {
        int iy=sy+yy, oy=dy+yy; if(iy<0||oy<0||iy>=(int)src->height||oy>=(int)dst->height)continue;
        if(tmp){ for(int xx=0;xx<w;xx++){int ix=sx+xx;tmp[xx]=(ix>=0&&ix<(int)src->width)?sp[iy*ss+ix]:0;} }
        for(int xx=0;xx<w;xx++) { int ix=sx+xx, ox=dx+xx; if(ix<0||ox<0||ix>=(int)src->width||ox>=(int)dst->width)continue; if(!gfx_clip_point(dst,ctx,ox,oy))continue; dp[oy*ds+ox]=tmp?tmp[xx]:sp[iy*ss+ix]; }
    }
    free(tmp);
}

extern "C" void gfx_draw_image(uint32_t fbp,int dx,int dy,int w,int h,uint32_t imgp,int sx,int sy,const WipiGraphicsContext *ctx) {
    g_trace.draw_image++; WipiFramebuffer *dst=as_fb(fbp); if(!dst||!ctx||!valid_ptr(imgp)) return;
    WipiImage *im=(WipiImage*)(uintptr_t)imgp; gfx_blit_fb(dst,dx,dy,w,h,&im->img,sx,sy,ctx);
}
extern "C" void gfx_copy_frame_buffer(uint32_t dstp,int dx,int dy,int w,int h,uint32_t srcp,int sx,int sy,const WipiGraphicsContext *ctx) {
    g_trace.copy_fb++; WipiFramebuffer *dst=as_fb(dstp),*src=as_fb(srcp); if(!dst||!src||!ctx)return; gfx_blit_fb(dst,dx,dy,w,h,src,sx,sy,ctx);
}
extern "C" void gfx_copy_area(uint32_t dstp,int dx,int dy,int w,int h,int sx,int sy,const WipiGraphicsContext *ctx) {
    g_trace.copy_area++; WipiFramebuffer *dst=as_fb(dstp); if(!dst||!ctx)return; gfx_blit_fb(dst,dx,dy,w,h,dst,sx+ctx->offset_x,sy+ctx->offset_y,ctx);
}
extern "C" void gfx_draw_arc(uint32_t fbp,int x,int y,int w,int h,int start_angle,int end_angle,const WipiGraphicsContext *ctx) {
    // Conservative boot implementation: keep bounds/clipping correct without
    // pulling floating-point libm into the ARM9 binary. Replace with a sampled
    // integer ellipse only if Zenonia later needs visible circular widgets.
    (void)start_angle; (void)end_angle; gfx_draw_rect(fbp,x,y,w,h,ctx);
}
extern "C" void gfx_fill_arc(uint32_t fbp,int x,int y,int w,int h,int start_angle,int end_angle,const WipiGraphicsContext *ctx) {
    (void)start_angle; (void)end_angle; gfx_fill_rect(fbp,x,y,w,h,ctx);
}
extern "C" void gfx_draw_string(uint32_t,int,int,const uint8_t*,int,const WipiGraphicsContext*) {
    g_trace.draw_string++; // Zenonia 2 carries bitmap fonts; exact handset font rasterization is not needed for boot.
}
extern "C" void gfx_get_rgb_pixels(uint32_t srcp,int x,int y,int w,int h,uint32_t dstp,int dst_bpl) {
    g_trace.get_rgb++; WipiFramebuffer *src=as_fb(srcp); uint16_t *sp=fb_pixels16(src); if(!sp||!valid_ptr(dstp)||w<=0||h<=0)return;
    uint32_t *dst=(uint32_t*)(uintptr_t)dstp; int ss=src->bpl/2; int stride=dst_bpl>0?dst_bpl/4:w;
    for(int yy=0;yy<h;yy++)for(int xx=0;xx<w;xx++){int px=x+xx,py=y+yy;if(px<0||py<0||px>=(int)src->width||py>=(int)src->height)continue;uint16_t p=sp[py*ss+px];uint32_t r=((p>>11)&31)*255/31,g=((p>>5)&63)*255/63,b=(p&31)*255/31;dst[yy*stride+xx]=0xff000000u|(r<<16)|(g<<8)|b;}
}
extern "C" void gfx_set_rgb_pixels(uint32_t dstp,int x,int y,int w,int h,uint32_t srcp,int src_bpl,const WipiGraphicsContext *ctx) {
    g_trace.set_rgb++; WipiFramebuffer *dst=as_fb(dstp); uint16_t *dp=fb_pixels16(dst); if(!dp||!valid_ptr(srcp)||!ctx||w<=0||h<=0)return;
    const uint32_t *src=(const uint32_t*)(uintptr_t)srcp; int ds=dst->bpl/2; int stride=src_bpl>0?src_bpl/4:w;
    for(int yy=0;yy<h;yy++)for(int xx=0;xx<w;xx++){int ox=x+xx+ctx->offset_x,oy=y+yy+ctx->offset_y;if(!gfx_clip_point(dst,ctx,ox,oy))continue;uint32_t c=src[yy*stride+xx];dp[oy*ds+ox]=(uint16_t)((((c>>16)&255)>>3)<<11|((((c>>8)&255)>>2)<<5)|((c&255)>>3));}
}
extern "C" int gfx_create_image(uint32_t *out_image,uint32_t data,uint32_t offset,uint32_t len) {
    g_trace.create_image++;
    if(g_trace.create_image<=8) printf("[IMG?] create data=%08lx off=%lu len=%lu\n",(unsigned long)data,(unsigned long)offset,(unsigned long)len);
    if(out_image)*out_image=0;
    // Encoded-image decode is still intentionally explicit. Zenonia 2 boot currently decodes its PZX assets itself;
    // if this path becomes active the trace gives us the exact encoded format to implement next.
    return -9;
}

extern "C" uint32_t knl_unk6a(uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3) {
    if(g_gfx_trace_budget){ printf("[STUB] 1fb:06a %08lx %08lx %08lx %08lx\n",(unsigned long)a0,(unsigned long)a1,(unsigned long)a2,(unsigned long)a3); g_gfx_trace_budget--; }
    return 0;
}
extern "C" uint32_t gfx_unk_eb(uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3) {
    if(g_gfx_trace_budget){ printf("[STUB] 1fb:0eb %08lx %08lx\n",(unsigned long)a0,(unsigned long)a1); g_gfx_trace_budget--; }
    return 0;
}
extern "C" uint32_t gfx_unk_ee(uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3) {
    if(g_gfx_trace_budget){ printf("[STUB] 1fb:0ee %08lx %08lx %08lx %08lx\n",(unsigned long)a0,(unsigned long)a1,(unsigned long)a2,(unsigned long)a3); g_gfx_trace_budget--; }
    return 0;
}


static int db_load_packaged(DbSlot &d,const char *name) {
    EmbeddedAssetView a{}; if(!embedded_asset_find(name,&a)) return -12;
    uint8_t *mem=(uint8_t*)malloc(a.size?a.size:1);
    if(!mem) return -3;
    if(a.size) memcpy(mem,a.data,a.size);
    embedded_asset_release_cache();
    free(d.data); d.data=mem; d.len=a.size; d.capacity=a.size; d.packaged=true;
    return 0;
}

extern "C" int db_open(const char *name,int mode,int type) {
    (void)type; g_trace.db_opens++; if(!name) return -9;
    audio_note_asset(name);
    // Each packaged database open must get an independent stream cursor.
    // Keep RAM/save databases persistent for now, but never reuse an active
    // packaged asset handle merely because its filename matches.
    for(int i=0;i<DB_SLOTS;i++) if(g_db[i].used && !g_db[i].packaged && !strncmp(g_db[i].name,name,sizeof(g_db[i].name))) {
        g_db[i].read_cursor=0; g_db[i].write_cursor=0;
        printf("[DB] reopen-save %s len=%lu -> %d\n",g_db[i].name,(unsigned long)g_db[i].len,i+1);
        return i+1;
    }
    for(int i=0;i<DB_SLOTS;i++) if(!g_db[i].used) {
        DbSlot &d=g_db[i]; memset(&d,0,sizeof(d)); d.used=true;
        strncpy(d.name,name,sizeof(d.name)-1);
        // Mode 4 is create/truncate in WIPI. For game assets, ordinary opens
        // materialize the packaged NitroFS file into record 1.
        int load=(mode==4)?-12:db_load_packaged(d,name);
        if(load==0) {
            printf("[DB] packaged %s len=%lu -> %d\n",d.name,(unsigned long)d.len,i+1);
            return i+1;
        }
        // WIPI mode 1 is open-existing. Missing save DB must report NOENT so
        // the game takes its fresh-save/create path instead of trying to load
        // an empty phantom database.
        if(mode==1) {
            memset(&d,0,sizeof(d));
            return -12;
        }
        printf("[DB] ram %s mode=%d -> %d\n",d.name,mode,i+1);
        return i+1;
    }
    g_trace.db_open_fail++;
    printf("[DB!] no free handle for %s (slots=%d)\n",name,DB_SLOTS);
    return -3;
}
extern "C" int db_read(int id,void *buf,uint32_t len) {
    if(id<1||id>DB_SLOTS||!g_db[id-1].used||(!buf&&len)) return -25;
    g_trace.db_reads++;
    DbSlot &d=g_db[id-1];
    uint32_t before=d.read_cursor;
    if(d.read_cursor>=d.len) {
        if(g_db_trace_budget) { printf("[DBR] h=%d %s pos=%lu req=%lu -> EOF\n",id,d.name,(unsigned long)before,(unsigned long)len); g_db_trace_budget--; }
        return -23;
    }
    uint32_t n=d.len-d.read_cursor; if(n>len)n=len;
    if(n) memcpy(buf,d.data+d.read_cursor,n);
    d.read_cursor+=n; g_trace.db_read_bytes+=n;
    if(g_db_trace_budget) { printf("[DBR] h=%d %s %lu+%lu -> %lu\n",id,d.name,(unsigned long)before,(unsigned long)n,(unsigned long)d.read_cursor); g_db_trace_budget--; }
    return (int)n;
}
extern "C" int db_write(int id,const void *buf,uint32_t len) {
    if(id<1||id>DB_SLOTS||!g_db[id-1].used||(!buf&&len)) return -25;
    DbSlot &d=g_db[id-1]; uint32_t end=d.write_cursor+len;
    if(end>d.capacity) {
        uint32_t cap=d.capacity?d.capacity:64; while(cap<end) cap*=2;
        uint8_t *p=(uint8_t*)realloc(d.data,cap); if(!p)return -3;
        if(cap>d.capacity) memset(p+d.capacity,0,cap-d.capacity);
        d.data=p; d.capacity=cap;
    }
    if(len) memcpy(d.data+d.write_cursor,buf,len);
    d.write_cursor=end; if(end>d.len)d.len=end; d.packaged=false;
    return (int)len;
}
extern "C" int db_close(int id) {
    if(id<1||id>DB_SLOTS||!g_db[id-1].used) return -25;
    DbSlot &d=g_db[id-1];
    // Packaged assets are immutable and can be materialized again on reopen;
    // release the handle so the next open receives an independent cursor.
    if(d.packaged) {
        free(d.data);
        memset(&d,0,sizeof(d));
    } else {
        // RAM save backing remains alive across close in this diagnostic layer.
        d.read_cursor=0; d.write_cursor=0;
    }
    return 0;
}
extern "C" int db_seek(int id,int offset,int origin) {
    if(id<1||id>DB_SLOTS||!g_db[id-1].used) return -25;
    DbSlot &d=g_db[id-1];
    int64_t base=(origin==0)?0:(origin==1?(int64_t)d.read_cursor:(origin==2?(int64_t)d.len:-1));
    if(base<0) return -1;
    int64_t pos=base+offset;
    if(pos<0) pos=0;
    if(pos>(int64_t)d.len) pos=d.len;
    d.read_cursor=d.write_cursor=(uint32_t)pos;
    if(g_db_trace_budget) { printf("[DBS] h=%d %s off=%d org=%d -> %ld\n",id,d.name,offset,origin,(long)pos); g_db_trace_budget--; }
    return (int)pos;
}
static int db_find_name(const char *name) {
    if(!name) return -1;
    for(int i=0;i<DB_SLOTS;i++) {
        if(g_db[i].used && !strncmp(g_db[i].name,name,sizeof(g_db[i].name))) return i;
    }
    return -1;
}

// LGT/WIPI database metadata helpers. These were still falling through to the
// generic zero-return stub in v011. Inotia uses them during first-run save
// setup; returning success without filling metadata makes the title conclude
// that storage/save state is invalid and display the "not enough storage"
// dialog.
extern "C" int db_list_record_info(const char *name, uint32_t *out, uint32_t capacity) {
    if(!name) return -9;
    int idx=db_find_name(name);
    EmbeddedAssetView a{};
    bool packaged=embedded_asset_find(name,&a);
    if(idx<0 && !packaged) {
        return -12;
    }
    uint32_t len=(idx>=0)?g_db[idx].len:a.size;
    if(out && capacity>0) {
        // One record: {record id, flags/reserved, byte length}.
        out[0]=1;
        out[1]=0;
        out[2]=len;
    }
    return 0;
}

extern "C" int db_list_record(int id, uint32_t *out, uint32_t buf_len) {
    if(id<1||id>DB_SLOTS||!g_db[id-1].used) return -25;
    if(out && buf_len>=4) out[0]=1;
    return 1; // one stream-backed record
}

extern "C" int db_update_record(int id,int rec_id,const void *buf,uint32_t len) {
    if(id<1||id>DB_SLOTS||!g_db[id-1].used) return -25;
    if(rec_id!=1 || (!buf&&len)) return -22;
    DbSlot &d=g_db[id-1];
    if(len>d.capacity) {
        uint32_t cap=d.capacity?d.capacity:64;
        while(cap<len) cap*=2;
        uint8_t *q=(uint8_t*)realloc(d.data,cap);
        if(!q) return -3;
        if(cap>d.capacity) memset(q+d.capacity,0,cap-d.capacity);
        d.data=q; d.capacity=cap;
    }
    if(len) memcpy(d.data,buf,len);
    d.len=len; d.read_cursor=0; d.write_cursor=len; d.packaged=false;
    return 0;
}

extern "C" int db_select_record(int id,int rec_id,void *buf,uint32_t len) {
    if(id<1||id>DB_SLOTS||!g_db[id-1].used) return -25;
    if(rec_id!=1) return -22;
    DbSlot &d=g_db[id-1];
    if(len<d.len) return -18;
    if(d.len && !buf) return -9;
    if(d.len) memcpy(buf,d.data,d.len);
    return 0;
}

extern "C" int db_exists(const char *name,int) {
    if(!name) return -9;
    EmbeddedAssetView a{};
    bool exists=(db_find_name(name)>=0) || embedded_asset_find(name,&a);
    return exists?0:-12;
}

extern "C" int db_delete(const char *name,int) {
    if(!name)return -9;
    int idx=db_find_name(name);
    if(idx>=0) {
        free(g_db[idx].data); memset(&g_db[idx],0,sizeof(g_db[idx]));
    }
    return 0;
}

// Standard WIPI database slot 12 (LGT method 0x19c): MC_dbListDataBase.
// Several WIPI titles use its no-argument return value as the number of
// bytes available in application database storage rather than as a literal
// database-list operation. Returning the generic stub value 0 makes Inotia 1
// immediately show its "not enough storage" dialog. Keep a deterministic
// 1 MiB quota for the RAM-backed diagnostic save layer.
extern "C" int db_available_storage(void) {
    constexpr int AVAILABLE_BYTES = 1024 * 1024;
    return AVAILABLE_BYTES;
}

extern "C" int std_atoi(const char *s) { return s?atoi(s):0; }
extern "C" char *std_strcpy(char *d,const char*s){return strcpy(d,s);}
extern "C" char *std_strncpy(char*d,const char*s,size_t n){return strncpy(d,s,n);}
extern "C" char *std_strcat(char*d,const char*s){return strcat(d,s);}
extern "C" int std_strcmp(const char*a,const char*b){return strcmp(a,b);}
extern "C" size_t std_strlen(const char*s){return strlen(s);}
extern "C" void *std_memcpy(void*d,const void*s,size_t n){return memcpy(d,s,n);}
extern "C" void *std_memset(void*d,int c,size_t n){return memset(d,c,n);}
extern "C" time_t std_time(time_t *p){return time(p);}
extern "C" struct tm *std_localtime(const time_t *p){return localtime(p);}
extern "C" int std_sprintf(char *dst,const char *fmt,uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3,uint32_t a4,uint32_t a5) {
    if(!dst||!fmt) return 0;
    return sprintf(dst,fmt,a0,a1,a2,a3,a4,a5);
}
static uint32_t g_std_rand_state=1;
extern "C" int std_rand(void) {
    g_std_rand_state=g_std_rand_state*1103515245u+12345u;
    return (int)((g_std_rand_state>>16)&0x7fffu);
}
extern "C" void std_srand(uint32_t seed) { g_std_rand_state=seed; }
extern "C" char *std_strstr(const char *hay,const char *needle) {
    if(!hay||!needle) return nullptr;
    return (char*)strstr(hay,needle);
}
extern "C" int std_unk2(uint32_t) { return 0; }
extern "C" int std_unk4(uint32_t,uint32_t,uint32_t,uint32_t) { return 0; }
extern "C" uint32_t std_unk3(uint32_t fn) {
    if(!valid_ptr(fn & ~1u)) return 0;
    typedef uint32_t (*Fn)(void);
    return ((Fn)(uintptr_t)fn)();
}

static DummyMediaClip *media_clip(uint32_t p) {
    if(!valid_ptr(p)) return nullptr;
    DummyMediaClip *c=(DummyMediaClip*)(uintptr_t)p;
    return c->magic==MEDIA_MAGIC?c:nullptr;
}

static inline uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static inline uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1]<<8));
}
static inline uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static inline int16_t clip_s16(int v) {
    if(v>32767) return 32767;
    if(v<-32768) return -32768;
    return (int16_t)v;
}

struct YamahaState { int predictor; int step; };
static int16_t yamaha_expand_nibble(YamahaState &s, uint8_t nibble) {
    static const int16_t indexscale[16]={
        230,230,230,230,307,409,512,614,
        230,230,230,230,307,409,512,614
    };
    static const int8_t difflookup[16]={
        1,3,5,7,9,11,13,15,-1,-3,-5,-7,-9,-11,-13,-15
    };
    if(!s.step) { s.predictor=0; s.step=127; }
    s.predictor += (s.step * difflookup[nibble & 15]) / 8;
    if(s.predictor>32767) s.predictor=32767;
    if(s.predictor<-32768) s.predictor=-32768;
    s.step=(s.step * indexscale[nibble & 15]) >> 8;
    if(s.step<127) s.step=127;
    if(s.step>24576) s.step=24576;
    return (int16_t)s.predictor;
}

static const uint32_t kMidiPhase11025[128]={
    3185015u,3374406u,3575058u,3787642u,4012867u,4251485u,4504291u,4772130u,
    5055896u,5356535u,5675051u,6012507u,6370030u,6748811u,7150117u,7575285u,
    8025735u,8502970u,9008582u,9544261u,10111792u,10713070u,11350103u,12025015u,
    12740059u,13497623u,14300233u,15150569u,16051469u,17005939u,18017165u,19088521u,
    20223584u,21426141u,22700205u,24050030u,25480119u,26995246u,28600467u,30301139u,
    32102938u,34011878u,36034330u,38177043u,40447168u,42852281u,45400411u,48100060u,
    50960238u,53990491u,57200933u,60602278u,64205876u,68023757u,72068660u,76354085u,
    80894335u,85704563u,90800821u,96200119u,101920476u,107980983u,114401866u,121204555u,
    128411753u,136047513u,144137319u,152708170u,161788671u,171409126u,181601643u,192400238u,
    203840952u,215961966u,228803732u,242409110u,256823506u,272095026u,288274639u,305416341u,
    323577341u,342818251u,363203285u,384800477u,407681904u,431923931u,457607465u,484818220u,
    513647012u,544190053u,576549277u,610832681u,647154683u,685636503u,726406571u,769600953u,
    815363807u,863847862u,915214929u,969636441u,1027294024u,1088380105u,1153098554u,1221665363u,
    1294309365u,1371273005u,1452813141u,1539201906u,1630727614u,1727695724u,1830429858u,1939272882u,
    2054588048u,2147483647u,2147483647u,2147483647u,2147483647u,2147483647u,2147483647u,2147483647u,
    2147483647u,2147483647u,2147483647u,2147483647u,2147483647u,2147483647u,2147483647u,2147483647u
};

static void media_release_pcm(DummyMediaClip *c) {
    if(!c) return;
    if(c->sound_id>=0) { soundKill(c->sound_id); c->sound_id=-1; }
    if(c->pcm) { free(c->pcm); c->pcm=nullptr; }
    c->pcm_samples=0;
    c->sample_rate=0;
    c->decoded_kind=0;
    c->playing=0;
}

static void media_stop_runner_duplicates(const char *source_name) {
    if(!source_name || !*source_name) return;
    for(int i=0;i<MEDIA_CLIP_SLOTS;i++) {
        DummyMediaClip *d=g_media_clips[i];
        if(!d || d->magic!=MEDIA_MAGIC || !d->runner_owned) continue;
        if(strcmp(d->source_name,source_name)!=0) continue;
        if(d==g_audio_fallback_bgm) g_audio_fallback_bgm=nullptr;
        media_release_pcm(d);
        g_media_clips[i]=nullptr;
        if(d->data) free(d->data);
        d->data=nullptr; d->magic=0; free(d);
    }
}

static void media_stop_previous_bgm(DummyMediaClip *next) {
    DummyMediaClip *old=g_audio_active_bgm;
    if(!old || old==next || old->magic!=MEDIA_MAGIC) return;
    if(old->sound_id>=0) soundKill(old->sound_id);
    old->sound_id=-1;
    old->playing=0;
    old->end_notified=1;
    // Do not send STOP here: this is an internal single-owner handoff and the
    // game may be in the middle of replacing the clip already.
    g_audio_active_bgm=nullptr;
}


static int smaf_timebase_ms(uint8_t code) {
    switch(code) {
        case 0x00: return 1;
        case 0x01: return 2;
        case 0x02: return 4;
        case 0x03: return 5;
        case 0x10: return 10;
        case 0x11: return 20;
        case 0x12: return 40;
        case 0x13: return 50;
        default: return 0;
    }
}

static bool smaf_read_vlq(const uint8_t *b,uint32_t n,uint32_t &p,uint32_t &out) {
    out=0;
    for(int i=0;i<4;i++) {
        if(p>=n) return false;
        uint8_t c=b[p++];
        out=(out<<7)|(uint32_t)(c&0x7f);
        if(!(c&0x80)) return true;
    }
    return false;
}

static bool smaf_find_mtsq(const uint8_t *mtr,uint32_t mtr_size,
                           const uint8_t **out_seq,uint32_t *out_size,
                           uint8_t *out_tbd,uint8_t *out_tbg,
                           uint8_t out_chstat[16]) {
    if(!mtr || mtr_size<20 || mtr[0]!=0x02) return false; // Mobile Standard, no compression
    if(out_tbd) *out_tbd=mtr[2];
    if(out_tbg) *out_tbg=mtr[3];
    if(out_chstat) memcpy(out_chstat,mtr+4,16);
    uint32_t p=20;
    while(p+8<=mtr_size) {
        uint32_t sz=read_be32(mtr+p+4);
        uint32_t payload=p+8;
        if(payload>mtr_size || sz>mtr_size-payload) break;
        if(!memcmp(mtr+p,"Mtsq",4)) {
            if(out_seq) *out_seq=mtr+payload;
            if(out_size) *out_size=sz;
            return true;
        }
        p=payload+sz;
    }
    return false;
}

struct SmafSynthState {
    uint8_t program[16];
    uint8_t volume[16];
    uint8_t expression[16];
    uint8_t velocity[16];
};

static void smaf_synth_state_init(SmafSynthState &s) {
    memset(&s,0,sizeof(s));
    for(int i=0;i<16;i++) {
        s.volume[i]=127;
        s.expression[i]=127;
        s.velocity[i]=64;
    }
}

static inline int32_t synth_wave(uint32_t phase,uint8_t program) {
    const uint16_t u=(uint16_t)(phase>>16);
    const int32_t saw=(int16_t)u;
    const int32_t tri=(u<32768u) ? ((int32_t)u*2-32768) : (98303-(int32_t)u*2);
    const int32_t sq=(phase&0x80000000u)?-32767:32767;
    switch(program>>3) {
        case 0: return (tri*3+saw)/4;             // piano
        case 1: return tri;                       // chromatic
        case 2: return (sq*2+tri)/3;              // organ
        case 3: return (saw+tri)/2;               // guitar
        case 4: return (sq+tri*2)/3;              // bass
        case 5: return (saw*2+tri)/3;              // strings
        case 6: return (saw+tri*2)/3;              // ensemble
        case 7: return (sq+saw)/2;                 // brass
        case 8: return (sq+tri)/2;                 // reed
        case 9: return tri;                        // pipe
        case 10:return (sq+saw)/2;                 // synth lead
        case 11:return (tri*3+saw)/4;              // synth pad
        case 12:return saw;                        // synth fx
        case 13:return (sq+tri)/2;                 // ethnic
        case 14:return tri;                        // percussion-ish melodic
        default:return (tri+saw)/2;
    }
}

static void smaf_mix_note(int16_t *pcm,uint32_t total_samples,
                          uint64_t start_ms,uint64_t gate_ms,
                          uint8_t ch,uint8_t note,uint8_t vel,
                          uint8_t program,uint8_t volume,uint8_t expression,
                          uint32_t seed) {
    if(!pcm || note>127 || gate_ms==0) return;
    uint64_t s64=start_ms*(uint64_t)MEDIA_SYNTH_RATE/1000u;
    uint64_t n64=gate_ms*(uint64_t)MEDIA_SYNTH_RATE/1000u;
    if(n64<2) n64=2;
    if(s64>=total_samples) return;
    if(n64>total_samples-s64) n64=total_samples-s64;
    // Very long pad gates are expensive on ARM9 and sound fine with a gentle
    // two-second release in this lightweight synth. This bounds worst-case work.
    const uint32_t max_gate=MEDIA_SYNTH_RATE*2u;
    if(n64>max_gate) n64=max_gate;
    uint32_t start=(uint32_t)s64, count=(uint32_t)n64;
    int amp=2200;
    amp=amp*(int)(vel?vel:1)/127;
    amp=amp*(int)volume/127;
    amp=amp*(int)expression/127;
    if(amp<32) return;
    const uint32_t attack=(count<(MEDIA_SYNTH_RATE/160u))?count:(MEDIA_SYNTH_RATE/160u);
    const uint32_t release=(count<(MEDIA_SYNTH_RATE/32u))?count:(MEDIA_SYNTH_RATE/32u);
    uint32_t phase=seed*2654435761u;
    // kMidiPhase11025 is the phase increment for 11.025 kHz. Rescale it for
    // the 8 kHz v031 synth without any floating point at runtime.
    uint32_t step=(uint32_t)(((uint64_t)kMidiPhase11025[note]*11025u)/MEDIA_SYNTH_RATE);
    uint32_t noise=seed^((uint32_t)note<<16)^0x9e3779b9u;
    const bool drum=(ch==9);
    for(uint32_t i=0;i<count;i++) {
        int env=32767;
        if(attack && i<attack) env=(int)(i*32767u/attack);
        uint32_t left=count-i;
        if(release && left<release) {
            int r=(int)(left*32767u/release);
            if(r<env) env=r;
        }
        int32_t w;
        if(drum && note>=45) {
            noise^=noise<<13; noise^=noise>>17; noise^=noise<<5;
            w=(int16_t)(noise>>16);
            uint32_t decay_len=MEDIA_SYNTH_RATE/5u;
            uint32_t decay=(i<decay_len)?(decay_len-i):0;
            env=(int)((uint32_t)env*decay/decay_len);
        } else if(drum) {
            w=synth_wave(phase,32);
        } else {
            w=synth_wave(phase,program);
        }
        // Two staged Q15 multiplies keep all arithmetic 32-bit. The v030
        // version used 64-bit multiply/divide for every sample and could look
        // completely frozen for dense BGM tracks on the ARM9.
        int32_t v=(w*amp)>>15;
        v=(v*env)>>15;
        int32_t mixed=(int32_t)pcm[start+i]+v;
        pcm[start+i]=clip_s16(mixed);
        phase+=step;
    }
}

// Decode Inotia's Mobile Standard (FormatType=0x02) score tracks into a
// lightweight GM-like software synth. It is not a bit-exact Yamaha MA-3/MA-5
// FM emulation, but it restores melody, harmony, bass and percussion without
// needing a ROM or external soundfont.
static int media_decode_smaf_mtr(DummyMediaClip *c,const uint8_t *mtr,uint32_t mtr_size) {
    const uint8_t *seq=nullptr; uint32_t seq_size=0; uint8_t tbd=0,tbg=0,chstat[16]{};
    if(!smaf_find_mtsq(mtr,mtr_size,&seq,&seq_size,&tbd,&tbg,chstat)) return -2;
    const int dur_ms=smaf_timebase_ms(tbd), gate_ms=smaf_timebase_ms(tbg);
    if(!dur_ms || !gate_ms || !seq || !seq_size) return -2;

    // Pass 1: determine track length while validating the event stream.
    uint32_t p=0; uint64_t tick=0,max_end_ms=0; uint32_t note_count=0,event_count=0;
    while(p<seq_size) {
        uint32_t d=0; if(!smaf_read_vlq(seq,seq_size,p,d)) return -2;
        tick+=d; event_count++;
        if(p>=seq_size) break;
        uint8_t st=seq[p++], hi=st&0xf0;
        if(hi==0x80 || hi==0x90) {
            if(p>=seq_size) return -2;
            p++; // note number
            if(hi==0x90) { if(p>=seq_size) return -2; p++; }
            uint32_t gate=0; if(!smaf_read_vlq(seq,seq_size,p,gate)) return -2;
            uint64_t end=tick*(uint64_t)dur_ms + gate*(uint64_t)gate_ms;
            if(end>max_end_ms) max_end_ms=end;
            note_count++;
        } else if(hi==0xb0 || hi==0xe0) {
            if(p+2>seq_size) return -2; p+=2;
        } else if(hi==0xc0) {
            if(p>=seq_size) return -2; p++;
        } else if(st==0xf0) {
            uint32_t len=0; if(!smaf_read_vlq(seq,seq_size,p,len) || len>seq_size-p) return -2; p+=len;
        } else if(st==0xff) {
            if(p>=seq_size) return -2;
            uint8_t sub=seq[p++];
            if(sub==0x2f) { if(p<seq_size) p++; break; }
            if(sub!=0x00) return -2;
        } else return -2;
    }
    uint64_t timeline_ms=tick*(uint64_t)dur_ms;
    if(timeline_ms>max_end_ms) max_end_ms=timeline_ms;
    if(max_end_ms<100 || note_count==0) return -2;
    uint64_t total64=(max_end_ms*(uint64_t)MEDIA_SYNTH_RATE)/1000u + 2u;
    if(total64>0xffffffffu) return -3;
    uint32_t total=(uint32_t)total64;
    uint64_t pcm_bytes=(uint64_t)total*sizeof(int16_t);
    if(pcm_bytes>MEDIA_MAX_PCM_BYTES) {
        printf("[SND?] MTR synth too long %lu ms\n",(unsigned long)max_end_ms);
        return -3;
    }
    printf("[SND] MTR fast render start %lu ms notes=%lu\n",
           (unsigned long)max_end_ms,(unsigned long)note_count);
    // v031 mixes straight into a zeroed 16-bit buffer: less than half the RAM
    // of v030 and no large normalization pass after synthesis.
    int16_t *pcm=(int16_t*)memalign(32,(size_t)pcm_bytes);
    if(!pcm) return -3;
    memset(pcm,0,(size_t)pcm_bytes);

    // Pass 2: apply channel state and mix each gated note into the output.
    SmafSynthState state{}; smaf_synth_state_init(state);
    p=0; tick=0; uint32_t serial=1;
    while(p<seq_size) {
        uint32_t d=0; if(!smaf_read_vlq(seq,seq_size,p,d)) break;
        tick+=d;
        if(p>=seq_size) break;
        uint8_t st=seq[p++],hi=st&0xf0,ch=st&0x0f;
        if(hi==0x80 || hi==0x90) {
            if(p>=seq_size) break;
            uint8_t note=seq[p++];
            uint8_t vel=state.velocity[ch];
            if(hi==0x90) { if(p>=seq_size) break; vel=seq[p++]; state.velocity[ch]=vel; }
            uint32_t gate=0; if(!smaf_read_vlq(seq,seq_size,p,gate)) break;
            smaf_mix_note(pcm,total,tick*(uint64_t)dur_ms,gate*(uint64_t)gate_ms,
                          ch,note,vel,state.program[ch],state.volume[ch],state.expression[ch],serial++);
        } else if(hi==0xb0) {
            if(p+2>seq_size) break;
            uint8_t cc=seq[p++], val=seq[p++];
            if(cc==7) state.volume[ch]=val;
            else if(cc==11) state.expression[ch]=val;
        } else if(hi==0xc0) {
            if(p>=seq_size) break; state.program[ch]=seq[p++];
        } else if(hi==0xe0) {
            if(p+2>seq_size) break; p+=2; // pitch bend ignored in stage 2
        } else if(st==0xf0) {
            uint32_t len=0; if(!smaf_read_vlq(seq,seq_size,p,len) || len>seq_size-p) break; p+=len;
        } else if(st==0xff) {
            if(p>=seq_size) break; uint8_t sub=seq[p++];
            if(sub==0x2f) { if(p<seq_size) p++; break; }
            if(sub!=0x00) break;
        } else break;
    }

    media_release_pcm(c);
    c->pcm=pcm; c->pcm_samples=total; c->sample_rate=MEDIA_SYNTH_RATE; c->decoded_kind=3;
    DC_FlushRange(c->pcm,c->pcm_samples*sizeof(int16_t));
    printf("[SND] SMAF MTR fast %lu ms notes=%lu events=%lu rate=%lu\n",
           (unsigned long)max_end_ms,(unsigned long)note_count,(unsigned long)event_count,
           (unsigned long)MEDIA_SYNTH_RATE);
    return 0;
}

// Decode Yamaha SMAF/MMF. Sampled ATRx + AwaX clips use Yamaha ADPCM;
// Mobile Standard uncompressed MTR score tracks are rendered by the lightweight
// v030 software synth above.
static int media_decode_smaf(DummyMediaClip *c) {
    if(!c || !c->data || c->data_len<16 || memcmp(c->data,"MMMD",4)!=0) return -1;
    const uint8_t *b=c->data;
    const uint32_t n=c->data_len;
    uint32_t p=8;
    const uint8_t *atr=nullptr, *awa=nullptr, *mtr=nullptr;
    uint32_t atr_size=0, awa_size=0, mtr_size=0;
    while(p+8<=n) {
        const uint8_t *tag=b+p;
        uint32_t size=read_be32(b+p+4);
        uint32_t payload=p+8;
        if(payload>n || size>n-payload) break;
        if(tag[0]=='M'&&tag[1]=='T'&&tag[2]=='R' && !mtr) {
            mtr=b+payload; mtr_size=size;
        }
        if(tag[0]=='A'&&tag[1]=='T'&&tag[2]=='R' && !atr) {
            atr=b+payload; atr_size=size;
        }
        p=payload+size;
    }
    // Audio-track SMAF remains the preferred path for sampled SFX.  Pure
    // score-track files (Inotia BGM 1..13, 37..39) fall through to the synth.
    if(!atr || atr_size<6) {
        if(mtr && mtr_size) return media_decode_smaf_mtr(c,mtr,mtr_size);
        return -1;
    }
    const uint8_t params=atr[2];
    const int format=(params>>4)&7;
    const int rate_code=params&15;
    const int channels=(params>>7)+1;
    static const int rates[5]={4000,8000,11025,22050,44100};
    if(format!=1 || rate_code<0 || rate_code>4 || channels<1 || channels>2) {
        printf("[SND?] SMAF ATR params fmt=%d rate=%d ch=%d\n",format,rate_code,channels);
        return -1;
    }
    uint32_t q=6;
    while(q+8<=atr_size) {
        const uint8_t *tag=atr+q;
        uint32_t size=read_be32(atr+q+4);
        uint32_t payload=q+8;
        if(payload>atr_size || size>atr_size-payload) break;
        if(tag[0]=='A'&&tag[1]=='w'&&tag[2]=='a') {
            awa=atr+payload; awa_size=size; break;
        }
        q=payload+size;
    }
    if(!awa || !awa_size) return -1;

    uint64_t out_samples = channels==1 ? (uint64_t)awa_size*2u : (uint64_t)awa_size;
    uint64_t out_bytes=out_samples*sizeof(int16_t);
    if(out_bytes>MEDIA_MAX_PCM_BYTES || out_samples==0) return -3;
    int16_t *pcm=(int16_t*)memalign(32,(size_t)out_bytes);
    if(!pcm) return -3;

    YamahaState st[2]={{0,0},{0,0}};
    uint32_t w=0;
    if(channels==1) {
        for(uint32_t i=0;i<awa_size;i++) {
            uint8_t v=awa[i];
            pcm[w++]=yamaha_expand_nibble(st[0],v&0x0f);
            pcm[w++]=yamaha_expand_nibble(st[0],v>>4);
        }
    } else {
        // Stereo SMAF stores one nibble per channel in each byte. Downmix to
        // mono because a WIPI clip maps most cleanly to one DS PCM channel.
        for(uint32_t i=0;i<awa_size;i++) {
            uint8_t v=awa[i];
            int l=yamaha_expand_nibble(st[0],v&0x0f);
            int r=yamaha_expand_nibble(st[1],v>>4);
            pcm[w++]=(int16_t)((l+r)/2);
        }
    }
    media_release_pcm(c);
    c->pcm=pcm;
    c->pcm_samples=w;
    c->sample_rate=(uint32_t)rates[rate_code];
    c->decoded_kind=1;
    DC_FlushRange(c->pcm,c->pcm_samples*sizeof(int16_t));
    printf("[SND] SMAF ATR %lu Hz %s -> %lu samples\n",
           (unsigned long)c->sample_rate,channels==1?"mono":"stereo->mono",
           (unsigned long)c->pcm_samples);
    return 0;
}

static int media_decode_wav(DummyMediaClip *c) {
    if(!c || !c->data || c->data_len<44) return -1;
    const uint8_t *b=c->data; const uint32_t n=c->data_len;
    if(memcmp(b,"RIFF",4)!=0 || memcmp(b+8,"WAVE",4)!=0) return -1;
    const uint8_t *fmt=nullptr,*dat=nullptr; uint32_t fmt_sz=0,dat_sz=0;
    uint32_t p=12;
    while(p+8<=n) {
        uint32_t sz=read_le32(b+p+4), payload=p+8;
        if(payload>n || sz>n-payload) break;
        if(memcmp(b+p,"fmt ",4)==0) { fmt=b+payload; fmt_sz=sz; }
        else if(memcmp(b+p,"data",4)==0) { dat=b+payload; dat_sz=sz; }
        p=payload+sz+(sz&1u);
    }
    if(!fmt||fmt_sz<16||!dat||!dat_sz) return -1;
    const int codec=read_le16(fmt+0), channels=read_le16(fmt+2), bits=read_le16(fmt+14);
    const uint32_t rate=read_le32(fmt+4);
    if(codec!=1 || (channels!=1&&channels!=2) || (bits!=8&&bits!=16) || rate<1000 || rate>48000) return -1;
    uint32_t bytes_per_frame=(uint32_t)channels*(bits/8);
    uint32_t frames=dat_sz/bytes_per_frame;
    uint64_t out_bytes=(uint64_t)frames*2u;
    if(!frames || out_bytes>MEDIA_MAX_PCM_BYTES) return -3;
    int16_t *pcm=(int16_t*)memalign(32,(size_t)out_bytes); if(!pcm) return -3;
    for(uint32_t i=0;i<frames;i++) {
        int a=0,bv=0;
        if(bits==8) {
            a=((int)dat[i*bytes_per_frame]-128)<<8;
            if(channels==2) bv=((int)dat[i*bytes_per_frame+1]-128)<<8;
        } else {
            a=(int16_t)read_le16(dat+i*bytes_per_frame);
            if(channels==2) bv=(int16_t)read_le16(dat+i*bytes_per_frame+2);
        }
        pcm[i]=(int16_t)(channels==2 ? (a+bv)/2 : a);
    }
    media_release_pcm(c);
    c->pcm=pcm; c->pcm_samples=frames; c->sample_rate=rate; c->decoded_kind=2;
    DC_FlushRange(c->pcm,c->pcm_samples*sizeof(int16_t));
    printf("[SND] RIFF PCM %lu Hz %d-bit %s -> %lu samples\n",
           (unsigned long)rate,bits,channels==1?"mono":"stereo->mono",(unsigned long)frames);
    return 0;
}

static int media_decode(DummyMediaClip *c) {
    if(!c || !c->data || !c->data_len) return -1;
    if(c->pcm && c->pcm_samples && c->sample_rate) return 0;
    if(c->data_len>=4 && memcmp(c->data,"MMMD",4)==0) return media_decode_smaf(c);
    if(c->data_len>=12 && memcmp(c->data,"RIFF",4)==0) return media_decode_wav(c);
    printf("[SND?] unsupported clip header %02x %02x %02x %02x len=%lu\n",
           c->data[0],c->data_len>1?c->data[1]:0,c->data_len>2?c->data[2]:0,c->data_len>3?c->data[3]:0,
           (unsigned long)c->data_len);
    return -2;
}

extern "C" uint32_t media_clip_create(uint32_t type,uint32_t buf_size,uint32_t callback) {
    DummyMediaClip *c=(DummyMediaClip*)calloc(1,sizeof(DummyMediaClip));
    if(!c) return 0;
    c->magic=MEDIA_MAGIC; c->type=type; c->volume=100; c->sound_id=-1; c->callback=callback;
    c->repeat=0; c->end_notified=0;
    if(g_audio_last_asset[0]) {
        strncpy(c->source_name,g_audio_last_asset,sizeof(c->source_name)-1);
        c->source_name[sizeof(c->source_name)-1]='\0';
    }
    media_register_clip(c);
    if(buf_size>0 && buf_size<=MEDIA_MAX_ENCODED) {
        c->data=(uint8_t*)malloc(buf_size);
        if(c->data) c->data_cap=buf_size;
    }
    g_audio_create_count++;
    if(g_audio_create_count<=8) {
        printf("[AUD] create #%lu %s %luB\n",(unsigned long)g_audio_create_count,
               c->source_name[0]?c->source_name:"(clip)",(unsigned long)buf_size);
    }
    return (uint32_t)(uintptr_t)c;
}
extern "C" int media_clip_free(uint32_t p) {
    if(!p) return 0;
    DummyMediaClip *c=media_clip(p); if(!c) return -9;
    g_audio_free_count++;
    if(c==g_audio_active_bgm) g_audio_active_bgm=nullptr;
    if(c==g_audio_fallback_bgm) g_audio_fallback_bgm=nullptr;
    media_unregister_clip(c);
    media_release_pcm(c);
    if(c->data) free(c->data);
    c->data=nullptr; c->magic=0; free(c); return 0;
}
extern "C" int media_clip_put_data(uint32_t p,uint32_t buf,uint32_t len) {
    DummyMediaClip *c=media_clip(p); if(!c || (len && !valid_ptr(buf))) return -1;
    if(!len) return 0;
    if(c->data_len > MEDIA_MAX_ENCODED-len) return -18;
    uint32_t need=c->data_len+len;
    if(need>c->data_cap) {
        uint32_t cap=c->data_cap?c->data_cap:1024;
        while(cap<need && cap<MEDIA_MAX_ENCODED) cap*=2;
        if(cap>MEDIA_MAX_ENCODED) cap=MEDIA_MAX_ENCODED;
        if(cap<need) return -18;
        uint8_t *q=(uint8_t*)realloc(c->data,cap); if(!q) return -3;
        c->data=q; c->data_cap=cap;
    }
    memcpy(c->data+c->data_len,(const void*)(uintptr_t)buf,len);
    c->data_len+=len;
    g_audio_bytes_count+=len;
    // Any new encoded bytes invalidate an older decoded copy.
    if(c->pcm) media_release_pcm(c);
    return (int)len;
}
extern "C" int media_clip_get_volume(uint32_t p) {
    DummyMediaClip *c=media_clip(p); return c?c->volume:0;
}
extern "C" int media_clip_set_volume(uint32_t p,int v) {
    DummyMediaClip *c=media_clip(p); if(!c) return -9;
    if(v<0) v=0;
    if(v>100) v=100;
    c->volume=v;
    g_audio_last_requested_volume=v;
    // v021 test path intentionally keeps DS output at full level. This lets us
    // distinguish a WIPI volume-semantics mismatch from a decoder/output fault.
    if(c->sound_id>=0) soundSetVolume(c->sound_id,127);
    return 0;
}
static uint32_t media_pcm_peak(const DummyMediaClip *c) {
    if(!c || !c->pcm || !c->pcm_samples) return 0;
    uint32_t peak=0;
    for(uint32_t i=0;i<c->pcm_samples;i++) {
        int32_t v=c->pcm[i]; if(v<0) v=-v;
        if((uint32_t)v>peak) peak=(uint32_t)v;
    }
    return peak;
}

static uint32_t media_normalize_for_ds(DummyMediaClip *c) {
    uint32_t peak=media_pcm_peak(c);
    if(!c || !c->pcm || !c->pcm_samples || peak==0) return peak;
    // The lightweight SMAF synth is deliberately conservative. On DS speakers
    // some tracks are effectively inaudible, so raise quiet PCM to a useful
    // peak while preserving already-loud sampled SFX.
    const uint32_t target=24000;
    if(peak<target) {
        uint32_t gain_q12=(target<<12)/peak;
        if(gain_q12>(8u<<12)) gain_q12=(8u<<12);
        for(uint32_t i=0;i<c->pcm_samples;i++) {
            int32_t v=(int32_t)(((int64_t)c->pcm[i]*gain_q12)>>12);
            c->pcm[i]=clip_s16(v);
        }
        DC_FlushRange(c->pcm,c->pcm_samples*sizeof(int16_t));
        peak=media_pcm_peak(c);
    }
    return peak;
}

static void media_notify(DummyMediaClip *c,int event,int parm=0) {
    if(!c || !c->callback) return;
    const uint32_t cb=c->callback;
    if(!valid_ptr(cb & ~1u)) return;
    typedef void (*MediaCb)(uint32_t,int,int);
    MediaCb f=(MediaCb)(uintptr_t)cb;
    DC_FlushAll(); IC_InvalidateAll();
    f((uint32_t)(uintptr_t)c,event,parm);
}

// Zenonia 2 native media ABI observations from binary.mod:
// 4C1 is called as (100,duration): WIPI Vibrator.on(level,duration), not clip prepare.
extern "C" int media_vibrator_on(int level,int duration) {
    (void)level; (void)duration;
    return 0;
}

// 4CE is called with the profile string "GENERAL" and its return value is
// immediately clamped/stored as the game's master media volume. Returning 0
// here (v023) muted the game at manager level before any SFX could play.
extern "C" int media_get_profile_volume(const char *profile) {
    (void)profile;
    return g_media_profile_volume;
}

// 4D1/4D2 are used as a set/get pair with device id 6. The returned byte is
// stored in Zenonia 2's audio-manager enable flag. Keep device 6 enabled.
extern "C" int media_set_device_enabled(uint32_t device,int enabled) {
    if(device==6) {
        g_media_device_enabled = enabled ? 1 : 0;
        if(g_media_device_enabled) {
            soundEnable();
            soundSetMixerVolume((g_media_profile_volume*127)/100);
        } else {
            soundSetMixerVolume(0);
        }
    }
    return 0;
}
extern "C" int media_get_device_enabled(uint32_t device) {
    if(device==6) return g_media_device_enabled;
    return 1;
}

extern "C" int media_clip_prepare(uint32_t p,uint32_t) {
    DummyMediaClip *c=media_clip(p); if(!c) return -9;
    g_audio_prepare_count++;
    const int rc=media_decode(c);
    if(rc==0 && c->pcm && c->pcm_samples) {
        g_audio_ok_count++;
        printf("[AUD] prepared kind=%u rate=%lu samples=%lu\n",
               (unsigned)c->decoded_kind,(unsigned long)c->sample_rate,(unsigned long)c->pcm_samples);
        return 0;
    }
    g_audio_fail_count++;
    printf("[AUD!] prepare rc=%d len=%lu\n",rc,(unsigned long)c->data_len);
    return rc?rc:-1;
}

static void media_stop_fallback_bgm(void) {
    DummyMediaClip *c=g_audio_fallback_bgm;
    if(!c) return;
    g_audio_fallback_bgm=nullptr;
    if(c->sound_id>=0) soundKill(c->sound_id);
    if(c==g_audio_active_bgm) g_audio_active_bgm=nullptr;
    c->sound_id=-1; c->playing=0; c->end_notified=1; c->end_due_ms=0;
    media_unregister_clip(c);
    media_release_pcm(c);
    if(c->data) free(c->data);
    c->data=nullptr; c->magic=0; free(c);
}

extern "C" int media_clip_play(uint32_t p,int repeat) {
    DummyMediaClip *c=media_clip(p); if(!c) return -9;
    const bool is_bgm=audio_asset_is_bgm(c->source_name);

    // Native Player.play always wins. Remove runner-owned copies of the exact
    // same MMF before starting this channel; this eliminates the delayed
    // fallback + native double-play that caused audible echo in v028.
    if(!c->runner_owned && c->source_name[0]) media_stop_runner_duplicates(c->source_name);
    if(!c->runner_owned && is_bgm) {
        g_audio_game_bgm_started=true;
        g_audio_bgm_pending=false;
        media_stop_previous_bgm(c);
        if(g_audio_fallback_bgm && g_audio_fallback_bgm!=c) media_stop_fallback_bgm();
    }

    g_audio_play_count++;
    if(media_decode(c)!=0 || !c->pcm || !c->pcm_samples) {
        g_audio_fail_count++;
        printf("[AUD!] decode fail len=%lu\n",(unsigned long)c->data_len);
        return -1;
    }
    if(c->sound_id>=0) soundKill(c->sound_id);

    g_audio_last_peak=media_normalize_for_ds(c);
    g_audio_last_rate=c->sample_rate;
    g_audio_last_samples=c->pcm_samples;
    g_audio_last_kind=c->decoded_kind;
    g_audio_last_requested_volume=c->volume;
    if(g_audio_last_peak==0) {
        g_audio_fail_count++;
        printf("[AUD!] decoded PCM is silent k=%u n=%lu\n",
               (unsigned)c->decoded_kind,(unsigned long)c->pcm_samples);
        return -4;
    }
    if(!g_media_device_enabled || g_media_profile_volume<=0) return -5;

    soundEnable();
    soundSetMixerVolume((g_media_profile_volume*127)/100);
    DC_FlushRange(c->pcm,c->pcm_samples*sizeof(int16_t));
    const bool loop=(repeat!=0);
    c->repeat=loop?1:0;
    c->end_notified=0;
    c->end_due_ms=loop?0:(real_monotonic_ms()+
        ((uint64_t)c->pcm_samples*1000ULL)/(c->sample_rate?c->sample_rate:8000u)+24ULL);
    c->sound_id=soundPlaySample(c->pcm,SoundFormat_16Bit,c->pcm_samples*sizeof(int16_t),
                                (u16)c->sample_rate,127,64,loop,0);
    c->playing=(c->sound_id>=0)?(loop?2:1):0;
    g_audio_last_channel=c->sound_id;
    if(c->sound_id>=0) {
        soundSetVolume(c->sound_id,127);
        soundSetPan(c->sound_id,64);
        soundSetFreq(c->sound_id,(u16)c->sample_rate);
        if(is_bgm) g_audio_active_bgm=c;
        g_audio_ok_count++;
        g_audio_start_count++;
        if(g_audio_start_count<=12)
            printf("[AUD] PLAY %s ch=%d loop=%d\n",
                   c->source_name[0]?c->source_name:"(clip)",c->sound_id,loop?1:0);
        media_notify(c,2,0); // START
        return 0;
    }
    g_audio_fail_count++;
    return -3;
}
extern "C" int media_clip_stop(uint32_t p) {
    DummyMediaClip *c=media_clip(p); if(!c) return -9;
    g_audio_stop_count++;
    const bool was_playing=(c->playing!=0 || c->sound_id>=0);
    if(c->sound_id>=0) soundKill(c->sound_id);
    c->sound_id=-1; c->playing=0; c->end_notified=1;
    if(was_playing) media_notify(c,3,0); // WIPI PlayListener.STOP
    return 0;
}

// Zenonia 2's audio manager relies on the clip callback to clear its busy state.
// The callback wrapper in binary.mod consumes the event in r1; event 1 is the
// normal end-of-data path used by the game. Without this notification the first
// one-shot clip leaves the sound manager permanently occupied, blocking later
// SFX and BGM requests.
static void media_poll_end_events(void) {
    // v029: no ARM9<->ARM7 channel-state polling. soundGetActiveChannels() is
    // synchronous IPC and was causing a visible hitch every few dozen ms.
    // One-shots have deterministic PCM length, so notify END from an unscaled
    // real clock instead. Keep the PCM alive slightly longer than nominal.
    const uint64_t now=real_monotonic_ms();
    for(int i=0;i<MEDIA_CLIP_SLOTS;i++) {
        DummyMediaClip *c=g_media_clips[i];
        if(!c || c->magic!=MEDIA_MAGIC || c->playing!=1 || c->repeat || c->end_notified) continue;
        if(!c->end_due_ms || now<c->end_due_ms) continue;
        if(c->sound_id>=0) soundKill(c->sound_id);
        c->playing=0; c->sound_id=-1; c->end_due_ms=0; c->end_notified=1;
        g_audio_end_count++;
        if(c->callback) media_notify(c,1,0); // END_OF_DATA
        if(c->runner_owned==2) {
            g_media_clips[i]=nullptr;
            media_release_pcm(c);
            if(c->data) free(c->data);
            c->data=nullptr; c->magic=0; free(c);
        }
    }
}

static int media_start_fallback_bgm(const char *asset_name) {
    if(!asset_name || !*asset_name || g_audio_game_bgm_started || g_audio_active_bgm) return -1;
    if(g_audio_fallback_bgm && strcmp(g_audio_fallback_bgm->source_name,asset_name)==0) return 0;
    EmbeddedAssetView a{};
    if(!embedded_asset_find(asset_name,&a) || !a.data || !a.size) {
        printf("[BGM!] asset missing %s\n",asset_name?asset_name:"-");
        return -12;
    }
    DummyMediaClip *c=(DummyMediaClip*)calloc(1,sizeof(DummyMediaClip));
    if(!c) { embedded_asset_release_cache(); return -3; }
    c->magic=MEDIA_MAGIC;
    c->type=0;
    c->volume=100;
    c->sound_id=-1;
    c->callback=0;
    c->runner_owned=1;
    strncpy(c->source_name,asset_name,sizeof(c->source_name)-1);
    c->source_name[sizeof(c->source_name)-1]='\0';
    c->data=(uint8_t*)malloc(a.size);
    if(!c->data) { c->magic=0; free(c); embedded_asset_release_cache(); return -3; }
    memcpy(c->data,a.data,a.size);
    c->data_len=a.size;
    c->data_cap=a.size;
    embedded_asset_release_cache();
    media_register_clip(c);
    const int rc=media_decode(c);
    if(rc!=0 || !c->pcm || !c->pcm_samples) {
        printf("[BGM!] decode %s rc=%d\n",asset_name,rc);
        media_unregister_clip(c);
        media_release_pcm(c);
        free(c->data); c->magic=0; free(c);
        return rc?rc:-1;
    }
    g_audio_last_peak=media_normalize_for_ds(c);
    if(!g_audio_last_peak) {
        printf("[BGM!] silent %s\n",asset_name);
        media_unregister_clip(c);
        media_release_pcm(c);
        free(c->data); c->magic=0; free(c);
        return -4;
    }
    soundEnable();
    soundSetMixerVolume((g_media_profile_volume*127)/100);
    DC_FlushRange(c->pcm,c->pcm_samples*sizeof(int16_t));
    c->repeat=1; c->end_notified=0; c->end_due_ms=0;
    c->sound_id=soundPlaySample(c->pcm,SoundFormat_16Bit,c->pcm_samples*sizeof(int16_t),
                                (u16)c->sample_rate,110,64,true,0);
    c->playing=(c->sound_id>=0)?2:0;
    if(c->sound_id<0) {
        printf("[BGM!] no channel %s\n",asset_name);
        media_unregister_clip(c);
        media_release_pcm(c);
        free(c->data); c->magic=0; free(c);
        return -3;
    }
    soundSetVolume(c->sound_id,110);
    soundSetPan(c->sound_id,64);
    g_audio_fallback_bgm=c;
    g_audio_fallback_bgm_count++;
    g_audio_last_channel=c->sound_id;
    printf("[BGM] fallback %s ch=%d %luHz n=%lu\n",asset_name,c->sound_id,
           (unsigned long)c->sample_rate,(unsigned long)c->pcm_samples);
    return 0;
}

static SfxCacheEntry *sfx_cache_find(const char *name) {
    if(!name) return nullptr;
    for(int i=0;i<SFX_CACHE_SLOTS;i++) {
        if(g_sfx_cache[i].pcm && strcmp(g_sfx_cache[i].name,name)==0) return &g_sfx_cache[i];
    }
    return nullptr;
}

static SfxCacheEntry *sfx_cache_decode(const char *asset_name) {
    if(!asset_name || !*asset_name) return nullptr;
    if(SfxCacheEntry *hit=sfx_cache_find(asset_name)) { g_sfx_cache_hits++; return hit; }
    int free_slot=-1;
    for(int i=0;i<SFX_CACHE_SLOTS;i++) if(!g_sfx_cache[i].pcm) { free_slot=i; break; }
    if(free_slot<0) return nullptr; // keep cached PCM alive; never evict while ARM7 may still read it

    EmbeddedAssetView a{};
    if(!embedded_asset_find(asset_name,&a) || !a.data || !a.size) return nullptr;
    DummyMediaClip tmp{};
    tmp.magic=MEDIA_MAGIC; tmp.type=0; tmp.volume=100; tmp.sound_id=-1;
    strncpy(tmp.source_name,asset_name,sizeof(tmp.source_name)-1);
    tmp.source_name[sizeof(tmp.source_name)-1]='\0';
    tmp.data=(uint8_t*)malloc(a.size);
    if(!tmp.data) { embedded_asset_release_cache(); return nullptr; }
    memcpy(tmp.data,a.data,a.size); tmp.data_len=a.size; tmp.data_cap=a.size;
    embedded_asset_release_cache();

    const int rc=media_decode(&tmp);
    free(tmp.data); tmp.data=nullptr;
    if(rc!=0 || !tmp.pcm || !tmp.pcm_samples || !tmp.sample_rate) {
        media_release_pcm(&tmp);
        return nullptr;
    }
    if(!media_normalize_for_ds(&tmp)) { media_release_pcm(&tmp); return nullptr; }

    SfxCacheEntry &e=g_sfx_cache[free_slot];
    memset(&e,0,sizeof(e));
    strncpy(e.name,asset_name,sizeof(e.name)-1);
    e.pcm=tmp.pcm; e.samples=tmp.pcm_samples; e.rate=tmp.sample_rate;
    tmp.pcm=nullptr; tmp.pcm_samples=0;
    DC_FlushRange(e.pcm,e.samples*sizeof(int16_t));
    g_sfx_cache_misses++;
    printf("[SFX] cached %s %luHz n=%lu\n",e.name,(unsigned long)e.rate,(unsigned long)e.samples);
    return &e;
}

static int media_start_fallback_sfx(const char *asset_name) {
    if(!asset_name || !*asset_name) return -1;
    SfxCacheEntry *e=sfx_cache_decode(asset_name);
    if(!e || !e->pcm || !e->samples || !e->rate) return -4;

    const uint64_t now=real_monotonic_ms();
    const uint64_t dur_ms=((uint64_t)e->samples*1000ULL)/e->rate;
    // Same effect can be requested several times by one logical action.  Do not
    // stack identical channels while the audible attack portion is still active.
    uint64_t guard=dur_ms;
    if(guard>220) guard=220;
    if(guard<80) guard=80;
    if(now<e->busy_until_ms || (e->last_play_ms && now-e->last_play_ms<guard)) return 0;

    soundEnable();
    soundSetMixerVolume((g_media_profile_volume*127)/100);
    const int ch=soundPlaySample(e->pcm,SoundFormat_16Bit,e->samples*sizeof(int16_t),
                                 (u16)e->rate,118,64,false,0);
    if(ch<0) return -3;
    soundSetVolume(ch,118); soundSetPan(ch,64);
    e->last_play_ms=now;
    e->busy_until_ms=now+guard;
    g_audio_fallback_sfx_count++;
    g_audio_last_channel=ch;
    if(g_audio_fallback_sfx_count<=12)
        printf("[SFX] play %s ch=%d cache=%lu/%lu\n",asset_name,ch,
               (unsigned long)g_sfx_cache_hits,(unsigned long)g_sfx_cache_misses);
    return 0;
}

static void media_poll_sfx_fallback(void) {
    if(!g_audio_sfx_pending || !g_audio_last_sfx_asset[0]) return;
    const uint64_t now=real_monotonic_ms();
    // Give Zenonia's own Player.play path enough time to win before recovery.
    if(now-g_audio_sfx_seen_ms < 70) return;
    g_audio_sfx_pending=false;
    if(g_audio_play_count!=g_audio_sfx_play_snapshot) return;
    // De-bounce repeated DB/resource probes of the same effect.
    if(now-g_audio_last_sfx_fire_ms < 70) return;
    g_audio_last_sfx_fire_ms=now;
    char path[32];
    strncpy(path,g_audio_last_sfx_asset,sizeof(path)-1); path[sizeof(path)-1]='\0';
    (void)media_start_fallback_sfx(path);
}

static void media_poll_bgm_fallback(void) {
    const uint64_t now=real_monotonic_ms();

    // If Zenonia reaches its native Play path, always give the game ownership.
    if(g_audio_game_bgm_started) {
        if(g_audio_fallback_bgm) media_stop_fallback_bgm();
        g_audio_bgm_pending=false;
        return;
    }

    // v026: a fallback track is never allowed to become a global soundtrack.
    // When the game requests a different 1xx.mmf, stop the old fallback and
    // replace it with the newly requested track after a short debounce.
    if(g_audio_fallback_bgm) {
        if(g_audio_bgm_pending && g_audio_last_bgm_asset[0] &&
           strcmp(g_audio_fallback_bgm->source_name,g_audio_last_bgm_asset)!=0 &&
           now-g_audio_bgm_seen_ms>=300) {
            char path[32];
            strncpy(path,g_audio_last_bgm_asset,sizeof(path)-1);
            path[sizeof(path)-1]='\0';
            g_audio_bgm_pending=false;
            media_stop_fallback_bgm();
            (void)media_start_fallback_bgm(path);
        } else if(g_audio_bgm_pending &&
                  strcmp(g_audio_fallback_bgm->source_name,g_audio_last_bgm_asset)==0) {
            g_audio_bgm_pending=false;
        }
        return;
    }

    // Only recover an actual BGM resource requested by Zenonia 2. The old v025
    // hard-coded sound/100.mmf five-second fallback is deliberately removed;
    // that caused one song to loop through the entire game.
    if(g_audio_bgm_pending && g_audio_last_bgm_asset[0] && now-g_audio_bgm_seen_ms>=300) {
        char path[32];
        strncpy(path,g_audio_last_bgm_asset,sizeof(path)-1);
        path[sizeof(path)-1]='\0';
        g_audio_bgm_pending=false;
        (void)media_start_fallback_bgm(path);
    }
}

extern "C" int media_noop4(uint32_t,uint32_t,uint32_t,uint32_t) { return 0; }

static void call_event(int type,int key) {
    if(!g_clet.started || !g_clet.callbacks[5]) return;
    typedef void (*Fn)(int,int,int);
    Fn f=(Fn)(uintptr_t)g_clet.callbacks[5];
    f(type,key,0);
}

static void paint_region(int x,int y,int w,int h) {
    if(!g_clet.started || !g_clet.callbacks[4] || g_in_paint) return;
    g_in_paint=true;
    typedef void (*Fn)(int,int,int,int);
    Fn f=(Fn)(uintptr_t)g_clet.callbacks[4];
    f(x,y,w,h);
    g_in_paint=false;
}

static void service_repaint(void) {
    if(!g_repaint_pending || g_in_paint) return;
    g_repaint_pending=false;
    if(g_trace_repaint_once) printf("[RPT] paint\n");
    // Full screen is deliberate for now: it is safer than guessing whether a
    // handset implementation interprets repaint coordinates as inclusive.
    paint_region(0,0,PHONE_W,PHONE_H);
    g_trace_repaint_once=false;
}

static void process_timers(void) {
    uint64_t t=timer_now_ms();
    for(int i=0;i<TIMER_SLOTS;i++) {
        if(g_timers[i].armed && g_timers[i].callback && t>=g_timers[i].due_ms) {
            auto cb=g_timers[i].callback; void *timer=g_timers[i].timer; void *param=g_timers[i].param;
            const uintptr_t cbaddr=(uintptr_t)cb;
            if((cbaddr&~1u)<0x02000000u || (cbaddr&~1u)>=0x04000000u) {
                printf("[TMR!] BAD cb=%p s=%d\n",(void*)cb,i);
                g_timers[i].armed=false;
                continue;
            }
            g_timers[i].armed=false; g_trace.timer_fire++;
            DC_FlushAll(); IC_InvalidateAll();
            cb(timer,param);
            g_trace_next_timer=false;
        }
    }
}
}

void wipi_note_sound_request(int id,int arg2,int flag) {
    audio_note_sound_request_internal(id,arg2,flag);
}

static void runner_release_all_logical(void);

void wipi_init_video(PrintConsole *log_console) {
    // Main/top: full 16-bit bitmap for the gameplay viewport.
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    g_main_bg=bgInit(3,BgType_Bmp16,BgSize_B16_256x256,0,0);
    g_ds_fb=(uint16_t*)bgGetGfxPtr(g_main_bg);
    memset(g_ds_fb,0,256*256*2);
    g_top_comp=(uint16_t*)memalign(32,256*192*2);
    if(g_top_comp) memset(g_top_comp,0,256*192*2);

    // Sub/bottom: full 16-bit bitmap in VRAM C so colors match the top LCD.
    // VRAM H cannot stay mapped as SUB_BG at the same time: it aliases the
    // beginning of the SUB BG address space and causes the horizontal garbage
    // band/flicker seen in v026. Initialize the settings console in VRAM H, then
    // unmap H during gameplay. We swap C/H only while the settings screen is open.
    videoSetModeSub(MODE_5_2D);
    vramSetBankC(VRAM_C_LCD);
    vramSetBankH(VRAM_H_SUB_BG);

    g_log_console=log_console;
    if(g_log_console) {
        consoleInit(g_log_console,1,BgType_Text4bpp,BgSize_T_256x256,8,0,false,true);
        g_log_bg=g_log_console->bgId;
        bgSetPriority(g_log_bg,0);
        bgHide(g_log_bg);
        consoleSelect(g_log_console);
        consoleClear();
    }

    // Gameplay mapping: VRAM C owns the whole SUB bitmap; VRAM H is detached.
    vramSetBankH(VRAM_H_LCD);
    vramSetBankC(VRAM_C_SUB_BG);
    g_sub_bg=bgInitSub(3,BgType_Bmp16,BgSize_B16_256x256,0,0);
    g_sub_fb=(uint16_t*)bgGetGfxPtr(g_sub_bg);
    memset(g_sub_fb,0,256*256*2);
    g_sub_comp=(uint16_t*)memalign(32,256*192*2);
    if(g_sub_comp) memset(g_sub_comp,0,256*192*2);
    bgSetPriority(g_sub_bg,3);

    // Reserve a simple text palette for the hidden settings/log console.
    BG_PALETTE_SUB[0]=RGB15(0,0,0);
    BG_PALETTE_SUB[15]=RGB15(31,31,31);

    g_phone_fb=(uint16_t*)memalign(32,PHONE_W*PHONE_H*2);
    if(g_phone_fb) memset(g_phone_fb,0,PHONE_W*PHONE_H*2);
    g_dialogue_snapshot=(uint16_t*)memalign(32,PHONE_W*UI_DIALOG_CAPTURE_H*2);
    if(g_dialogue_snapshot) memset(g_dialogue_snapshot,0,PHONE_W*UI_DIALOG_CAPTURE_H*2);
    g_thunk_pool=(uint32_t*)memalign(32,UNKNOWN_THUNK_SLOTS*5*sizeof(uint32_t));
    if(g_thunk_pool) memset(g_thunk_pool,0,UNKNOWN_THUNK_SLOTS*5*sizeof(uint32_t));
    g_screen_fb.width=PHONE_W; g_screen_fb.height=PHONE_H; g_screen_fb.bpl=PHONE_W*2;
    g_screen_fb.bpp=16; g_screen_fb.buf=(uint32_t)(uintptr_t)g_phone_fb;

    runner_ui_defaults();
    runner_ui_log_push_line("WIPI Runner settings ready");

    // cpuStartTiming() ensures the Calico tick system is initialized in current
    // libnds. The WIPI scheduler itself uses tickGetCount()/TICK_FREQ.
    cpuStartTiming(0);
    g_tick_base=tickGetCount();
    g_clock_started=true;
    g_timer_clock_ms=0;
    g_timer_wait_next_ms=0;
    g_loop_report_next_ms=0;
    g_loop_count=0;
    printf("[CLK] tick base=%lu freq=%lu\n",
           (unsigned long)g_tick_base,(unsigned long)TICK_FREQ);
}

static inline uint16_t rgb565_to_ds15(uint16_t p) {
    // WIPI exposes the handset framebuffer as RGB565 (RRRRRGGGGGGBBBBB).
    // Nintendo DS bitmap backgrounds use RGB15 (0BBBBBGGGGGRRRRR) with
    // bit 15 set for an opaque pixel. Convert channels instead of copying
    // the 16-bit value directly, otherwise red/blue are swapped.
    const uint16_t r=(p>>11)&31u;
    const uint16_t g=(p>>6)&31u; // 6-bit WIPI green -> 5-bit DS green
    const uint16_t b=p&31u;
    return (uint16_t)(BIT(15) | r | (g<<5) | (b<<10));
}

static inline uint8_t rgb565_to_sub8(uint16_t p) {
    const int r5=(p>>11)&31;
    const int g6=(p>>5)&63;
    const int b5=p&31;
    const int r=(r5*5+15)/31;
    const int g=(g6*7+31)/63;
    const int b=(b5*4+15)/31;
    return (uint8_t)(16 + r*40 + g*5 + b);
}

static void sub_put_to(uint16_t *fb,int x,int y,uint16_t c) {
    if(!fb || x<0 || y<0 || x>=DS_W || y>=DS_H) return;
    fb[y*256+x]=c;
}

static void sub_put(int x,int y,uint16_t c) {
    sub_put_to(g_sub_fb,x,y,c);
}

static void sub_rect_to(uint16_t *fb,int x,int y,int w,int h,uint16_t c) {
    if(!fb || w<=0 || h<=0) return;
    for(int yy=0;yy<h;yy++) for(int xx=0;xx<w;xx++) sub_put_to(fb,x+xx,y+yy,c);
}

static void sub_rect(int x,int y,int w,int h,uint16_t c) {
    sub_rect_to(g_sub_fb,x,y,w,h,c);
}

static void draw_3x5_glyph_to(uint16_t *fb,int x,int y,const uint8_t rows[5],int scale,uint16_t c) {
    for(int yy=0;yy<5;yy++) for(int xx=0;xx<3;xx++) {
        if(rows[yy]&(1u<<(2-xx))) sub_rect_to(fb,x+xx*scale,y+yy*scale,scale,scale,c);
    }
}

static void draw_log_button_to(uint16_t *fb) {
    if(!fb || g_log_visible) return;
    const uint16_t black=(uint16_t)(BIT(15) | RGB15(0,0,0));
    const uint16_t white=(uint16_t)(BIT(15) | RGB15(31,31,31));
    sub_rect_to(fb,LOG_BTN_X,LOG_BTN_Y,LOG_BTN_W,LOG_BTN_H,black);
    for(int x=LOG_BTN_X;x<LOG_BTN_X+LOG_BTN_W;x++) { sub_put_to(fb,x,LOG_BTN_Y,white); sub_put_to(fb,x,LOG_BTN_Y+LOG_BTN_H-1,white); }
    for(int y=LOG_BTN_Y;y<LOG_BTN_Y+LOG_BTN_H;y++) { sub_put_to(fb,LOG_BTN_X,y,white); sub_put_to(fb,LOG_BTN_X+LOG_BTN_W-1,y,white); }
    static const uint8_t L[5]={4,4,4,4,7};
    static const uint8_t O[5]={7,5,5,5,7};
    static const uint8_t G[5]={7,4,5,5,7};
    const int sy=LOG_BTN_Y+5;
    draw_3x5_glyph_to(fb,LOG_BTN_X+5, sy, L, 2, white);
    draw_3x5_glyph_to(fb,LOG_BTN_X+15,sy, O, 2, white);
    draw_3x5_glyph_to(fb,LOG_BTN_X+25,sy, G, 2, white);
}

static inline int rgb565_luma(uint16_t p) {
    const int r=((p>>11)&31)*255/31;
    const int g=((p>>5)&63)*255/63;
    const int b=(p&31)*255/31;
    return (r*3 + g*6 + b) / 10;
}

// The phone renderer is one already-composited framebuffer, so there is no
// native UI layer we can simply move to the touch screen.  v017/v018 copied a
// large y=176..319 slice to the lower LCD; that is exactly why normal map pixels
// appeared there.  v019 keeps the lower LCD empty and only enables the dialogue
// window when the lower source area actually looks like a broad UI panel.
// Hysteresis keeps a dialogue from blinking while letters/portraits animate.
static bool detect_dialogue_panel(void) {
    if(!g_phone_fb) return false;
    int dark=0, samples=0, strong_rows=0;
    for(int y=176; y<PHONE_H; y+=3) {
        int row_dark=0, row_samples=0;
        for(int x=6; x<PHONE_W-6; x+=3) {
            const int l=rgb565_luma(g_phone_fb[y*PHONE_W+x]);
            if(l<92) { dark++; row_dark++; }
            samples++; row_samples++;
        }
        if(row_samples && row_dark*100 >= row_samples*52) strong_rows++;
    }
    // Require both a sizeable dark area and several long dark rows.  Ordinary
    // grass/road tiles have many dark pixels but rarely form a wide flat panel.
    return samples && dark*100 >= samples*25 && strong_rows>=4;
}

static bool detect_info_page(void) {
    if(!g_phone_fb) return false;
    int bright=0, red=0, samples=0;
    for(int y=24; y<PHONE_H-24; y+=4) {
        for(int x=16; x<PHONE_W-16; x+=4) {
            const uint16_t p=g_phone_fb[y*PHONE_W+x];
            const int r=(p>>11)&31;
            const int g=(p>>5)&63;
            const int b=p&31;
            const int l=rgb565_luma(p);
            if(l>220) bright++;
            if(r>20 && g<20 && b<20) red++;
            samples++;
        }
    }
    // White instruction/help pages in Zenonia 2 use a very bright background
    // with dense red text. Detect that so the bottom screen does not become an
    // empty white square.
    return samples && bright*100 >= samples*55 && red*100 >= samples*2;
}

static void copy_phone_rect_to_main(int sx,int sy,int w,int h,int dx,int dy) {
    if(!g_phone_fb || !g_ds_fb) return;
    for(int yy=0; yy<h; yy++) {
        const int py=sy+yy, ty=dy+yy;
        if(py<0 || py>=PHONE_H || ty<0 || ty>=DS_H) continue;
        for(int xx=0; xx<w; xx++) {
            const int px=sx+xx, tx=dx+xx;
            if(px<0 || px>=PHONE_W || tx<0 || tx>=DS_W) continue;
            g_ds_fb[ty*256+tx]=rgb565_to_ds15(g_phone_fb[py*PHONE_W+px]);
        }
    }
}

static void copy_phone_rect_to_sub_buf(uint16_t *dst,int sx,int sy,int w,int h,int dx,int dy) {
    if(!g_phone_fb || !dst) return;
    for(int yy=0; yy<h; yy++) {
        const int py=sy+yy, ty=dy+yy;
        if(py<0 || py>=PHONE_H || ty<0 || ty>=DS_H) continue;
        for(int xx=0; xx<w; xx++) {
            const int px=sx+xx, tx=dx+xx;
            if(px<0 || px>=PHONE_W || tx<0 || tx>=DS_W) continue;
            dst[ty*256+tx]=rgb565_to_ds15(g_phone_fb[py*PHONE_W+px]);
        }
    }
}

static void copy_phone_rect_to_sub(int sx,int sy,int w,int h,int dx,int dy) {
    copy_phone_rect_to_sub_buf(g_sub_fb,sx,sy,w,h,dx,dy);
}

static void copy_phone_rect_scaled_to_sub_buf(uint16_t *dst,int sx,int sy,int sw,int sh,int dx,int dy,int dw,int dh) {
    if(!g_phone_fb || !dst || sw<=0 || sh<=0 || dw<=0 || dh<=0) return;
    for(int yy=0; yy<dh; yy++) {
        const int py=sy + (yy*sh)/dh;
        const int ty=dy+yy;
        if(py<0 || py>=PHONE_H || ty<0 || ty>=DS_H) continue;
        for(int xx=0; xx<dw; xx++) {
            const int px=sx + (xx*sw)/dw;
            const int tx=dx+xx;
            if(px<0 || px>=PHONE_W || tx<0 || tx>=DS_W) continue;
            dst[ty*256+tx]=rgb565_to_ds15(g_phone_fb[py*PHONE_W+px]);
        }
    }
}

static void copy_snapshot_rect_to_sub(int w,int h,int dx,int dy) {
    if(!g_dialogue_snapshot || !g_sub_comp) return;
    for(int yy=0; yy<h; yy++) {
        const int ty=dy+yy;
        if(ty<0 || ty>=DS_H) continue;
        for(int xx=0; xx<w; xx++) {
            const int tx=dx+xx;
            if(tx<0 || tx>=DS_W) continue;
            g_sub_comp[ty*256+tx]=rgb565_to_ds15(g_dialogue_snapshot[yy*PHONE_W+xx]);
        }
    }
}

static void snapshot_dialogue(int src_y) {
    if(!g_phone_fb || !g_dialogue_snapshot) return;
    if(src_y<0) src_y=0;
    if(src_y>PHONE_H-UI_DIALOG_CAPTURE_H) src_y=PHONE_H-UI_DIALOG_CAPTURE_H;
    g_dialogue_capture_y=src_y;
    for(int yy=0; yy<UI_DIALOG_CAPTURE_H; yy++) {
        memcpy(g_dialogue_snapshot + yy*PHONE_W,
               g_phone_fb + (src_y+yy)*PHONE_W,
               PHONE_W*2);
    }
    g_dialogue_snapshot_valid=true;
}

void wipi_present(void) {
    if(!g_phone_fb||!g_ds_fb||!g_sub_fb) return;
    g_present_pending=false;

    uint16_t *top = g_top_comp ? g_top_comp : g_ds_fb;
    uint16_t *subdst = g_sub_comp ? g_sub_comp : g_sub_fb;
    const uint16_t black=(uint16_t)(BIT(15) | RGB15(0,0,0));

    // v020: full-fit split; quiet runtime log + explicit MMF prepare/play diagnostics.
    // Split the 240x320 handset framebuffer into two exact 240x160 halves and
    // scale each half to fill a full 256x192 DS screen. This removes the side
    // black bars while keeping the split layout from v017.
    constexpr int HALF_H=160;
    constexpr int TOP_SRC_Y=0;
    constexpr int BOT_SRC_Y=160;

    for(int i=0;i<DS_W*DS_H;i++) top[i]=black;
    for(int i=0;i<DS_W*DS_H;i++) subdst[i]=black;

    for(int dy=0; dy<DS_H; dy++) {
        const int sy = TOP_SRC_Y + (dy*HALF_H)/DS_H;
        uint16_t *td=top + dy*DS_W;
        const uint16_t *src=g_phone_fb + sy*PHONE_W;
        for(int dx=0; dx<DS_W; dx++) {
            const int sx=(dx*15)>>4; // 240/256 = 15/16; avoids ARM9 division
            td[dx]=rgb565_to_ds15(src[sx]);
        }
    }

    if(!g_log_visible) {
        for(int dy=0; dy<DS_H; dy++) {
            const int sy = BOT_SRC_Y + (dy*HALF_H)/DS_H;
            uint16_t *td=subdst + dy*DS_W;
            const uint16_t *src=g_phone_fb + sy*PHONE_W;
            for(int dx=0; dx<DS_W; dx++) {
                const int sx=(dx*15)>>4; // 240/256 = 15/16; avoids ARM9 division
                td[dx]=rgb565_to_ds15(src[sx]);
            }
        }
        draw_log_button_to(subdst);
    }

    if(top != g_ds_fb) memcpy(g_ds_fb,top,DS_W*DS_H*2);
    DC_FlushRange(g_ds_fb,DS_W*DS_H*2);

    if(!g_log_visible) {
        if(subdst != g_sub_fb) memcpy(g_sub_fb,subdst,DS_W*DS_H*2);
        DC_FlushRange(g_sub_fb,DS_W*DS_H*2);
    }
}

void wipi_set_log_visible(bool visible) {
    if(!g_log_console || g_log_bg<0 || g_sub_bg<0) return;
    if(g_log_visible==visible) return;
    if(visible) runner_release_all_logical();
    g_log_visible=visible;
    g_ui_capture_single=-1;
    g_ui_capture_combo=-1;

    // v005 melonDS debug: do not block on VBlank while switching the log view.
    if(visible) {
        bgHide(g_sub_bg);
        vramSetBankC(VRAM_C_LCD);
        vramSetBankH(VRAM_H_SUB_BG);
        bgShow(g_log_bg);
        consoleSelect(g_log_console);
        g_ui_tab=UI_TAB_LOG;
        g_ui_dirty=true;
        runner_ui_log_push_line("settings opened");
        runner_ui_draw();
    } else {
        runner_ui_log_push_line("settings closed");
        bgHide(g_log_bg);
        vramSetBankH(VRAM_H_LCD);
        vramSetBankC(VRAM_C_SUB_BG);
        bgShow(g_sub_bg);
        g_ui_prev_logical=0;
        // Rebuild the full bottom frame after remapping C back to SUB_BG.
        wipi_present();
    }
    bgUpdate();
}

bool wipi_log_visible(void) { return g_log_visible; }

void wipi_handle_touch(uint32_t down,int x,int y) {
    if(!(down&KEY_TOUCH)) return;

    // v016: lower LCD normally shows the lower half of the game.  Keep the
    // LOG button interactive and also expose the on-screen quick slots as touch.
    if(!g_log_visible) {
        if(x>=LOG_BTN_X && x<LOG_BTN_X+LOG_BTN_W && y>=LOG_BTN_Y && y<LOG_BTN_Y+LOG_BTN_H) {
            wipi_set_log_visible(true);
            return;
        }
        if(y>=QUICK_SLOT_Y0 && y<QUICK_SLOT_Y1 &&
           x>=QUICK_SLOT_X0 && x<QUICK_SLOT_X0+QUICK_SLOT_W*QUICK_SLOT_COUNT) {
            const int slot=(x-QUICK_SLOT_X0)/QUICK_SLOT_W;
            const int key='4'+slot;
            printf("[TOUCH] quick %c\n",key);
            call_event(502,key);
            call_event(503,key);
            g_trace_next_timer=false;
            g_trace_repaint_once=true;
            return;
        }
        return;
    }

    if(g_log_visible) {
        // Top row: SINGLE | COMBO | LOG | CLOSE. No MAP tab is present.
        if(y<24) {
            if(x<72) g_ui_tab=UI_TAB_SINGLE;
            else if(x<144) g_ui_tab=UI_TAB_COMBO;
            else if(x<200) g_ui_tab=UI_TAB_LOG;
            else { wipi_set_log_visible(false); return; }
            g_ui_capture_single=-1;
            g_ui_capture_combo=-1;
            g_ui_dirty=true;
            runner_ui_draw();
            return;
        }
        // Ten large 16-pixel target rows from y=24..183.
        if((g_ui_tab==UI_TAB_SINGLE || g_ui_tab==UI_TAB_COMBO) && y>=24 && y<184) {
            int idx=(y-24)/16;
            if(idx<0) idx=0; if(idx>=UI_TARGET_COUNT) idx=UI_TARGET_COUNT-1;
            if(g_ui_tab==UI_TAB_SINGLE) {
                g_ui_selected_single=idx;
                g_ui_capture_single=idx;
                g_ui_capture_combo=-1;
            } else {
                g_ui_selected_combo=idx;
                g_ui_capture_combo=idx;
                g_ui_capture_single=-1;
            }
            g_ui_dirty=true;
            runner_ui_draw();
        }
        return;
    }

}

void wipi_debug_dump(const char *tag) {
    uint32_t nz=0;
    if(g_phone_fb) {
        for(int i=0;i<PHONE_W*PHONE_H;i++) if(g_phone_fb[i]) nz++;
    }
    printf("[TRACE] %s\n", tag?tag:"-");
    printf("[DBS] open=%lu fail=%lu read=%lu bytes=%lu\n",
           (unsigned long)g_trace.db_opens,(unsigned long)g_trace.db_open_fail,
           (unsigned long)g_trace.db_reads,(unsigned long)g_trace.db_read_bytes);
    printf("[GFX] fb=%lu ctx=%lu/%lu rect=%lu/%lu img=%lu str=%lu\n",
           (unsigned long)g_trace.get_fb,(unsigned long)g_trace.init_ctx,(unsigned long)g_trace.set_ctx,
           (unsigned long)g_trace.draw_rect,(unsigned long)g_trace.fill_rect,
           (unsigned long)g_trace.draw_image,(unsigned long)g_trace.draw_string);
    printf("[GFX] px=%lu create=%lu flush=%lu repaint=%lu nz=%lu\n",
           (unsigned long)g_trace.put_pixel,(unsigned long)g_trace.create_image,
           (unsigned long)g_trace.flush,(unsigned long)g_trace.repaint,(unsigned long)nz);
    printf("[GFX2] off=%lu/%lu copy=%lu/%lu rgb=%lu/%lu\n",
           (unsigned long)g_trace.offscreen_create,(unsigned long)g_trace.offscreen_destroy,
           (unsigned long)g_trace.copy_fb,(unsigned long)g_trace.copy_area,
           (unsigned long)g_trace.get_rgb,(unsigned long)g_trace.set_rgb);
    unsigned armed=0; for(int i=0;i<TIMER_SLOTS;i++) if(g_timers[i].armed) armed++;
    printf("[TSTAT] set=%lu fire=%lu armed=%u\n",(unsigned long)g_trace.timer_set,(unsigned long)g_trace.timer_fire,armed);
}

void wipi_poll(void) {
    // v009 melonDS debug: nonblocking main loop, timed by Calico's monotonic
    // system tick counter rather than VBlank or accumulated cpuGetTiming deltas.
    g_loop_count++;
    process_timers();
    media_poll_end_events();
    media_poll_bgm_fallback();
    media_poll_sfx_fallback();
    // A timer/event may have requested a repaint. Run it only after that guest
    // callback has returned, then present the finished frame once.
    service_repaint();
    if(g_present_pending) {
        g_present_pending=false;
        wipi_present();
    }
    if(g_log_visible) runner_ui_draw();
}

uint32_t wipi_get_import_table(uint32_t table) { return table; }

void *wipi_get_import_function(uint32_t table, uint32_t index) {
    void *fn=nullptr;
    if(table==0x1fb) {
        switch(index) {
            case 0x03: fn=(void*)clet_register; break;
            case 0x32: fn=(void*)fb_ptr; break;
            case 0x33: fn=(void*)fb_w; break;
            case 0x34: fn=(void*)fb_h; break;
            case 0x35: fn=(void*)fb_bpl; break;
            case 0x36: fn=(void*)fb_bpp; break;
            case 0x64: fn=(void*)knl_printk; break;
            case 0x65: fn=(void*)knl_sprintk; break;
            case 0x68: fn=(void*)knl_unk68; break;
            case 0x6a: fn=(void*)knl_unk6a; break;
            case 0x6b: fn=(void*)knl_exit; break;
            case 0x6f: fn=(void*)knl_get_program_name; break;
            case 0x75: fn=(void*)knl_alloc; break;
            case 0x76: fn=(void*)knl_calloc; break;
            case 0x77: fn=(void*)knl_free; break;
            case 0x78: fn=(void*)knl_total_mem; break;
            case 0x79: fn=(void*)knl_free_mem; break;
            case 0x7a: fn=(void*)knl_def_timer; break;
            case 0x7b: fn=(void*)knl_set_timer; break;
            case 0x7c: fn=(void*)knl_unset_timer; break;
            case 0x7d: fn=(void*)knl_current_time; break;
            case 0x7e: fn=(void*)knl_get_system_property; break;
            case 0x7f: fn=(void*)knl_set_system_property; break;
            case 0x80: fn=(void*)knl_get_resource_id; break;
            case 0x81: fn=(void*)knl_get_resource; break;
            case 0xc8: fn=(void*)gfx_get_image_property; break;
            case 0xc9: fn=(void*)gfx_get_image_framebuffer; break;
            case 0xca: fn=(void*)gfx_get_screen_fb; break;
            case 0xcb: fn=(void*)gfx_destroy_offscreen_framebuffer; break;
            case 0xcc: fn=(void*)gfx_create_offscreen_framebuffer; break;
            case 0xcd: fn=(void*)gfx_init_context; break;
            case 0xce: fn=(void*)gfx_set_context; break;
            case 0xd0: fn=(void*)gfx_put_pixel; break;
            case 0xd1: fn=(void*)gfx_draw_line; break;
            case 0xd2: fn=(void*)gfx_draw_rect; break;
            case 0xd3: fn=(void*)gfx_fill_rect; break;
            case 0xd4: fn=(void*)gfx_copy_frame_buffer; break;
            case 0xd5: fn=(void*)gfx_draw_image; break;
            case 0xd7: fn=(void*)gfx_copy_area; break;
            case 0xd8: fn=(void*)gfx_draw_arc; break;
            case 0xd9: fn=(void*)gfx_fill_arc; break;
            case 0xda: fn=(void*)gfx_draw_string; break;
            case 0xdc: fn=(void*)gfx_get_rgb_pixels; break;
            case 0xdd: fn=(void*)gfx_set_rgb_pixels; break;
            case 0xde: fn=(void*)gfx_flush; break;
            case 0xdf: fn=(void*)gfx_pixel_from_rgb; break;
            case 0xe0: fn=(void*)gfx_rgb_from_pixel; break;
            case 0xe1: fn=(void*)gfx_get_display_info; break;
            case 0xe2: fn=(void*)gfx_repaint; break;
            case 0xe3: fn=(void*)gfx_get_font; break;
            case 0xe4: fn=(void*)gfx_get_font_height; break;
            case 0xe5: fn=(void*)gfx_get_font_ascent; break;
            case 0xe6: fn=(void*)gfx_get_font_descent; break;
            case 0xe7: fn=(void*)gfx_get_string_width; break;
            case 0xe9: fn=(void*)gfx_create_image; break;
            case 0xeb: fn=(void*)gfx_unk_eb; break;
            case 0xee: fn=(void*)gfx_unk_ee; break;
            // WIPI input-method API. Zenonia 2 needs these in CB0.
            case 0x12c: fn=(void*)im_get_support_mode_count; break;
            case 0x12d: fn=(void*)im_get_supported_modes; break;
            case 0x12e: fn=(void*)im_set_current_mode; break;
            case 0x12f: fn=(void*)im_get_current_mode; break;
            case 0x130: fn=(void*)im_handle_input; break;
            case 0x190: fn=(void*)db_open; break;
            case 0x191: fn=(void*)db_read; break;
            case 0x192: fn=(void*)db_write; break;
            case 0x193: fn=(void*)db_close; break;
            case 0x194: fn=(void*)db_seek; break;
            case 0x195: fn=(void*)db_list_record_info; break;
            case 0x196: fn=(void*)db_delete; break;
            case 0x197: fn=(void*)db_list_record; break;
            case 0x198: fn=(void*)db_update_record; break;
            case 0x199: fn=(void*)db_select_record; break;
            case 0x19c: fn=(void*)db_available_storage; break;
            case 0x1a0: fn=(void*)db_exists; break;
            // WIPI media compatibility. Zenonia 2 ships SMAF/MMF resources;
            // these calls feed the real buffered clip + DS PCM/SMAF synth path.
            case 0x4b0: fn=(void*)media_clip_create; break;
            case 0x4b1: fn=(void*)media_clip_free; break;
            case 0x4b3: fn=(void*)media_clip_put_data; break;
            case 0x4b6: fn=(void*)media_noop4; break;
            case 0x4b8: fn=(void*)media_clip_get_volume; break;
            case 0x4b9: fn=(void*)media_clip_set_volume; break;
            case 0x4ba: fn=(void*)media_clip_play; break;
            case 0x4bd: fn=(void*)media_clip_stop; break;
            case 0x4c0: fn=(void*)media_noop4; break;
            case 0x4c1: fn=(void*)media_vibrator_on; break;
            case 0x4c2: fn=(void*)media_noop4; break;
            case 0x4c5: fn=(void*)media_clip_prepare; break;
            case 0x4c6: fn=(void*)media_noop4; break;
            case 0x4ce: fn=(void*)media_get_profile_volume; break;
            case 0x4d1: fn=(void*)media_set_device_enabled; break;
            case 0x4d2: fn=(void*)media_get_device_enabled; break;
            default: break;
        }
    } else if(table==0x1f8 && index==0x16) {
        fn=(void*)vendor_1f8_016;
    } else if(table==1) {
        switch(index) {
            case 0x3f6: fn=(void*)std_unk2; break;
            case 0x3f7: fn=(void*)std_sprintf; break;
            case 0x3fb: fn=(void*)std_atoi; break;
            case 0x403: fn=(void*)std_rand; break;
            case 0x404: fn=(void*)std_srand; break;
            case 0x405: fn=(void*)std_strcpy; break;
            case 0x406: fn=(void*)std_strncpy; break;
            case 0x407: fn=(void*)std_strcat; break;
            case 0x409: fn=(void*)std_strcmp; break;
            case 0x40a: fn=(void*)std_unk4; break;
            case 0x410: fn=(void*)std_strstr; break;
            case 0x411: fn=(void*)std_strlen; break;
            case 0x414: fn=(void*)std_memcpy; break;
            case 0x418: fn=(void*)std_memset; break;
            case 0x41a: fn=(void*)std_time; break;
            case 0x420: fn=(void*)std_localtime; break;
            case 0x424: fn=(void*)std_unk3; break;
            default: break;
        }
    }
    if(fn) return fn;
    void *stub=make_unknown_thunk(table,index);
    return stub?stub:(void*)wipi_stub;
}

const WipiCletState *wipi_clet_state(void) { return &g_clet; }

bool wipi_start_clet(void) {
    printf("[CLET] enter start helper\n");
    if(!g_clet.registered) { printf("[CLET!] not registered\n"); return false; }
    if(!g_clet.callbacks[0]) { printf("[CLET!] cb0 NULL\n"); return false; }
    const uint32_t cb=g_clet.callbacks[0];
    printf("[CLET] cb0=%08lx thumb=%lu\n",(unsigned long)cb,(unsigned long)(cb&1u));
    if((cb&~1u)<0x02000000u || (cb&~1u)>=0x04000000u) {
        printf("[CLET!] cb0 outside RAM\n");
        return false;
    }
    typedef void (*Fn)(void);
    Fn f=(Fn)(uintptr_t)cb;
    printf("[CLET] flush D-cache\n");
    DC_FlushAll();
    printf("[CLET] invalidate I-cache\n");
    IC_InvalidateAll();
    printf("[CLET>] calling cb0 now\n");
    // v005: call immediately; the previous VBlank wait was the actual hang.
    f();
    printf("[CLET<] cb0 returned\n");
    g_clet.started=true;
    g_audio_clet_started_ms=real_monotonic_ms();
    printf("[CLET] started=YES\n");
    return true;
}

void wipi_paint_clet(void) {
    paint_region(0,0,PHONE_W,PHONE_H);
}

static int runner_logical_wipi_for_bit(int bit) {
    if(bit==0) return -1;
    if(bit==1) return -2;
    if(bit==2) return -3;
    if(bit==3) return -4;
    int idx=bit-4;
    return (idx>=0 && idx<UI_TARGET_COUNT)?g_runner_targets[idx].wipi_key:0;
}

static void runner_release_all_logical(void) {
    for(int bit=0;bit<4+UI_TARGET_COUNT;bit++) if(g_ui_prev_logical&(1u<<bit)) {
        call_event(503,runner_logical_wipi_for_bit(bit));
    }
    g_ui_prev_logical=0;
}

void wipi_dispatch_keys(uint32_t down,uint32_t up) {
    (void)up;
    uint32_t rawHeld=keysHeld() & ~(KEY_TOUCH|KEY_LID);
    uint32_t rawDown=down & ~(KEY_TOUCH|KEY_LID);

    // v011: the runtime LOG overlay may stay visible while testing the game.
    // Only consume physical buttons when the user is actively capturing a
    // SINGLE/COMBO remap.  Merely having the LOG screen open must NOT block
    // game input (v010 did, which made every key appear dead).
    if(g_log_visible && (g_ui_capture_single>=0 || g_ui_capture_combo>=0)) {
        if(g_ui_capture_single>=0) {
            uint32_t key=runner_first_physical(rawDown);
            if(key) {
                int idx=g_ui_capture_single;
                g_ui_single_map[idx]=key;
                char msg[32]; snprintf(msg,sizeof(msg),"single %s=%s",g_runner_targets[idx].name,runner_physical_name(key));
                runner_ui_log_push_line(msg);
                g_ui_capture_single=-1;
                g_ui_dirty=true;
            }
        }
        if(g_ui_capture_combo>=0) {
            uint32_t a,b; runner_first_two_physical(rawHeld,&a,&b);
            if(a&&b) {
                int idx=g_ui_capture_combo;
                g_ui_combo_map1[idx]=a;
                g_ui_combo_map2[idx]=b;
                char msg[32]; snprintf(msg,sizeof(msg),"combo %s=%s+%s",g_runner_targets[idx].name,runner_physical_name(a),runner_physical_name(b));
                runner_ui_log_push_line(msg);
                g_ui_capture_combo=-1;
                g_ui_dirty=true;
            }
        }
        runner_ui_draw();
        return;
    }

    uint32_t logical=0;
    // D-pad remains fixed to WIPI directional codes.
    if(rawHeld&KEY_UP) logical|=1u<<0;
    if(rawHeld&KEY_DOWN) logical|=1u<<1;
    if(rawHeld&KEY_LEFT) logical|=1u<<2;
    if(rawHeld&KEY_RIGHT) logical|=1u<<3;

    uint32_t activeComboPhysical=0;
    for(int i=0;i<UI_TARGET_COUNT;i++) {
        if(g_ui_combo_map1[i]&&g_ui_combo_map2[i]) {
            uint32_t need=g_ui_combo_map1[i]|g_ui_combo_map2[i];
            if((rawHeld&need)==need) {
                logical|=1u<<(4+i);
                activeComboPhysical|=need;
            }
        }
    }
    for(int i=0;i<UI_TARGET_COUNT;i++) {
        uint32_t key=g_ui_single_map[i];
        if(key && (rawHeld&key) && !(activeComboPhysical&key)) logical|=1u<<(4+i);
    }

    uint32_t logicalDown=logical & ~g_ui_prev_logical;
    uint32_t logicalUp=g_ui_prev_logical & ~logical;
    for(int bit=0;bit<4+UI_TARGET_COUNT;bit++) {
        const uint32_t mask=1u<<bit;
        const int wipi=runner_logical_wipi_for_bit(bit);
        if(logicalDown&mask) {
            static unsigned key_log_budget=12;
            if(key_log_budget) {
                printf("[KEY] %s k=%d\n",bit<4?(bit==0?"UP":bit==1?"DOWN":bit==2?"LEFT":"RIGHT"):g_runner_targets[bit-4].name,wipi);
                key_log_budget--;
            }
            g_trace_next_timer=false;
            g_trace_repaint_once=true;
            g_db_trace_budget=16;
            g_unknown_trace_budget=12;
            call_event(502,wipi);
        }
        if(logicalUp&mask) call_event(503,wipi);
    }
    g_ui_prev_logical=logical;
}

