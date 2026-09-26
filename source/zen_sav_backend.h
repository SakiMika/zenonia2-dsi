#pragma once
#include <stdint.h>

namespace ZenSav {
// Zenonia 2 has exactly three character slots (Save0.dat..Save2.dat) plus
// option.sav.  The DSi port stores all four logical WIPI databases inside one
// 128 KiB NTR cartridge-save image (.sav).  A single FAT ZLOM.sav container is
// kept only as a fallback for runtimes that expose writable FAT but no native
// cartridge-save backend.
void InitFatFallback();
void EnableNativeProbe();
void Flush();

bool IsPersistentName(const char *name);
int RecordIdForName(const char *name); // Save0..2 => 0..2, option.sav => 3

bool Exists(const char *name, uint32_t *out_len = nullptr);
int Load(const char *name, uint8_t **out_data, uint32_t *out_len);
int Store(const char *name, const uint8_t *data, uint32_t len);
int Remove(const char *name);

const char *BackendName();
}
