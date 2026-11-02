// bus.cpp - minimal 68000 bus for smoke-testing the lifted code.
// Map inferred from the code:
//   000000-03FFFF ROM            080000-083FFF work RAM (init code tests 080000-082FFF; SP starts at 083000)
//   0C0000-0C0FFF palette RAM    140000-14000F video chip ports (+8 = address/index, +C = data)
//   190000-190FFF RAM shared with the Z80 sound CPU (odd byte lane only)    19C000-19C0FF inputs / DIP switches / coin
#include "m68k_rt.h"
#include "hw.h"
#include "state_io.h"
#include "sound.h"
#include <cstring>
#include <atomic>
#include <vector>
#include <algorithm>
#include <thread>
#include <chrono>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <map>
#include <mutex>
#if defined(_WIN32)
extern "C" unsigned long __stdcall GetModuleFileNameA(void*, char*, unsigned long);
#elif __has_include(<unistd.h>)
#include <unistd.h>
#endif
#if __has_include(<execinfo.h>) && !defined(__ANDROID__)   // (bionic only has backtrace() from API 33)
#include <execinfo.h>
#define HAVE_BT 1
#endif

uint8_t rom68k[0x40000];
uint8_t z80rom[0x8000];
static uint8_t ram[0x4000], pal[0x1000];
// Sound-CPU shared RAM: Z80 sees 8000-87FF; the 68000 sees it only on the ODD byte lane of 190000-190FFF
// (Z80 byte 8000+k <-> 68000 address 190001+2k). Even lane = plain latches (no Z80 side).
static uint8_t zsh[0x800], zeven[0x800];
volatile uint8_t* hw_shared_ram() { return zsh; }
static uint8_t vdpregs[0x10];
// video chip model inferred from the self-test at 012ad6: +0 = VRAM address, +4 = data port (auto-increment),
// +8 = register index, +C = register value, +D bit0 = busy
static uint16_t vram[0x8000], vreg[0x100];
static uint8_t vtag[0x1800];                         // 1 = this tile-layer word was written by a picture routine (skip_level_clear_scenes hides those)
static uint32_t vaddr = 0, vidx = 0;
static unsigned io_reads = 0, frames = 0;
// Input/DIP ports at 19C000-19C0FF (byte lanes: the data is in the ODD byte of each word).
// Layout inferred from the IRQ4 input code (see README): 19C02D system (coins active-LOW bits3/4, start bits5/6,
// bit0 = direct credit), 19C031 / 19C035 = player 1 / 2 (bits0-3 up/down/left/right, bit4/5 buttons, active HIGH).
static uint8_t ioport[0x100] = {};
static std::atomic<bool> realtime{false};
void hw_set_input(uint32_t addr, uint8_t v) { ioport[addr & 0xFF] = v; }
uint8_t hw_get_input(uint32_t addr) { return ioport[addr & 0xFF]; }
// Consistent frame snapshot: the game finishes all of its video updates for a frame before it waits for VBLANK
// (STOP in the main loop). hw_latch_video() copies VRAM/palette/registers at that moment; the renderer reads the copy.
// If no snapshot has been taken for 100 ms (a scene that never STOPs) the renderer falls back to the live state.
static uint16_t vram_l[2][0x8000], vreg_l[2][0x100];
static uint8_t tag_l[2][0x1800];
static uint8_t  pal_l[2][0x1000];
static std::atomic<int> latched{-1};
static std::atomic<long long> latch_time_ms{0};
static long long now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
// debug statistics over all end-of-frame snapshots (enabled with M68K_LATCHLOG=1)
static std::vector<int> st_spr, st_bg, st_fg, st_top;
void hw_latch_report() {
    auto dips = [](const std::vector<int>& v, int minNeighbour) {
        long d = 0; for (size_t i = 1; i + 1 < v.size(); ++i) { int m = std::min(v[i - 1], v[i + 1]); if (m >= minNeighbour && v[i] * 2 < m) ++d; } return d; };
    std::fprintf(stderr, "latched frames=%zu\n single-frame DROPOUTS (count < half of both neighbours): sprites=%ld  bg-tiles=%ld  fg-tiles=%ld  top-tiles=%ld\n",
                 st_spr.size(), dips(st_spr, 6), dips(st_bg, 40), dips(st_fg, 40), dips(st_top, 10));
    long jumps = 0; for (size_t i = 1; i < st_spr.size(); ++i) if (st_spr[i] != st_spr[i - 1]) ++jumps;
    std::fprintf(stderr, " frames where the sprite count differs from the previous frame: %ld (%.1f%%)\n", jumps, 100.0 * jumps / std::max<size_t>(1, st_spr.size()));
}
static std::atomic<unsigned long long> latch_count{0};
unsigned long long hw_latch_count() { return latch_count; }
// Compare live state at IRQ exit with the snapshot taken at STOP (statistics only; M68K_LATCHLOG=1)
static long cmp_frames = 0, cmp_reg = 0, cmp_spr = 0, cmp_tile = 0, cmp_pal = 0;
void hw_irq_exit_compare() {
    static bool logon = std::getenv("M68K_LATCHLOG") != nullptr;
    int l = latched.load(); if (!logon || l < 0) return;
    ++cmp_frames;
    bool r = std::memcmp(vreg, vreg_l[l], 16 * 2) != 0, s = std::memcmp(vram + 0x1800, vram_l[l] + 0x1800, 0x800 * 2) != 0,
         t = std::memcmp(vram, vram_l[l], 0x1800 * 2) != 0, p = std::memcmp(pal, pal_l[l], sizeof pal) != 0;
    cmp_reg += r; cmp_spr += s; cmp_tile += t; cmp_pal += p;
}
void hw_irq_compare_report() {
    std::fprintf(stderr, "IRQ-exit vs STOP-snapshot over %ld frames: video REGS changed in %ld, SPRITE list changed in %ld, TILE layers changed in %ld, palette changed in %ld\n",
                 cmp_frames, cmp_reg, cmp_spr, cmp_tile, cmp_pal);
}
void hw_latch_video() {
    static bool logon = std::getenv("M68K_LATCHLOG") != nullptr;
    if (logon) {
        int s = 0, c0 = 0, c1 = 0, c2 = 0;
        for (int i = 0; i < 256; ++i) s += (vram[0x1800 + i * 4] >> 15) & 1;
        for (int i = 0; i < 0x800; i += 2) { c0 += (vram[i] | vram[i + 1]) != 0; c1 += (vram[0x800 + i] | vram[0x801 + i]) != 0; c2 += (vram[0x1000 + i] | vram[0x1001 + i]) != 0; }
        if (st_spr.size() < 400000) { st_spr.push_back(s); st_bg.push_back(c0); st_fg.push_back(c1); st_top.push_back(c2); }
    }
    int n = latched.load() == 0 ? 1 : 0;
    std::memcpy(vram_l[n], vram, sizeof vram); std::memcpy(tag_l[n], vtag, sizeof vtag); std::memcpy(vreg_l[n], vreg, sizeof vreg); std::memcpy(pal_l[n], pal, sizeof pal);
    latch_time_ms = now_ms(); latched = n; ++latch_count;
}
static std::atomic<long> view_calls{0}, view_live{0};
void hw_view_stats(long& calls, long& live) { calls = view_calls; live = view_live; }
static int view() { int l = latched.load(); ++view_calls; if (l >= 0 && now_ms() - latch_time_ms < 100) return l; ++view_live; return -1; }
const uint8_t* hw_ram() { return ram; }
const uint16_t* hw_live_vram() { return vram; }
const uint8_t* hw_vram_tags() { int v = view(); return v < 0 ? vtag : tag_l[v]; }
const uint16_t* hw_vram() { int v = view(); return v < 0 ? vram : vram_l[v]; }
const uint16_t* hw_vreg() { int v = view(); return v < 0 ? vreg : vreg_l[v]; }
const uint8_t*  hw_pal()  { int v = view(); return v < 0 ? pal : pal_l[v]; }
void hw_start_realtime() {          // 60 Hz VBLANK interrupt from wall-clock time
    if (realtime.exchange(true)) return;
    std::thread([] {
        auto next = std::chrono::steady_clock::now();
        const auto period = std::chrono::nanoseconds((long long)(16666667.0 * 100.0 / hw_settings_speed()));   // speed = % of the original 60 Hz
        for (;;) {
            next += period;
#ifdef _WIN32
            // Windows sleeps in coarse steps (about 1-2 ms at best, often 15 ms), which made high speeds uneven and slower than
            // set: sleep until shortly before the tick, then wait the last stretch on the clock itself.
            const auto coarse = next - std::chrono::microseconds(2000);
            if (std::chrono::steady_clock::now() < coarse) std::this_thread::sleep_until(coarse);
            while (std::chrono::steady_clock::now() < next) std::this_thread::yield();
#else
            std::this_thread::sleep_until(next);
#endif
            ++frames; irq_level_pending = 4;
            auto now = std::chrono::steady_clock::now();
            if (now - next > std::chrono::milliseconds(100)) next = now;     // after a long stall (window drag, suspend) resume instead of bursting
        }
    }).detach();
}

