#include "z80cpu.h"
#include "state_io.h"
#include <utility>

static uint8_t PAR[256];
static bool par_ready = false;
static void init_par() {
    for (int v = 0; v < 256; ++v) { int n = 0; for (int b = 0; b < 8; ++b) n += (v >> b) & 1; PAR[v] = (n & 1) ? 0 : 0x04; }
    par_ready = true;
}
static inline uint8_t szxy(uint8_t r) { return (r & 0xA8) | (r == 0 ? 0x40 : 0); }   // S, Y, X from result, Z

void Z80::reset() {
    if (!par_ready) init_par();
    a = f = 0xFF; b = c = d = e = h = l = 0xFF; a2 = f2 = b2 = c2 = d2 = e2 = h2 = l2 = 0xFF;
    ix = iy = sp = 0xFFFF; pc = 0; i = r = 0; iff1 = iff2 = false; im = 0; halted = false; ei_delay_ = false; tstates = 0;
}

uint16_t Z80::hlx() const { return prefix_ == 0xDD ? ix : prefix_ == 0xFD ? iy : hl(); }
void Z80::set_hlx(uint16_t v) { if (prefix_ == 0xDD) ix = v; else if (prefix_ == 0xFD) iy = v; else set_hl(v); }

uint8_t Z80::get_r(int idx) {
    switch (idx) {
    case 0: return b; case 1: return c; case 2: return d; case 3: return e;
    case 4: return prefix_ == 0xDD ? ix >> 8 : prefix_ == 0xFD ? iy >> 8 : h;
    case 5: return prefix_ == 0xDD ? (uint8_t)ix : prefix_ == 0xFD ? (uint8_t)iy : l;
    default: return a;
    }
}
void Z80::set_r(int idx, uint8_t v) {
    switch (idx) {
    case 0: b = v; break; case 1: c = v; break; case 2: d = v; break; case 3: e = v; break;
    case 4: if (prefix_ == 0xDD) ix = (ix & 0xFF) | (v << 8); else if (prefix_ == 0xFD) iy = (iy & 0xFF) | (v << 8); else h = v; break;
    case 5: if (prefix_ == 0xDD) ix = (ix & 0xFF00) | v; else if (prefix_ == 0xFD) iy = (iy & 0xFF00) | v; else l = v; break;
    default: a = v; break;
    }
}
// plain register access (no IX/IY substitution) used by instructions with a memory operand and by CB/ED
static inline uint8_t plain_get(const Z80& z, int idx) {
    switch (idx) { case 0: return z.b; case 1: return z.c; case 2: return z.d; case 3: return z.e;
                   case 4: return z.h; case 5: return z.l; default: return z.a; }
}
static inline void plain_set(Z80& z, int idx, uint8_t v) {
    switch (idx) { case 0: z.b = v; break; case 1: z.c = v; break; case 2: z.d = v; break; case 3: z.e = v; break;
                   case 4: z.h = v; break; case 5: z.l = v; break; default: z.a = v; break; }
}

uint16_t Z80::get_rp(int p) { switch (p) { case 0: return bc(); case 1: return de(); case 2: return hlx(); default: return sp; } }
void Z80::set_rp(int p, uint16_t v) { switch (p) { case 0: set_bc(v); break; case 1: set_de(v); break; case 2: set_hlx(v); break; default: sp = v; } }
uint16_t Z80::get_rp2(int p) { switch (p) { case 0: return bc(); case 1: return de(); case 2: return hlx(); default: return (a << 8) | f; } }
void Z80::set_rp2(int p, uint16_t v) { switch (p) { case 0: set_bc(v); break; case 1: set_de(v); break; case 2: set_hlx(v); break; default: a = v >> 8; f = (uint8_t)v; } }

bool Z80::cond(int cc) const {
    switch (cc) {
    case 0: return !(f & FZ); case 1: return f & FZ; case 2: return !(f & FC); case 3: return f & FC;
    case 4: return !(f & FP); case 5: return f & FP; case 6: return !(f & FS); default: return f & FS;
    }
}

uint16_t Z80::mem_addr() {
    if (prefix_) { int8_t dsp = (int8_t)fetch(); cyc_ += 8; return (uint16_t)(hlx() + dsp); }
    return hl();
}

