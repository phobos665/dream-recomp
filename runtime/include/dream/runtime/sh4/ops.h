// Helpers for SH-4 instructions whose C++ lowering is more than a line (docs/emitter-design.md).
// Emitted code calls these; they are also what the differential harness exercises first.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "dream/runtime/fenv.h"
#include "dream/runtime/memory.h"
#include "dream/runtime/sh4/ctx.h"

namespace dream::sh4 {

inline std::uint32_t addc(Ctx& c, std::uint32_t a, std::uint32_t b) noexcept {
    const std::uint32_t tmp = a + b;
    const std::uint32_t res = tmp + c.t;
    c.t = (tmp < a) || (res < tmp);
    return res;
}

inline std::uint32_t subc(Ctx& c, std::uint32_t a, std::uint32_t b) noexcept {
    const std::uint32_t tmp = a - b;
    const std::uint32_t res = tmp - c.t;
    c.t = (a < tmp) || (tmp < res);
    return res;
}

// ADDV/SUBV: T = signed overflow.
inline std::uint32_t addv(Ctx& c, std::uint32_t a, std::uint32_t b) noexcept {
    const std::uint32_t res = a + b;
    c.t = (~(a ^ b) & (a ^ res)) >> 31;
    return res;
}

inline std::uint32_t subv(Ctx& c, std::uint32_t a, std::uint32_t b) noexcept {
    const std::uint32_t res = a - b;
    c.t = ((a ^ b) & (a ^ res)) >> 31;
    return res;
}

inline std::uint32_t negc(Ctx& c, std::uint32_t a) noexcept {
    const std::uint32_t tmp = 0u - a;
    const std::uint32_t res = tmp - c.t;
    c.t = (0u < tmp) || (tmp < res);
    return res;
}

// SHAD: positive count shifts left, negative shifts right arithmetically; count is the low 5 bits.
inline std::uint32_t shad(std::uint32_t v, std::uint32_t count) noexcept {
    const std::uint32_t s = count & 0x1F;
    if ((count & 0x80000000u) == 0)
        return v << s;
    if (s == 0)
        return (v & 0x80000000u) ? 0xFFFFFFFFu : 0u;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(v) >> (32 - s));
}

inline std::uint32_t shld(std::uint32_t v, std::uint32_t count) noexcept {
    const std::uint32_t s = count & 0x1F;
    if ((count & 0x80000000u) == 0)
        return v << s;
    if (s == 0)
        return 0;
    return v >> (32 - s);
}

inline std::uint32_t rotcl(Ctx& c, std::uint32_t v) noexcept {
    const std::uint32_t t = v >> 31;
    const std::uint32_t res = (v << 1) | c.t;
    c.t = t;
    return res;
}

inline std::uint32_t rotcr(Ctx& c, std::uint32_t v) noexcept {
    const std::uint32_t t = v & 1;
    const std::uint32_t res = (v >> 1) | (c.t << 31);
    c.t = t;
    return res;
}

// DIV0S/DIV0U/DIV1 operate on the Q and M bits of SR and on T.
inline void div0u(Ctx& c) noexcept {
    c.sr &= ~(SR_Q | SR_M);
    c.t = 0;
}

inline void div0s(Ctx& c, std::uint32_t rn, std::uint32_t rm) noexcept {
    const std::uint32_t q = rn >> 31, m = rm >> 31;
    c.sr = (c.sr & ~(SR_Q | SR_M)) | (q ? SR_Q : 0) | (m ? SR_M : 0);
    c.t = q ^ m;
}

