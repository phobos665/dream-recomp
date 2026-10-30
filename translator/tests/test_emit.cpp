// Emitter passes that read only the image, checked on hand-built images.
#include <cstdint>
#include <initializer_list>
#include <string>

#include "dream/translator/analysis/discover.h"
#include "dream/translator/emit.h"

#include "doctest.h"

using dream::translator::discover;
using dream::translator::DiscoverOptions;
using dream::translator::emit_unit;
using dream::translator::EmitOptions;
using dream::translator::FunctionSpec;
using dream::translator::Image;

namespace {
Image image_of(std::uint32_t base, std::initializer_list<std::uint16_t> words) {
    Image img;
    img.base = base;
    for (std::uint16_t w : words) {
        img.bytes.push_back(static_cast<std::uint8_t>(w));
        img.bytes.push_back(static_cast<std::uint8_t>(w >> 8));
    }
    return img;
}
}  // namespace

TEST_CASE("emit: a store just below 4 GB does not stall the patched-literal scan") {
    // Data that discovery took for code, as at 0x0C0E983E in Daytona USA 2001:
    //   mov #-2,r0 / mov.b r0,@(0,r0) / rts / nop
    // The store lands at 0xFFFFFFFE. The scan for pool words the unit writes stepped through the
    // words it touches with a 32-bit counter, which wrapped past zero and never reached the end.
    const Image img = image_of(0x0C010000, {0xE0FE, 0x8000, 0x000B, 0x0009});
    const auto r =
        emit_unit(img, {FunctionSpec{img.base, img.base + 8, "topstore"}}, EmitOptions{});
    CHECK(r.patched_words == 0);
    CHECK(r.source.find("topstore") != std::string::npos);
}

TEST_CASE("emit: a store into the image's own pool is still found") {
    // mov.l @(4,pc),r1 (the pool word at +8) / mov.l r0,@r1 / rts / nop / pool: base + 8
    const std::uint32_t base = 0x0C010000, pool = base + 8;
    const Image img =
        image_of(base, {0xD101, 0x2102, 0x000B, 0x0009, static_cast<std::uint16_t>(pool),
                        static_cast<std::uint16_t>(pool >> 16)});
    const auto r = emit_unit(img, {FunctionSpec{base, base + 8, "poolstore"}}, EmitOptions{});
    CHECK(r.patched_words == 1);
}

TEST_CASE("discover: a branch into a delay slot carries on after the branch pair") {
    // tests/sh4/programs/p14_slotjoin.s, from Daytona USA 2001's 0x0c0244e0: with r4 = 0 the bt/s
    // enters the slot of `jmp @r0` to reach the rest of the body. The slot is decoded with the jmp,
    // so the walk used to stop there and end the function at 0x0C, cutting that body off.
    const std::uint32_t base = 0x0C040000;
    const Image img = image_of(base, {0x6043, 0x2008, 0x8D01, 0x7501, 0x402B, 0x7502, 0x6053,
                                      0x000B, 0x7064, 0x000B, 0xE007});
    DiscoverOptions opt;
    opt.seeds = {base};
    opt.follow_pointers = false;
    opt.aggressive_sweep = false;
    const auto r = discover(img, opt);
    REQUIRE(!r.functions.empty());
    CHECK(r.functions.front().entry == base);
    CHECK(r.functions.front().end == base + 0x12);  // through the rts and its slot
}