static bool slurp(const char* dir, const char* name, uint8_t* dst, size_t n, size_t stride, size_t off) {
    char p[1024]; std::snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE* f = std::fopen(p, "rb"); if (!f) { std::fprintf(stderr, "cannot open %s\n", p); return false; }
    for (size_t i = 0; i < n; ++i) { int c = std::fgetc(f); if (c == EOF) break; dst[i * stride + off] = (uint8_t)c; }
    std::fclose(f); return true;
}

// ============================================================================================================
// pipibibs_settings.txt : DIP switches + game speed, read once at start-up (see kSettingsTemplate for the text).
//   Port layout (verified: the game's own DIP-switch screen at 012eca prints exactly these labels; behaviour of each field was
//   also checked by running the game, see tests/test_settings.cpp):
//     19C021 = DSWA   bit0 unused | bit1 flip screen (012942) | bit2 test mode (000572, 012a52) | bit3 advertise sound OFF (0079d0)
//                     bits4-5 coin A table index (013094) | bits6-7 coin B table index (0130ae)
//     19C025 = DSWB   bits0-1 difficulty (0009fc -> $080653) | bits2-3 extend / bonus life (0011d6 reads the port directly)
//                     bits4-5 number of heroes (000a3e) | bit6 "debug mode" (0005c6, 005536) | bit7 unused
//     19C029 = jumpers: bits0-2 region (-> $080542, 012a3c), bits3-4 alternate text/data tables, bit5 = status line polled
//              by the game (toggled by hw_rd), bits6-7 unused
//   flip_screen is NOT passed to the game (see video.cpp): the renderer turns the finished picture by 180 degrees instead.
//   Game speed = rate of the 60 Hz VBLANK interrupt (the whole game is locked to it). The sound board is clocked by the
//   audio device, so it is not affected.
// ============================================================================================================
static const char* kSettingsTemplate = R"CFG(# ================================================================================================
#  Pipi & Bibis  -  settings file   (pipibibs_settings.txt)
#
#  Keep this file next to pipibibs_play.exe. It is read ONCE when the game starts, so close the
#  game, edit, save, and start it again. Lines starting with # are comments. Format:  key = value
#  Values are not case sensitive. If you delete this file, a fresh one is written on the next start.
# ================================================================================================


# ------------------------------------------------------------------------------------------------
#  GAME SPEED   (audio is NOT affected: music and sound effects keep their normal pitch and tempo)
# ------------------------------------------------------------------------------------------------
#  Percent of the original speed.  100 = original (60 frames per second).
#  50 = half speed, 150 = one and a half, 200 = double.  Allowed range: 10 to 400.
#  Note: the very fast settings need a fast PC, and sound effects can bunch up or be missed
#  because the sound board still runs at its normal rate.
speed = 100


# ------------------------------------------------------------------------------------------------
#  DIP SWITCH BANK A
# ------------------------------------------------------------------------------------------------
#  Turn the whole picture upside down (cocktail cabinet):   off / on
#  (done by turning the finished picture 180 degrees; the game itself is told "normal screen".)
flip_screen = off

#  Test mode: starts the game's own service menu (INPUT CHECK / DIPSW DISPLAY / SOUND CHECK):   off / on
#  Handy to see the DIP switches exactly as the game reads them. In that menu use the arrows and Z (button 1).
service_mode = off

#  Demo sounds: on = the attract mode plays sound.  off = the attract mode is silent.
demo_sounds = on

#  Coin slot A and coin slot B. Each takes 0, 1, 2 or 3. What the number means depends on "region"
#  (see below): regions 0-5 use one coinage table, regions 6-7 another.
#
#     value | regions 0-5, coin A and B | regions 6-7, coin A  | regions 6-7, coin B
#     ------+--------------------------+----------------------+---------------------
#       0   | 1 coin  = 1 credit       | 1 coin  = 1 credit   | 1 coin = 2 credits
#       1   | 1 coin  = 2 credits      | 2 coins = 1 credit   | 1 coin = 3 credits
#       2   | 2 coins = 1 credit       | 3 coins = 1 credit   | 1 coin = 4 credits
#       3   | 2 coins = 3 credits      | 4 coins = 1 credit   | 1 coin = 6 credits
#
#  (Coin A is the "5" key, coin B is the "6" key.)
coin_a = 0
coin_b = 0