void Z80::alu(int op, uint8_t v) {
    int cf = f & FC;
    switch (op) {
    case 0: case 1: {
        int c0 = (op == 1) ? cf : 0; int r_ = a + v + c0; uint8_t r8 = (uint8_t)r_;
        f = szxy(r8) | (((a ^ v ^ r8) & 0x10) ? FH : 0) | (((a ^ r8) & (v ^ r8) & 0x80) ? FP : 0) | (r_ > 0xFF ? FC : 0);
        a = r8; break; }
    case 2: case 3: case 7: {
        int c0 = (op == 3) ? cf : 0; int r_ = a - v - c0; uint8_t r8 = (uint8_t)r_;
        f = (op == 7 ? ((v & 0x28) | (r8 & 0x80) | (r8 == 0 ? 0x40 : 0)) : szxy(r8)) | FN | (((a ^ v ^ r8) & 0x10) ? FH : 0)
            | (((a ^ v) & (a ^ r8) & 0x80) ? FP : 0) | (r_ & 0x100 ? FC : 0);
        if (op != 7) a = r8;
        break; }
    case 4: a &= v; f = szxy(a) | FH | PAR[a]; break;
    case 5: a ^= v; f = szxy(a) | PAR[a]; break;
    case 6: a |= v; f = szxy(a) | PAR[a]; break;
    }
}
uint8_t Z80::inc8(uint8_t v) { uint8_t r_ = v + 1; f = (f & FC) | szxy(r_) | ((v & 0xF) == 0xF ? FH : 0) | (v == 0x7F ? FP : 0); return r_; }
uint8_t Z80::dec8(uint8_t v) { uint8_t r_ = v - 1; f = (f & FC) | szxy(r_) | FN | ((v & 0xF) == 0 ? FH : 0) | (v == 0x80 ? FP : 0); return r_; }
uint16_t Z80::add16(uint16_t x, uint16_t y) {
    uint32_t r_ = (uint32_t)x + y;
    f = (f & (FS | FZ | FP)) | ((r_ >> 8) & 0x28) | ((((x & 0xFFF) + (y & 0xFFF)) > 0xFFF) ? FH : 0) | (r_ > 0xFFFF ? FC : 0);
    return (uint16_t)r_;
}
void Z80::adc_hl(uint16_t v) {
    uint16_t x = hl(); int c0 = f & FC; uint32_t r_ = (uint32_t)x + v + c0; uint16_t r16 = (uint16_t)r_;
    f = ((r16 >> 8) & 0xA8) | (r16 == 0 ? FZ : 0) | ((((x & 0xFFF) + (v & 0xFFF) + c0) > 0xFFF) ? FH : 0)
        | (((x ^ r16) & (v ^ r16) & 0x8000) ? FP : 0) | (r_ > 0xFFFF ? FC : 0);
    set_hl(r16);
}
void Z80::sbc_hl(uint16_t v) {
    uint16_t x = hl(); int c0 = f & FC; int32_t r_ = (int32_t)x - v - c0; uint16_t r16 = (uint16_t)r_;
    f = ((r16 >> 8) & 0xA8) | (r16 == 0 ? FZ : 0) | FN | (((x & 0xFFF) < ((v & 0xFFF) + c0)) ? FH : 0)
        | (((x ^ v) & (x ^ r16) & 0x8000) ? FP : 0) | (r_ < 0 ? FC : 0);
    set_hl(r16);
}
uint8_t Z80::rot(int op, uint8_t v) {
    uint8_t cout, r_;
    switch (op) {
    case 0: cout = v >> 7; r_ = (v << 1) | cout; break;                 // RLC
    case 1: cout = v & 1; r_ = (v >> 1) | (cout << 7); break;           // RRC
    case 2: cout = v >> 7; r_ = (v << 1) | (f & FC); break;             // RL
    case 3: cout = v & 1; r_ = (v >> 1) | ((f & FC) << 7); break;       // RR
    case 4: cout = v >> 7; r_ = v << 1; break;                          // SLA
    case 5: cout = v & 1; r_ = (v >> 1) | (v & 0x80); break;            // SRA
    case 6: cout = v >> 7; r_ = (v << 1) | 1; break;                    // SLL (undocumented)
    default: cout = v & 1; r_ = v >> 1; break;                          // SRL
    }
    f = szxy(r_) | PAR[r_] | cout;
    return r_;
}
void Z80::daa() {
    uint8_t t = 0, old = a; int cy = f & FC;
    if ((f & FH) || (a & 0xF) > 9) t |= 6;
    if (cy || a > 0x99) { t |= 0x60; cy = 1; }
    if (f & FN) a -= t; else a += t;
    bool hf = (f & FN) ? ((f & FH) && (old & 0xF) < 6) : ((old & 0xF) > 9);
    f = (f & FN) | cy | szxy(a) | PAR[a] | (hf ? FH : 0);
}

