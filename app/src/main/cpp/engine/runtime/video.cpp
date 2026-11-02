#include "video.h"
#include "hw.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>

VideoOpts video_opts;
const char* video_opts_text() {
    static char buf[320];
    std::snprintf(buf, sizeof buf, "[H hold:%d F2:%d F3:%d F4:%d F5:%d F6:%d U flipR:%d O prio-sort:%d T tile-prio:%d | show bg%d fg%d spr%d top%d | spr offset %d,%d | F12 overlay:%d]",
                  video_opts.hold_blank, video_opts.sprites_over_top, video_opts.swap_sprite_flip, video_opts.col_major_sprites,
                  video_opts.swap_tile_flip, video_opts.reverse_sprite_order, video_opts.flipx_right_anchor, video_opts.sprite_priority_sort, video_opts.tile_priority, video_opts.show_bg, video_opts.show_fg,
                  video_opts.show_spr, video_opts.show_top, video_opts.spr_dx, video_opts.spr_dy, video_opts.overlay);
    return buf;
}
static std::vector<uint8_t> gA, gB;                  // tp025-4 (planes 2,3) and tp025-3 (planes 0,1)
static std::vector<uint8_t> cache16, cache8;         // lazily decoded tiles (1 byte/pixel); 0xFF in [0] = not decoded
static std::vector<uint8_t> have16, have8;

static bool slurp(const std::string& p, std::vector<uint8_t>& v) {
    FILE* f = std::fopen(p.c_str(), "rb"); if (!f) return false;
    v.resize(0x100000); size_t n = std::fread(v.data(), 1, v.size(), f); std::fclose(f); return n == v.size();
}
bool video_init(const char* dir) {
    std::string d = dir;
    if (!slurp(d + "/tp025-4.bin", gA) || !slurp(d + "/tp025-3.bin", gB)) return false;
    cache16.assign(16384 * 256, 0); have16.assign(16384, 0);
    cache8.assign(65536 * 64, 0);   have8.assign(65536, 0);
    return true;
}

// 8x8 block: 16 bytes per ROM, row r = (byte0, byte1); pen = p0<<3|p1<<2|p2<<1|p3
static const uint8_t* tile8(uint32_t code) {
    code &= 0xFFFF;
    uint8_t* o = &cache8[code * 64];
    if (!have8[code]) {
        const uint8_t* h = &gB[code * 16]; const uint8_t* l = &gA[code * 16];
        for (int r = 0; r < 8; ++r) for (int x = 0; x < 8; ++x) {
            int bit = 7 - x;
            o[r * 8 + x] = (uint8_t)((((h[r * 2 + 1] >> bit) & 1) << 3) | (((h[r * 2] >> bit) & 1) << 2) |
                                     (((l[r * 2 + 1] >> bit) & 1) << 1) | ((l[r * 2] >> bit) & 1));
        }
        have8[code] = 1;
    }
    return o;
}
// 16x16 tile = four consecutive 8x8 blocks: TL, TR, BL, BR
static const uint8_t* tile16(uint32_t code) {
    code &= 0x3FFF;
    uint8_t* o = &cache16[code * 256];
    if (!have16[code]) {
        for (int b = 0; b < 4; ++b) {
            const uint8_t* t = tile8(code * 4 + b);
            int bx = (b & 1) * 8, by = (b >> 1) * 8;
            for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) o[(by + y) * 16 + bx + x] = t[y * 8 + x];
        }
        have16[code] = 1;
    }
    return o;
}

const uint8_t* video_tile16(uint32_t code) { return tile16(code); }

static uint32_t rgb_of(const uint8_t* pal, int idx) {
    uint32_t c = (pal[idx * 2] << 8) | pal[idx * 2 + 1];
    uint32_t r = (c & 31) * 255 / 31, g = ((c >> 5) & 31) * 255 / 31, b = ((c >> 10) & 31) * 255 / 31;
    return (r << 16) | (g << 8) | b;
}

