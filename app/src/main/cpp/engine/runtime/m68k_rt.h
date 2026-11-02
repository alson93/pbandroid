// m68k_rt.h - runtime support for the lifted Pipi & Bibis (Toaplan TP-025) 68000 code.
//
// CPU state is global because the original code passes arguments in registers.
// d[0..7], a[0..7] (a[7] = stack pointer), condition codes as separate 0/1 bytes.
//
// Build options:
//   -DM68K_FAST_CALLS   plain CALL/RTS (no shadow return stack, no non-local-return support)
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <type_traits>

extern uint32_t d[8], a[8];
extern uint8_t fX, fN, fZ, fV, fC;
extern uint32_t usp;
extern uint16_t sr_sys;                 // system byte of SR (bits 15..8)
extern volatile int irq_level_pending;  // 0 = none, 1..7 = autovector level raised by the host

// ---------------------------------------------------------------- bus (bus.cpp)
uint8_t  rd8(uint32_t addr);
uint16_t rd16(uint32_t addr);
uint32_t rd32(uint32_t addr);
void wr8(uint32_t addr, uint8_t v);
void wr16(uint32_t addr, uint16_t v);
void wr32(uint32_t addr, uint32_t v);

// ---------------------------------------------------------------- addressing helpers
inline uint32_t postinc(uint32_t& r, int n) { uint32_t t = r; r += n; return t; }
inline uint32_t predec(uint32_t& r, int n) { r -= n; return r; }
inline void setb(uint32_t& r, uint8_t v)  { r = (r & 0xFFFFFF00u) | v; }
inline void setw(uint32_t& r, uint16_t v) { r = (r & 0xFFFF0000u) | v; }

// ---------------------------------------------------------------- condition-code helpers
template <typename T> inline constexpr unsigned bits_() { return sizeof(T) * 8; }
template <typename T> inline uint8_t msb_(T v) { return (uint8_t)((v >> (bits_<T>() - 1)) & 1); }

template <typename T> inline void lg_(T r) { fN = msb_(r); fZ = (r == 0); fV = 0; fC = 0; }
inline void lg8(uint8_t r)   { lg_<uint8_t>(r); }
inline void lg16(uint16_t r) { lg_<uint16_t>(r); }
inline void lg32(uint32_t r) { lg_<uint32_t>(r); }

template <typename T> inline T add_(T s, T dd) {
    T r = (T)(dd + s);
    fN = msb_(r); fZ = (r == 0); fV = msb_((T)((s ^ r) & (dd ^ r))); fC = fX = (r < s);
    return r;
}
template <typename T> inline T sub_(T s, T dd) {
    T r = (T)(dd - s);
    fN = msb_(r); fZ = (r == 0); fV = msb_((T)((s ^ dd) & (r ^ dd))); fC = fX = (s > dd);
    return r;
}
template <typename T> inline void cmp_(T s, T dd) {
    T r = (T)(dd - s);
    fN = msb_(r); fZ = (r == 0); fV = msb_((T)((s ^ dd) & (r ^ dd))); fC = (s > dd);
}
template <typename T> inline T neg_(T dd) {
    T r = (T)(0 - dd);
    fN = msb_(r); fZ = (r == 0); fV = msb_((T)(dd & r)); fC = fX = (dd != 0);
    return r;
}
template <typename T> inline T addx_(T s, T dd) {
    uint64_t x = fX;
    T r = (T)(dd + s + x);
    fN = msb_(r); if (r) fZ = 0; fV = msb_((T)((s ^ r) & (dd ^ r)));
    fC = fX = (uint8_t)((((uint64_t)dd + s + x) >> bits_<T>()) & 1);
    return r;
}
template <typename T> inline T subx_(T s, T dd) {
    uint64_t x = fX;
    T r = (T)(dd - s - x);
    fN = msb_(r); if (r) fZ = 0; fV = msb_((T)((s ^ dd) & (r ^ dd)));
    fC = fX = ((uint64_t)s + x > (uint64_t)dd);
    return r;
}
template <typename T> inline T negx_(T dd) {
    uint64_t x = fX;
    T r = (T)(0 - dd - x);
    fN = msb_(r); if (r) fZ = 0; fV = msb_((T)(dd & r));
    fC = fX = ((uint64_t)dd + x != 0);
    return r;
}

