#include <vector>

#include "dream/runtime/sh4/ops.h"
#include "dream/runtime/system.h"

#include "doctest.h"

using namespace dream;

TEST_CASE("scheduler: events run in deadline order and may re-arm") {
    sched::Scheduler s;
    std::vector<std::pair<std::string, std::uint64_t>> log;
    int a = s.add("a", [&](std::uint64_t now, std::uint64_t) { log.push_back({"a", now}); });
    int b = s.add("b", [&](std::uint64_t now, std::uint64_t) {
        log.push_back({"b", now});
        s.request(b, 50);  // periodic
    });
    s.request(a, 100);
    s.request(b, 30);
    CHECK(s.next_deadline() == 30);
    s.advance(120);
    // b at 30 and 80, a at 100; b's re-arm for 130 is beyond the target.
    REQUIRE(log.size() == 3);
    CHECK(log[0] == std::make_pair(std::string("b"), std::uint64_t{30}));
    CHECK(log[1] == std::make_pair(std::string("b"), std::uint64_t{80}));
    CHECK(log[2] == std::make_pair(std::string("a"), std::uint64_t{100}));
    CHECK(s.deadline(b) == 130);
    CHECK(s.now() == 120);
    CHECK(s.armed(b));
    CHECK(!s.armed(a));
    s.cancel(b);
    CHECK(s.next_deadline() == sched::kNever);
}

TEST_CASE("intc: selection honours priorities, IMASK and BL") {
    sh4::Intc intc;
    CHECK(!intc.select(0x700000F0u));
    intc.set_pending(sh4::Irq::Irl9, true);
    CHECK(!intc.select(0x700000F0u));              // IMASK 15 blocks everything
    CHECK(!intc.select(0x10000000u | (5u << 4)));  // BL set
    auto s = intc.select(5u << 4);                 // IMASK 5 < priority 6
    REQUIRE(s);
    CHECK(s->irq == sh4::Irq::Irl9);
    CHECK(s->intevt == 0x320u);
    CHECK(!intc.select(6u << 4));                  // IMASK 6 blocks priority 6
    intc.write(sh4::Intc::kBase + 4, 0xF000u, 2);  // IPRA: TMU0 priority 15
    intc.set_pending(sh4::Irq::Tmu0, true);
    s = intc.select(5u << 4);
    REQUIRE(s);
    CHECK(s->irq == sh4::Irq::Tmu0);
    CHECK(s->intevt == 0x400u);
    CHECK(intc.read(sh4::Intc::kBase + 4, 2) == 0xF000u);
    intc.set_pending(sh4::Irq::Tmu0, false);
    CHECK(intc.select(5u << 4)->irq == sh4::Irq::Irl9);
    intc.write(sh4::Intc::kBase + 4, 0x0000u, 2);  // TMU0 priority 0: never delivered
    intc.set_pending(sh4::Irq::Tmu0, true);
    CHECK(intc.select(0)->irq == sh4::Irq::Irl9);
}

TEST_CASE("tmu: counts down on the prescaled clock, underflows, reloads and interrupts") {
    System sys;
    auto& m = sys.memory;
    const std::uint32_t base = sh4::Tmu::kBase;
    m.write32(base + 0x08, 99);      // TCOR0
    m.write32(base + 0x0C, 99);      // TCNT0
    m.write16(base + 0x10, 0x0020);  // TCR0: TPSC 0 (CPU/16), UNIE
    m.write8(base + 0x04, 1);        // TSTR: start channel 0
    CHECK(m.read32(base + 0x0C) == 99u);
    sys.sched.advance(16 * 10);
    CHECK(m.read32(base + 0x0C) == 89u);
    CHECK(!sys.intc.pending(sh4::Irq::Tmu0));
    // 99 ticks bring the counter to 0; the 100th underflows: UNF, interrupt, reload from TCOR.
    sys.sched.advance(16 * 89);
    CHECK(m.read32(base + 0x0C) == 0u);
    CHECK(!sys.intc.pending(sh4::Irq::Tmu0));
    sys.sched.advance(16);
    CHECK(sys.intc.pending(sh4::Irq::Tmu0));
    CHECK((m.read16(base + 0x10) & 0x100u) != 0);  // UNF
    CHECK(m.read32(base + 0x0C) == 99u);           // reloaded from TCOR on the underflow tick
    sys.sched.advance(16);
    CHECK(m.read32(base + 0x0C) == 98u);
    m.write16(base + 0x10, 0x0020);  // clear UNF (write 0)
    CHECK((m.read16(base + 0x10) & 0x100u) == 0);
    CHECK(!sys.intc.pending(sh4::Irq::Tmu0));
    // TPSC 1: peripheral clock / 16 = CPU / 64. Stop, reprogram, start.
    m.write8(base + 0x04, 0);
    m.write16(base + 0x10, 0x0001);
    m.write32(base + 0x0C, 10);
    m.write8(base + 0x04, 1);
    sys.sched.advance(64 * 4);
    CHECK(m.read32(base + 0x0C) == 6u);
    // A stopped channel holds its value.
    m.write8(base + 0x04, 0);
    sys.sched.advance(64 * 100);
    CHECK(m.read32(base + 0x0C) == 6u);
}

