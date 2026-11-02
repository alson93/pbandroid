// hw.h - hardware model interface (bus.cpp). All hardware names below are inferred from how the
// original code touches the addresses; they are NOT verified against a reference.
#pragma once
#include <cstdint>
#include <cstddef>
bool load_roms(const char* dir);   // expects tp025-1.bin / tp025-2.bin (68000), tp025-5.bin (Z80)
void hw_reset();
void hw_dump(const char* prefix);  // debug: dump VRAM/palette/video regs/work RAM
void hw_set_input(uint32_t addr, uint8_t v);   // addr = 68000 address 19C0xx (use the ODD byte of each port)
uint8_t hw_get_input(uint32_t addr);
const uint8_t* hw_ram();   // 68000 work RAM (080000-083FFF), for the debug overlay
const uint16_t* hw_live_vram(); const uint16_t* hw_vram(); const uint16_t* hw_vreg(); const uint8_t* hw_pal();   // video state for the renderer
void hw_latch_report();            // debug: summary of per-frame snapshot statistics (M68K_LATCHLOG=1)
void hw_view_stats(long& calls, long& live);   // debug: how often the renderer had to use live (unlatched) video state
unsigned long long hw_latch_count();   // number of finished game frames so far (for frame-exact presentation)
void hw_irq_exit_compare(); void hw_irq_compare_report();   // debug statistics
void hw_latch_video();             // snapshot video state (called when the game waits for VBLANK)
void hw_start_realtime();          // 60 Hz VBLANK from wall-clock time instead of the free-running fake one
volatile uint8_t* hw_shared_ram();   // Z80 8000-87FF (odd bytes of 68000 190000-190FFF)
void host_idle();                  // called while the CPU waits (STOP): advances fake time / raises VBLANK
extern uint8_t rom68k[0x40000];
extern uint8_t z80rom[0x8000];

// pipibibs_settings.txt (DIP switches, region jumpers, game speed) - loaded by load_roms()
const char* hw_settings_warnings();   // "" when the file parsed cleanly, otherwise one line per problem
const char* hw_settings_source();     // which file was used
double hw_settings_speed();           // percent (100 = original)
bool hw_settings_flip();                 // flip_screen: the output picture is turned 180 degrees by video_render()

// save states (bus.cpp): requests are executed by the emulation thread at the next safe moment (the main-loop VBLANK wait)
void hw_state_request_save();
void hw_state_request_load();
void hw_state_request_selftest();                          // test aid: save -> load -> save must be identical, sound replay must match
bool hw_state_message(unsigned& seq, char* buf, size_t n); // true when there is a new status message (seq is the caller's last seen number)
const char* hw_settings_save_key();                         // key names for the front end: "9", "f5", "pageup", "none" ...
const char* hw_settings_load_key();
bool hw_settings_swap_players();          // swap_players
bool hw_settings_skip_clear();           // skip_level_clear_scenes
const uint8_t* hw_vram_tags();             // per tile-layer word: 1 = written by a picture routine (same latching as hw_vram())
bool hw_settings_hide_pictures();         // draw tagged tiles black
