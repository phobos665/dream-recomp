// Runs the translated test programs (tests/sh4/programs) and checks their results.
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "dream/runtime/fenv.h"
#include "dream/runtime/memory.h"
#include "dream/runtime/sh4/ctx.h"
#include "dream/runtime/sh4/ops.h"

#include "doctest.h"
#include "emitted_programs.h"

using dream::sh4::Ctx;

namespace {
// The guest image must be present in RAM: code reads its own literal pools and tables through
// ordinary loads (only PC-relative literals are folded by the emitter).
void load_image(dream::BareMemory& m, const char* bin, std::uint32_t base) {
    std::ifstream in(std::string(DREAM_SH4_PROGRAMS_DIR) + "/" + bin, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "missing program binary " << bin);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        m.write8(base + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(bytes[i]));
    }
}

Ctx fresh() {
    Ctx c{};
    c.sr = 0x700000F0;  // MD, RB=0, BL, IMASK=15 (Katana startup values)
    // Katana default FPSCR: PR=0, SZ=0, DN=1, RM=round-toward-zero. Applied through write_fpscr
    // so the host rounding and denormal modes follow, as the runtime does at guest entry; setting
    // the field alone leaves the host at round-to-nearest and the FPU results drift in the last
    // bit.
    dream::sh4::write_fpscr(c, 0x00040001);
    c.r[15] = 0x8C00F400;  // stack
    return c;
}
}  // namespace

TEST_CASE("p1_sum: delayed loop with the add in the slot computes n*n") {
    dream::BareMemory m;
    load_image(m, "p1_sum.bin", 0x8C010000);
    for (std::uint32_t n : {0u, 1u, 5u, 100u}) {
        Ctx c = fresh();
        c.r[4] = n;
        dream::gen::sum(c, m);
        CHECK(c.r[0] == n * n);
    }
}

TEST_CASE("p2_alu: division, shifts, carries, rotates, swaps") {
    dream::BareMemory m;
    load_image(m, "p2_alu.bin", 0x8C010000);
    Ctx c = fresh();
    dream::gen::alu(c, m);
    CHECK(c.r[0] == (14u | 0x34u));  // 100/7 = 14, then or #0x34
    CHECK(c.r[2] == 5u);             // shld 0x40 >> 4 = 4, plus shad 8 >> 3 = 1
    CHECK(c.r[3] == 0xFFFFFFFEu);    // exts.b of 0xfe
    CHECK(c.r[5] == 1u);             // addc with carry in
    CHECK(c.r[4] == 1u);             // rotcl then rotcr restores
    CHECK(c.r[6] == 1u);             // movt after rotcr: bit 0 of 3
    CHECK(c.r[7] == 0x12u);          // shll8 then swap.b
    CHECK(c.t == 1u);                // last T from rotcr
}

TEST_CASE("p3_mem: addressing modes and literal pools") {
    dream::BareMemory m;
    load_image(m, "p3_mem.bin", 0x8C010000);
    Ctx c = fresh();
    const std::uint32_t buf = 0x8C100000;
    c.r[4] = buf;
    dream::gen::mem(c, m);
    CHECK(m.read32(buf) == 0x11223344u);
    CHECK(m.read32(buf + 4) == 0xFFFFFFFEu);
    CHECK(m.read8(buf + 8) == 0x5Au);
    CHECK(m.read8(buf + 9) == 0x7Bu);
    CHECK(m.read16(buf + 10) == 0x0066u);
    CHECK(c.r[5] == 0x11223344u);
    CHECK(c.r[6] == 0xFFFFFFFEu);
    CHECK(c.r[0] == 0x8C010034u);  // mova of the literal pool (aligned)
    CHECK(c.r[7] == 0x7Bu);
    CHECK(c.r[2] == 0x66u);
    CHECK(c.r[3] == 12u);
    CHECK(c.r[9] == 0x11223344u);
    CHECK(c.r[8] == buf + 4);
}

