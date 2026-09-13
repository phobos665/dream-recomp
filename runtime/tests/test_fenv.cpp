#include <cstring>

#include "dream/runtime/fenv.h"

#include "doctest.h"

namespace {

// Operands go through volatile so the compiler cannot fold the arithmetic at build time under the
// build machine's default rounding; the point is to observe the host control register at run time.
float divide(float a, float b) {
    volatile float va = a, vb = b;
    return va / vb;
}

std::uint32_t bits(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    return u;
}

}  // namespace

TEST_CASE("fpscr decoding") {
    using namespace dream::fenv;
    CHECK(from_fpscr(kKatanaDefaultFpscr) == Mode{Rounding::TowardZero, true});
    CHECK(from_fpscr(0) == Mode{Rounding::Nearest, false});
    CHECK(from_fpscr(0x00040000) == Mode{Rounding::Nearest, true});
    CHECK(from_fpscr(0x3) == Mode{Rounding::TowardZero, false});  // reserved RM maps to toward-zero
}

TEST_CASE("rounding mode changes 1/3 in the last bit") {
    using namespace dream::fenv;
    apply(Mode{Rounding::Nearest, false});
    const std::uint32_t nearest = bits(divide(1.0f, 3.0f));
    apply(Mode{Rounding::TowardZero, false});
    const std::uint32_t toward_zero = bits(divide(1.0f, 3.0f));
    reset_host();
    CHECK(nearest == 0x3EAAAAABu);
    CHECK(toward_zero == 0x3EAAAAAAu);
}

TEST_CASE("denormal flushing") {
    using namespace dream::fenv;
    volatile float tiny = 1e-40f;  // a denormal
    volatile float one = 1.0f;

    apply(Mode{Rounding::Nearest, false});
    const float kept = tiny * one;
    apply(Mode{Rounding::Nearest, true});
    const float flushed = tiny * one;
    reset_host();

    CHECK(kept != 0.0f);
    CHECK(flushed == 0.0f);
}

TEST_CASE("current() reads back what apply() set") {
    using namespace dream::fenv;
    apply(Mode{Rounding::TowardZero, true});
    CHECK(current() == Mode{Rounding::TowardZero, true});
    apply(Mode{Rounding::Nearest, false});
    CHECK(current() == Mode{Rounding::Nearest, false});
    reset_host();
}