# ------------------------------------------------------------------------------------------------
#  DIP SWITCH BANK B
# ------------------------------------------------------------------------------------------------
#  Difficulty:   normal / easy / hard / hardest
difficulty = normal

#  Number of heroes (lives) per credit:   1 / 2 / 3 / 5
lives = 3

#  Extra heroes (extend):
#     200k-300k = first at 200,000 points, then every 300,000     (default)
#     150k-200k = first at 150,000 points, then every 200,000
#     200k-only = a single one, at 200,000 points
#     none      = no extra heroes
bonus_life = 200k-300k

#  "Debug mode": DIP2-7 of the original board, labelled DEBUG MODE on the game's DIP screen.
#  What it changes in play has not been investigated; leave it off unless you are curious.   off / on
debug_mode = off


# ------------------------------------------------------------------------------------------------
#  REGION JUMPERS  (read once at power-on, so they take effect when the game starts)
# ------------------------------------------------------------------------------------------------
#  region: 0 - 7.   Which territory the board is set up for:
#        0, 1  =  Hong Kong      (the "NOTICE" screen at power-on says "HONG KONG ONLY")
#        2, 3, 4  =  U.S.A.
#        5, 6, 7  =  Europe
#  It also chooses the coinage table (regions 0-5 use the first, 6-7 the second, see coin_a / coin_b above)
#  and changes some texts of the service screens. The original build of this port behaved like region 7.
region = 7



# ------------------------------------------------------------------------------------------------
#  OPTIONAL GAME CHANGES
# ------------------------------------------------------------------------------------------------
#  skip_level_clear_scenes:   off / on
#  When on, two things happen:
#   1. Clearing a level takes you straight to the next one. Skipped: the picture that is uncovered
#      behind the cloud effect after every level, and the longer timed picture scene that follows
#      every fourth level. Dying ("try again") is not affected.
#   2. As a safety net, the pictures of the women are drawn solid black wherever they still appear:
#      the comic panel of the attract mode, the silhouette screens (attract mode and the start of a
#      game), the pictures of the ending after the last level, and the portraits on the flipped cards of
#      the screen wipe. Those screens show black instead (the text, the timer and the characters stay).
#  Not skipped: the title and attract-mode screens (they are just shown with the pictures black) and
#  the scenes of the ending after the last level (the game needs them to finish properly; they show
#  the plain level or a black picture area).
#  It works by fast-forwarding the game's own scene routine, so the next level starts exactly as it
#  would have. No score is awarded inside those scenes (none of their code adds points), but their
#  jingles and effects are skipped with them.
skip_level_clear_scenes = off


# ------------------------------------------------------------------------------------------------
#  SAVE / LOAD GAME  (a snapshot of the whole game, written to a text file)
# ------------------------------------------------------------------------------------------------
#  Press save_key to save the game exactly as it is, and load_key to jump back to it later (even
#  after closing and restarting the game, as long as the ROM files are the same).
#  The window title shows "Game saved" / "Game loaded" for a few seconds.
#  Saving and loading happen in the game's normal frame loop. During a fade or transition screen
#  the game waits (up to 15 seconds) for it to come back, and the title says so.
#  Keys: a letter, a digit, F1-F12, pageup, pagedown, home, end, insert, delete, tab, backspace,
#  space, enter, or none to switch the key off.  Most of F1-F12 and many letters already do other
#  things in this window, so the defaults are the digits 9 and 0 (top row of the keyboard).
save_key = 9
load_key = 0

#  The file that is written / read. It lives in the same folder as this settings file. Change the
#  name to keep several saves (restart the game after changing it), or to switch to another save.
save_file = pipibibs_save.txt

# ------------------------------------------------------------------------------------------------
#  SWAP PLAYERS   (off / on)
# ------------------------------------------------------------------------------------------------
#  on = the keys of player 1 (arrows, Z, X, 5 coin, 1 start) control player 2, and the keys of
#  player 2 (W A S D, F, G, 6 coin, 2 start) control player 1.
swap_players = off


# ------------------------------------------------------------------------------------------------
#  ADVANCED: raw switch bytes (optional - remove the leading # to use)
# ------------------------------------------------------------------------------------------------
#  Sets the whole 8-bit register at once (decimal or 0x hex). The named switches above are then applied
#  on top, so leave them out of the file if you want the raw value to win. Bit layout:
#     DSWA:  bit0 unused | bit1 flip screen (see note) | bit2 test mode | bit3 advertise sound OFF | bits4-5 coin A | bits6-7 coin B
#     DSWB:  bits0-1 difficulty (0 normal, 1 easy, 2 hard, 3 hardest) | bits2-3 extend (0 = 200k+300k every, 1 = 150k+200k every,
#            2 = 200k only, 3 = none) | bits4-5 heroes (0 = 3, 1 = 5, 2 = 2, 3 = 1) | bit6 debug mode | bit7 unused
#     Note: bit1 of DSWA is applied as the "flip_screen" picture rotation, it is not sent to the game.
#dswa_raw = 0x00
#dswb_raw = 0x00

#  Second region jumper pair (0 - 3): the game reads it in many places to pick alternate data tables.
#  In my tests it changed nothing visible on the power-on notice, title or attract screens, and what it
#  does during play has NOT been investigated. 3 is what this port has always used; leave it alone
#  unless you want to experiment.
#jumper_34 = 3
)CFG";

struct Settings {
    double speed = 100.0;
    uint8_t dswa = 0x00, dswb = 0x00;
    uint8_t jumper = 0xFF;                        // bits0-2 region, bits3-4 alt tables, bit5 (status, forced), bits6-7 = 1 as before
    bool flip = false, skip_clear = false, swap_players = false;
    std::string save_key = "9", load_key = "0", save_file = "pipibibs_save.txt";
    std::string dir;                              // folder of the settings file: save files live there
    std::string warnings, source;
} g_set;