TEST_CASE("p12_selfcmp: a register compared with itself gives the hardware's constant") {
    // The emitter special-cases these, because emitting them literally produces `c.r[15] >
    // c.r[15]` and -Wtautological-compare rejects it under -Werror: a title using the idiom would
    // not compile. The constant it substitutes has to be the one the hardware would have produced,
    // which is what this checks, for the unsigned and signed forms and for a negative register.
    dream::BareMemory m;
    load_image(m, "p12_selfcmp.bin", 0x8C010000);
    Ctx c = fresh();
    dream::gen::selfcmp(c, m);
    const std::uint32_t buf = 0x8C020000;
    // r1 = 5: eq, hs, ge are true; hi and gt are false.
    CHECK(m.read8(buf + 0) == 1u);  // cmp/eq
    CHECK(m.read8(buf + 1) == 1u);  // cmp/hs
    CHECK(m.read8(buf + 2) == 1u);  // cmp/ge
    CHECK(m.read8(buf + 3) == 0u);  // cmp/hi
    CHECK(m.read8(buf + 4) == 0u);  // cmp/gt
    // r3 = -1: the same, which is the point. The signed forms are where "compared with itself"
    // could plausibly have differed.
    CHECK(m.read8(buf + 5) == 1u);  // cmp/eq
    CHECK(m.read8(buf + 6) == 1u);  // cmp/hs
    CHECK(m.read8(buf + 7) == 1u);  // cmp/ge
    CHECK(m.read8(buf + 8) == 0u);  // cmp/hi
    CHECK(m.read8(buf + 9) == 0u);  // cmp/gt
}

TEST_CASE("p4_calls: bsr/jsr/rts, delay-slot hazards, bra with slot") {
    dream::BareMemory m;
    load_image(m, "p4_calls.bin", 0x8C010000);
    for (std::uint32_t n : {0u, 7u, 1000u}) {
        Ctx c = fresh();
        const std::uint32_t sp = c.r[15];
        c.r[4] = n;
        dream::gen::calls(c, m);
        CHECK(c.r[0] == ((n + 1) * 2) + 100 + 5);
        CHECK(c.r[15] == sp);  // pr saved and restored, stack balanced
    }
}

TEST_CASE("p5_fpu: single, double via lds fpscr, pair move via fschg") {
    dream::BareMemory m;
    load_image(m, "p5_fpu.bin", 0x8C010000);
    Ctx c = fresh();
    const std::uint32_t buf = 0x8C100000;
    c.r[4] = buf;
    dream::gen::fpu(c, m);
    CHECK(c.fr[1] == 2.0f);
    CHECK(c.fr[2] == 2.0f);
    CHECK(c.fr[3] == -2.0f);
    CHECK(c.fr[4] == 5.5f);
    CHECK(c.fr[5] == 1.0f);
    CHECK(c.fr[6] == 0.0f);
    CHECK(c.fr[7] == 5.5f);
    CHECK(c.r[1] == 3u);
    CHECK(c.r[2] == 1u);
    CHECK(m.read32(buf) == 0x40000000u);      // 2.0f
    CHECK(m.read32(buf + 4) == 0x40B00000u);  // 5.5f
    CHECK(m.read32(buf + 8) == 0x40B00000u);
    // double section
    CHECK(c.r[3] == 1u);
    CHECK(c.r[6] == 7u);
    CHECK(dream::sh4::get_dr(c, 10) == 7.0);
    CHECK(m.read32(buf + 12) == 0x401C0000u);  // high word of 7.0 first
    CHECK(m.read32(buf + 16) == 0u);
    CHECK(c.fpscr == 0x00040001u);  // restored, SZ toggled back
    // the lds fpscr writes programmed the host rounding mode (ADR 16)
    CHECK(dream::fenv::current() == dream::fenv::Mode{dream::fenv::Rounding::TowardZero, true});
    dream::fenv::reset_host();
}

