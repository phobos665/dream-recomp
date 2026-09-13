// ARM7DI core and AICA control block (WP2.5). The ARM programs are hand-assembled words placed in
// sound RAM; encodings were checked against arm-eabi-as in the toolchain container.
#include <cstdint>
#include <vector>

#include "dream/runtime/aica/aica.h"
#include "dream/runtime/system.h"

#include "doctest.h"

namespace {

struct Rig {
    dream::System sys;
    dream::aica::Aica aica{sys.sched, sys.holly, sys.memory};

    void load(const std::vector<std::uint32_t>& words, std::uint32_t at = 0) {
        for (std::size_t i = 0; i < words.size(); ++i) {
            const std::uint32_t w = words[i];
            std::uint8_t* p = sys.memory.aram() + at + 4 * i;
            p[0] = static_cast<std::uint8_t>(w);
            p[1] = static_cast<std::uint8_t>(w >> 8);
            p[2] = static_cast<std::uint8_t>(w >> 16);
            p[3] = static_cast<std::uint8_t>(w >> 24);
        }
    }
    void release_arm() {
        aica.write(dream::aica::Aica::kRegBase + dream::aica::Aica::kArmRst, 0, 1);
    }
    std::uint32_t aram32(std::uint32_t a) const {
        const std::uint8_t* p = const_cast<dream::System&>(sys).memory.aram() + a;
        return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    }
};

constexpr std::uint32_t kHalt = 0xEAFFFFFEu;  // b .

}  // namespace

TEST_CASE("arm7: data processing and flags") {
    Rig r;
    // mov r0,#5; mov r1,#7; add r2,r0,r1; subs r3,r2,#12; b .
    r.load({0xE3A00005u, 0xE3A01007u, 0xE0802001u, 0xE252300Cu, kHalt});
    r.release_arm();
    CHECK(r.aica.arm.enabled());
    r.aica.arm.run(2000);
    CHECK(r.aica.arm.reg(2) == 12);
    CHECK(r.aica.arm.reg(3) == 0);
    CHECK((r.aica.arm.cpsr() & 0x40000000u) != 0);  // Z
    CHECK(r.aica.arm.instructions > 4);
}

TEST_CASE("arm7: loads and stores to sound RAM") {
    Rig r;
    // mov r0,#0x100; mov r1,#0xAB; str r1,[r0]; ldr r2,[r0]; strb r1,[r0,#4]; ldrb r3,[r0,#4]; b .
    r.load({0xE3A00C01u, 0xE3A010ABu, 0xE5801000u, 0xE5902000u, 0xE5C01004u, 0xE5D03004u, kHalt});
    r.release_arm();
    r.aica.arm.run(2000);
    CHECK(r.aram32(0x100) == 0xABu);
    CHECK(r.aica.arm.reg(2) == 0xABu);
    CHECK(r.aica.arm.reg(3) == 0xABu);
}

TEST_CASE("arm7: loop with a conditional branch") {
    Rig r;
    // mov r0,#0; mov r1,#10; L: add r0,r0,#3; subs r1,r1,#1; bne L; b .
    r.load({0xE3A00000u, 0xE3A0100Au, 0xE2800003u, 0xE2511001u, 0x1AFFFFFCu, kHalt});
    r.release_arm();
    r.aica.arm.run(4000);
    CHECK(r.aica.arm.reg(0) == 30);
    CHECK(r.aica.arm.reg(1) == 0);
}

TEST_CASE("arm7: block store and load through the stack pointer") {
    Rig r;
    // mov r13,#0x200; mov r0,#1; mov r1,#2; stmdb r13!,{r0,r1}; mov r0,#0; mov r1,#0;
    // ldmia r13!,{r0,r1}; b .
    r.load({0xE3A0DC02u, 0xE3A00001u, 0xE3A01002u, 0xE92D0003u, 0xE3A00000u, 0xE3A01000u,
            0xE8BD0003u, kHalt});
    r.release_arm();
    r.aica.arm.run(3000);
    CHECK(r.aica.arm.reg(0) == 1);
    CHECK(r.aica.arm.reg(1) == 2);
    CHECK(r.aica.arm.reg(13) == 0x200u);
    CHECK(r.aram32(0x1F8) == 1);
    CHECK(r.aram32(0x1FC) == 2);
}

