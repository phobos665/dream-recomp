#include "dream/runtime/fenv.h"

#include <cfenv>

#if defined(__x86_64__) || defined(_M_X64)
#define DREAM_HOST_X86_64 1
#include <immintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#define DREAM_HOST_ARM64 1
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#else
#error "dream::fenv: unsupported host ISA (need x86-64 or ARM64)"
#endif

namespace dream::fenv {
namespace {

#if defined(DREAM_HOST_X86_64)
constexpr unsigned kMxcsrFtz = 1u << 15;
constexpr unsigned kMxcsrDaz = 1u << 6;

void set_flush(bool on) noexcept {
    unsigned csr = _mm_getcsr();
    if (on) {
        csr |= kMxcsrFtz | kMxcsrDaz;
    } else {
        csr &= ~(kMxcsrFtz | kMxcsrDaz);
    }
    _mm_setcsr(csr);
}

bool get_flush() noexcept {
    return (_mm_getcsr() & kMxcsrFtz) != 0;
}

#elif defined(DREAM_HOST_ARM64)
constexpr std::uint64_t kFpcrFz = 1ull << 24;

std::uint64_t read_fpcr() noexcept {
#if defined(_MSC_VER)
    return static_cast<std::uint64_t>(_ReadStatusReg(ARM64_FPCR));
#else
    std::uint64_t v;
    asm volatile("mrs %0, fpcr" : "=r"(v));
    return v;
#endif
}

void write_fpcr(std::uint64_t v) noexcept {
#if defined(_MSC_VER)
    _WriteStatusReg(ARM64_FPCR, static_cast<__int64>(v));
#else
    asm volatile("msr fpcr, %0" : : "r"(v));
#endif
}

void set_flush(bool on) noexcept {
    std::uint64_t v = read_fpcr();
    v = on ? (v | kFpcrFz) : (v & ~kFpcrFz);
    write_fpcr(v);
}

bool get_flush() noexcept {
    return (read_fpcr() & kFpcrFz) != 0;
}
#endif

}  // namespace

void apply(Mode mode) noexcept {
    std::fesetround(mode.rounding == Rounding::Nearest ? FE_TONEAREST : FE_TOWARDZERO);
    set_flush(mode.flush_denormals);
}

Mode current() noexcept {
    return Mode{std::fegetround() == FE_TOWARDZERO ? Rounding::TowardZero : Rounding::Nearest,
                get_flush()};
}

void reset_host() noexcept {
    apply(Mode{Rounding::Nearest, false});
}

}  // namespace dream::fenv