void Z80::do_interrupt() {
    halted = false; iff1 = iff2 = false;
    r = (r & 0x80) | ((r + 1) & 0x7F);
    push(pc);
    if (im == 2) { pc = rd16((uint16_t)((i << 8) | 0xFF)); cyc_ = 19; }
    else { pc = 0x38; cyc_ = 13; }
}

void Z80::exec_cb() {
    uint8_t op; uint16_t addr = 0; bool mem;
    if (prefix_) { int8_t dsp = (int8_t)fetch(); addr = (uint16_t)(hlx() + dsp); op = fetch(); mem = true; }
    else { op = fetch(); r = (r & 0x80) | ((r + 1) & 0x7F); mem = ((op & 7) == 6); if (mem) addr = hl(); }
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    uint8_t v = mem ? rd(addr) : plain_get(*this, z);
    if (x == 1) {
        bool zf = !(v & (1 << y));
        f = (f & FC) | FH | (zf ? (FZ | FP) : 0) | ((y == 7 && !zf) ? FS : 0) | ((mem ? (addr >> 8) : v) & 0x28);
        cyc_ += prefix_ ? 16 : (mem ? 12 : 8);
        return;
    }
    if (x == 0) v = rot(y, v);
    else if (x == 2) v &= ~(1 << y);
    else v |= (1 << y);
    if (mem) wr(addr, v);
    if (!prefix_ && !mem) plain_set(*this, z, v);
    else if (prefix_ && z != 6) plain_set(*this, z, v);          // undocumented: DDCB/FDCB also copy the result to a register
    cyc_ += prefix_ ? 19 : (mem ? 15 : 8);
}

void Z80::exec_ed() {
    uint8_t op = fetch(); r = (r & 0x80) | ((r + 1) & 0x7F);
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    if (x == 1) {
        switch (z) {
        case 0: { uint8_t v = bus->in(bc()); if (y != 6) plain_set(*this, y, v); f = (f & FC) | szxy(v) | PAR[v]; cyc_ += 12; break; }
        case 1: bus->out(bc(), y == 6 ? 0 : plain_get(*this, y)); cyc_ += 12; break;
        case 2: if (q == 0) sbc_hl(get_rp(p)); else adc_hl(get_rp(p)); cyc_ += 15; break;
        case 3: { uint16_t nn = fetch16(); if (q == 0) wr16(nn, get_rp(p)); else set_rp(p, rd16(nn)); cyc_ += 20; break; }
        case 4: { uint8_t t = a; a = 0; alu(2, t); cyc_ += 8; break; }
        case 5: pc = pop(); iff1 = iff2; cyc_ += 14; break;
        case 6: { static const int ims[8] = {0, 0, 1, 2, 0, 0, 1, 2}; im = ims[y]; cyc_ += 8; break; }
        default:
            switch (y) {
            case 0: i = a; cyc_ += 9; break;
            case 1: r = a; cyc_ += 9; break;
            case 2: a = i; f = (f & FC) | szxy(a) | (iff2 ? FP : 0); cyc_ += 9; break;
            case 3: a = r; f = (f & FC) | szxy(a) | (iff2 ? FP : 0); cyc_ += 9; break;
            case 4: { uint8_t t = rd(hl()); wr(hl(), (uint8_t)((a << 4) | (t >> 4))); a = (a & 0xF0) | (t & 0x0F); f = (f & FC) | szxy(a) | PAR[a]; cyc_ += 18; break; }   // RRD
            case 5: { uint8_t t = rd(hl()); wr(hl(), (uint8_t)((t << 4) | (a & 0x0F))); a = (a & 0xF0) | (t >> 4); f = (f & FC) | szxy(a) | PAR[a]; cyc_ += 18; break; }   // RLD
            default: cyc_ += 8; break;
            }
        }
        return;
    }
    if (x == 2 && z <= 3 && y >= 4) {
        bool dec = (y & 1), rep = (y >= 6);
        switch (z) {
        case 0: {                                        // LDI / LDD / LDIR / LDDR
            uint8_t v = rd(hl()); wr(de(), v);
            set_hl(hl() + (dec ? -1 : 1)); set_de(de() + (dec ? -1 : 1)); set_bc(bc() - 1);
            uint8_t n = v + a;
            f = (f & (FS | FZ | FC)) | ((n & 2) ? FY : 0) | (n & FX) | (bc() ? FP : 0);
            cyc_ += 16;
            if (rep && bc()) { pc -= 2; cyc_ += 5; }
            break; }
        case 1: {                                        // CPI / CPD / CPIR / CPDR
            uint8_t v = rd(hl()); uint8_t res = a - v; bool hf = ((a ^ v ^ res) & 0x10) != 0;
            set_hl(hl() + (dec ? -1 : 1)); set_bc(bc() - 1);
            uint8_t n = res - (hf ? 1 : 0);
            f = (f & FC) | (res & FS) | (res == 0 ? FZ : 0) | (hf ? FH : 0) | ((n & 2) ? FY : 0) | (n & FX) | (bc() ? FP : 0) | FN;
            cyc_ += 16;
            if (rep && bc() && res != 0) { pc -= 2; cyc_ += 5; }
            break; }
        case 2: {                                        // INI / IND / INIR / INDR
            uint8_t v = bus->in(bc()); wr(hl(), v); b--; set_hl(hl() + (dec ? -1 : 1));
            f = (f & FC) | szxy(b) | FN; cyc_ += 16;
            if (rep && b) { pc -= 2; cyc_ += 5; }
            break; }
        default: {                                       // OUTI / OUTD / OTIR / OTDR
            uint8_t v = rd(hl()); b--; bus->out(bc(), v); set_hl(hl() + (dec ? -1 : 1));
            f = (f & FC) | szxy(b) | FN; cyc_ += 16;
            if (rep && b) { pc -= 2; cyc_ += 5; }
            break; }
        }
        return;
    }
    cyc_ += 8;   // undefined ED opcodes behave as NOP
}