TEST_CASE("arm7: software interrupt through the vector at 0x08") {
    Rig r;
    // 0x00: b main (0x20); 0x04: b .; 0x08: mov r7,#0x99; 0x0C: movs pc,lr; 0x10..0x1C: b .
    // main: mov r0,#1; swi 0; mov r1,#2; b .
    r.load({0xEA000006u, kHalt, 0xE3A07099u, 0xE1B0F00Eu, kHalt, kHalt, kHalt, kHalt, 0xE3A00001u,
            0xEF000000u, 0xE3A01002u, kHalt});
    r.release_arm();
    r.aica.arm.run(4000);
    CHECK(r.aica.arm.swis == 1);
    CHECK(r.aica.arm.reg(7) == 0x99u);
    CHECK(r.aica.arm.reg(0) == 1);
    CHECK(r.aica.arm.reg(1) == 2);
    CHECK(r.aica.arm.mode() == 0x13);
}

TEST_CASE("arm7: FIQ from the AICA interrupt controller, level from SCILV") {
    Rig r;
    using A = dream::aica::Aica;
    // 0x00: b main (0x24); 0x04..0x18: b .; 0x1C: mov r7,#0x77; b . (FIQ vector: the handler
    // parks with FIQ masked, as taking the exception leaves it); 0x24 main: mrs r0,cpsr;
    // bic r0,r0,#0x40; msr cpsr_c,r0; b .
    r.load({0xEA000007u, kHalt, kHalt, kHalt, kHalt, kHalt, kHalt, 0xE3A07077u, kHalt, 0xE10F0000u,
            0xE3C00040u, 0xE121F000u, kHalt});
    r.release_arm();
    r.aica.arm.run(2000);
    CHECK(r.aica.arm.fiq_enabled());
    // Enable the CPU-to-ARM source at level 3 and fire it from the SH-4 side.
    r.aica.write(A::kRegBase + A::kScilv0, 0x20, 4);
    r.aica.write(A::kRegBase + A::kScilv1, 0x20, 4);
    r.aica.write(A::kRegBase + A::kScieb, A::kIntScpu, 4);
    r.aica.write(A::kRegBase + A::kScipd, A::kIntScpu, 4);
    r.aica.arm.run(2000);
    CHECK(r.aica.arm.fiqs == 1);
    CHECK(r.aica.arm.reg(7) == 0x77u);
    CHECK(r.aica.arm.mode() == 0x11);
    CHECK(r.aica.arm_reg_read(A::kIntReqL, 4) == 3);
    // Acknowledging through INTREQ M drops the latch; the still-pending source re-latches.
    r.aica.arm_reg_write(A::kIntReqM, 1, 4);
    CHECK(r.aica.arm_reg_read(A::kIntReqL, 4) == 3);
    r.aica.write(A::kRegBase + A::kScire, A::kIntScpu, 4);
    r.aica.arm_reg_write(A::kIntReqM, 1, 4);
    CHECK(r.aica.arm.reg(dream::aica::INTR_PEND) == 0);
}

TEST_CASE("aica: timer A wraps, interrupts both sides, SH-4 line follows MCIEB") {
    Rig r;
    using A = dream::aica::Aica;
    r.aica.write(A::kRegBase + A::kTimerA, 0x00FE, 4);  // md 0: one step per sample, count 0xFE
    r.aica.write(A::kRegBase + A::kMcieb, A::kIntTimerA, 4);
    r.aica.sample_tick();
    CHECK((r.aica.reg16(A::kScipd) & A::kIntTimerA) == 0);
    r.aica.sample_tick();
    CHECK((r.aica.reg16(A::kScipd) & A::kIntTimerA) != 0);
    CHECK((r.aica.reg16(A::kMcipd) & A::kIntTimerA) != 0);
    CHECK(r.aica.timer_irqs == 1);
    CHECK(r.sys.holly.raised(dream::holly::Irq::AicaIrq));
    r.aica.write(A::kRegBase + A::kMcire, A::kIntTimerA, 4);
    CHECK_FALSE(r.sys.holly.raised(dream::holly::Irq::AicaIrq));
    // SAMPLE_DONE is set every tick and readable
    CHECK((r.aica.reg16(A::kScipd) & A::kIntSampleDone) != 0);
    // md 2: one step every four samples
    r.aica.write(A::kRegBase + A::kTimerA, 0x02FF, 4);
    for (int i = 0; i < 3; ++i) r.aica.sample_tick();
    CHECK((r.aica.reg16(A::kTimerA) & 0xFFu) == 0xFFu);
    r.aica.sample_tick();
    CHECK((r.aica.reg16(A::kTimerA) & 0xFFu) == 0);
}