// One step of the non-restoring division; returns the new Rn. Transcribed from the SH-4 manual.
inline std::uint32_t div1(Ctx& c, std::uint32_t rn, std::uint32_t rm) noexcept {
    std::uint32_t q = (c.sr & SR_Q) ? 1u : 0u;
    const std::uint32_t m = (c.sr & SR_M) ? 1u : 0u;
    const std::uint32_t old_q = q;
    q = rn >> 31;
    rn = (rn << 1) | c.t;
    const std::uint32_t tmp0 = rn;
    if (old_q == 0) {
        if (m == 0) {
            rn -= rm;
            const std::uint32_t tmp1 = rn > tmp0;
            q = q ? (tmp1 == 0) : tmp1;
        } else {
            rn += rm;
            const std::uint32_t tmp1 = rn < tmp0;
            q = q ? tmp1 : (tmp1 == 0);
        }
    } else {
        if (m == 0) {
            rn += rm;
            const std::uint32_t tmp1 = rn < tmp0;
            q = q ? (tmp1 == 0) : tmp1;
        } else {
            rn -= rm;
            const std::uint32_t tmp1 = rn > tmp0;
            q = q ? tmp1 : (tmp1 == 0);
        }
    }
    c.sr = (c.sr & ~SR_Q) | (q ? SR_Q : 0);
    c.t = (q == m);
    return rn;
}

inline void dmuls(Ctx& c, std::uint32_t rn, std::uint32_t rm) noexcept {
    const std::int64_t p = static_cast<std::int64_t>(static_cast<std::int32_t>(rn)) *
                           static_cast<std::int64_t>(static_cast<std::int32_t>(rm));
    c.mach = static_cast<std::uint32_t>(static_cast<std::uint64_t>(p) >> 32);
    c.macl = static_cast<std::uint32_t>(p);
}

inline void dmulu(Ctx& c, std::uint32_t rn, std::uint32_t rm) noexcept {
    const std::uint64_t p = static_cast<std::uint64_t>(rn) * rm;
    c.mach = static_cast<std::uint32_t>(p >> 32);
    c.macl = static_cast<std::uint32_t>(p);
}

// MAC.L: MAC += Rm*Rn (signed 32x32->64); with SR.S the result saturates to 48 bits.
inline void mac_l(Ctx& c, std::int32_t a, std::int32_t b) noexcept {
    const std::int64_t prod = static_cast<std::int64_t>(a) * b;
    std::int64_t mac =
        static_cast<std::int64_t>((static_cast<std::uint64_t>(c.mach) << 32) | c.macl);
    if (c.sr & SR_S) {
        mac = static_cast<std::int64_t>(static_cast<std::uint64_t>(mac) << 16) >>
              16;  // 48-bit sign-extend
        std::int64_t sum = mac + prod;
        const std::int64_t lo = -(static_cast<std::int64_t>(1) << 47),
                           hi = (static_cast<std::int64_t>(1) << 47) - 1;
        if (sum > hi)
            sum = hi;
        if (sum < lo)
            sum = lo;
        mac = sum;
    } else {
        mac += prod;
    }
    c.mach = static_cast<std::uint32_t>(static_cast<std::uint64_t>(mac) >> 32);
    c.macl = static_cast<std::uint32_t>(mac);
}

// MAC.W: MAC += Rm*Rn (signed 16x16->32); with SR.S the low word saturates to 32 bits and MACH
// records overflow; without it the product is added to the 64-bit accumulator.
inline void mac_w(Ctx& c, std::int16_t a, std::int16_t b) noexcept {
    const std::int32_t prod = static_cast<std::int32_t>(a) * b;
    if (c.sr & SR_S) {
        const std::int64_t sum =
            static_cast<std::int64_t>(static_cast<std::int32_t>(c.macl)) + prod;
        if (sum > 0x7FFFFFFFLL) {
            c.macl = 0x7FFFFFFFu;
            c.mach |= 1;
        } else if (sum < -0x80000000LL) {
            c.macl = 0x80000000u;
            c.mach |= 1;
        } else {
            c.macl = static_cast<std::uint32_t>(sum);
        }
    } else {
        std::int64_t mac =
            static_cast<std::int64_t>((static_cast<std::uint64_t>(c.mach) << 32) | c.macl);
        mac += prod;
        c.mach = static_cast<std::uint32_t>(static_cast<std::uint64_t>(mac) >> 32);
        c.macl = static_cast<std::uint32_t>(mac);
    }
}

inline std::uint32_t cmp_str(std::uint32_t a, std::uint32_t b) noexcept {
    const std::uint32_t x = a ^ b;
    return ((x & 0xFF000000u) == 0) || ((x & 0x00FF0000u) == 0) || ((x & 0x0000FF00u) == 0) ||
           ((x & 0x000000FFu) == 0);
}