#define DEF3(name, tmpl) \
    inline uint8_t  name##8 (uint8_t s,  uint8_t dd)  { return tmpl<uint8_t>(s, dd); } \
    inline uint16_t name##16(uint16_t s, uint16_t dd) { return tmpl<uint16_t>(s, dd); } \
    inline uint32_t name##32(uint32_t s, uint32_t dd) { return tmpl<uint32_t>(s, dd); }
DEF3(add, add_) DEF3(sub, sub_) DEF3(addx, addx_) DEF3(subx, subx_)
inline void cmp8 (uint8_t s,  uint8_t dd)  { cmp_<uint8_t>(s, dd); }
inline void cmp16(uint16_t s, uint16_t dd) { cmp_<uint16_t>(s, dd); }
inline void cmp32(uint32_t s, uint32_t dd) { cmp_<uint32_t>(s, dd); }
inline uint8_t  neg8 (uint8_t v)  { return neg_<uint8_t>(v); }
inline uint16_t neg16(uint16_t v) { return neg_<uint16_t>(v); }
inline uint32_t neg32(uint32_t v) { return neg_<uint32_t>(v); }
inline uint8_t  negx8 (uint8_t v)  { return negx_<uint8_t>(v); }
inline uint16_t negx16(uint16_t v) { return negx_<uint16_t>(v); }
inline uint32_t negx32(uint32_t v) { return negx_<uint32_t>(v); }

