#include "opl2.h"
#include <cstdio>
#include "state_io.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace {
const double kPi = 3.14159265358979323846;     // M_PI is not defined by MinGW in strict -std=c++17 mode
uint16_t g_logsin[256];     // -log2(sin) in 1/256 octave units, quarter wave
uint16_t g_exp[256];        // exp table with the implicit leading bit: 1024..2042
float g_slope[64];          // envelope step per chip sample (units of the 9-bit attenuation) for effective rate 0..63
bool g_init = false;
const uint8_t MULT[16] = {1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 30, 30};
const uint8_t KSLROM[16] = {0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64};
const uint8_t KSLSHIFT[4] = {8, 1, 2, 0};

void init_tables() {
    if (g_init) return;
    for (int i = 0; i < 256; ++i) {
        g_logsin[i] = (uint16_t)std::lround(-std::log2(std::sin((i + 0.5) * kPi / 512.0)) * 256.0);
        g_exp[i] = (uint16_t)(std::lround((std::pow(2.0, (255 - i) / 256.0) - 1.0) * 1024.0) + 1024);
    }
    for (int r = 0; r < 64; ++r) {
        int hi = r >> 2, lo = r & 3;
        g_slope[r] = r == 0 ? 0.f : (hi >= 15 ? 4.f : (float)((4 + lo) * std::pow(2.0, hi - 15)));
    }
    g_init = true;
}
inline int slot_of(int off) {          // register offset within a 0x20 group -> operator slot 0..17 (or -1)
    if (off >= 0x16 || (off & 7) >= 6) return -1;
    return off < 8 ? off : (off < 16 ? off - 8 + 6 : off - 16 + 12);
}
inline int slot_ch(int s) { return (s / 6) * 3 + (s % 6) % 3; }
inline int slot_op(int s) { return (s % 6) / 3; }
}

void OPL2::reset() {
    init_tables();
    addr_ = 0; status_ = 0; std::memset(regs_, 0, sizeof regs_);
    for (auto& c : ch_) c = Ch();
    wse_ = nts_ = deep_am_ = deep_vib_ = rhythm_ = false; rhythm_keys_ = 0;
    t1_ = t2_ = 0; t1_run_ = t2_run_ = t1_mask_ = t2_mask_ = false; t1_cnt_ = t2_cnt_ = 0;
    sample_ctr_ = 0; trem_pos_ = 0; noise_ = 1;
}

void OPL2::set_irq_flag(uint8_t flag) {
    status_ |= flag;
    uint8_t mask = (t1_mask_ ? 0x40 : 0) | (t2_mask_ ? 0x20 : 0);
    if ((status_ & 0x60) & ~mask) status_ |= 0x80;
}

void OPL2::key_on(Op& o) { if (!o.key) { o.key = true; o.state = 1; o.phase = 0; } }
void OPL2::key_off(Op& o) { if (o.key) { o.key = false; if (o.state != 0) o.state = 4; } }

void OPL2::update_keys() {
    for (int c = 0; c < 9; ++c) {
        bool k0 = ch_[c].kon, k1 = ch_[c].kon;
        if (rhythm_ && c >= 6) {
            if (c == 6) k0 = k1 = rhythm_keys_ & 0x10;
            else if (c == 7) { k0 = rhythm_keys_ & 0x01; k1 = rhythm_keys_ & 0x08; }    // hi-hat / snare
            else { k0 = rhythm_keys_ & 0x04; k1 = rhythm_keys_ & 0x02; }                // tom / cymbal
        }
        if (k0) key_on(ch_[c].op[0]); else key_off(ch_[c].op[0]);
        if (k1) key_on(ch_[c].op[1]); else key_off(ch_[c].op[1]);
    }
}

void OPL2::write_op(int slot, int reg, uint8_t v) {
    Op& o = ch_[slot_ch(slot)].op[slot_op(slot)];
    switch (reg & 0xE0) {
    case 0x20: o.am = v >> 7; o.vib = (v >> 6) & 1; o.egt = (v >> 5) & 1; o.ksr = (v >> 4) & 1; o.mult = v & 15; break;
    case 0x40: o.ksl = v >> 6; o.tl = v & 63; break;
    case 0x60: o.ar = v >> 4; o.dr = v & 15; break;
    case 0x80: o.sl = v >> 4; o.rr = v & 15; break;
    case 0xE0: o.wf = v & 3; break;
    }
}