// LDC Rm,SR: writes SR, swapping register banks if RB changes, and refreshes the cached T.
inline void write_sr(Ctx& c, std::uint32_t v) noexcept {
    const bool old_rb = (c.sr & SR_RB) != 0, new_rb = (v & SR_RB) != 0;
    if (old_rb != new_rb) {
        for (int i = 0; i < 8; ++i) {
            const std::uint32_t tmp = c.r[i];
            c.r[i] = c.r_bank[i];
            c.r_bank[i] = tmp;
        }
    }
    // A change to BL or IMASK may unmask a pending interrupt: force a poll at the next safe point.
    if (((c.sr ^ v) & (SR_BL | SR_IMASK)) != 0)
        c.next_event = 0;
    c.sr = v & 0x700083F3u;  // architecturally defined bits only
    c.t = v & SR_T;
}

inline std::uint32_t read_sr(const Ctx& c) noexcept {
    return (c.sr & ~SR_T) | (c.t & 1u);
}

// FRCHG: swap the two FP banks.
inline void frchg(Ctx& c) noexcept {
    for (int i = 0; i < 16; ++i) {
        const float tmp = c.fr[i];
        c.fr[i] = c.xf[i];
        c.xf[i] = tmp;
    }
    c.fpscr ^= FPSCR_FR;
}

}  // namespace dream::sh4

// ---------------------------------------------------------------------------------------------
// FPU helpers (WP1.3). The register file is 32 singles; a double DRn occupies FRn (high 32 bits of
// the IEEE pattern) and FRn+1 (low 32 bits), which is the SH-4's own layout, so doubles are
// assembled and split here rather than stored separately. c.fr is always the front bank.
// ---------------------------------------------------------------------------------------------

