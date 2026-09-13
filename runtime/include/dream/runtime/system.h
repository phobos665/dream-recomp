// The running Dreamcast (WP2.2 onward): memory, virtual clock, the SH-4 interrupt model and the
// devices, wired into the ABI hooks emitted code calls. Interrupts are cooperative (ADR 7): emitted
// code polls `cycles >= next_event` at function entries and back-edges; on_poll runs the scheduler
// up to the guest clock, and if an interrupt is deliverable it performs the SH-4 entry sequence and
// runs the handler at VBR+0x600 as a nested guest call. The handler's RTE restores SR and returns
// to the poll site, which is where hardware would have resumed.
#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "dream/runtime/holly/intc.h"
#include "dream/runtime/mem/dc_memory.h"
#include "dream/runtime/pvr/spg.h"
#include "dream/runtime/sched/scheduler.h"
#include "dream/runtime/sh4/abi.h"
#include "dream/runtime/sh4/ctx.h"
#include "dream/runtime/sh4/intc.h"
#include "dream/runtime/sh4/tmu.h"

namespace dream {

// Thrown by the release configuration (no dev interpreter) when emitted code transfers control to
// an address with no translation. The fault log holds every miss recorded before it (ADR 2).
struct UntranslatedCall : std::runtime_error {
    UntranslatedCall(std::uint32_t target_, std::uint32_t from_, const std::string& what)
        : std::runtime_error(what), target(target_), from(from_) {}
    std::uint32_t target, from;
};

class System final : public sh4::Hooks {
public:
    System();
    ~System() override;

    // Katana reset state: SR 0x700000F0, FPSCR 0x00040001, VBR 0, clock at zero.
    void reset();
    // Installs this system as the ABI hook target (one system at a time).
    void install();

    void on_poll(sh4::Ctx& c, ::dream::Memory& m) override;
    void on_trapa(sh4::Ctx& c, ::dream::Memory& m, std::uint32_t imm) override;
    void on_rte(sh4::Ctx& c, ::dream::Memory& m) override;
    void on_untranslated(sh4::Ctx& c, ::dream::Memory& m, std::uint32_t target) override;
    void on_resume(sh4::Ctx& c, ::dream::Memory& m, std::uint32_t pc) override;
    bool in_exception_frame() const noexcept override { return nesting > 0; }

    // Delivers every interrupt SR currently lets through, most urgent first, running handlers to
    // completion. Returns how many were delivered.
    unsigned deliver_pending(sh4::Ctx& c, ::dream::Memory& m);

    mem::DcMemory memory;
    sched::Scheduler sched;
    sh4::Intc intc;
    sh4::Tmu tmu;
    holly::Intc holly;
    pvr::Spg spg;
    sh4::Ctx ctx{};

    // Diagnostics
    std::uint64_t interrupts_delivered = 0, traps_taken = 0;
    unsigned nesting = 0, max_nesting = 0;
    // Diagnostics for exception frames: an RTE whose SPC is not the interrupted pc is a task
    // switch inside the handler (the guest kernel resumed a different context).
    std::uint64_t task_switch_rtes = 0;
    // Dev builds: deliver exception handlers through the interpreter too (launcher --interpret).
    bool interpret_all = false;
    bool advancing_for_device = false;  // re-entrancy guard for the device-access clock advance
    std::vector<std::uint32_t> frame_pcs, frame_sps;
    std::vector<std::array<std::uint32_t, 4>> first_switches;  // from pc, from r15, to pc, to r15
    // (target, call site) of the first untranslated calls, for the launcher report.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> untranslated_sites;
    mem::FaultLog untranslated;  // call targets with no translation (size field = 4, write = 0)
    // Dev interpreter (DREAM_DEV_INTERPRETER only; zero otherwise)
    std::uint64_t interpreter_runs = 0, interpreted_instructions = 0, interpreter_native_calls = 0;

private:
    void enter_exception(sh4::Ctx& c, ::dream::Memory& m, std::uint32_t vector_offset);
    void arm_next_poll(sh4::Ctx& c) noexcept;
};

}  // namespace dream