static std::string trim(std::string v) {
    size_t b = v.find_first_not_of(" \t\r\n"), e = v.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : v.substr(b, e - b + 1);
}
static std::string lower(std::string v) { for (char& c : v) c = (char)std::tolower((unsigned char)c); return v; }
static bool parse_num(const std::string& v, double& out) {
    if (v.empty()) return false;
    char* end = nullptr;
    if (v.size() > 2 && v[0] == '0' && (v[1] == 'x')) { long x = std::strtol(v.c_str() + 2, &end, 16); if (*end) return false; out = (double)x; return true; }
    out = std::strtod(v.c_str(), &end); return *end == 0 && std::isfinite(out);
}
static bool parse_bool(const std::string& v, bool& out) {
    if (v == "on" || v == "yes" || v == "true" || v == "1")  { out = true;  return true; }
    if (v == "off" || v == "no" || v == "false" || v == "0") { out = false; return true; }
    return false;
}

static std::string exe_dir() {
    char buf[1024] = {};
#if defined(_WIN32)
    unsigned long n = GetModuleFileNameA(nullptr, buf, sizeof buf - 1);
    if (n == 0 || n >= sizeof buf - 1) return "";
#elif __has_include(<unistd.h>)
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "";
    buf[n] = 0;
#else
    return "";
#endif
    std::string p = buf; size_t k = p.find_last_of("/\\");
    return k == std::string::npos ? "" : p.substr(0, k);
}
static bool file_exists(const std::string& p) { if (FILE* f = std::fopen(p.c_str(), "rb")) { std::fclose(f); return true; } return false; }

static void settings_parse(const std::string& text) {
    Settings s; s.source = g_set.source; s.dir = g_set.dir;
    std::string src = (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) ? text.substr(3) : text;   // UTF-8 BOM (Notepad)
    std::map<std::string, std::string> kv, kvraw; std::map<std::string, int> line_of; std::vector<std::pair<int, std::string>> warn;
    int ln = 0; size_t pos = 0;
    while (pos <= src.size()) {
        size_t nl = src.find('\n', pos); if (nl == std::string::npos) nl = src.size();
        std::string line = src.substr(pos, nl - pos); pos = nl + 1; ++ln;
        size_t c = line.find('#'); if (c != std::string::npos) line.erase(c);
        line = trim(line); if (line.empty()) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) { warn.push_back({ ln, "line " + std::to_string(ln) + ": expected  key = value  ->  " + line }); continue; }
        std::string k = lower(trim(line.substr(0, eq))), v = lower(trim(line.substr(eq + 1)));
        if (v.empty()) { warn.push_back({ ln, "line " + std::to_string(ln) + ": '" + k + "' has no value" }); continue; }
        kv[k] = v; kvraw[k] = trim(line.substr(eq + 1)); line_of[k] = ln;
    }
    auto bad = [&](const std::string& k, const char* allowed) {
        warn.push_back({ line_of[k], "line " + std::to_string(line_of[k]) + ": invalid value '" + kv[k] + "' for '" + k + "' (allowed: " + allowed + ") - ignored" }); };
    auto take = [&](const char* key) -> const std::string* { auto it = kv.find(key); if (it == kv.end()) return nullptr; return &it->second; };
    static const char* known[] = { "speed", "flip_screen", "service_mode", "demo_sounds", "coin_a", "coin_b", "difficulty", "lives", "bonus_life", "debug_mode", "region", "jumper_34", "dswa_raw", "dswb_raw", "save_key", "load_key", "save_file", "skip_level_clear_scenes", "swap_players" };
    for (auto& e : kv) { bool ok = false; for (const char* k : known) ok |= e.first == k;
        if (!ok) warn.push_back({ line_of[e.first], "line " + std::to_string(line_of[e.first]) + ": unknown setting '" + e.first + "' - ignored" }); }

    double x;
    if (auto v = take("speed")) { if (parse_num(*v, x) && x >= 10.0 && x <= 400.0) s.speed = x; else bad("speed", "a number from 10 to 400"); }
    if (auto v = take("dswa_raw")) { if (parse_num(*v, x) && x >= 0 && x <= 255) s.dswa = (uint8_t)x; else bad("dswa_raw", "0..255 or 0x00..0xFF"); }
    if (auto v = take("dswb_raw")) { if (parse_num(*v, x) && x >= 0 && x <= 255) s.dswb = (uint8_t)x; else bad("dswb_raw", "0..255 or 0x00..0xFF"); }
    bool b;
    auto bit = [&](const char* key, uint8_t& reg, int n, bool invert) {
        if (auto v = take(key)) { if (parse_bool(*v, b)) { if (b != invert) reg |= (1u << n); else reg &= ~(1u << n); } else bad(key, "on / off"); } };
    bit("flip_screen", s.dswa, 1, false);
    bit("service_mode", s.dswa, 2, false);
    bit("demo_sounds", s.dswa, 3, true);                   // switch bit set = demo sounds OFF
    auto field = [&](const char* key, uint8_t& reg, int shift, int maxv) {
        if (auto v = take(key)) { if (parse_num(*v, x) && x >= 0 && x <= maxv && x == std::floor(x)) reg = (uint8_t)((reg & ~(3u << shift)) | ((unsigned)x << shift)); else bad(key, "0, 1, 2 or 3"); } };
    field("coin_a", s.dswa, 4, 3);
    field("coin_b", s.dswa, 6, 3);
    if (auto v = take("difficulty")) {
        int n = *v == "normal" ? 0 : *v == "easy" ? 1 : *v == "hard" ? 2 : *v == "hardest" ? 3 : -1;
        if (n < 0 && parse_num(*v, x) && x >= 0 && x <= 3 && x == std::floor(x)) n = (int)x;
        if (n >= 0) s.dswb = (uint8_t)((s.dswb & ~3u) | n); else bad("difficulty", "normal / easy / hard / hardest");
    }
    if (auto v = take("lives")) {
        int n = *v == "3" ? 0 : *v == "5" ? 1 : *v == "2" ? 2 : *v == "1" ? 3 : -1;
        if (n >= 0) s.dswb = (uint8_t)((s.dswb & ~0x30u) | (n << 4)); else bad("lives", "1 / 2 / 3 / 5");
    }
    auto keyname = [&](const char* key, std::string& dst) {
        if (auto v = take(key)) {
            static const char* names[] = { "pageup", "pagedown", "home", "end", "insert", "delete", "tab", "backspace", "space", "enter", "none" };
            bool ok = v->size() == 1 && std::isalnum((unsigned char)(*v)[0]);
            if (!ok && v->size() >= 2 && v->size() <= 3 && (*v)[0] == 'f' && std::isdigit((unsigned char)(*v)[1])) { int n = std::atoi(v->c_str() + 1); ok = n >= 1 && n <= 12; }
            for (const char* nm : names) ok |= (*v == nm);
            if (ok) dst = *v; else bad(key, "a letter, a digit, F1-F12, pageup, pagedown, home, end, insert, delete, tab, backspace, space, enter or none");
        } };
    keyname("save_key", s.save_key);
    keyname("load_key", s.load_key);
    if (take("save_file")) {
        const std::string& f = kvraw["save_file"];
        size_t colon = f.find(':');
        if (f.find_first_of("<>\"|?*") == std::string::npos && (colon == std::string::npos || colon == 1)) s.save_file = f;
        else bad("save_file", "a file name such as pipibibs_save.txt");
    }
    if (auto v = take("bonus_life")) {
        int n = *v == "200k-300k" ? 0 : *v == "150k-200k" ? 1 : *v == "200k-only" ? 2 : *v == "none" ? 3 : -1;
        if (n < 0 && parse_num(*v, x) && x >= 0 && x <= 3 && x == std::floor(x)) n = (int)x;
        if (n >= 0) s.dswb = (uint8_t)((s.dswb & ~0x0Cu) | (n << 2)); else bad("bonus_life", "200k-300k / 150k-200k / 200k-only / none");
    }
    bit("debug_mode", s.dswb, 6, false);
    if (auto v = take("swap_players")) { if (parse_bool(*v, b)) s.swap_players = b; else bad("swap_players", "on / off"); }
    if (auto v = take("skip_level_clear_scenes")) { if (parse_bool(*v, b)) s.skip_clear = b; else bad("skip_level_clear_scenes", "on / off"); }
    if (auto v = take("region")) {
        if (parse_num(*v, x) && x >= 0 && x <= 7 && x == std::floor(x)) s.jumper = (uint8_t)((s.jumper & ~7u) | (unsigned)x); else bad("region", "0..7");
    }
    if (auto v = take("jumper_34")) {
        if (parse_num(*v, x) && x >= 0 && x <= 3 && x == std::floor(x)) s.jumper = (uint8_t)((s.jumper & ~0x18u) | ((unsigned)x << 3)); else bad("jumper_34", "0..3");
    }
    std::stable_sort(warn.begin(), warn.end(), [](const std::pair<int, std::string>& x, const std::pair<int, std::string>& y) { return x.first < y.first; });
    for (auto& w : warn) s.warnings += w.second + "\n";
    s.flip = (s.dswa & 2) != 0; s.dswa &= (uint8_t)~2u;     // flip is done by the renderer, never shown to the game
    g_set = s;
}