// ---------------------------------------------------------------- shifts / rotates (count 0..63)
template <typename T> inline T lsl_(T v, unsigned c) {
    const unsigned B = bits_<T>();
    if (c == 0) { fC = 0; fN = msb_(v); fZ = (v == 0); fV = 0; return v; }
    T r; if (c < B) { r = (T)(v << c); fC = (v >> (B - c)) & 1; } else { r = 0; fC = (c == B) ? (v & 1) : 0; }
    fX = fC; fN = msb_(r); fZ = (r == 0); fV = 0; return r;
}
template <typename T> inline T lsr_(T v, unsigned c) {
    const unsigned B = bits_<T>();
    if (c == 0) { fC = 0; fN = msb_(v); fZ = (v == 0); fV = 0; return v; }
    T r; if (c < B) { r = (T)(v >> c); fC = (v >> (c - 1)) & 1; } else { r = 0; fC = (c == B) ? msb_(v) : 0; }
    fX = fC; fN = msb_(r); fZ = (r == 0); fV = 0; return r;
}
template <typename T> inline T asl_(T v, unsigned c) {
    const unsigned B = bits_<T>();
    if (c == 0) { fC = 0; fN = msb_(v); fZ = (v == 0); fV = 0; return v; }
    T r; 
    if (c < B) {
        r = (T)(v << c); fC = (v >> (B - c)) & 1;
        uint64_t m = ((uint64_t)1 << (c + 1)) - 1, top = ((uint64_t)v >> (B - 1 - c)) & m;
        fV = (top != 0 && top != m);
    } else { r = 0; fC = (c == B) ? (v & 1) : 0; fV = (v != 0); }
    fX = fC; fN = msb_(r); fZ = (r == 0); return r;
}
template <typename T> inline T asr_(T v, unsigned c) {
    using S = typename std::make_signed<T>::type;
    const unsigned B = bits_<T>();
    if (c == 0) { fC = 0; fN = msb_(v); fZ = (v == 0); fV = 0; return v; }
    T r; if (c < B) { r = (T)((S)v >> c); fC = (uint8_t)(((S)v >> (c - 1)) & 1); } else { r = msb_(v) ? (T)~(T)0 : 0; fC = msb_(v); }
    fX = fC; fN = msb_(r); fZ = (r == 0); fV = 0; return r;
}
template <typename T> inline T rol_(T v, unsigned c) {
    const unsigned B = bits_<T>(); unsigned k = c % B; T r = k ? (T)((v << k) | (v >> (B - k))) : v;
    if (c) fC = r & 1; else fC = 0;
    fN = msb_(r); fZ = (r == 0); fV = 0; return r;
}
template <typename T> inline T ror_(T v, unsigned c) {
    const unsigned B = bits_<T>(); unsigned k = c % B; T r = k ? (T)((v >> k) | (v << (B - k))) : v;
    if (c) fC = msb_(r); else fC = 0;
    fN = msb_(r); fZ = (r == 0); fV = 0; return r;
}
template <typename T> inline T roxl_(T v, unsigned c) {
    const unsigned B = bits_<T>(); const unsigned W = B + 1;
    if (c == 0) { fC = fX; fN = msb_(v); fZ = (v == 0); fV = 0; return v; }
    unsigned n = c % W; uint64_t val = ((uint64_t)fX << B) | v, full = ((uint64_t)1 << W) - 1;
    uint64_t r = n ? (((val << n) | (val >> (W - n))) & full) : val;
    fX = fC = (r >> B) & 1; T t = (T)r; fN = msb_(t); fZ = (t == 0); fV = 0; return t;
}
template <typename T> inline T roxr_(T v, unsigned c) {
    const unsigned B = bits_<T>(); const unsigned W = B + 1;
    if (c == 0) { fC = fX; fN = msb_(v); fZ = (v == 0); fV = 0; return v; }
    unsigned n = c % W; uint64_t val = ((uint64_t)fX << B) | v, full = ((uint64_t)1 << W) - 1;
    uint64_t r = n ? (((val >> n) | (val << (W - n))) & full) : val;
    fX = fC = (r >> B) & 1; T t = (T)r; fN = msb_(t); fZ = (t == 0); fV = 0; return t;
}
#define DEFSH(name) \
    inline uint8_t  name##8 (uint8_t v,  unsigned c) { return name##_<uint8_t>(v, c); } \
    inline uint16_t name##16(uint16_t v, unsigned c) { return name##_<uint16_t>(v, c); } \
    inline uint32_t name##32(uint32_t v, unsigned c) { return name##_<uint32_t>(v, c); }
DEFSH(lsl) DEFSH(lsr) DEFSH(asl) DEFSH(asr) DEFSH(rol) DEFSH(ror) DEFSH(roxl) DEFSH(roxr)
// flag-free rotates (used when the generator proved the flags dead)
template <typename T> inline T rolnf_(T v, unsigned c) { unsigned B = bits_<T>(), k = c % B; return k ? (T)((v << k) | (v >> (B - k))) : v; }
template <typename T> inline T rornf_(T v, unsigned c) { unsigned B = bits_<T>(), k = c % B; return k ? (T)((v >> k) | (v << (B - k))) : v; }
inline uint8_t  rol8_nf (uint8_t v,  unsigned c) { return rolnf_<uint8_t>(v, c); }
inline uint16_t rol16_nf(uint16_t v, unsigned c) { return rolnf_<uint16_t>(v, c); }
inline uint32_t rol32_nf(uint32_t v, unsigned c) { return rolnf_<uint32_t>(v, c); }
inline uint8_t  ror8_nf (uint8_t v,  unsigned c) { return rornf_<uint8_t>(v, c); }
inline uint16_t ror16_nf(uint16_t v, unsigned c) { return rornf_<uint16_t>(v, c); }
inline uint32_t ror32_nf(uint32_t v, unsigned c) { return rornf_<uint32_t>(v, c); }

