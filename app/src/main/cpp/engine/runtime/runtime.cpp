// runtime.cpp - CPU state, interrupts, exceptions for the lifted 68000 code.
#include "m68k_rt.h"
#include "hw.h"
#if defined(__has_include)
#  if __has_include(<execinfo.h>) && !defined(__ANDROID__)   // (bionic only has backtrace() from API 33)
#    include <execinfo.h>
#    define HAVE_BT 1
#  endif
#endif

uint32_t d[8], a[8];
uint8_t fX, fN, fZ, fV, fC;
uint32_t usp;
uint16_t sr_sys = 0x2700;
volatile int irq_level_pending = 0;
#ifndef M68K_FAST_CALLS
uint32_t shadow_stack[4096];
int shadow_sp = 0;
#endif

extern void (*vec_handlers[64])();   // generated

void (*fatal_hook)(const char*) = nullptr;     // GUI front ends set this to show a message box
static void fatal_msg(const char* msg) {
    std::fprintf(stderr, "%s\n", msg);
    if (fatal_hook) fatal_hook(msg);
    std::exit(1);
}
static char fbuf[160];
static int in_irq_poll = 0;
unsigned long irq_serviced = 0;
unsigned g_poll_tick = 0;

void irq_poll_now() {
    int level = irq_level_pending;
    if (level <= ((sr_sys >> 8) & 7) || in_irq_poll > 4) return;
    irq_level_pending = 0;
    ++irq_serviced;
    uint16_t old = get_sr();
    a[7] -= 4; wr32(a[7], 0xFFFFFFFFu);     // fake PC: the handler's RTE discards it
    a[7] -= 2; wr16(a[7], old);
    sr_sys = 0x2000 | (level << 8);
    ++in_irq_poll;
#ifndef M68K_FAST_CALLS
    CallGuard g(0xFFFFFFFFu);
#endif
    vec_handlers[24 + level]();
    --in_irq_poll;
}

void do_rte() {
    uint16_t s = rd16(a[7]); a[7] += 2;
    a[7] += 4;
    set_ccr(s); sr_sys = s & 0xFF00;
    // The VBLANK handler has just applied this frame's scroll registers and palette, and the main loop is idle (sprites/tiles
    // unchanged since it stopped): this is exactly what the display shows. Publish it as the frame to render.
    hw_latch_video();
}

#ifdef _WIN32
static const long kMaxIdleSpins = 2000000000L;      // host_idle() only yields on Windows (no 1 ms sleep), so a spin is a lot shorter there
#else
static const long kMaxIdleSpins = 2000000;
#endif
void cpu_stop(uint16_t sr) {
    set_sr(sr);
    for (long spin = 0; ; ++spin) {
        if (!in_irq_poll) hw_state_poll();
        host_idle();
        if (irq_level_pending > ((sr_sys >> 8) & 7)) { irq_poll_now(); return; }
#ifdef HAVE_BT
        if (spin == 1000 && std::getenv("M68K_BT")) {
            void* bt[64]; int n = backtrace(bt, 64);
            std::fprintf(stderr, "[stuck in STOP sr=%04x] lifted call chain (innermost first):\n", sr);
            backtrace_symbols_fd(bt, n, 2);
        }
#endif
        if (spin > kMaxIdleSpins) { std::fprintf(stderr, "STOP with interrupts masked forever (sr=%04x)\n", sr); std::exit(0); }
    }
}

void cpu_reset_line() { hw_reset(); }

void do_trap(int vec) {
    std::fprintf(stderr, "exception vector %d (trap/divzero/chk)\n", vec);
    if (vec_handlers[vec]) vec_handlers[vec]();
}
void do_illegal(uint32_t pc)        { std::snprintf(fbuf, sizeof fbuf, "illegal/line-A/F instruction at %06x", pc); fatal_msg(fbuf); }
void bad_target(uint32_t ad)        { std::snprintf(fbuf, sizeof fbuf, "branch into undecoded code %06x", ad); fatal_msg(fbuf); }
void bad_jump(uint32_t ad)          { std::snprintf(fbuf, sizeof fbuf, "computed jump to unmapped address %06x", ad); fatal_msg(fbuf); }
void bad_call(uint32_t ad)          {
    std::snprintf(fbuf, sizeof fbuf, "computed call to unmapped address %06x", ad);
    std::fprintf(stderr, "%s\n", fbuf);
#ifdef HAVE_BT
    if (std::getenv("M68K_BT")) { void* bt[64]; int n = backtrace(bt, 64); backtrace_symbols_fd(bt, n, 2); }
#endif
    if (fatal_hook) fatal_hook(fbuf);
    std::exit(1);
}
void bad_fallthrough(uint32_t ad)   { std::snprintf(fbuf, sizeof fbuf, "execution falls off decoded code at %06x", ad); fatal_msg(fbuf); }
void unimplemented_insn(uint32_t pc){ std::snprintf(fbuf, sizeof fbuf, "unimplemented instruction at %06x", pc); fatal_msg(fbuf); }

#ifndef M68K_FAST_CALLS
// A RTS whose popped address does not match the innermost CALL: either the code dropped
// return addresses (non-local exit) or used PEA/RTS as a computed jump.
void rts_slow(uint32_t r) {
    for (int i = shadow_sp - 1; i >= 0; --i)
        if (shadow_stack[i] == r) throw RetUnwind{r};
    dyn_call(r);        // computed jump through the stack
}
#endif

#ifdef M68K_TRACE
#include <set>
#include <string>
void trace_fn(const char* name) {            // prints each function the first time it is entered
    static std::set<std::string> seen; static int n = 0;
    if (seen.insert(name).second && n++ < 400) std::fprintf(stderr, "%03d %s\n", n, name);
}
#endif