// Looks for pipibibs_settings.txt (env PIPIBIBS_SETTINGS, exe folder, current folder, ROM folder, its parent); writes a
// commented default next to the exe when none exists. Never fails: bad lines are reported through hw_settings_warnings().
static void settings_load(const char* romdir) {
    std::vector<std::string> tries;
    if (const char* e = std::getenv("PIPIBIBS_SETTINGS")) tries.push_back(e);
    std::string ed = exe_dir(), rd = romdir ? romdir : "";
    const char* name = "pipibibs_settings.txt";
    if (!ed.empty()) tries.push_back(ed + "/" + name);
    tries.push_back(name);
    if (!rd.empty()) { tries.push_back(rd + "/" + name); tries.push_back(rd + "/../" + name); }
    std::string found;
    for (auto& t : tries) if (file_exists(t)) { found = t; break; }
    std::string text;
    if (found.empty()) {
        text = kSettingsTemplate;
        std::string w = std::getenv("PIPIBIBS_SETTINGS") ? std::getenv("PIPIBIBS_SETTINGS") : (ed.empty() ? std::string(name) : ed + "/" + name);
        if (FILE* f = std::fopen(w.c_str(), "wb")) { std::fwrite(text.data(), 1, text.size(), f); std::fclose(f); found = w + " (created with defaults)"; }
        else found = "(built-in defaults)";
    } else if (FILE* f = std::fopen(found.c_str(), "rb")) {
        char buf[4096]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n); std::fclose(f);
    }
    g_set.source = found;
    {   // folder that holds the settings file (save states go there too); no real file -> next to the exe
        std::string real = found.rfind(" (created", std::string::npos) != std::string::npos ? found.substr(0, found.rfind(" (created")) : found;
        size_t k = real.find_last_of("/\\");
        g_set.dir = real[0] == '(' ? ed : (k == std::string::npos ? std::string() : real.substr(0, k));
    }
    settings_parse(text);
    ioport[0x20] = 0; ioport[0x21] = g_set.dswa;       // DSWA  (word port 19C020, data on the odd byte)
    ioport[0x24] = 0; ioport[0x25] = g_set.dswb;       // DSWB  (word port 19C024)
}
const char* hw_settings_warnings() { return g_set.warnings.c_str(); }
const char* hw_settings_source()   { return g_set.source.c_str(); }
double hw_settings_speed()         { return g_set.speed; }
const char* hw_settings_save_key() { return g_set.save_key.c_str(); }
const char* hw_settings_load_key() { return g_set.load_key.c_str(); }
bool hw_settings_flip()            { return g_set.flip; }
bool hw_settings_skip_clear()      { return g_set.skip_clear; }
bool hw_settings_swap_players()    { return g_set.swap_players; }
bool hw_settings_hide_pictures()   { return g_set.skip_clear; }       // the same option: pictures are drawn black