// "no scroll" register values (observed) -> effective scroll = reg - base
struct LayerBase { int x, y; };
static const LayerBase BG{0x1d6, 0x1ef}, FG{0x1d8, 0x1ef}, TOP{0x1da, 0x1ef}, SPR{0x1d4, 0x1f7};

static void draw_layer(uint32_t* out, const uint16_t* vram, const uint8_t* pal, int base, int sx, int sy, int level = -1, const uint8_t* hide = nullptr) {
    for (int y = 0; y < VIDEO_H; ++y) {
        int ly = (y + sy) & 511, ty = ly >> 4, py = ly & 15;
        for (int x = 0; x < VIDEO_W; ++x) {
            int lx = (x + sx) & 511, tx = lx >> 4, px = lx & 15;
            int i = base + (ty * 32 + tx) * 2;
            uint16_t attr = vram[i], code = vram[i + 1];
            if (!(attr | code)) continue;
            if (level >= 0 && ((attr >> 9) & 7) != level) continue;     // tile priority level (bits 9-11)
            // skip_level_clear_scenes: tiles written by the picture routines (tagged in bus.cpp), and the 16 tiles 028E-029D (the woman's
            // portrait on the flipped cards of the card-flip scene wipe, a top-layer tile set used for nothing else), are drawn solid black
            if (hide && (hide[i] || ((code & 0x3FFF) >= 0x028E && (code & 0x3FFF) <= 0x029D))) { out[y * VIDEO_W + x] = 0; continue; }
            // same option: the collected-coin icons of the HUD (top layer, tiles 1568-156B: the routines at 009B7A / 009BBE write the coin
            // counter $08066A as that many tiles from top-layer cell $1050 on) are not drawn at all (transparent, the HUD bar shows through)
            if (hide && base == 0x1000 && (code & 0x3FFF) >= 0x1568 && (code & 0x3FFF) <= 0x156B) continue;
            int fx = px, fy = py;
            bool bfy = attr & (video_opts.swap_tile_flip ? 0x8000 : 0x4000), bfx = attr & (video_opts.swap_tile_flip ? 0x4000 : 0x8000);
            if (bfy) fy = 15 - py;
            if (bfx) fx = 15 - px;
            uint8_t pen = tile16(code)[fy * 16 + fx];
            if (!pen) continue;
            out[y * VIDEO_W + x] = rgb_of(pal, (attr & 0x7F) * 16 + pen);
        }
    }
}

static void draw_sprites(uint32_t* out, const uint16_t* vram, const uint8_t* pal, int sx0, int sy0, int only_prio = -1) {
    // draw order: higher priority value on top; within one priority, higher slot on top (F6 reverses the slot order)
    int order[256], n = 0;
    for (int pr = 0; pr < (video_opts.sprite_priority_sort ? 4 : 1); ++pr)
        for (int si = 0; si < 256; ++si) {
            int s = video_opts.reverse_sprite_order ? 255 - si : si;
            if (video_opts.sprite_priority_sort && ((vram[0x1800 + s * 4] >> 10) & 3) != pr) continue;
            if (only_prio >= 0 && ((vram[0x1800 + s * 4] >> 10) & 3) != only_prio) continue;
            order[n++] = s;
        }
    for (int oi = 0; oi < n; ++oi) {
        int s = order[oi];
        const uint16_t* q = &vram[0x1800 + s * 4];
        uint16_t a = q[0], c = q[1], xw = q[2], yw = q[3];
        if (!(a & 0x8000)) continue;
        int color = (a >> 2) & 0x3F;
        uint32_t tile = ((a & 3) << 16) | c;
        int w = (xw & 15) + 1, h = (yw & 15) + 1;
        int x = (xw >> 7) + sx0 + video_opts.spr_dx, y = (yw >> 7) + sy0 + video_opts.spr_dy;
        bool fx = a & (video_opts.swap_sprite_flip ? 0x2000 : 0x1000), fy = a & (video_opts.swap_sprite_flip ? 0x1000 : 0x2000);
        if (fx && video_opts.flipx_right_anchor) x -= w * 8 - 1;   // flipped sprite: X is its right edge (inclusive)
        for (int ty = 0; ty < h; ++ty) for (int tx = 0; tx < w; ++tx) {
            const uint8_t* t = tile8(tile + (video_opts.col_major_sprites ? tx * h + ty : ty * w + tx));
            int px = x + (fx ? (w - 1 - tx) : tx) * 8, py = y + (fy ? (h - 1 - ty) : ty) * 8;
            for (int ddx = 0; ddx >= -512; ddx -= 512) for (int ddy = 0; ddy >= -512; ddy -= 512) {
                int X = px + ddx, Y = py + ddy;
                if (X >= VIDEO_W || Y >= VIDEO_H || X + 8 <= 0 || Y + 8 <= 0) continue;
                for (int j = 0; j < 8; ++j) for (int i = 0; i < 8; ++i) {
                    int xx = X + i, yy = Y + j;
                    if (xx < 0 || yy < 0 || xx >= VIDEO_W || yy >= VIDEO_H) continue;
                    uint8_t pen = t[(fy ? 7 - j : j) * 8 + (fx ? 7 - i : i)];
                    if (pen) out[yy * VIDEO_W + xx] = rgb_of(pal, color * 16 + pen);
                }
            }
        }
    }
}