void Z80::exec_main(uint8_t op) {
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    switch (x) {
    case 0:
        switch (z) {
        case 0:
            if (y == 0) cyc_ += 4;
            else if (y == 1) { std::swap(a, a2); std::swap(f, f2); cyc_ += 4; }
            else if (y == 2) { int8_t dsp = (int8_t)fetch(); if (--b) { pc += dsp; cyc_ += 13; } else cyc_ += 8; }
            else if (y == 3) { int8_t dsp = (int8_t)fetch(); pc += dsp; cyc_ += 12; }
            else { int8_t dsp = (int8_t)fetch(); if (cond(y - 4)) { pc += dsp; cyc_ += 12; } else cyc_ += 7; }
            break;
        case 1:
            if (q == 0) { set_rp(p, fetch16()); cyc_ += 10; }
            else { set_hlx(add16(hlx(), get_rp(p))); cyc_ += 11; }
            break;
        case 2:
            if (q == 0) {
                if (p == 0) wr(bc(), a); else if (p == 1) wr(de(), a);
                else if (p == 2) { wr16(fetch16(), hlx()); cyc_ += 3; } else wr(fetch16(), a);
                cyc_ += (p >= 2) ? 13 : 7;
            } else {
                if (p == 0) a = rd(bc()); else if (p == 1) a = rd(de());
                else if (p == 2) { set_hlx(rd16(fetch16())); cyc_ += 3; } else a = rd(fetch16());
                cyc_ += (p >= 2) ? 13 : 7;
            }
            break;
        case 3: set_rp(p, (uint16_t)(get_rp(p) + (q == 0 ? 1 : -1))); cyc_ += 6; break;
        case 4: case 5:
            if (y == 6) { uint16_t ad = mem_addr(); uint8_t v = rd(ad); wr(ad, z == 4 ? inc8(v) : dec8(v)); cyc_ += 11; }
            else { uint8_t v = get_r(y); set_r(y, z == 4 ? inc8(v) : dec8(v)); cyc_ += 4; }
            break;
        case 6:
            if (y == 6) { uint16_t ad = mem_addr(); wr(ad, fetch()); cyc_ += 10; }
            else { set_r(y, fetch()); cyc_ += 7; }
            break;
        default:
            switch (y) {
            case 0: { uint8_t cy = a >> 7; a = (a << 1) | cy; f = (f & (FS | FZ | FP)) | (a & 0x28) | cy; break; }
            case 1: { uint8_t cy = a & 1; a = (a >> 1) | (cy << 7); f = (f & (FS | FZ | FP)) | (a & 0x28) | cy; break; }
            case 2: { uint8_t cy = a >> 7; a = (a << 1) | (f & FC); f = (f & (FS | FZ | FP)) | (a & 0x28) | cy; break; }
            case 3: { uint8_t cy = a & 1; a = (a >> 1) | ((f & FC) << 7); f = (f & (FS | FZ | FP)) | (a & 0x28) | cy; break; }
            case 4: daa(); break;
            case 5: a = ~a; f = (f & (FS | FZ | FP | FC)) | FH | FN | (a & 0x28); break;
            case 6: f = (f & (FS | FZ | FP)) | (a & 0x28) | FC; break;
            default: f = (f & (FS | FZ | FP)) | (a & 0x28) | ((f & FC) ? FH : 0) | ((f & FC) ? 0 : FC); break;
            }
            cyc_ += 4;
        }
        break;
    case 1:
        if (y == 6 && z == 6) { halted = true; cyc_ += 4; }
        else if (y == 6) { uint16_t ad = mem_addr(); wr(ad, plain_get(*this, z)); cyc_ += 7; }
        else if (z == 6) { uint16_t ad = mem_addr(); plain_set(*this, y, rd(ad)); cyc_ += 7; }
        else { set_r(y, get_r(z)); cyc_ += 4; }
        break;
    case 2:
        if (z == 6) { uint16_t ad = mem_addr(); alu(y, rd(ad)); cyc_ += 7; }
        else { alu(y, get_r(z)); cyc_ += 4; }
        break;
    default:
        switch (z) {
        case 0: if (cond(y)) { pc = pop(); cyc_ += 11; } else cyc_ += 5; break;
        case 1:
            if (q == 0) { set_rp2(p, pop()); cyc_ += 10; }
            else if (p == 0) { pc = pop(); cyc_ += 10; }
            else if (p == 1) { std::swap(b, b2); std::swap(c, c2); std::swap(d, d2); std::swap(e, e2); std::swap(h, h2); std::swap(l, l2); cyc_ += 4; }
            else if (p == 2) { pc = hlx(); cyc_ += 4; }
            else { sp = hlx(); cyc_ += 6; }
            break;
        case 2: { uint16_t nn = fetch16(); if (cond(y)) pc = nn; cyc_ += 10; break; }
        case 3:
            switch (y) {
            case 0: pc = fetch16(); cyc_ += 10; break;
            case 1: exec_cb(); break;
            case 2: { uint8_t n = fetch(); bus->out((uint16_t)((a << 8) | n), a); cyc_ += 11; break; }
            case 3: { uint8_t n = fetch(); a = bus->in((uint16_t)((a << 8) | n)); cyc_ += 11; break; }
            case 4: { uint16_t t = rd16(sp); wr16(sp, hlx()); set_hlx(t); cyc_ += 19; break; }
            case 5: { uint16_t t = de(); set_de(hl()); set_hl(t); cyc_ += 4; break; }
            case 6: iff1 = iff2 = false; cyc_ += 4; break;
            default: iff1 = iff2 = true; ei_delay_ = true; cyc_ += 4; break;
            }
            break;
        case 4: { uint16_t nn = fetch16(); if (cond(y)) { push(pc); pc = nn; cyc_ += 17; } else cyc_ += 10; break; }
        case 5:
            if (q == 0) { push(get_rp2(p)); cyc_ += 11; }
            else { uint16_t nn = fetch16(); push(pc); pc = nn; cyc_ += 17; }
            break;
        case 6: alu(y, fetch()); cyc_ += 7; break;
        default: push(pc); pc = y * 8; cyc_ += 11; break;
        }
    }
}

