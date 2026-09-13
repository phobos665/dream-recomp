// Host floating-point environment control, driven by the guest's FPSCR (ADR 16).
//
// The SH-4 exposes two things the host must mirror for bit-exact results: the rounding mode
// (FPSCR.RM: 00 round to nearest, 01 round toward zero) and denormal flushing (FPSCR.DN: 1 means
// denormalised inputs and results are treated as zero). x86-64 implements these in MXCSR (RC bits,
// FTZ and DAZ), ARM64 in FPCR (RMode, FZ). Nothing else in the runtime touches those registers.
//
// Katana's default FPSCR is 0x00040001: DN set, RM = toward zero. That non-IEEE default is why this
// layer exists at all: a host running with default rounding would disagree with the console on the
// last bit of almost every division.
#pragma once

#include <cstdint>

namespace dream::fenv {

enum class Rounding : std::uint8_t { Nearest = 0, TowardZero = 1 };

struct Mode {
    Rounding rounding = Rounding::TowardZero;
    bool flush_denormals = true;

    friend bool operator==(const Mode&, const Mode&) = default;
};

inline constexpr std::uint32_t kFpscrRmMask = 0x3u;
inline constexpr std::uint32_t kFpscrDnBit = 1u << 18;
inline constexpr std::uint32_t kKatanaDefaultFpscr = 0x00040001u;

// Decode the two relevant fields of an FPSCR value. RM values 10 and 11 are reserved on SH-4; they
// are mapped to toward-zero, matching hardware behaviour as documented by Renesas.
constexpr Mode from_fpscr(std::uint32_t fpscr) noexcept {
    return Mode{(fpscr & kFpscrRmMask) == 0 ? Rounding::Nearest : Rounding::TowardZero,
                (fpscr & kFpscrDnBit) != 0};
}

// Program the host FP control register(s) for the calling thread.
void apply(Mode mode) noexcept;

// Read back what the host is currently set to (for tests and for the interrupt path, which saves
// and restores the mode around a handler).
Mode current() noexcept;

// Host defaults: nearest, no flushing. Call when handing control back to host library code that
// expects a sane environment (e.g. the renderer).
void reset_host() noexcept;

}  // namespace dream::fenv