// ============================================================================================================
// SAVE STATES  (pipibibs_save.txt)
//   The 68000 code is lifted to native functions, so the native call stack itself can not be saved. What makes a state
//   restorable is the "shadow stack" (the return addresses of the live CALLs, see m68k_rt.h): when the shadow stack at load time
//   is identical to the one that was saved, every native frame is at exactly the position the saved registers/RAM describe.
//   In play the game waits for each VBLANK in the main loop with the shadow stack {0005c6}; that is the one place where states
//   are saved and loaded (a request made during a fade/transition waits a moment for the main loop to come back).
//   Saved: 68000 registers + flags, work RAM, palette, video RAM + registers, the video chip pointers, the sound board
//   (Z80, FM chip, shared RAM). Not saved: the keyboard, the DIP switches (they come from the settings file), the settings.
// ============================================================================================================
static bool g_clear_fix = false;                     // skip_level_clear_scenes: true for the one frame in which the level index is 1 too high
static const uint32_t kMainLoopRet = 0x0005C6;       // shadow stack of the main-loop VBLANK wait (verified by recording every STOP)
static std::mutex st_mx;
static std::string st_msg; static unsigned st_seq = 0;
static std::atomic<int> st_req{0};                   // 0 none, 1 save, 2 load, 3 self-test
static void st_say(const std::string& m) { std::lock_guard<std::mutex> lk(st_mx); st_msg = m; ++st_seq; std::fprintf(stderr, "[state] %s\n", m.c_str()); }
void hw_state_request_save() { st_req = 1; }
void hw_state_request_load() { st_req = 2; }
void hw_state_request_selftest() { st_req = 3; }
bool hw_state_message(unsigned& seq, char* buf, size_t n) {
    std::lock_guard<std::mutex> lk(st_mx);
    if (seq == st_seq) return false;
    seq = st_seq; std::snprintf(buf, n, "%s", st_msg.c_str()); return true;
}
static std::string save_path() {
    const std::string& f = g_set.save_file;
    bool abs = !f.empty() && (f[0] == '/' || f[0] == '\\' || (f.size() > 1 && f[1] == ':'));
    return abs || g_set.dir.empty() ? f : g_set.dir + "/" + f;
}
#ifndef M68K_FAST_CALLS
static std::vector<uint32_t> shadow_now() { return std::vector<uint32_t>(shadow_stack, shadow_stack + shadow_sp); }
#else
static std::vector<uint32_t> shadow_now() { return std::vector<uint32_t>(1, 0xFFFFFFFFu); }     // no shadow stack in this build: states are refused below
#endif
static uint64_t rom_sum() { uint64_t h = 1469598103934665603ULL; for (uint32_t i = 0; i < sizeof rom68k; ++i) h = (h ^ rom68k[i]) * 1099511628211ULL; return h; }

// One routine for saving, for the dry-run pass and for the real load. `shadow` receives the saved shadow stack.
static bool state_all(StateIO& io, std::vector<uint32_t>& shadow) {
    io.comment("Pipi & Bibis save state. Plain text: edit at your own risk (a wrong value can crash the game).");
    io.comment("It only loads while the game is in its normal frame loop, with the same ROMs.");
    io.section("header");
    uint32_t version = 2; io.val_in("version", version);
    uint64_t sum = rom_sum(), file_sum = sum; io.val_in("rom_sum", file_sum);
    if (!io.ok()) return false;
    if (version != 1 && version != 2) { io.set_error("unsupported save file version " + std::to_string(version)); return false; }
    if (file_sum != sum) { io.set_error("this save was made with different ROM files"); return false; }
    if (io.saving()) shadow = shadow_now();
    io.u32list("shadow_stack", shadow);

    io.section("cpu68k");
    for (int i = 0; i < 8; ++i) { char n[8]; std::snprintf(n, sizeof n, "d%d", i); io.val(n, d[i]); }
    for (int i = 0; i < 8; ++i) { char n[8]; std::snprintf(n, sizeof n, "a%d", i); io.val(n, a[i]); }
    io.val("fX", fX); io.val("fN", fN); io.val("fZ", fZ); io.val("fV", fV); io.val("fC", fC);
    io.val("usp", usp); io.val("sr_sys", sr_sys);

    io.section("video_and_ram");
    io.bytes("work_ram", ram, sizeof ram);
    io.bytes("palette", pal, sizeof pal);
    io.words("vram", vram, 0x8000);
    io.words("vreg", vreg, 0x100);
    io.bytes("vdpregs", vdpregs, sizeof vdpregs);
    io.val("vaddr", vaddr); io.val("vidx", vidx);
    io.bytes("shared_even_lane", zeven, sizeof zeven);
    if (io.saving() || version >= 2) io.bytes("picture_tags", vtag, sizeof vtag);
    else if (io.applying()) std::memset(vtag, 0, sizeof vtag);                   // a version 1 file has no tags
    sound_state_io(io);
    return io.ok();
}

static bool state_write(std::string& text) {
    StateIO io; std::vector<uint32_t> sh;
    if (!state_all(io, sh)) return false;
    text = io.str(); return true;
}
// returns 0 = loaded, 1 = text is not a usable state (err filled), 2 = valid but saved in a different part of the game
static int state_read(const std::string& text, std::string& err) {
    std::vector<uint32_t> saved, now = shadow_now();
    { StateIO dry(text, false); if (!state_all(dry, saved)) { err = dry.error(); return 1; } }
    if (saved != now) return 2;
    std::vector<uint32_t> ignore; StateIO io(text, true);
    state_all(io, ignore);
    irq_level_pending = 0;
    g_clear_fix = false;
    hw_latch_video();
    return 0;
}


// ---- skip_level_clear_scenes -------------------------------------------------------------------------------------------
// What the game does when a level is cleared (found by reading the code and by forcing it in the running game):
//   * 00858E sees a player's "cleared" bits in $08062A (mask $18) and spawns the SCENE TASK (slot 1, RAM $080012) with parameter 1,
//     which makes the task start in its state 1 (handler 001744, state table at 001774).
//   * state 1 counts frames in 4(a5) = $080016: until $200 it reveals the picture step by step (tile writes every $28 frames + the
//     32 cloud sprites), at $200 it cleans the sprites up, until $2C0 it waits (with a button check), then (001A56) it adds 1 to the
//     level index $08064E and - if the index is now a multiple of 4 - starts state 2 (a timed picture scene, then a wipe).
//     Otherwise it sets the task to state 0 (002218), which loads and starts the next level.
//   The hook below, run once per frame at the main-loop wait (where the 68000 is between two frames, so nothing is half done),
//   jumps the frame counter to $200 (the game itself then does the clean-up), then to $2C0, and when the index would become a
//   multiple of 4 it adds 1 first so that the game's own test fails and state 2 never starts; the extra 1 is taken off again as
//   soon as the task is back in state 0, before the next level reads the index. The ending after the last level (index $18) is
//   left alone, because that path needs the scene states.
// Return addresses of the call sites of the routines that draw the pictures (all found by logging which call chains write the
// tile layers on every screen of attract mode and of the forced level clears):
//   0018F0 001B88 001EEE 001F06 001F38  = callers of 0020A0 (picture blitter: start-of-level picture, bonus scene, ending pictures)
//   001D10 001D16                       = callers of 0020B6 / 0020D2 (the same blitter's scrolling steps)
//   0006D0                              = the 11-row call of 000774 (the comic panel of the attract mode); the 3-row call at 0006B4 draws the
//                                         region-dependent PIPI&BIBIS logo above it and is left alone
static bool in_picture_chain() {
#ifndef M68K_FAST_CALLS
    static const uint32_t pic[] = { 0x0018F0, 0x001B88, 0x001EEE, 0x001F06, 0x001F38, 0x001D10, 0x001D16, 0x0006D0 };
    for (int i = 0; i < shadow_sp; ++i) for (uint32_t r : pic) if (shadow_stack[i] == r) return true;
#endif
    return false;
}
static inline bool at_main_loop() {
#ifndef M68K_FAST_CALLS
    return shadow_sp == 1 && shadow_stack[0] == kMainLoopRet;
#else
    return false;
#endif
}
static void hook_skip_level_clear() {
    const uint32_t TASK = 0x080012, LEVEL = 0x08064E, FLAGS = 0x08062A;
    bool& fix = g_clear_fix;
    const uint16_t w0 = rd16(TASK), st = rd16(TASK + 2);
    const unsigned state = (st & 0x1C) >> 2;
    if (!(w0 & 0x8000)) { fix = false; return; }                  // scene task not running
    if (fix) {                                                     // the game took the "next level" path: give back the extra 1
        if (state == 0) { wr16(LEVEL, (uint16_t)(rd16(LEVEL) - 1)); fix = false; }
        else if (state != 1) fix = false;
        return;
    }
    if (state != 1 || !(st & 2) || !(rd16(FLAGS) & 0x18)) return;  // only state 1, after its first frame, and only for a CLEARED level (not "try again")
    const uint16_t c = rd16(TASK + 4);
    if (c < 0x200) { wr16(TASK + 4, 0x200); return; }              // skip the reveal; the game cleans up its sprites on the next frame
    if (c > 0x200 && c < 0x2C0) {
        const uint16_t level = rd16(LEVEL);
        if ((level & 3) == 3 && level + 1 < 0x18) { wr16(LEVEL, (uint16_t)(level + 1)); fix = true; }   // see above
        wr16(TASK + 4, 0x2C0);
    }
}