TEST_CASE("p6_switch: braf offset table and jmp address table") {
    dream::BareMemory m;
    load_image(m, "p6_switch.bin", 0x8C010000);
    const std::uint32_t expect[] = {11u, 22u, 33u, 44u};
    for (std::uint32_t i = 0; i < 4; ++i) {
        Ctx c = fresh();
        c.r[4] = i;
        dream::gen::sw(c, m);
        CHECK(c.r[0] == expect[i]);
    }
    Ctx c = fresh();
    c.r[4] = 7;
    dream::gen::sw(c, m);
    CHECK(c.r[0] == 0xFFFFFFFFu);
}

// Expected values are the Flycast interpreter's final state for this program, captured by the
// differential harness on macOS ARM64 (docs/differential-harness.md). Running them here, on every
// CI host, is the cross-ISA check ADR 16 asks for: a mismatch on x86-64 is a build failure.
TEST_CASE("p7_fpuedge: FPU edge cases match the interpreter's captured state") {
    dream::BareMemory m;
    load_image(m, "p7_fpuedge.bin", 0x8C010000);
    Ctx c = fresh();
    const std::uint32_t buf = 0x8C100000;
    c.r[4] = buf;
    dream::gen::fpuedge(c, m);
    // FTRC saturation and specials: 3e9, -3e9, +inf, NaN, 2^31, -(2^31+256), -2.5
    CHECK(c.r[1] == 0x7FFFFFFFu);
    CHECK(c.r[2] == 0x80000000u);
    CHECK(c.r[3] == 0x7FFFFFFFu);
    CHECK(c.r[6] == 0x80000000u);
    CHECK(c.r[7] == 0x7FFFFFFFu);
    CHECK(c.r[8] == 0x80000000u);
    CHECK(c.r[9] == 0xFFFFFFFEu);
    CHECK(c.r[10] == 0u);           // NaN == NaN
    CHECK(c.r[11] == 0u);           // NaN > x
    CHECK(c.r[13] == 0x3F9837F0u);  // (float)sqrt((double)sqrt2f) via FCNVSD/FCNVDS
    const std::uint32_t fr[16] = {0x3FF306FE, 0x0776695D, 0x28800000, 0x3F21E89B,
                                  0xBF89A027, 0x40C3A19A, 0x3E801A37, 0xBF8DC28D,
                                  0x3F8CCCCD, 0x400CCCCD, 0x40533333, 0x40533333,
                                  0x3FB504F3, 0x3FDDB3D9, 0x3F42F287, 0x3F25ED1F};
    const std::uint32_t xf[16] = {0x11111111, 0x22222222, 0x33333333, 0x44444444,
                                  0xBA83126F, 0x40E00000, 0x3A83126F, 0x41480000,
                                  0xC0D80000, 0x40490FD0, 0x3F000000, 0xBF800000,
                                  0x40000000, 0x411FD70A, 0x3D800000, 0xC2053333};
    for (unsigned i = 0; i < 16; ++i) {
        CAPTURE(i);
        CHECK(dream::sh4::f2u(c.fr[i]) ==
              fr[i]);  // fr2: fused FMAC 2^-46; fr4..7: FTRV; fr11: FIPR
        CHECK(dream::sh4::f2u(c.xf[i]) == xf[i]);  // xf0..3 from SZ=1 pair loads, rest the matrix
    }
    // [0],[4] FLOAT of INT_MAX/INT_MIN+1 under round-toward-zero; [8] flushed denormal;
    // [12] 1/3 toward zero (a folded constant gives ...AB); [16..28] FSCA 90 and 45 degrees.
    const std::uint32_t mem[8] = {0x4EFFFFFF, 0xCEFFFFFF, 0x00000000, 0x3EAAAAAA,
                                  0x3F800000, 0x80000000, 0x3F3504F3, 0x3F3504F3};
    for (unsigned i = 0; i < 8; ++i) {
        CAPTURE(i);
        CHECK(m.read32(buf + 4 * i) == mem[i]);
    }
    CHECK(c.fpscr == 0x00040001u);
}