namespace dream::sh4 {

inline std::uint32_t f2u(float f) noexcept {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
inline float u2f(std::uint32_t u) noexcept {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

inline double get_dr(const Ctx& c, unsigned n) noexcept {
    const std::uint64_t bits = (static_cast<std::uint64_t>(f2u(c.fr[n])) << 32) | f2u(c.fr[n + 1]);
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
}
inline void set_dr(Ctx& c, unsigned n, double d) noexcept {
    std::uint64_t bits;
    std::memcpy(&bits, &d, 8);
    c.fr[n] = u2f(static_cast<std::uint32_t>(bits >> 32));
    c.fr[n + 1] = u2f(static_cast<std::uint32_t>(bits));
}
inline double get_xd(const Ctx& c, unsigned n) noexcept {
    const std::uint64_t bits = (static_cast<std::uint64_t>(f2u(c.xf[n])) << 32) | f2u(c.xf[n + 1]);
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
}
inline void set_xd(Ctx& c, unsigned n, double d) noexcept {
    std::uint64_t bits;
    std::memcpy(&bits, &d, 8);
    c.xf[n] = u2f(static_cast<std::uint32_t>(bits >> 32));
    c.xf[n + 1] = u2f(static_cast<std::uint32_t>(bits));
}

// 64-bit pair moves (FPSCR.SZ = 1). The register field's low bit selects the back bank (XDn).
inline void fmov_pair_load(Ctx& c, unsigned field, std::uint64_t v) noexcept {
    float* bank = (field & 1) ? c.xf : c.fr;
    const unsigned n = field & ~1u;
    bank[n] = u2f(static_cast<std::uint32_t>(v >> 32));
    bank[n + 1] = u2f(static_cast<std::uint32_t>(v));
}
inline std::uint64_t fmov_pair_store(const Ctx& c, unsigned field) noexcept {
    const float* bank = (field & 1) ? c.xf : c.fr;
    const unsigned n = field & ~1u;
    return (static_cast<std::uint64_t>(f2u(bank[n])) << 32) | f2u(bank[n + 1]);
}
// Memory order of a pair: the word at the lower address is FRn (the high half).
inline std::uint64_t read_pair(::dream::Memory& m, std::uint32_t addr) {
    return (static_cast<std::uint64_t>(m.read32(addr)) << 32) | m.read32(addr + 4);
}
inline void write_pair(::dream::Memory& m, std::uint32_t addr, std::uint64_t v) {
    m.write32(addr, static_cast<std::uint32_t>(v >> 32));
    m.write32(addr + 4, static_cast<std::uint32_t>(v));
}

// FTRC: truncate toward zero with saturation; NaN gives the negative-overflow pattern.
inline std::uint32_t ftrc(float f) noexcept {
    if (std::isnan(f))
        return 0x80000000u;
    if (f >= 2147483648.0f)
        return 0x7FFFFFFFu;
    if (f <= -2147483648.0f)
        return 0x80000000u;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(f));
}
inline std::uint32_t ftrc(double d) noexcept {
    if (std::isnan(d))
        return 0x80000000u;
    if (d >= 2147483648.0)
        return 0x7FFFFFFFu;
    if (d <= -2147483648.0)
        return 0x80000000u;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(d));
}

// FMAC: FRn = FR0 * FRm + FRn with a single rounding (the SH-4 FMAC is a fused unit). Recorded
// here as the one place the choice lives (ADR 16); WP1.5 checks it against the interpreter.
inline float fmac(float fr0, float frm, float frn) noexcept {
    return std::fmaf(fr0, frm, frn);
}

// FSCA: FPUL holds an angle as a 16.16 fixed fraction of a turn in its low 16 bits. The result is
// a table lookup (fsca.cpp) using the hardware-captured coefficients Flycast ships, not sin/cos:
// the real unit is a table too, and computing live under the guest's rounding mode gave
// sin(quarter turn) = 0x3F7FFFFF where hardware and the oracle give exactly 1.0.
namespace detail {
struct SinCos {
    float s, c;
};
const SinCos* fsca_table() noexcept;
}  // namespace detail

inline void fsca(Ctx& c, unsigned n) noexcept {
    const detail::SinCos& e = detail::fsca_table()[c.fpul & 0xFFFFu];
    c.fr[n] = e.s;
    c.fr[n + 1] = e.c;
}

// FIPR FVm,FVn: FR[n+3] = dot(FV[m], FV[n]). Products are exact in double (24+24 significand
// bits), the sum is accumulated in double left to right and rounded to single once: this is the
// reference (Flycast) evaluation and it is contraction-proof, since an FMA of an exact product
// equals the separate multiply and add.
inline void fipr(Ctx& c, unsigned m, unsigned n) noexcept {
    const float* a = &c.fr[n];
    const float* b = &c.fr[m];
    double d = static_cast<double>(a[0]) * b[0];
    d += static_cast<double>(a[1]) * b[1];
    d += static_cast<double>(a[2]) * b[2];
    d += static_cast<double>(a[3]) * b[3];
    c.fr[n + 3] = static_cast<float>(d);
}

// FTRV XMTRX,FVn: FV[n] = XMTRX * FV[n], XMTRX being the back bank as a 4x4 column-major matrix.
// Same double accumulation as FIPR, one row at a time, left to right.
inline void ftrv(Ctx& c, unsigned n) noexcept {
    const float* v = &c.fr[n];
    const float x = v[0], y = v[1], z = v[2], w = v[3];
    const float* xm = c.xf;
    double r[4];
    for (unsigned i = 0; i < 4; ++i) {
        r[i] = static_cast<double>(xm[i]) * x + static_cast<double>(xm[i + 4]) * y +
               static_cast<double>(xm[i + 8]) * z + static_cast<double>(xm[i + 12]) * w;
    }
    for (unsigned i = 0; i < 4; ++i) c.fr[n + i] = static_cast<float>(r[i]);
}

// LDS Rm,FPSCR: store, swap banks if FR changed, and program the host rounding/denormal mode.
inline void write_fpscr(Ctx& c, std::uint32_t v) noexcept {
    v &= 0x003FFFFFu;
    if ((v ^ c.fpscr) & FPSCR_FR) {
        for (int i = 0; i < 16; ++i) {
            const float tmp = c.fr[i];
            c.fr[i] = c.xf[i];
            c.xf[i] = tmp;
        }
    }
    c.fpscr = v;
    ::dream::fenv::apply(::dream::fenv::from_fpscr(v));
}

}  // namespace dream::sh4