// ---- debug overlay: game-object logical positions vs. the sprite as drawn --------------------------------------
// Object records (64 bytes each in work RAM; found by their sprite-slot pointer at +62, validated against the sprite list):
//   +2 attr  +4 tile  +6 Y (<<7, measured UP from the screen bottom)  +8 X (<<7)  +10 y offset  +12 x offset | size
// The game's own sprite writer makes screen_x = X + xoff, screen_top = 240 - Y - yoff, so (X, 240-Y) is the logical position.
static void put(uint32_t* o, int x, int y, uint32_t c) { if (x >= 0 && x < VIDEO_W && y >= 0 && y < VIDEO_H) o[y * VIDEO_W + x] = c; }
static int s16(int v) { return ((v + 256) & 511) - 256; }
static void draw_overlay(uint32_t* out, const uint16_t* vram, int sx0, int sy0) {
    const uint8_t* ram = hw_ram();
    auto be = [&](int o) { return (ram[o] << 8) | ram[o + 1]; };
    for (int p = 0; p + 64 <= 0x4000; p += 2) {
        int slot = be(p + 62);
        if (slot < 0x1800 || slot >= 0x1c00 || (slot & 3)) continue;
        int attr = be(p + 2), tile = be(p + 4);
        if (!(attr & 0x8000)) continue;
        const uint16_t* s = &vram[slot];
        if (s[0] != attr || s[1] != tile) continue;
        int X = s16(be(p + 8) >> 7), Y = be(p + 6) >> 7, cx = X, cy = s16(240 - Y);
        for (int k = -4; k <= 4; ++k) { put(out, cx + k, cy, 0xFFFF00); put(out, cx, cy + k, 0xFFFF00); }     // yellow cross = logical position
        int w = (s[2] & 15) + 1, h = (s[3] & 15) + 1;
        int x0 = (s[2] >> 7) + sx0 + video_opts.spr_dx, y0 = s16((s[3] >> 7) + sy0 + video_opts.spr_dy);
        if ((attr & (video_opts.swap_sprite_flip ? 0x2000 : 0x1000)) && video_opts.flipx_right_anchor) x0 -= w * 8 - 1;
        x0 = s16(x0);
        for (int i = 0; i < w * 8; ++i) { put(out, x0 + i, y0, 0x00FFFF); put(out, x0 + i, y0 + h * 8 - 1, 0x00FFFF); }   // cyan = sprite box
        for (int j = 0; j < h * 8; ++j) { put(out, x0, y0 + j, 0x00FFFF); put(out, x0 + w * 8 - 1, y0 + j, 0x00FFFF); }
    }
}

static uint32_t last_frame[VIDEO_W * VIDEO_H];
static bool have_last = false;
static std::chrono::steady_clock::time_point blank_since;
static bool in_blank = false;