// ---------------------------------------------------------------- BCD, TAS, DIV
inline uint8_t abcd(uint8_t s, uint8_t dd) {
    int res = (s & 0x0F) + (dd & 0x0F) + fX;
    if (res > 9) res += 6;
    res += (s & 0xF0) + (dd & 0xF0);
    fC = fX = res > 0x99;
    if (fC) res -= 0xA0;
    res &= 0xFF; if (res) fZ = 0; fN = (res >> 7) & 1;
    return (uint8_t)res;
}
inline uint8_t sbcd(uint8_t s, uint8_t dd) {
    int res = (dd & 0x0F) - (s & 0x0F) - fX;
    if (res > 9 || res < 0) { if (res < 0) { res -= 6; } else res -= 6; }
    res += (dd & 0xF0) - (s & 0xF0);
    fC = fX = (res & 0x300) != 0 || res < 0;
    if (res < 0) res += 0xA0;
    res &= 0xFF; if (res) fZ = 0; fN = (res >> 7) & 1;
    return (uint8_t)res;
}
inline uint8_t tas8(uint8_t v) { lg8(v); return v | 0x80; }

void do_trap(int vec);                       // exception entry (see runtime.cpp)
inline void divu(uint32_t* dn, uint16_t s) {
    if (s == 0) { do_trap(5); return; }
    uint32_t q = *dn / s;
    if (q > 0xFFFF) { fV = 1; fC = 0; return; }
    uint32_t r = *dn % s; *dn = (r << 16) | q; fN = (q >> 15) & 1; fZ = (q == 0); fV = 0; fC = 0;
}
inline void divs(uint32_t* dn, uint16_t s) {
    if (s == 0) { do_trap(5); return; }
    int32_t n = (int32_t)*dn; int16_t dv = (int16_t)s;
    int32_t q = n / dv, r = n % dv;
    if (q > 32767 || q < -32768) { fV = 1; fC = 0; return; }
    *dn = ((uint32_t)(uint16_t)r << 16) | (uint16_t)q; fN = (q < 0); fZ = (q == 0); fV = 0; fC = 0;
}

// ---------------------------------------------------------------- MOVEM / MOVEP
// mask is normalised: bit0 = d0 ... bit7 = d7, bit8 = a0 ... bit15 = a7
inline uint32_t& reg_(int i) { return i < 8 ? d[i] : a[i - 8]; }
inline void movem_store_pre16(uint16_t m, int r) { for (int i = 15; i >= 0; --i) if (m & (1u << i)) { a[r] -= 2; wr16(a[r], (uint16_t)reg_(i)); } }
inline void movem_store_pre32(uint16_t m, int r) { for (int i = 15; i >= 0; --i) if (m & (1u << i)) { a[r] -= 4; wr32(a[r], reg_(i)); } }
inline void movem_store16(uint16_t m, uint32_t ea) { for (int i = 0; i < 16; ++i) if (m & (1u << i)) { wr16(ea, (uint16_t)reg_(i)); ea += 2; } }
inline void movem_store32(uint16_t m, uint32_t ea) { for (int i = 0; i < 16; ++i) if (m & (1u << i)) { wr32(ea, reg_(i)); ea += 4; } }
inline void movem_load16(uint16_t m, uint32_t ea) { for (int i = 0; i < 16; ++i) if (m & (1u << i)) { reg_(i) = (uint32_t)(int16_t)rd16(ea); ea += 2; } }
inline void movem_load32(uint16_t m, uint32_t ea) { for (int i = 0; i < 16; ++i) if (m & (1u << i)) { reg_(i) = rd32(ea); ea += 4; } }
inline void movem_load_post16(uint16_t m, int r) { uint32_t ea = a[r]; movem_load16(m, ea); for (int i = 0; i < 16; ++i) if (m & (1u << i)) ea += 2; a[r] = ea; }
inline void movem_load_post32(uint16_t m, int r) { uint32_t ea = a[r]; movem_load32(m, ea); for (int i = 0; i < 16; ++i) if (m & (1u << i)) ea += 4; a[r] = ea; }
inline void movep_store16(uint32_t ea, uint32_t v) { wr8(ea, v >> 8); wr8(ea + 2, v); }
inline void movep_store32(uint32_t ea, uint32_t v) { wr8(ea, v >> 24); wr8(ea + 2, v >> 16); wr8(ea + 4, v >> 8); wr8(ea + 6, v); }
inline uint32_t movep_load16(uint32_t ea, uint32_t old) { return (old & 0xFFFF0000u) | (rd8(ea) << 8) | rd8(ea + 2); }
inline uint32_t movep_load32(uint32_t ea, uint32_t) { return (rd8(ea) << 24) | (rd8(ea + 2) << 16) | (rd8(ea + 4) << 8) | rd8(ea + 6); }

