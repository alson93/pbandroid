// video.h - software renderer for the Toaplan-style video chip (formats inferred; see gfx/ and README).
#pragma once
#include <cstdint>
bool video_init(const char* romdir);             // loads tp025-3.bin / tp025-4.bin (graphics)
void video_render(uint32_t* xrgb /*320*240*/);
const uint8_t* video_tile16(uint32_t code);      // decoded 16x16 tile (256 pen numbers, 0 = transparent) - used by tools/extract_pictures.cpp   // composes the current video state into a 320x240 XRGB frame
// Rendering options for the parts of the video format that were guessed (toggled live in the Windows build).
struct VideoOpts {
    bool hold_blank = true;           // H : keep the previous frame during the game's one-frame blank at scene changes
    bool sprites_over_top = false;    // F2: draw sprites above the HUD layer instead of below it
    bool swap_sprite_flip = false;    // F3: sprite attr bit 12/13 = flip Y/X instead of flip X/Y
    bool col_major_sprites = false;   // F4: tiles inside a multi-tile sprite are numbered column-major
    bool swap_tile_flip = false;      // F5: tile attr bit 14/15 = flip X/Y instead of flip Y/X
    bool reverse_sprite_order = false;// F6: draw sprite 255 first so sprite 0 ends up on top (default: higher slot on top)
    bool tile_priority = true;        // T : tiles with attr bits 9-11 = N hide sprites of lower priority (elevators behind slabs, HUD strip)
    bool sprite_priority_sort = true; // O : draw sprites by priority bits (attr 10-11) first, slot order second
    bool flipx_right_anchor = true;   // U : for X-flipped sprites the X coordinate is the RIGHT edge (measured from the game's own data)
    bool show_bg = true, show_fg = true, show_spr = true, show_top = true;   // F7..F10: hide a layer to isolate artifacts
    bool overlay = false;             // F12: draw each object's logical position (cross) and its sprite outline
    int spr_dx = -8, spr_dy = -8;     // J/L and I/K: nudge sprite position (sprite coords look biased by 8 px vs tiles)
};
extern VideoOpts video_opts;
const char* video_opts_text();        // short description of the non-default options (for the window title)
enum { VIDEO_W = 320, VIDEO_H = 240 };
