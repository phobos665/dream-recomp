#include "dream/runtime/sh4/abi.h"
#include "dream/runtime/system.h"

#include "doctest.h"

using namespace dream;

namespace {
int hits_a = 0, hits_b = 0;
void fa(sh4::Ctx&, Memory&) {
    ++hits_a;
}
void fb(sh4::Ctx&, Memory&) {
    ++hits_b;
}
const std::uint16_t sig_a[] = {0x6206, 0xD002};
const std::uint16_t sig_b[] = {0x64F6, 0x60F6};
const sh4::FunctionEntry overlay_table[] = {{0x8C0F0000u, &fa, sig_a, 2},
                                            {0x8C0F0000u, &fb, sig_b, 2}};
}  // namespace

TEST_CASE("overlay: one address dispatches to the translation whose code is in RAM") {
    System sys;
    sys.install();
    sh4::register_functions(overlay_table, 2);
    auto& m = sys.memory;
    m.write16(0x8C0F0000u, 0x6206);
    m.write16(0x8C0F0002u, 0xD002);
    sh4::call_indirect(sys.ctx, m, 0x8C0F0000u);
    CHECK(hits_a == 1);
    CHECK(hits_b == 0);
    m.write16(0x8C0F0000u, 0x64F6);
    m.write16(0x8C0F0002u, 0x60F6);
    sh4::call_indirect(sys.ctx, m, 0xAC0F0000u);  // any RAM alias
    CHECK(hits_a == 1);
    CHECK(hits_b == 1);
    CHECK(sh4::find_function(0x8C0F0000u) == nullptr);  // no memory to check against
    // Neither translation matches: the address is untranslated for the miss handler.
    m.write16(0x8C0F0000u, 0x000B);  // rts
    m.write16(0x8C0F0002u, 0x0009);  // nop
    sys.ctx.pr = 0x8C0F0100u;
#ifdef DREAM_DEV_INTERPRETER
    sh4::call_indirect(sys.ctx, m, 0x8C0F0000u);
#else
    CHECK_THROWS_AS(sh4::call_indirect(sys.ctx, m, 0x8C0F0000u), UntranslatedCall);
#endif
    CHECK(hits_a == 1);
    CHECK(hits_b == 1);
    CHECK(sys.untranslated.total() == 1);
}
