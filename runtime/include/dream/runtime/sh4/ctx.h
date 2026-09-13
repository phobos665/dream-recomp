// Guest CPU context shared by emitted code, the runtime and the differential harness (WP1.2).
//
// Layout rules, because emitted C++ indexes this struct by name and the harness dumps it byte for
// byte to compare against the interpreter oracle:
//   - plain std::uint32_t fields, no padding surprises (static_assert below);
//   - the banked registers R0..R7 are stored as two explicit banks plus a "current bank" copy in
//     r[], so SR.RB switches are an 8-word swap and ordinary code never checks the bank;
//   - FR0..15 and XF0..15 are stored as two banks of 16 floats; FPSCR.FR selects which is "front".
//     Doubles (DRn) and pairs are views over consecutive singles, matching the hardware's register
//     file, so no separate double storage exists;
//   - the T bit lives in sr and is also cached in t for cheap emitted code; the emitter keeps them
//     coherent at instruction boundaries where it matters (see emitter docs).
#pragma once

#include <cstdint>

namespace dream::sh4 {

struct Ctx {
    std::uint32_t r[16];      // general registers, current bank in r[0..7]
    std::uint32_t r_bank[8];  // the other bank of R0..R7
    std::uint32_t pc;         // address of the instruction being executed (for traps/faults)
    std::uint32_t pr;
    std::uint32_t gbr;
    std::uint32_t vbr;
    std::uint32_t sr;  // full status register
    std::uint32_t ssr;
    std::uint32_t spc;
    std::uint32_t sgr;
    std::uint32_t dbr;
    std::uint32_t mach;
    std::uint32_t macl;
    std::uint32_t fpul;
    std::uint32_t fpscr;
    std::uint32_t t;       // cached SR.T (0 or 1)
    float fr[16];          // front FP bank (FR0..15 when FPSCR.FR == 0)
    float xf[16];          // back FP bank
    std::uint64_t cycles;  // virtual clock, advanced by emitted code estimates
    // Emitted code polls `cycles >= next_event` at function entries and loop back-edges and then
    // calls deliver_irq, which runs the scheduler up to `cycles`, delivers interrupts, and sets the
    // next deadline. The bare harness sets it to UINT64_MAX on the first poll.
    std::uint64_t next_event;
};

static_assert(sizeof(Ctx) == 4 * (16 + 8 + 16) + 4 * 32 + 8,
              "Ctx layout changed; update emitter and harness");

// SR bit positions.
inline constexpr std::uint32_t SR_T = 1u << 0;
inline constexpr std::uint32_t SR_S = 1u << 1;
inline constexpr std::uint32_t SR_IMASK = 0xFu << 4;
inline constexpr std::uint32_t SR_Q = 1u << 8;
inline constexpr std::uint32_t SR_M = 1u << 9;
inline constexpr std::uint32_t SR_FD = 1u << 15;
inline constexpr std::uint32_t SR_BL = 1u << 28;
inline constexpr std::uint32_t SR_RB = 1u << 29;
inline constexpr std::uint32_t SR_MD = 1u << 30;

// FPSCR bit positions (see runtime/fenv.h for RM/DN handling).
inline constexpr std::uint32_t FPSCR_RM = 0x3u;
inline constexpr std::uint32_t FPSCR_DN = 1u << 18;
inline constexpr std::uint32_t FPSCR_PR = 1u << 19;
inline constexpr std::uint32_t FPSCR_SZ = 1u << 20;
inline constexpr std::uint32_t FPSCR_FR = 1u << 21;

}  // namespace dream::sh4