TEST_CASE("holly intc: sources route to IRL levels through the mask registers") {
    System sys;
    auto& m = sys.memory;
    const std::uint32_t base = holly::Intc::kBase;
    sys.holly.raise(holly::Irq::VBlankIn);
    CHECK(m.read32(0xA05F6900u) == 0x8u);
    CHECK(!sys.intc.pending(sh4::Irq::Irl9));
    m.write32(0xA05F6930u, 0x8u);  // IML6NRM: VBlank-in -> level 9
    CHECK(sys.intc.pending(sh4::Irq::Irl9));
    CHECK(!sys.intc.pending(sh4::Irq::Irl11));
    m.write32(0xA05F6920u, 0x8u);  // IML4NRM too -> level 11 as well
    CHECK(sys.intc.pending(sh4::Irq::Irl11));
    m.write32(0xA05F6900u, 0x8u);  // acknowledge: write 1 to clear
    CHECK(m.read32(base) == 0u);
    CHECK(!sys.intc.pending(sh4::Irq::Irl9));
    CHECK(!sys.intc.pending(sh4::Irq::Irl11));
    // External sources show as bit 30 of ISTNRM and are not cleared by writing ISTNRM.
    sys.holly.raise(holly::Irq::GdromCmd);
    CHECK(m.read32(base) == 0x40000000u);
    CHECK(m.read32(base + 4) == 0x1u);
    m.write32(base, 0xFFFFFFFFu);
    CHECK(m.read32(base + 4) == 0x1u);
    m.write32(0xA05F6914u, 0x1u);  // IML2EXT -> level 13
    CHECK(sys.intc.pending(sh4::Irq::Irl13));
    sys.holly.clear(holly::Irq::GdromCmd);
    CHECK(!sys.intc.pending(sh4::Irq::Irl13));
}

TEST_CASE("spg: NTSC defaults give a 59.8 Hz frame with VBlank-in on the programmed line") {
    System sys;
    // 858 pixels per line at 13.5 MHz on a 200 MHz clock: 12,711 cycles; 263 lines.
    CHECK(sys.spg.line_cycles() == 12711u);
    CHECK(sys.spg.lines() == 263u);
    const std::uint64_t frame = sys.spg.frame_cycles();
    CHECK(frame == 12711u * 263u);
    CHECK(200'000'000.0 / static_cast<double>(frame) > 59.7);
    CHECK(200'000'000.0 / static_cast<double>(frame) < 59.9);
    // VBlank-in is line 0x104 = 260; nothing before it.
    sys.sched.advance(12711u * 259u + 100);
    CHECK(!sys.holly.raised(holly::Irq::VBlankIn));
    sys.sched.advance(12711u);
    CHECK(sys.holly.raised(holly::Irq::VBlankIn));
    CHECK(sys.spg.scanline() == 260u);
    CHECK((sys.memory.read32(0xA05F810Cu) & 0x3FFu) == 260u);
    CHECK((sys.memory.read32(0xA05F810Cu) & (1u << 13)) != 0);  // vsync from vstart 260
    // VBlank-out at line 21 of the next frame.
    sys.holly.clear(holly::Irq::VBlankIn);
    sys.sched.advance(12711u * 24u);
    CHECK(sys.holly.raised(holly::Irq::VBlankOut));
    CHECK(sys.spg.frames() == 1u);
    CHECK((sys.memory.read32(0xA05F810Cu) & (1u << 13)) == 0);
}

namespace {
// Guest-side handler stand-ins registered in the function table at VBR+0x600 / VBR+0x100.
std::vector<std::uint32_t> g_intevt_seen;
std::uint32_t g_sr_in_handler = 0, g_bank_r0_in_handler = 0, g_tra_seen = 0;
unsigned g_depth = 0, g_max_depth = 0;

void irq_handler(sh4::Ctx& c, ::dream::Memory& m) {
    ++g_depth;
    g_max_depth = std::max(g_max_depth, g_depth);
    g_intevt_seen.push_back(m.read32(0xFF000028u));
    g_sr_in_handler = sh4::read_sr(c);
    g_bank_r0_in_handler = c.r[0];
    c.r[0] = 0xBA4Cu;  // bank 1 r0: must not leak into the interrupted code
    // Acknowledge the source the way a real handler does, then RTE.
    if (m.read32(0xFF000028u) == 0x320u)
        m.write32(0xA05F6900u, 0x8u);  // clear VBlank-in
    if (m.read32(0xFF000028u) == 0x400u)
        m.write16(0xFFD80010u, 0x0020u);  // clear TMU0 UNF, keep UNIE
    c.cycles += 50;
    --g_depth;
    sh4::rte(c, m);
}

void trap_handler(sh4::Ctx& c, ::dream::Memory& m) {
    g_tra_seen = m.read32(0xFF000020u);
    CHECK(m.read32(0xFF000024u) == 0x160u);
    sh4::rte(c, m);
}

const sh4::FunctionEntry kHandlers[] = {{0x8C00FA00u, irq_handler}, {0x8C00F500u, trap_handler}};
struct Registrar {
    Registrar() { sh4::register_functions(kHandlers, 2); }
} g_registrar;
}  // namespace

