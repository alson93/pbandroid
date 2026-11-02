// opl2.h - Yamaha YM3812 (OPL2) emulation: 9 two-operator FM channels, 4 waveforms, ADSR + key scaling,
// tremolo/vibrato, rhythm mode and the two timers that raise the Z80 interrupt.
// Implementation written from the chip's documented behaviour (tables are generated from their defining formulas).
#pragma once
#include <cstdint>
class StateIO;

class OPL2 {
public:
    void reset();
    void write_addr(uint8_t v) { addr_ = v; }
    void write_data(uint8_t v);
    uint8_t read_status() const { return status_; }
    bool irq() const { return (status_ & 0x80) != 0; }
    int clock();                         // advance one chip sample (clock/72); returns a signed mix (about +-32767)
    void state_io(StateIO& io);          // save / load the whole chip state (save states)

private:
    struct Op {
        // registers
        uint8_t am = 0, vib = 0, egt = 0, ksr = 0, mult = 0, ksl = 0, tl = 0, ar = 0, dr = 0, sl = 0, rr = 0, wf = 0;
        // state
        uint32_t phase = 0;
        float env = 511.f;
        int state = 0;                   // 0 off, 1 attack, 2 decay, 3 sustain, 4 release
        bool key = false;
        int out = 0, out1 = 0, out2 = 0;
    };
    struct Ch {
        uint16_t fnum = 0; uint8_t block = 0; bool kon = false; uint8_t fb = 0, conn = 0;
        Op op[2];
    };

    uint8_t addr_ = 0;
    uint8_t status_ = 0;
    uint8_t regs_[256] = {};
    Ch ch_[9];
    bool wse_ = false, nts_ = false, deep_am_ = false, deep_vib_ = false, rhythm_ = false;
    uint8_t rhythm_keys_ = 0;
    // timers
    uint8_t t1_ = 0, t2_ = 0; bool t1_run_ = false, t2_run_ = false, t1_mask_ = false, t2_mask_ = false;
    int t1_cnt_ = 0, t2_cnt_ = 0;
    // LFOs / noise
    uint32_t sample_ctr_ = 0; int trem_pos_ = 0; uint32_t noise_ = 1;

    void write_op(int slot, int reg, uint8_t v);
    void update_keys();
    void key_on(Op& o);
    void key_off(Op& o);
    int keycode(const Ch& c) const;
    int ksl_att(const Ch& c, const Op& o) const;
    void update_env(Ch& c, Op& o);
    int gen(const Ch& c, Op& o, uint32_t phase10, int modulation, int tremolo);
    uint32_t phase_inc(const Ch& c, const Op& o, int vib_delta) const;
    void set_irq_flag(uint8_t flag);
};