static void video_render_raw(uint32_t* out) {
    const uint16_t* vram = hw_vram(); const uint16_t* vr = hw_vreg(); const uint8_t* pal = hw_pal();
    const uint8_t* hide = hw_settings_hide_pictures() ? hw_vram_tags() : nullptr;
    // the game presents one frame with both background layers empty at scene changes (only the HUD stays)
    int bgfg = 0, vis = 0;
    for (int i = 0; i < 0x1000; i += 2) bgfg += (vram[i] | vram[i + 1]) != 0;
    for (int s = 0; s < 256; ++s) vis += (vram[0x1800 + s * 4] >> 15) & 1;
    const bool blank = (bgfg == 0 && vis == 0);
    auto now = std::chrono::steady_clock::now();
    if (blank && !in_blank) { in_blank = true; blank_since = now; }
    if (!blank) in_blank = false;
    if (blank && video_opts.hold_blank && have_last && now - blank_since < std::chrono::milliseconds(100)) {
        std::memcpy(out, last_frame, sizeof last_frame); return;       // re-show the last NON-blank frame (max 100 ms)
    }
    std::memset(out, 0, sizeof(uint32_t) * VIDEO_W * VIDEO_H);
    auto sc = [&](const LayerBase& b, int ix, int& sx, int& sy) { sx = (vr[ix] - b.x) & 511; sy = (vr[ix + 1] - b.y) & 511; };
    int sx, sy, px, py;
    int ovx = 0, ovy = 0;
    int bgx, bgy, fgx, fgy, tpx, tpy;
    sc(BG, 0, bgx, bgy); sc(FG, 2, fgx, fgy); sc(TOP, 4, tpx, tpy); sc(SPR, 6, px, py);
    ovx = px ? (-px) & 511 : 0; ovy = py ? (-py) & 511 : 0;
    auto tiles = [&](int lvl) {                      // all three tile layers, restricted to one priority level (-1 = every level)
        if (video_opts.show_bg)  draw_layer(out, vram, pal, 0x0000, bgx, bgy, lvl, hide);
        if (video_opts.show_fg)  draw_layer(out, vram, pal, 0x0800, fgx, fgy, lvl, hide);
        if (video_opts.show_top) draw_layer(out, vram, pal, 0x1000, tpx, tpy, lvl, hide);
    };
    auto spr = [&](int prio) { if (video_opts.show_spr) draw_sprites(out, vram, pal, ovx, ovy, prio); };
    if (video_opts.tile_priority) {
        // tiles of level L are drawn after the sprites of priority < L and before sprites of priority >= L
        for (int lvl = 0; lvl < 8; ++lvl) {
            tiles(lvl);
            if (!video_opts.sprites_over_top) {
                if (video_opts.sprite_priority_sort) { if (lvl <= 3) spr(lvl); }
                else if (lvl == 0) spr(-1);
            }
        }
        if (video_opts.sprites_over_top) spr(-1);
    } else {                                          // old model: bg, fg, sprites, HUD layer
        if (video_opts.show_bg) draw_layer(out, vram, pal, 0x0000, bgx, bgy, -1, hide);
        if (video_opts.show_fg) draw_layer(out, vram, pal, 0x0800, fgx, fgy, -1, hide);
        if (!video_opts.sprites_over_top) spr(-1);
        if (video_opts.show_top) draw_layer(out, vram, pal, 0x1000, tpx, tpy, -1, hide);
        if (video_opts.sprites_over_top) spr(-1);
    }
    if (!blank) { std::memcpy(last_frame, out, sizeof last_frame); have_last = true; }
    if (video_opts.overlay) draw_overlay(out, vram, ovx, ovy);
}

// flip_screen in pipibibs_settings.txt: the hardware flip is a 180-degree turn of the finished picture, so it is done here on the
// output. (The game's own flip bit is NOT passed to the game: it would make the game switch the video chip to a register bank
// this renderer does not know, which scrambles the layers.)
void video_render(uint32_t* out) {
    video_render_raw(out);
    if (hw_settings_flip()) std::reverse(out, out + VIDEO_W * VIDEO_H);
}
