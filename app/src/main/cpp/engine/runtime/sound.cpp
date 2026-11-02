#include "sound.h"
#include "z80cpu.h"
#include "opl2.h"
#include "state_io.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
const double CLK = 3375000.0;
const double OPL_RATE = CLK / 72.0;

struct Board : Z80Bus {
    const uint8_t* rom = nullptr;
    volatile uint8_t* shram = nullptr;
    OPL2 opl;
    unsigned long long chip_writes = 0, key_ons = 0;
    uint8_t last_addr = 0; unsigned hist[256] = {}; unsigned kon_hist[9] = {}; uint8_t flo[9] = {}; unsigned notes[8][1024] = {};
    uint8_t read(uint16_t a) override {
        if (a < 0x8000) return rom[a];
        if (a < 0x8800) return shram[a - 0x8000];
        if (a == 0xE000) return opl.read_status();
        return 0xFF;
    }
    void write(uint16_t a, uint8_t v) override {
        if (a >= 0x8000 && a < 0x8800) shram[a - 0x8000] = v;
        else if (a == 0xE000) { last_addr = v; opl.write_addr(v); }
        else if (a == 0xE001) {
            ++chip_writes; ++hist[last_addr];
            if (last_addr >= 0xA0 && last_addr <= 0xA8) flo[last_addr - 0xA0] = v;
            if (last_addr >= 0xB0 && last_addr <= 0xB8 && (v & 0x20)) { ++key_ons; ++kon_hist[last_addr - 0xB0]; notes[(v >> 2) & 7][((v & 3) << 8) | flo[last_addr - 0xB0]]++; }
            opl.write_data(v);
        }
    }
};

Board g_board;
Z80 g_z80;
std::mutex g_mutex;
std::atomic<bool> g_pulled{false}, g_muted{false}, g_stop{false}, g_booted{false};
unsigned long long g_insns = 0, g_irqs = 0;
int g_cycle_debt = 0;
bool g_prev_irq = false;
double g_t = 0.0; int g_s0 = 0, g_s1 = 0;
std::vector<int16_t> g_capture; std::string g_capture_path;
std::thread g_pacer;

int next_opl_sample() {
    g_cycle_debt -= 72;                                    // the chip produces one sample every 72 clocks
    while (g_cycle_debt < 0) {
        g_z80.irq_line = g_board.opl.irq();
        if (g_z80.irq_line && !g_prev_irq) ++g_irqs;
        g_prev_irq = g_z80.irq_line;
        g_cycle_debt += g_z80.step(); ++g_insns;
    }
    return g_board.opl.clock();
}

void write_wav(const std::string& path, const std::vector<int16_t>& s, int rate) {
    FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return;
    uint32_t dlen = (uint32_t)(s.size() * 2), r = rate, br = rate * 2; uint16_t fmt = 1, ch = 1, ba = 2, bits = 16; uint32_t fl = 16, rl = 36 + dlen;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&rl, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f); std::fwrite(&fl, 4, 1, f);
    std::fwrite(&fmt, 2, 1, f); std::fwrite(&ch, 2, 1, f); std::fwrite(&r, 4, 1, f); std::fwrite(&br, 4, 1, f);
    std::fwrite(&ba, 2, 1, f); std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&dlen, 4, 1, f);
    std::fwrite(s.data(), 2, s.size(), f); std::fclose(f);
}

void pacer_main() {
    using clk = std::chrono::steady_clock;
    const int RATE = 48000;
    auto t0 = clk::now(); long long produced = 0;
    std::vector<int16_t> buf(RATE / 50 * 2);
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        if (g_pulled) { t0 = clk::now(); produced = 0; continue; }          // a real audio device is the clock now
        long long want = (long long)(std::chrono::duration<double>(clk::now() - t0).count() * RATE) - produced;
        while (want > 0 && !g_stop && !g_pulled) {
            size_t n = (size_t)std::min<long long>(want, (long long)(buf.size() / 2));
            sound_render(buf.data(), n, RATE);
            if (!g_capture_path.empty()) for (size_t i = 0; i < n; ++i) g_capture.push_back(buf[i * 2]);
            produced += (long long)n; want -= (long long)n;
        }
    }
}
}  // namespace

bool sound_boot(const uint8_t* z80rom, volatile uint8_t* shared_ram) {
    if (g_booted.exchange(true)) return true;
    g_board.rom = z80rom; g_board.shram = shared_ram; g_board.opl.reset();
    g_z80.bus = &g_board; g_z80.reset();
    g_pacer = std::thread(pacer_main);
    g_pacer.detach();
    return true;
}

size_t sound_render(int16_t* out, size_t frames, int rate) {
    std::lock_guard<std::mutex> lk(g_mutex);
    const double step = OPL_RATE / rate;
    const bool mute = g_muted;
    for (size_t i = 0; i < frames; ++i) {
        while (g_t >= 1.0) { g_t -= 1.0; g_s0 = g_s1; g_s1 = next_opl_sample(); }
        double v = g_s0 + (g_s1 - g_s0) * g_t;
        g_t += step;
        int o = mute ? 0 : (int)(v * 1.5);
        if (o > 32767) o = 32767; else if (o < -32768) o = -32768;
        out[i * 2] = out[i * 2 + 1] = (int16_t)o;
    }
    return frames;
}