void hw_state_poll() {                               // called from cpu_stop() before every wait on the emulation thread
    static const bool no_hook = std::getenv("PIPIBIBS_NOHOOK") != nullptr;        // test aid: black pictures without the skipping
    if (g_set.skip_clear && !no_hook && at_main_loop()) hook_skip_level_clear();
    using clk = std::chrono::steady_clock;
    static int pending = 0; static clk::time_point deadline, started; static bool told = false; static std::string text, err;
    int r = st_req.exchange(0);
    if (r) { pending = r; started = clk::now(); deadline = started + std::chrono::seconds(15); told = false; text.clear(); }
    if (!pending) return;
    const std::vector<uint32_t> sh = shadow_now();
#ifndef M68K_FAST_CALLS
    const bool main_loop = sh.size() == 1 && sh[0] == kMainLoopRet;
#else
    const bool main_loop = false;
    if (pending) { st_say("Save states need the build without M68K_FAST_CALLS"); pending = 0; return; }
#endif
    if (pending == 2 && text.empty()) {              // read the file once
        FILE* f = std::fopen(save_path().c_str(), "rb");
        if (!f) { st_say("No saved game found (" + save_path() + ")"); pending = 0; return; }
        char buf[65536]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
        if (text.empty()) { st_say("The save file is empty (" + save_path() + ")"); pending = 0; return; }
    }
    if (!main_loop) {
        if (!told && clk::now() > started + std::chrono::milliseconds(500)) { told = true; if (pending != 3) st_say(pending == 1 ? "Saving: waiting for a safe moment..." : "Loading: waiting for a safe moment..."); }
        if (clk::now() > deadline) { st_say(pending == 1 ? "Could not save: the game stayed in a transition screen. Try again." :
                                          pending == 2 ? "Could not load: the game stayed in a transition screen. Try again." : "Self-test: not in the main loop");
                                     pending = 0; text.clear(); }
        return;
    }
    if (g_clear_fix) return;                         // never save/load while the level index carries the hook's temporary +1
    const int what = pending; pending = 0;
    if (what == 1) {
        std::string t;
        if (!state_write(t)) { st_say("Could not save: internal error"); return; }
        const std::string path = save_path(), tmp = path + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f || std::fwrite(t.data(), 1, t.size(), f) != t.size()) { if (f) std::fclose(f); st_say("Could not write " + path + " (is the folder writable?)"); return; }
        std::fclose(f); std::remove(path.c_str());
        if (std::rename(tmp.c_str(), path.c_str()) != 0) { st_say("Could not write " + path); return; }
        st_say("Game saved (" + std::to_string(t.size() / 1024) + " KB)");
    } else if (what == 2) {
        int rc = state_read(text, err); text.clear();
        if (rc == 0) st_say("Game loaded");
        else if (rc == 1) st_say("The save file cannot be used: " + err);
        else st_say("The save was made in a different part of the game and cannot be loaded here");
    } else {                                         // self-test: save -> load -> save must give identical text; then the sound replay check
        std::string a, b, e; bool ok = state_write(a);
        if (ok) { int rc = state_read(a, e); ok = rc == 0; if (!ok) e = rc == 1 ? e : "shadow stack differs"; }
        if (ok) {                                    // the sound board keeps running between the two writes, so only the part before [sound] is compared
            ok = state_write(b);
            size_t ea = a.find("[sound]"), eb = b.find("[sound]");
            if (ok && (ea == std::string::npos || ea != eb || a.compare(0, ea, b, 0, eb) != 0)) {
                size_t i = 0; while (i < a.size() && i < b.size() && a[i] == b[i]) ++i; ok = false; e = "round trip differs at text offset " + std::to_string(i); }
        }
        std::string snd; bool sok = sound_state_selftest(snd);
        st_say(std::string("Self-test: 68000/video round trip ") + (ok ? "identical" : "FAILED (" + e + ")") + " (" + std::to_string(a.size() / 1024) + " KB); " + snd + (sok ? "" : " - FAILED"));
    }
}
bool load_roms(const char* dir) {
    settings_load(dir);
    ioport[0x2D] = 0x18;                       // coin switches released (active low)
    return slurp(dir, "tp025-1.bin", rom68k, 0x20000, 2, 0) &&   // high bytes
           slurp(dir, "tp025-2.bin", rom68k, 0x20000, 2, 1) &&   // low bytes
           slurp(dir, "tp025-5.bin", z80rom, 0x8000, 1, 0);
}
void hw_reset() {
    ioport[0x2D] = 0x18;                       // coin switches released (active low)
    std::memset(vdpregs, 0, sizeof vdpregs); std::memset(vtag, 0, sizeof vtag); vaddr = vidx = 0;
}

