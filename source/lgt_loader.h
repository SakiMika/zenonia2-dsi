#pragma once
#include <nds.h>
#include <stdint.h>
#include <stddef.h>

struct LgtLoadedImage {
    uint8_t *text; uint32_t text_old; uint32_t text_size;
    uint8_t *data; uint32_t data_old; uint32_t data_size;
    uint8_t *bss; uint32_t bss_old; uint32_t bss_size;
    uint32_t entry_old; uintptr_t entry_new;
};

bool lgt_load_binary_memory(const void *data,size_t size,LgtLoadedImage *out);
void *lgt_translate_ptr(const LgtLoadedImage *img,uint32_t old_addr);