TEST_CASE("system: interrupt delivery runs the VBR+0x600 handler as a nested call and unwinds") {
    System sys;
    sys.install();
    g_intevt_seen.clear();
    auto& c = sys.ctx;
    c.vbr = 0x8C00F400u;
    sh4::write_sr(c, 0x40000000u);  // MD, IMASK 0, BL clear; reset state had RB set, bank 0 now
    c.r[0] = 0x1111u;
    sys.memory.write32(0xA05F6930u, 0x8u);  // VBlank-in -> IRL9
    // Nothing pending yet: the poll only re-arms the deadline.
    c.cycles = 100;
    sh4::deliver_irq(c, sys.memory);
    CHECK(g_intevt_seen.empty());
    CHECK(c.next_event == sys.spg.line_cycles());
    // Run guest time past the VBlank-in line, then poll.
    c.cycles = sys.spg.line_cycles() * 261;
    sh4::deliver_irq(c, sys.memory);
    REQUIRE(g_intevt_seen.size() == 1);
    CHECK(g_intevt_seen[0] == 0x320u);
    CHECK((g_sr_in_handler & sh4::SR_BL) != 0);
    CHECK((g_sr_in_handler & sh4::SR_RB) != 0);
    CHECK(g_bank_r0_in_handler == 0u);  // bank 1 was empty
    CHECK(c.r[0] == 0x1111u);           // bank 0 restored by RTE's SR write
    CHECK(c.r_bank[0] == 0xBA4Cu);      // the handler's write lives in bank 1
    CHECK(sh4::read_sr(c) == 0x40000000u);
    CHECK(sys.interrupts_delivered == 1);
    CHECK(sys.max_nesting == 1);
    CHECK(!sys.holly.raised(holly::Irq::VBlankIn));  // acknowledged by the handler
    CHECK(c.next_event > c.cycles);
    // TMU0 with priority 15 alongside a masked IRL: only the timer is delivered under IMASK 7.
    sys.memory.write16(0xFFD00004u, 0xF000u);
    sys.memory.write32(0xFFD80008u, 10);
    sys.memory.write32(0xFFD8000Cu, 10);
    sys.memory.write16(0xFFD80010u, 0x0020u);
    sys.memory.write8(0xFFD80004u, 1);
    sh4::write_sr(c, 0x40000000u | (7u << 4));
    sys.holly.raise(holly::Irq::VBlankIn);
    c.cycles += 16 * 12;
    sh4::deliver_irq(c, sys.memory);
    REQUIRE(g_intevt_seen.size() == 2);
    CHECK(g_intevt_seen[1] == 0x400u);
    CHECK(sys.holly.raised(holly::Irq::VBlankIn));  // still pending, masked
    // Lowering IMASK forces a poll (next_event = 0) and delivers the IRL.
    sh4::write_sr(c, 0x40000000u);
    CHECK(c.next_event == 0);
    sh4::deliver_irq(c, sys.memory);
    REQUIRE(g_intevt_seen.size() == 3);
    CHECK(g_intevt_seen[2] == 0x320u);
    sh4::set_hooks(nullptr);
}

TEST_CASE("system: TRAPA enters the general exception vector with TRA and EXPEVT set") {
    System sys;
    sys.install();
    auto& c = sys.ctx;
    c.vbr = 0x8C00F400u;
    sh4::write_sr(c, 0x40000000u);
    c.pc = 0x8C010100u;
    sh4::trapa(c, sys.memory, 0x21);
    CHECK(g_tra_seen == (0x21u << 2));
    CHECK(sys.traps_taken == 1);
    CHECK(sh4::read_sr(c) == 0x40000000u);
    CHECK(c.pc == 0x8C010100u);  // SPC restored by RTE
    // An untranslated target is logged; with the dev interpreter it is executed from memory, in
    // the release configuration it is fatal (ADR 2).
    c.pr = 0x8C010200u;
#ifdef DREAM_DEV_INTERPRETER
    sys.memory.write16(0x8C123456u, 0x000Bu);  // rts
    sys.memory.write16(0x8C123458u, 0x0009u);  // nop
    sh4::call_indirect(c, sys.memory, 0x8C123456u);
    CHECK(sys.interpreter_runs == 1);
#else
    CHECK_THROWS_AS(sh4::call_indirect(c, sys.memory, 0x8C123456u), UntranslatedCall);
#endif
    CHECK(sys.untranslated.total() == 1);
    sh4::set_hooks(nullptr);
}
