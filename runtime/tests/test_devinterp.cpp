// WP2.7: the dev interpreter as the function-table miss handler, and the release configuration's
// fault instead. Guest code is hand-assembled into RAM; addresses are chosen so nothing here
// collides with the translated stand-ins other tests register.
#include <initializer_list>

#include "dream/runtime/sh4/ops.h"
#include "dream/runtime/system.h"

#include "doctest.h"
#ifdef DREAM_DEV_INTERPRETER
#include "dream/runtime/devinterp/interpreter.h"
#endif

using namespace dream;

namespace {

#ifdef DREAM_DEV_INTERPRETER
void put(Memory& m, std::uint32_t at, std::initializer_list<std::uint16_t> words) {
    for (std::uint16_t w : words) {
        m.write16(at, w);
        at += 2;
    }
}
#endif

// Translated stand-ins. `caller` does what emitted code does at a jsr whose target the table
// cannot resolve: sets PR to the return address, PC to the call site, and goes through
// call_indirect. `leaf` is a translated callee for interpreted code to call natively.
void caller(sh4::Ctx& c, Memory& m) {
    c.pr = 0x8C030008u;
    c.pc = 0x8C030004u;
    sh4::call_indirect(c, m, 0x8C020000u);
    c.r[7] = 0x77u;  // runs after the interpreted callee returned
}
void leaf(sh4::Ctx& c, Memory&) {
    c.r[1] += 100u;
}

const sh4::FunctionEntry kFns[] = {{0x8C030000u, caller}, {0x8C030100u, leaf}};
struct Registrar {
    Registrar() { sh4::register_functions(kFns, 2); }
} g_registrar;

}  // namespace

#ifdef DREAM_DEV_INTERPRETER

TEST_CASE(
    "devinterp: an untranslated callee runs from memory and returns to the translated caller") {
    System sys;
    sys.install();
    auto& c = sys.ctx;
    sh4::write_sr(c, 0x700000F0u);
    // mov.l @r4,r0 ; add #5,r0 ; rts ; add #1,r0 (in the delay slot)
    put(sys.memory, 0x8C020000u, {0x6042u, 0x7005u, 0x000Bu, 0x7001u});
    sys.memory.write32(0x8C100000u, 40u);
    c.r[4] = 0x8C100000u;
    sh4::call_indirect(c, sys.memory, 0x8C030000u);
    CHECK(c.r[0] == 46u);
    CHECK(c.r[7] == 0x77u);
    CHECK(sys.untranslated.total() == 1);
    CHECK(sys.interpreter_runs == 1);
    CHECK(sys.interpreted_instructions == 4);
    CHECK(sys.interpreter_native_calls == 0);
    sh4::set_hooks(nullptr);
}

TEST_CASE("devinterp: interpreted code calls a translated function natively and resumes") {
    System sys;
    sys.install();
    auto& c = sys.ctx;
    sh4::write_sr(c, 0x700000F0u);
    // The shape every compiler emits around a call: PR saved and restored across the jsr.
    // sts.l pr,@-r15 ; mov.l @(3,pc),r2 -> 0x8C030100 ; jsr @r2 ; nop ; lds.l @r15+,pr ; rts ; nop
    // ; (pad) ; literal
    put(sys.memory, 0x8C020000u,
        {0x4F22u, 0xD203u, 0x420Bu, 0x0009u, 0x4F26u, 0x000Bu, 0x0009u, 0x0009u});
    sys.memory.write32(0x8C020010u, 0x8C030100u);
    c.r[15] = 0x8C00F400u;  // a stack for the PR save
    c.r[1] = 5u;
    sh4::call_indirect(c, sys.memory, 0x8C030000u);
    CHECK(c.r[1] == 105u);
    CHECK(c.r[7] == 0x77u);
    CHECK(c.r[15] == 0x8C00F400u);  // balanced
    CHECK(sys.interpreter_native_calls == 1);
    CHECK(sys.interpreted_instructions == 7);  // sts.l, mov.l, jsr, nop, lds.l, rts, nop
    sh4::set_hooks(nullptr);
}

TEST_CASE("devinterp: an untranslated interrupt handler runs in bank 1 and its RTE unwinds") {
    System sys;
    sys.install();
    auto& c = sys.ctx;
    c.vbr = 0x8C040000u;  // handler at VBR+0x600, an address with no translation
    sh4::write_sr(c, 0x40000000u);
    sys.memory.write32(0xA05F6930u, 0x8u);  // IML6NRM: VBlank-in -> IRL9
    // mov #8,r1 ; mov.l @(2,pc),r0 -> SB_ISTNRM ; mov.l r1,@r0 (acknowledge) ; mov #0x11,r2 ;
    // rte ; nop ; literal 0xA05F6900
    put(sys.memory, 0x8C040600u, {0xE108u, 0xD002u, 0x2012u, 0xE211u, 0x002Bu, 0x0009u});
    sys.memory.write32(0x8C04060Cu, 0xA05F6900u);
    c.r[2] = 0x2222u;  // bank 0 value, must survive
    sys.holly.raise(holly::Irq::VBlankIn);
    c.cycles = 100;
    c.next_event = 0;
    sh4::deliver_irq(c, sys.memory);
    CHECK(sys.interrupts_delivered == 1);
    CHECK(sys.interpreter_runs == 1);
    CHECK(!sys.holly.raised(holly::Irq::VBlankIn));  // acknowledged by the interpreted handler
    CHECK(c.r_bank[2] == 0x11u);                     // written while RB was set
    CHECK(c.r[2] == 0x2222u);                        // bank 0 restored by RTE
    CHECK(sh4::read_sr(c) == 0x40000000u);
    CHECK(sys.nesting == 0);
    sh4::set_hooks(nullptr);
}

TEST_CASE("devinterp: the step limit stops a runaway loop") {
    System sys;
    sys.install();
    auto& c = sys.ctx;
    put(sys.memory, 0x8C020000u, {0xAFFEu, 0x0009u});  // bra . ; nop
    c.pc = 0x8C020000u;
    devinterp::Options opt;
    opt.max_instructions = 50;
    devinterp::Stats st;
    CHECK(devinterp::run(c, sys.memory, 0x8C00FFF0u, opt, &st) == devinterp::Stop::StepLimit);
    CHECK(st.instructions >= 50);
    sh4::set_hooks(nullptr);
}

#else

TEST_CASE("release: an untranslated target is recorded and fatal") {
    System sys;
    sys.install();
    auto& c = sys.ctx;
    c.pc = 0x8C030004u;
    CHECK_THROWS_AS(sh4::call_indirect(c, sys.memory, 0x8C020000u), UntranslatedCall);
    CHECK(sys.untranslated.total() == 1);
    CHECK(sys.interpreter_runs == 0);
    sh4::set_hooks(nullptr);
}

#endif
