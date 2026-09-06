#include "embedded_assets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
static uint8_t *g_cache=nullptr;
static size_t g_cache_cap=0;

static bool make_path(const char *name,char *out,size_t cap) {
    if(!name||!out||cap<8) return false;
    while(*name=='/'||*name=='\\') ++name;
    if(!*name) return false;
    const int n=snprintf(out,cap,"nitro:/%s",name);
    return n>0 && (size_t)n<cap;
}
}

bool embedded_asset_find(const char *name, EmbeddedAssetView *out) {
    if(out) { out->data=nullptr; out->size=0; }
    char path[160];
    if(!make_path(name,path,sizeof(path))) return false;
    FILE *f=fopen(path,"rb");
    if(!f) return false;
    if(fseek(f,0,SEEK_END)!=0) { fclose(f); return false; }
    long end=ftell(f);
    if(end<0 || (unsigned long)end>0xffffffffUL) { fclose(f); return false; }
    rewind(f);
    const size_t need=(size_t)end;
    if(need>g_cache_cap) {
        size_t cap=(need+31u)&~31u;
        uint8_t *p=(uint8_t*)realloc(g_cache,cap?cap:32);
        if(!p) { fclose(f); return false; }
        g_cache=p; g_cache_cap=cap;
    }
    if(need && fread(g_cache,1,need,f)!=need) { fclose(f); return false; }
    fclose(f);
    if(out) { out->data=g_cache; out->size=(uint32_t)need; }
    return true;
}

void embedded_asset_release_cache(void) {
    free(g_cache); g_cache=nullptr; g_cache_cap=0;
}

uint32_t embedded_asset_count(void) {
    return 1407u; // JAR payload files excluding META-INF/MANIFEST.MF
}

uint32_t embedded_asset_pack_size(void) {
    return 5177192u; // uncompressed payload bytes excluding MANIFEST.MF
}
