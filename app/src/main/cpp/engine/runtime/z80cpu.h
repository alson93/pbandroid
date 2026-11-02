// z80cpu.h - Z80 interpreter used for the Toaplan sound CPU (TP-025 sound ROM).
#pragma once
#include <cstdint>
class StateIO;

struct Z80Bus {
    virtual uint8_t read(uint16_t addr) = 0;
    virtual void write(uint16_t addr, uint8_t v) = 0;
    virtual uint8_t in(uint16_t port) { (void)port; return 0xFF; }
    virtual void out(uint16_t port, uint8_t v) { (void)port; (void)v; }
};

class Z80 {
public:
    Z80Bus* bus = nullptr;
    uint8_t a = 0xFF, f = 0xFF, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0;
    uint8_t a2 = 0, f2 = 0, b2 = 0, c2 = 0, d2 = 0, e2 = 0, h2 = 0, l2 = 0;
    uint16_t ix = 0xFFFF, iy = 0xFFFF, sp = 0xFFFF, pc = 0;
    uint8_t i = 0, r = 0;
    bool iff1 = false, iff2 = false, halted = false;
    int im = 0;
    bool irq_line = false;          // level-triggered INT input (set/cleared by the host)
    uint64_t tstates = 0;

    void reset();
    int step();                      // executes one instruction (or takes an interrupt); returns T-states used
    void state_io(StateIO& io);      // save / load every register and flag (save states)

private:
    bool ei_delay_ = false;
    int prefix_ = 0;                 // 0, 0xDD, 0xFD for the instruction being executed
    int cyc_ = 0;

    // flag bits
    enum { FS = 0x80, FZ = 0x40, FY = 0x20, FH = 0x10, FX = 0x08, FP = 0x04, FN = 0x02, FC = 0x01 };

    uint8_t rd(uint16_t ad) { return bus->read(ad); }
    void wr(uint16_t ad, uint8_t v) { bus->write(ad, v); }
    uint16_t rd16(uint16_t ad) { return rd(ad) | (rd((uint16_t)(ad + 1)) << 8); }
    void wr16(uint16_t ad, uint16_t v) { wr(ad, (uint8_t)v); wr((uint16_t)(ad + 1), (uint8_t)(v >> 8)); }
    uint8_t fetch() { return rd(pc++); }
    uint16_t fetch16() { uint16_t v = fetch(); v |= fetch() << 8; return v; }
    void push(uint16_t v) { sp -= 2; wr16(sp, v); }
    uint16_t pop() { uint16_t v = rd16(sp); sp += 2; return v; }

    uint16_t bc() const { return (b << 8) | c; }
    uint16_t de() const { return (d << 8) | e; }
    uint16_t hl() const { return (h << 8) | l; }
    void set_bc(uint16_t v) { b = v >> 8; c = (uint8_t)v; }
    void set_de(uint16_t v) { d = v >> 8; e = (uint8_t)v; }
    void set_hl(uint16_t v) { h = v >> 8; l = (uint8_t)v; }
    uint16_t hlx() const;            // HL, or IX/IY under a prefix
    void set_hlx(uint16_t v);
    uint8_t get_r(int idx);          // 0..7 (6 = memory via HL/(IX+d) must be handled by caller)
    void set_r(int idx, uint8_t v);
    uint8_t get_rp_plain(int idx);
    uint16_t get_rp(int p);          // BC DE HLX SP
    void set_rp(int p, uint16_t v);
    uint16_t get_rp2(int p);         // BC DE HLX AF
    void set_rp2(int p, uint16_t v);

    bool cond(int cc) const;
    void alu(int op, uint8_t v);
    uint8_t inc8(uint8_t v);
    uint8_t dec8(uint8_t v);
    uint16_t add16(uint16_t a_, uint16_t b_);
    void adc_hl(uint16_t v);
    void sbc_hl(uint16_t v);
    uint8_t rot(int op, uint8_t v);  // CB rotates/shifts with full flags
    void daa();
    void exec_main(uint8_t op);
    void exec_cb();
    void exec_ed();
    uint16_t mem_addr();             // (HL) or (IX+d) address for the current instruction (reads d when prefixed)
    void do_interrupt();
};
