#pragma once
#include <nds.h>
#include <stdint.h>

void wipi_init_video(PrintConsole *log_console);
void wipi_init_storage(void);
void wipi_flush_storage(void);
void wipi_poll(void);
uint32_t wipi_get_import_table(uint32_t table);
void *wipi_get_import_function(uint32_t table, uint32_t index);
void wipi_present(void);
void wipi_debug_dump(const char *tag);

bool wipi_start_clet(void);
void wipi_paint_clet(void);
void wipi_dispatch_keys(uint32_t keys_down, uint32_t keys_up);
void wipi_handle_touch(uint32_t keys_down, int x, int y);
void wipi_set_log_visible(bool visible);
bool wipi_log_visible(void);
void wipi_note_sound_request(int id, int arg2, int flag);

struct WipiCletState {
    uint32_t register_arg0;
    uint32_t register_arg1;
    uint32_t callbacks[6]; // start, pause, resume, destroy, paint, event
    bool registered;
    bool started;
};
const WipiCletState *wipi_clet_state(void);
