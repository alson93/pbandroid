// sound.h - the Toaplan sound board: Z80 + YM3812, sharing RAM with the 68000.
// ASSUMPTION (from memory of this board family, not verifiable offline): Z80 and YM3812 both run at 27 MHz / 8 = 3.375 MHz.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
class StateIO;

bool sound_boot(const uint8_t* z80rom /*32 KB*/, volatile uint8_t* shared_ram /*0x800 bytes, Z80 8000-87FF*/);
// Pull audio (stereo, interleaved int16). Advances the Z80 and the chip in step with the requested sample count.
size_t sound_render(int16_t* stereo, size_t frames, int rate);
void sound_set_pulled(bool on);          // an audio device is pulling samples: the internal real-time pacing thread stands down
void sound_set_muted(bool m);
bool sound_muted();
void sound_set_capture(const char* wav_path);   // headless testing: record what the pacing thread renders (mono 48 kHz WAV)
void sound_finish();                              // writes the capture file
void sound_stats(unsigned long long& z80_insns, unsigned long long& chip_writes, unsigned long long& key_ons, unsigned long long& irqs);
void sound_report();                              // debug: register write histogram to stderr

bool sound_state_io(StateIO& io);                   // save states: Z80 + FM chip + shared RAM (a no-op when no sound board is running)
bool sound_state_selftest(std::string& report);     // checks that restoring a snapshot replays the same audio (call with the 68000 stopped)