// ---- save states ------------------------------------------------------------------------------------------------------
// Everything the sound board needs to continue exactly where it was: Z80 registers, the FM chip, the 2 KB RAM shared with the
// 68000 and the resampler position. Taken under the audio mutex, so the audio thread never sees a half-restored board.
static void sound_io_locked(StateIO& io) {
    io.section("sound");
    io.val("opl_last_addr", g_board.last_addr);
    io.val("cycle_debt", g_cycle_debt); io.val("prev_irq", g_prev_irq);
    io.val("resample_t", g_t); io.val("resample_s0", g_s0); io.val("resample_s1", g_s1);
    io.bytes("z80_shared_ram", (uint8_t*)g_board.shram, 0x800);
    g_z80.state_io(io);
    g_board.opl.state_io(io);
}
bool sound_state_io(StateIO& io) {
    if (!g_booted || !g_board.shram) return true;          // no sound board running (console tests): nothing to do
    std::lock_guard<std::mutex> lk(g_mutex);
    sound_io_locked(io);
    return io.ok();
}

// Self-test: with the 68000 stopped (the caller is the 68000 thread), snapshot the board, render N samples and hash them, put the
// snapshot back, render N samples again: identical hashes prove that nothing the sound board needs is missing from the state.
bool sound_state_selftest(std::string& report) {
    if (!g_booted || !g_board.shram) { report = "sound board not running"; return true; }
    std::lock_guard<std::mutex> lk(g_mutex);
    StateIO a; sound_io_locked(a);
    const std::string snap = a.str();
    auto run = [&](size_t frames) {
        uint64_t h = 1469598103934665603ULL;
        for (size_t i = 0; i < frames; ++i) {
            while (g_t >= 1.0) { g_t -= 1.0; g_s0 = g_s1; g_s1 = next_opl_sample(); }
            double v = g_s0 + (g_s1 - g_s0) * g_t; g_t += OPL_RATE / 48000.0;
            h = (h ^ (uint64_t)(int64_t)(v * 16.0)) * 1099511628211ULL;
        }
        for (int k = 0; k < 0x800; ++k) h = (h ^ g_board.shram[k]) * 1099511628211ULL;    // the Z80 may also have answered the 68000
        return h;
    };
    const uint64_t h1 = run(48000);                         // one second of audio
    StateIO chk(snap, false); sound_io_locked(chk);
    if (!chk.ok()) { report = "sound state does not parse: " + chk.error(); return false; }
    StateIO ld(snap, true); sound_io_locked(ld);
    const uint64_t h2 = run(48000);
    char b[160]; std::snprintf(b, sizeof b, "sound replay %s (hash %016llx / %016llx, %zu bytes of text)", h1 == h2 ? "identical" : "DIFFERENT", (unsigned long long)h1, (unsigned long long)h2, snap.size());
    report = b; return h1 == h2;
}

void sound_set_pulled(bool on) { g_pulled = on; }
void sound_set_muted(bool m) { g_muted = m; }
bool sound_muted() { return g_muted; }
void sound_set_capture(const char* p) { g_capture_path = p ? p : ""; g_capture.clear(); }
void sound_finish() { if (!g_capture_path.empty()) write_wav(g_capture_path, g_capture, 48000); }
void sound_stats(unsigned long long& i, unsigned long long& w, unsigned long long& k, unsigned long long& q) {
    i = g_insns; w = g_board.chip_writes; k = g_board.key_ons; q = g_irqs;
}

void sound_report() {
    std::fprintf(stderr, "chip register write histogram (reg-range: count):\n");
    struct R { const char* n; int a, b; } rs[] = {{"01-08 control/timers", 0x01, 0x08}, {"20-35 AM/VIB/EGT/KSR/MULT", 0x20, 0x35}, {"40-55 KSL/TL", 0x40, 0x55},
        {"60-75 AR/DR", 0x60, 0x75}, {"80-95 SL/RR", 0x80, 0x95}, {"A0-A8 FNUM lo", 0xA0, 0xA8}, {"B0-B8 KON/BLOCK/FNUM hi", 0xB0, 0xB8},
        {"BD rhythm", 0xBD, 0xBD}, {"C0-C8 FB/CONN", 0xC0, 0xC8}, {"E0-F5 waveform", 0xE0, 0xF5}};
    for (auto& r : rs) { unsigned n = 0; for (int i = r.a; i <= r.b; ++i) n += g_board.hist[i]; std::fprintf(stderr, "  %-28s %u\n", r.n, n); }
    std::fprintf(stderr, "note numbers used at key-on (block fnum count):\n");
    for (int b = 0; b < 8; ++b) for (int f = 0; f < 1024; ++f) if (g_board.notes[b][f] > 4) std::fprintf(stderr, "  NOTE %d %d %u\n", b, f, g_board.notes[b][f]);
    std::fprintf(stderr, "key-on writes: %llu  per channel:", g_board.key_ons);
    for (int i = 0; i < 9; ++i) std::fprintf(stderr, " %u", g_board.kon_hist[i]);
    std::fprintf(stderr, "\n");
}

// Android diagnostics: how many Z80 instructions the sound board has executed (0 = the board never ran)
unsigned long long sound_debug_insns() { return g_insns; }