void OPL2::write_data(uint8_t v) {
    int reg = addr_;
    regs_[reg] = v;
    if (reg < 0x20) {
        switch (reg) {
        case 0x01: wse_ = v & 0x20; break;
        case 0x02: t1_ = v; break;
        case 0x03: t2_ = v; break;
        case 0x04:
            if (v & 0x80) { status_ &= 0x1F; }                       // reset the timer flags / IRQ
            else {
                t1_mask_ = v & 0x40; t2_mask_ = v & 0x20;
                if ((v & 1) && !t1_run_) t1_cnt_ = 0;
                if ((v & 2) && !t2_run_) t2_cnt_ = 0;
                t1_run_ = v & 1; t2_run_ = v & 2;
            }
            break;
        case 0x08: nts_ = v & 0x40; break;
        }
        return;
    }
    if (reg == 0xBD) {
        deep_am_ = v & 0x80; deep_vib_ = v & 0x40; rhythm_ = v & 0x20; rhythm_keys_ = v & 0x1F;
        update_keys();
        return;
    }
    if (reg >= 0xA0 && reg <= 0xA8) { Ch& c = ch_[reg - 0xA0]; c.fnum = (c.fnum & 0x300) | v; return; }
    if (reg >= 0xB0 && reg <= 0xB8) {
        Ch& c = ch_[reg - 0xB0];
        c.fnum = (c.fnum & 0xFF) | ((v & 3) << 8); c.block = (v >> 2) & 7; c.kon = v & 0x20;
        update_keys();
        return;
    }
    if (reg >= 0xC0 && reg <= 0xC8) { Ch& c = ch_[reg - 0xC0]; c.fb = (v >> 1) & 7; c.conn = v & 1; return; }
    if (reg >= 0x20 && reg <= 0xF5) {
        int slot = slot_of(reg & 0x1F);
        if (slot >= 0) write_op(slot, reg, v);
    }
}

int OPL2::keycode(const Ch& c) const { return (c.block << 1) | ((c.fnum >> (nts_ ? 8 : 9)) & 1); }

int OPL2::ksl_att(const Ch& c, const Op& o) const {
    int k = (KSLROM[c.fnum >> 6] << 2) - ((8 - c.block) << 5);
    if (k < 0) k = 0;
    return k >> KSLSHIFT[o.ksl];
}

void OPL2::update_env(Ch& c, Op& o) {
    if (o.state == 0) { o.env = 511.f; return; }
    int kc = keycode(c);
    int ks = o.ksr ? kc : (kc >> 2);
    auto rate = [&](int r4) { if (!r4) return 0; int r = r4 * 4 + ks; return r > 63 ? 63 : r; };
    int sl_env = (o.sl == 15) ? 496 : o.sl * 32;
    switch (o.state) {
    case 1: {                                                         // attack: exponential approach to 0
        int r = rate(o.ar);
        if (r >= 60) o.env = 0.f;
        else if (r > 0) o.env -= (o.env + 1.f) * g_slope[r] * 0.125f;
        if (o.env < 0.5f) { o.env = 0.f; o.state = 2; }
        break; }
    case 2: {                                                         // decay to the sustain level
        o.env += g_slope[rate(o.dr)];
        if (o.env >= (float)sl_env) { o.env = (float)sl_env; o.state = 3; }
        break; }
    case 3:                                                           // sustain: held (EGT=1) or decaying at the release rate (EGT=0)
        if (!o.egt) { o.env += g_slope[rate(o.rr)]; if (o.env >= 511.f) { o.env = 511.f; o.state = 0; } }
        break;
    default:                                                          // release
        o.env += g_slope[rate(o.rr)];
        if (o.env >= 511.f) { o.env = 511.f; o.state = 0; }
        break;
    }
    if (o.env > 511.f) o.env = 511.f;
}

uint32_t OPL2::phase_inc(const Ch& c, const Op& o, int vib_delta) const {
    int fn = c.fnum + (o.vib ? vib_delta : 0);
    if (fn < 0) fn = 0;
    return (uint32_t)(((uint32_t)fn << c.block) * MULT[o.mult]) >> 1;
}

