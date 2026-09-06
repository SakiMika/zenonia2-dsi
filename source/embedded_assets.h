#pragma once
#include <stdint.h>
#include <stddef.h>

struct EmbeddedAssetView {
    const uint8_t *data;
    uint32_t size;
};

bool embedded_asset_find(const char *name, EmbeddedAssetView *out);
void embedded_asset_release_cache(void);
uint32_t embedded_asset_count(void);
uint32_t embedded_asset_pack_size(void);