// ---------------------------------------------------------------- status register
inline uint16_t get_sr() { return sr_sys | (fX << 4) | (fN << 3) | (fZ << 2) | (fV << 1) | fC; }
void irq_poll_now();
inline void set_ccr(uint16_t v) { fX = (v >> 4) & 1; fN = (v >> 3) & 1; fZ = (v >> 2) & 1; fV = (v >> 1) & 1; fC = v & 1; }
inline void set_sr(uint16_t v) { set_ccr(v); sr_sys = v & 0xFF00; irq_poll_now(); }
inline void set_sr_sys(uint16_t v) { set_sr(v); }

// ---------------------------------------------------------------- optional tracing (-DM68K_TRACE)
#ifdef M68K_TRACE
void trace_fn(const char* name);
#define TRACE_FN(n) trace_fn(n)
#else
#define TRACE_FN(n) ((void)0)
#endif

// ---------------------------------------------------------------- control flow
extern unsigned g_poll_tick;
void hw_state_poll();                         // save/load requests, serviced at the main-loop VBLANK wait (bus.cpp)
void host_tick_irq();                        // host time base: raises the VBLANK interrupt request
#define IRQ_POLL() do { if (++g_poll_tick > 20000) { g_poll_tick = 0; host_tick_irq(); } \
                        if (irq_level_pending > ((sr_sys >> 8) & 7)) irq_poll_now(); } while (0)
void cpu_stop(uint16_t sr);                  // STOP #sr: wait for an interrupt
#define CPU_STOP(v) cpu_stop(v)
void cpu_reset_line();
[[noreturn]] void do_illegal(uint32_t pc);
[[noreturn]] void bad_target(uint32_t addr);
[[noreturn]] void bad_jump(uint32_t addr);
[[noreturn]] void bad_call(uint32_t addr);
[[noreturn]] void bad_fallthrough(uint32_t addr);
void unimplemented_insn(uint32_t pc);
void dyn_call(uint32_t addr);                // generated: address -> function dispatcher

#ifdef M68K_FAST_CALLS
#define CALL(ret, fn) do { a[7] -= 4; wr32(a[7], (ret)); fn(); } while (0)
#define RTS() do { a[7] += 4; return; } while (0)
#define RTR() do { set_ccr(rd16(a[7])); a[7] += 6; return; } while (0)
#else
struct RetUnwind { uint32_t addr; };
extern uint32_t shadow_stack[4096];
extern int shadow_sp;
struct CallGuard {
    explicit CallGuard(uint32_t r) { shadow_stack[shadow_sp++] = r; }
    ~CallGuard() { --shadow_sp; }
};
void rts_slow(uint32_t r);                   // non-local / computed return
inline void do_rts() {
    uint32_t r = rd32(a[7]); a[7] += 4;
    if (shadow_sp && r == shadow_stack[shadow_sp - 1]) return;
    rts_slow(r);
}
#define CALL(ret, fn) do { a[7] -= 4; wr32(a[7], (ret)); CallGuard g_((ret)); \
                           try { fn(); } catch (const RetUnwind& u_) { if (u_.addr != (ret)) throw; } } while (0)
#define RTS() do { do_rts(); return; } while (0)
#define RTR() do { set_ccr(rd16(a[7])); a[7] += 2; do_rts(); return; } while (0)
#endif
void do_rte();
#define RTE() do { do_rte(); return; } while (0)
