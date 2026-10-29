#include "dream/runtime/system.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "dream/runtime/hle/bios.h"
#include "dream/runtime/sh4/ops.h"
#ifdef DREAM_DEV_INTERPRETER
#include "dream/runtime/devinterp/interpreter.h"
#endif

#include <cctype>
#include <cstring>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// windows.h first: dbghelp.h needs its types.
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace dream {

namespace {

// DREAM_WATCH_CALL=addr,addr,...: when a call or jump through a register reaches one of these
// untranslated targets, print the caller's registers and the guest call chain. Built for a title
// that jumps to 0x00000000: the interpreter then faults on the target, and the caller is gone.
bool watched_call(std::uint32_t target) {
    static const std::vector<std::uint32_t> targets = [] {
        std::vector<std::uint32_t> v;
        const char* s = std::getenv("DREAM_WATCH_CALL");
        for (const char* p = s ? s : ""; *p;) {
            char* end = nullptr;
            const unsigned long a = std::strtoul(p, &end, 0);
            if (end == p)
                break;
            v.push_back(static_cast<std::uint32_t>(a) & 0x1FFFFFFFu);
            p = (*end == ',') ? end + 1 : end;
        }
        return v;
    }();
    return std::find(targets.begin(), targets.end(), target & 0x1FFFFFFFu) != targets.end();
}

// Every guest function is emitted as a C++ function named fn_<8 hex digits>, so the host stack
// gives the guest call chain, innermost first. Needs a build with debug info (a .pdb) on Windows.
void print_guest_chain() {
#if defined(_WIN32)
    const HANDLE process = GetCurrentProcess();
    static const bool ready = SymInitialize(process, nullptr, TRUE) != FALSE;
    if (!ready) {
        std::fprintf(stderr, "  guest call chain: no symbols\n");
        return;
    }
    void* frames[62];
    const USHORT n = CaptureStackBackTrace(0, 62, frames, nullptr);
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    std::fprintf(stderr, "  guest call chain (innermost first):\n");
    unsigned shown = 0;
    for (USHORT i = 0; i < n && shown < 16; ++i) {
        std::memset(buf, 0, sizeof buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (!SymFromAddr(process, reinterpret_cast<DWORD64>(frames[i]), &disp, sym))
            continue;
        const char* fn = std::strstr(sym->Name, "fn_");
        bool emitted = fn != nullptr && std::strlen(fn) >= 11;
        for (int k = 3; emitted && k < 11; ++k)
            emitted = std::isxdigit(static_cast<unsigned char>(fn[k])) != 0;
        if (!emitted)
            continue;
        std::fprintf(stderr, "    %.11s\n", fn);
        ++shown;
    }
    if (shown == 0)
        std::fprintf(stderr, "    (no emitted frames on the stack)\n");
#else
    std::fprintf(stderr, "  guest call chain: not implemented here; see DREAM_STOP_ON_UNMAPPED\n");
#endif
}

void report_watched_call(const sh4::Ctx& c, ::dream::Memory& m, std::uint32_t target) {
    std::fprintf(stderr, "watched call to 0x%08x: pc 0x%08x pr 0x%08x\n", target, c.pc, c.pr);
    for (int i = 0; i < 16; i += 4)
        std::fprintf(stderr, "  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n", i, c.r[i], i + 1,
                     c.r[i + 1], i + 2, c.r[i + 2], i + 3, c.r[i + 3]);
    // The guest stack holds what the caller saved: return addresses and spilled arguments that
    // the registers no longer show.
    std::fprintf(stderr, "  stack at r15:");
    for (std::uint32_t k = 0; k < 12; ++k)
        std::fprintf(stderr, "%s %08x", k % 6 == 0 ? "\n   " : "", m.read32(c.r[15] + 4 * k));
    std::fprintf(stderr, "\n");
    print_guest_chain();
}

}  // namespace

void print_guest_call_chain() {
    print_guest_chain();
}

System::System() : tmu(sched, intc), holly(intc), spg(sched, holly) {
    memory.map_p4(sh4::Intc::kBase, sh4::Intc::kBase + 0x10, &intc);
    memory.map_p4(sh4::Tmu::kBase, sh4::Tmu::kEnd, &tmu);
    memory.map_mmio(holly::Intc::kBase, holly::Intc::kEnd, &holly);
    memory.map_mmio(pvr::Spg::kRegLo, pvr::Spg::kRegHi, &spg);
    memory.map_mmio(pvr::Spg::kStatus, pvr::Spg::kStatus + 4, &spg);
    reset();
}

System::~System() {
    if (sh4::hooks() == this)
        sh4::set_hooks(nullptr);
}

void System::reset() {
    ctx = sh4::Ctx{};
    ctx.sr = 0x700000F0u;
    sh4::write_fpscr(ctx, 0x00040001u);
    ctx.next_event = 0;
    tmu.reset();
    holly.reset();
    spg.reset();
    interrupts_delivered = traps_taken = 0;
    nesting = max_nesting = 0;
}

void System::install() {
    sh4::set_hooks(this);
    // Devices live in the present: bring the scheduler up to the guest clock before any register
    // access, so timers and drive status reflect the cycles emitted code has accumulated since the
    // last interrupt poll. Guarded because an event run here may itself touch a device.
    memory.on_device_access = [this] {
        if (advancing_for_device || sched.now() >= ctx.cycles)
            return;
        advancing_for_device = true;
        sched.advance_to(ctx.cycles);
        advancing_for_device = false;
    };
}

void System::arm_next_poll(sh4::Ctx& c) noexcept {
    // Poll again when the next event is due, or immediately if something is already deliverable.
    c.next_event = intc.select(sh4::read_sr(c)) ? c.cycles : sched.next_deadline();
}

// SH-4 exception entry (SH7750 manual 5.x; Flycast Do_Interrupt): SSR/SPC/SGR saved, BL, MD and
// RB set (bank swap), then the handler at VBR + offset runs as a nested guest call until its RTE.
void System::enter_exception(sh4::Ctx& c, ::dream::Memory& m, std::uint32_t vector_offset) {
    c.ssr = sh4::read_sr(c);
    c.spc = c.pc;
    c.sgr = c.r[15];
    sh4::write_sr(c, c.ssr | sh4::SR_BL | sh4::SR_MD | sh4::SR_RB);
    const std::uint32_t handler = c.vbr + vector_offset;
    c.pc = handler;
    ++nesting;
    max_nesting = std::max(max_nesting, nesting);
    frame_pcs.push_back(c.spc);
    frame_sps.push_back(c.sgr);
    // A non-local return inside the handler unwinds through here: keep the frame books straight.
    struct Frame {
        System& s;
        ~Frame() {
            s.frame_pcs.pop_back();
            s.frame_sps.pop_back();
            --s.nesting;
        }
    } frame{*this};
    static const bool trace = std::getenv("DREAM_TRACE_NONLOCAL") != nullptr;
    const std::uint32_t pr_before = c.pr, sp_before = c.r[15];
    if (trace)
        std::fprintf(stderr, "irq: vector 0x%03x at pc 0x%08x pr 0x%08x r15 0x%08x nesting %u\n",
                     vector_offset, c.spc, c.pr, c.r[15], nesting);
    struct Report {
        const bool on;
        sh4::Ctx& c;
        std::uint32_t pr, sp;
        ~Report() {
            if (on)
                std::fprintf(stderr, "irq done: pr 0x%08x -> 0x%08x, r15 0x%08x -> 0x%08x\n", pr,
                             c.pr, sp, c.r[15]);
        }
    } report{trace, c, pr_before, sp_before};
#ifdef DREAM_DEV_INTERPRETER
    if (interpret_all) {
        devinterp::Options iopt;
        iopt.call_translated = false;
        iopt.native_lo = hle::Bios::kHookSystem;
        iopt.native_hi = hle::Bios::kHookGd2 + 2;
        devinterp::run(c, m, 0, iopt, nullptr);  // the handler's RTE ends the run
    } else
#endif
        sh4::call_indirect(c, m, handler);  // returns after the handler's RTE
}

unsigned System::deliver_pending(sh4::Ctx& c, ::dream::Memory& m) {
    unsigned delivered = 0;
    while (auto sel = intc.select(sh4::read_sr(c))) {
        memory.intevt = sel->intevt;
        ++interrupts_delivered;
        ++delivered;
        enter_exception(c, m, 0x600);
        if (delivered > 64)  // a handler that never clears its source would loop forever
            break;
    }
    return delivered;
}

void System::on_poll(sh4::Ctx& c, ::dream::Memory& m) {
    if (c.cycles > sched.now())
        sched.advance_to(c.cycles);  // devices raise their interrupts here
    deliver_pending(c, m);
    arm_next_poll(c);
}

void System::on_trapa(sh4::Ctx& c, ::dream::Memory& m, std::uint32_t imm) {
    memory.tra = (imm & 0xFFu) << 2;
    memory.expevt = 0x160;
    ++traps_taken;
    enter_exception(c, m, 0x100);
    arm_next_poll(c);
}

void System::on_rte(sh4::Ctx& c, ::dream::Memory& m) {
    const bool switched =
        !frame_pcs.empty() && (c.spc != frame_pcs.back() || c.r[15] != frame_sps.back());
    sh4::write_sr(c, c.ssr);
    c.pc = c.spc;
    if (switched) {
        // The handler resumes a different context (Katana's scheduler ran inside the VBlank
        // handler): the interrupted host frames belong to the suspended task. Drop them and let
        // run_guest re-enter the resumed task at its SPC (docs/emitter-design.md, "Non-local
        // returns"). The delivery frame's bookkeeping unwinds through enter_exception.
        ++task_switch_rtes;
        if (first_switches.size() < 8)
            first_switches.push_back({frame_pcs.back(), frame_sps.back(), c.spc, c.r[15]});
        sh4::nonlocal_return(c, m, c.spc);
    }
    // The emitted `return` after rte() unwinds to enter_exception's call site.
}

void System::on_untranslated(sh4::Ctx& c, ::dream::Memory& m, std::uint32_t target) {
    if (watched_call(target))
        report_watched_call(c, m, target);
    // Recorded either way: the log is what the next translate pass reads (ADR 2).
    untranslated.record(target, 4, false);
    if (untranslated_sites.size() < 32)
        untranslated_sites.emplace_back(target, c.pc);
#ifdef DREAM_DEV_INTERPRETER
    // Run the untranslated code from memory until it returns to the caller's PR (or RTEs, for an
    // exception handler), calling translated functions natively on the way.
    const std::uint32_t return_to = c.pr;
    c.pc = target;
    devinterp::Stats st;
    devinterp::run(c, m, return_to, {}, &st);
    interpreter_runs += st.runs;
    interpreted_instructions += st.instructions;
    interpreter_native_calls += st.native_calls;
#else
    (void)m;
    char buf[96];
    std::snprintf(buf, sizeof buf, "untranslated call target 0x%08x from pc 0x%08x", target, c.pc);
    throw UntranslatedCall(target, c.pc, buf);
#endif
}

void System::on_resume(sh4::Ctx& c, ::dream::Memory& m, std::uint32_t pc) {
    untranslated.record(pc, 4, false);
    if (untranslated_sites.size() < 32)
        untranslated_sites.emplace_back(pc, c.pc);
#ifdef DREAM_DEV_INTERPRETER
    // No return address to stop at: interpret until the run ends or the next non-local return
    // (which propagates to sh4::run_guest).
    c.pc = pc;
    devinterp::Stats st;
    devinterp::run(c, m, 0xFFFFFFFFu, {}, &st);
    interpreter_runs += st.runs;
    interpreted_instructions += st.instructions;
    interpreter_native_calls += st.native_calls;
#else
    (void)m;
    char buf[96];
    std::snprintf(buf, sizeof buf, "cannot resume at 0x%08x (non-local return from pc 0x%08x)", pc,
                  c.pc);
    throw UntranslatedCall(pc, c.pc, buf);
#endif
}

}  // namespace dream