int Z80::step() {
    cyc_ = 0; prefix_ = 0;
    bool allow = !ei_delay_;
    ei_delay_ = false;
    if (allow && irq_line && iff1) { do_interrupt(); tstates += cyc_; return cyc_; }
    if (halted) { r = (r & 0x80) | ((r + 1) & 0x7F); cyc_ = 4; tstates += 4; return 4; }
    r = (r & 0x80) | ((r + 1) & 0x7F);
    uint8_t op = fetch();
    while (op == 0xDD || op == 0xFD) { prefix_ = op; cyc_ += 4; r = (r & 0x80) | ((r + 1) & 0x7F); op = fetch(); }
    if (op == 0xED) { prefix_ = 0; exec_ed(); }
    else exec_main(op);
    tstates += cyc_;
    return cyc_;
}

void Z80::state_io(StateIO& io) {
    io.section("z80");
    io.val("a", a); io.val("f", f); io.val("b", b); io.val("c", c); io.val("d", d); io.val("e", e); io.val("h", h); io.val("l", l);
    io.val("a2", a2); io.val("f2", f2); io.val("b2", b2); io.val("c2", c2); io.val("d2", d2); io.val("e2", e2); io.val("h2", h2); io.val("l2", l2);
    io.val("ix", ix); io.val("iy", iy); io.val("sp", sp); io.val("pc", pc); io.val("i", i); io.val("r", r);
    io.val("iff1", iff1); io.val("iff2", iff2); io.val("halted", halted); io.val("im", im); io.val("irq_line", irq_line);
    io.val("tstates", tstates); io.val("ei_delay", ei_delay_); io.val("prefix", prefix_); io.val("cyc", cyc_);
    if (io.applying() && !io.saving() && (im < 0 || im > 2)) im = 0;      // hand-edited garbage must not crash the interrupt code
}