static void tick() {                       // crude time base: raise level-4 interrupt (VBLANK) periodically
    if (++io_reads % 1500 == 0) { ++frames; irq_level_pending = 4; }
}
void hw_dump(const char* prefix) {   // debugging aid: write video state to files
    char p[512];
    std::snprintf(p, sizeof p, "%s_vram.bin", prefix); if (FILE* f = std::fopen(p, "wb")) { for (int i = 0; i < 0x8000; ++i) { std::fputc(vram[i] >> 8, f); std::fputc(vram[i] & 255, f); } std::fclose(f); }
    std::snprintf(p, sizeof p, "%s_pal.bin", prefix);  if (FILE* f = std::fopen(p, "wb")) { std::fwrite(pal, 1, sizeof pal, f); std::fclose(f); }
    std::snprintf(p, sizeof p, "%s_vreg.bin", prefix); if (FILE* f = std::fopen(p, "wb")) { for (int i = 0; i < 0x100; ++i) { std::fputc(vreg[i] >> 8, f); std::fputc(vreg[i] & 255, f); } std::fclose(f); }
    std::snprintf(p, sizeof p, "%s_ram.bin", prefix);  if (FILE* f = std::fopen(p, "wb")) { std::fwrite(ram, 1, sizeof ram, f); std::fclose(f); }
}
unsigned hw_frames() { return frames; }
void host_idle() {
    if (realtime) {
#ifdef _WIN32
        std::this_thread::yield();      // a 1 ms sleep takes 1-2 ms (or 15 ms) on Windows, so every wait for the next tick arrived late: at high speeds
                                        // that is where the game lost time. Yield-spin instead (the caller re-checks the interrupt each time).
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
    } else { ++frames; irq_level_pending = 4; }
}
void host_tick_irq() { if (!realtime) { ++frames; irq_level_pending = 4; } }

static uint8_t hw_rd(uint32_t a) {
    if (a == 0x14000D) { static unsigned n; return (++n >> 1) & 1; }   // ASSUMPTION: bit0 = beam/blank-style status that changes over time; polled in both polarities
    if (a >= 0x140000 && a < 0x140010) return 0;
    if (a == 0x19C029) { ++io_reads; uint8_t base = (uint8_t)(g_set.jumper | 0x20); return (io_reads >> 4) & 1 ? (uint8_t)(base & 0xDF) : base; }  // ASSUMPTION: bit5 = busy/vblank-like status, polled by sub_0012f6
    if (a >= 0x19C000 && a < 0x19C100) {                       // inputs/DIPs (see ioport[])
        if (const char* e = std::getenv("M68K_IO_IDLE")) { (void)e; }
        return ioport[a & 0xFF];
    }
    return 0;
}
static void hw_wr(uint32_t a, uint8_t v) {
    if (a >= 0x140000 && a < 0x140010) vdpregs[a & 15] = v;
}
uint8_t rd8(uint32_t a) {
    a &= 0xFFFFFF;
    if (a < 0x40000) return rom68k[a];
    if (a >= 0x80000 && a < 0x84000) return ram[a - 0x80000];
    if (a >= 0xC0000 && a < 0xC1000) return pal[a - 0xC0000];
    if (a >= 0x190000 && a < 0x191000) {
        uint32_t k = (a - 0x190000) >> 1;
        if (a & 1) return *(volatile uint8_t*)&zsh[k];
        return zeven[k];
    }
    return hw_rd(a);
}
void wr8(uint32_t a, uint8_t v) {
    a &= 0xFFFFFF;
    if (a >= 0x80000 && a < 0x84000) ram[a - 0x80000] = v;
    else if (a >= 0xC0000 && a < 0xC1000) {
        pal[a - 0xC0000] = v;
#ifdef HAVE_BT
        if (std::getenv("M68K_PALBT") && ((ram[0x564] << 8) | ram[0x565]) > 5) {
            void* bt[16]; int k = backtrace(bt, 16);
            static void* seen[64]; static int ns = 0;
            // identify the writer by the first frame that is not wr8/wr16/wr32
            for (int f = 1; f < k && f < 6; ++f) {
                char** s = backtrace_symbols(&bt[f], 1);
                bool helper = std::strstr(s[0], "_Z3wr") || std::strstr(s[0], "_Z4wr");
                if (!helper) {
                    bool dup = false; for (int i = 0; i < ns; ++i) if (std::strcmp((const char*)seen[i], s[0]) == 0) dup = true;
                    if (!dup && ns < 64) { seen[ns++] = strdup(s[0]); std::fprintf(stderr, "palette writer: %s  (byte off %x)\n", s[0], a - 0xC0000); }
                    free(s); break;
                }
                free(s);
            }
        }
#endif
        static unsigned long nw = 0; ++nw;
        if (std::getenv("M68K_PALLOG") && (a - 0xC0000) >= 0x400 && (nw % 4096) == 1) std::fprintf(stderr, "palette write idx=%03x val=%02x (write #%lu)\n", (a - 0xC0000) >> 1, v, nw);
    }
    else if (a >= 0x190000 && a < 0x191000) { uint32_t k = (a - 0x190000) >> 1; if (a & 1) *(volatile uint8_t*)&zsh[k] = v; else zeven[k] = v; }
    else if (a >= 0x40000) hw_wr(a, v);
}
static bool in_vdp(uint32_t a) { a &= 0xFFFFFF; return a >= 0x140000 && a < 0x140010; }
uint16_t rd16(uint32_t a) {
    if (in_vdp(a)) {
        uint32_t o = a & 0xE;
        if (o == 4 || o == 6) return vram[vaddr++ & 0x7FFF];
        if (o == 0xC) return vreg[vidx & 0xFF];
        if (o == 0) return (uint16_t)vaddr;
        return 0;                              // +8 index, +A, +E: write-only / unused
    }
    return (uint16_t)((rd8(a) << 8) | rd8(a + 1));
}
uint32_t rd32(uint32_t a) { return ((uint32_t)rd16(a) << 16) | rd16(a + 2); }
void wr16(uint32_t a, uint16_t v) {
    if (in_vdp(a)) {
        uint32_t o = a & 0xE;
        if (o == 0) vaddr = v;
        else if (o == 4 || o == 6) {
            const uint32_t ad = vaddr++ & 0x7FFF;
            if (ad < 0x1800) vtag[ad] = (g_set.skip_clear && in_picture_chain()) ? 1 : 0;     // later writes by other code clear the tag again
            vram[ad] = v;
        }
        else if (o == 8) vidx = v;
        else if (o == 0xC) vreg[vidx & 0xFF] = v;
        return;
    }
    wr8(a, v >> 8); wr8(a + 1, (uint8_t)v);
}
void wr32(uint32_t a, uint32_t v) { wr16(a, v >> 16); wr16(a + 2, (uint16_t)v); }