TEST_CASE("aica: ARM reset register and the scheduled sample tick") {
    Rig r;
    using A = dream::aica::Aica;
    CHECK(r.aica.arm_in_reset());
    CHECK_FALSE(r.aica.arm.enabled());
    r.load({0xE3A00005u, kHalt});
    r.release_arm();
    CHECK_FALSE(r.aica.arm_in_reset());
    // Two sample ticks on the virtual clock run the ARM for its share of cycles.
    r.sys.sched.advance(2 * A::kSh4CyclesPerSample + 1);
    CHECK(r.aica.samples == 2);
    CHECK(r.aica.arm.reg(0) == 5);
    r.aica.write(A::kRegBase + A::kArmRst, 1, 1);
    CHECK(r.aica.arm_in_reset());
    CHECK_FALSE(r.aica.arm.enabled());
    // 16-bit write: VREG in the high byte, ARMRST low
    r.aica.write(A::kRegBase + A::kArmRst, 0x0100, 2);
    CHECK(r.aica.arm.enabled());
    CHECK(r.aica.arm.next_pc() == 0);  // reset again on release
    CHECK(r.aica.read(A::kRegBase + A::kArmRst, 2) == 0x0100u);
}

TEST_CASE("aica: an immediate operand rotated by zero is the immediate itself") {
    // A data-processing immediate is an 8-bit value rotated right by an even amount, and almost
    // every one uses no rotation. That case has to be spelled out, because `v << (32 - 0)` is a
    // shift by 32: undefined in C++, and answered by the two host ISAs as a shift by zero, so the
    // wrong code gave the right number until a compiler decided otherwise. The arithmetic form was
    // unguarded, which put every ADD, SUB, CMP and CMN with a plain immediate into that state.
    Rig r;
    r.load({
        0xE3A00000u,  // mov  r0, #0
        0xE2801012u,  // add  r1, r0, #0x12      rotate 0: the immediate is 0x12
        0xE2412008u,  // sub  r2, r1, #8         rotate 0
        0xE3A03102u,  // mov  r3, #0x80000000    rotate 2: 0x02 ror 4
        0xE2434C01u,  // sub  r4, r3, #0x100     rotate 8, to keep a rotating case covered
        kHalt,
    });
    r.release_arm();
    r.sys.sched.advance(8 * dream::aica::Aica::kSh4CyclesPerSample + 1);
    CHECK(r.aica.arm.reg(1) == 0x12u);
    CHECK(r.aica.arm.reg(2) == 0x0Au);
    CHECK(r.aica.arm.reg(3) == 0x80000000u);
    CHECK(r.aica.arm.reg(4) == 0x7FFFFF00u);
}

TEST_CASE("aica: comparing against an unrotated immediate sets the flags") {
    // CMP and CMN take the same operand path and were reported by the sanitizer alongside ADD and
    // SUB, so the flag they produce is worth pinning too.
    Rig r;
    r.load({
        0xE3A00005u,  // mov  r0, #5
        0xE3500005u,  // cmp  r0, #5       equal, so Z is set
        0x03A01001u,  // moveq r1, #1
        0x13A01002u,  // movne r1, #2
        kHalt,
    });
    r.release_arm();
    r.sys.sched.advance(8 * dream::aica::Aica::kSh4CyclesPerSample + 1);
    CHECK(r.aica.arm.reg(1) == 1u);
}