int OPL2::gen(const Ch& c, Op& o, uint32_t ph, int mod, int tremolo) {
    ph = (ph + mod) & 0x3FF;
    int wf = wse_ ? o.wf : 0;
    int idx; bool neg = false;
    switch (wf) {
    case 0: neg = ph & 0x200; { int x = ph & 0x1FF; idx = (x & 0x100) ? (~x & 0xFF) : (x & 0xFF); } break;
    case 1: if (ph & 0x200) return 0; { int x = ph & 0x1FF; idx = (x & 0x100) ? (~x & 0xFF) : (x & 0xFF); } break;
    case 2: { int x = ph & 0x1FF; idx = (x & 0x100) ? (~x & 0xFF) : (x & 0xFF); } break;
    default: if (ph & 0x100) return 0; idx = ph & 0xFF; break;
    }
    int total = (int)o.env + (o.tl << 2) + ksl_att(c, o) + (o.am ? tremolo : 0);
    int level = g_logsin[idx] + (total << 3);
    if (level >= 0x2000) return 0;
    int v = (g_exp[level & 0xFF] << 1) >> (level >> 8);
    return neg ? -v : v;
}

int OPL2::clock() {
    ++sample_ctr_;
    if (!(sample_ctr_ & 63)) trem_pos_ = (trem_pos_ + 1) % 210;
    int tv = trem_pos_ < 105 ? trem_pos_ : 210 - trem_pos_;
    int tremolo = deep_am_ ? (tv >> 2) : (tv >> 4);
    int vibpos = (sample_ctr_ >> 10) & 7;
    noise_ = (noise_ >> 1) | ((((noise_ >> 14) ^ noise_) & 1) << 22);
    int noise_bit = noise_ & 1;

    // timers
    if (t1_run_ && ++t1_cnt_ >= (256 - t1_) * 4) { t1_cnt_ = 0; set_irq_flag(0x40); }
    if (t2_run_ && ++t2_cnt_ >= (256 - t2_) * 16) { t2_cnt_ = 0; set_irq_flag(0x20); }

    int mix = 0;
    uint32_t ph10[9][2];
    int vibd[9];
    for (int i = 0; i < 9; ++i) {
        Ch& c = ch_[i];
        int range = (c.fnum >> 7) & 7;
        if (!(vibpos & 3)) range = 0; else if (vibpos & 1) range >>= 1;
        range >>= deep_vib_ ? 0 : 1;
        vibd[i] = (vibpos & 4) ? -range : range;
        for (int k = 0; k < 2; ++k) { update_env(c, c.op[k]); ph10[i][k] = c.op[k].phase >> 10; }
    }
    for (int i = 0; i < 9; ++i) {
        Ch& c = ch_[i];
        Op& m = c.op[0]; Op& r = c.op[1];
        if (rhythm_ && i >= 6) {
            if (i == 6) {                                              // bass drum: an ordinary two-operator voice
                int fbmod = c.fb ? (m.out1 + m.out2) >> (9 - c.fb) : 0;
                m.out = gen(c, m, ph10[i][0], fbmod, tremolo);
                m.out2 = m.out1; m.out1 = m.out;
                int o2 = gen(c, r, ph10[i][1], c.conn ? 0 : (m.out >> 1), tremolo);
                mix += c.conn ? m.out + o2 : o2;
            } else if (i == 7) {                                       // hi-hat (op0) + snare (op1)
                uint32_t hh = ph10[7][0], tc = ph10[8][1];
                int xorbit = (((hh >> 2) ^ (hh >> 7)) | ((hh >> 3) ^ (tc >> 5)) | ((tc >> 3) ^ (tc >> 5))) & 1;
                uint32_t hp = (xorbit << 9) | ((xorbit ^ noise_bit) ? 0xD0 : 0x34);
                uint32_t sp = (((hh >> 8) & 1) << 9) | ((((hh >> 8) & 1) ^ noise_bit) << 8);
                int a = gen(c, m, hp, 0, tremolo), b = gen(c, r, sp, 0, tremolo);
                mix += (a + b) * 2;
            } else {                                                   // tom (op0) + top cymbal (op1)
                uint32_t hh = ph10[7][0], tc = ph10[8][1];
                int xorbit = (((hh >> 2) ^ (hh >> 7)) | ((hh >> 3) ^ (tc >> 5)) | ((tc >> 3) ^ (tc >> 5))) & 1;
                uint32_t cp = (xorbit << 9) | 0x80;
                int a = gen(c, m, ph10[8][0], 0, tremolo), b = gen(c, r, cp, 0, tremolo);
                mix += (a + b) * 2;
            }
        } else {
            int fbmod = c.fb ? (m.out1 + m.out2) >> (9 - c.fb) : 0;
            m.out = gen(c, m, ph10[i][0], fbmod, tremolo);
            m.out2 = m.out1; m.out1 = m.out;
            int o2 = gen(c, r, ph10[i][1], c.conn ? 0 : (m.out >> 1), tremolo);
            mix += c.conn ? m.out + o2 : o2;
        }
    }
    for (int i = 0; i < 9; ++i) for (int k = 0; k < 2; ++k) {          // advance the phase generators
        Ch& c = ch_[i]; Op& o = c.op[k];
        o.phase = (o.phase + phase_inc(c, o, vibd[i])) & 0xFFFFF;
    }
    return mix;
}

void OPL2::state_io(StateIO& io) {
    io.section("opl2");
    io.val("addr", addr_); io.val("status", status_); io.bytes("regs", regs_, sizeof regs_);
    for (int n = 0; n < 9; ++n) {
        Ch& c = ch_[n]; char nm[32];
        std::snprintf(nm, sizeof nm, "ch%d.fnum", n);  io.val(nm, c.fnum);
        std::snprintf(nm, sizeof nm, "ch%d.block", n); io.val(nm, c.block);
        std::snprintf(nm, sizeof nm, "ch%d.kon", n);   io.val(nm, c.kon);
        std::snprintf(nm, sizeof nm, "ch%d.fb", n);    io.val(nm, c.fb);
        std::snprintf(nm, sizeof nm, "ch%d.conn", n);  io.val(nm, c.conn);
        for (int k = 0; k < 2; ++k) {
            Op& o = c.op[k];
            struct F8 { const char* n; uint8_t* p; } f8[] = { {"am", &o.am}, {"vib", &o.vib}, {"egt", &o.egt}, {"ksr", &o.ksr}, {"mult", &o.mult}, {"ksl", &o.ksl},
                {"tl", &o.tl}, {"ar", &o.ar}, {"dr", &o.dr}, {"sl", &o.sl}, {"rr", &o.rr}, {"wf", &o.wf} };
            for (auto& f : f8) { std::snprintf(nm, sizeof nm, "ch%d.op%d.%s", n, k, f.n); io.val(nm, *f.p); }
            std::snprintf(nm, sizeof nm, "ch%d.op%d.phase", n, k); io.val(nm, o.phase);
            std::snprintf(nm, sizeof nm, "ch%d.op%d.env", n, k);   io.val(nm, o.env);
            std::snprintf(nm, sizeof nm, "ch%d.op%d.state", n, k); io.val(nm, o.state);
            std::snprintf(nm, sizeof nm, "ch%d.op%d.key", n, k);   io.val(nm, o.key);
            std::snprintf(nm, sizeof nm, "ch%d.op%d.out", n, k);   io.val(nm, o.out);
            std::snprintf(nm, sizeof nm, "ch%d.op%d.out1", n, k);  io.val(nm, o.out1);
            std::snprintf(nm, sizeof nm, "ch%d.op%d.out2", n, k);  io.val(nm, o.out2);
            if (io.applying() && !io.saving() && (o.state < 0 || o.state > 4)) o.state = 0;
        }
    }
    io.val("wse", wse_); io.val("nts", nts_); io.val("deep_am", deep_am_); io.val("deep_vib", deep_vib_); io.val("rhythm", rhythm_); io.val("rhythm_keys", rhythm_keys_);
    io.val("t1", t1_); io.val("t2", t2_); io.val("t1_run", t1_run_); io.val("t2_run", t2_run_); io.val("t1_mask", t1_mask_); io.val("t2_mask", t2_mask_);
    io.val("t1_cnt", t1_cnt_); io.val("t2_cnt", t2_cnt_);
    io.val("sample_ctr", sample_ctr_); io.val("trem_pos", trem_pos_); io.val("noise", noise_);
}
