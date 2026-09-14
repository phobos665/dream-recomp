// Harness implementation of the emitted-code ABI: a function table, direct calls, and hard
// failures for anything that needs the real runtime (exceptions, interrupts, untranslated code).
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "dream/runtime/sh4/abi.h"
#ifdef DREAM_DEV_INTERPRETER
#include "dream/runtime/devinterp/replay.h"
#endif
#include <exception>

#include "dream/runtime/sh4/ops.h"

namespace dream::sh4 {
namespace {

std::vector<FunctionEntry>& table() {
    static std::vector<FunctionEntry> t;
    return t;
}

std::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}

}  // namespace

void register_functions(const FunctionEntry* entries, std::size_t count) noexcept {
    // Keyed by physical address so every RAM alias (P0/P1/P2/P3) of an entry resolves in O(log n).
    auto& t = table();
    for (std::size_t i = 0; i < count; ++i)
        t.push_back(FunctionEntry{entries[i].address & 0x1FFFFFFFu, entries[i].fn,
                                  entries[i].signature, entries[i].signature_words,
                                  entries[i].end ? entries[i].end & 0x1FFFFFFFu : 0u,
                                  entries[i].resume});
    // Signed (overlay) entries sort before the unconditional one at the same address.
    std::sort(t.begin(), t.end(), [](const FunctionEntry& a, const FunctionEntry& b) {
        if (a.address != b.address)
            return a.address < b.address;
        return (a.signature != nullptr) > (b.signature != nullptr);
    });
}

namespace {
std::uint32_t g_interp_lo = 0, g_interp_hi = 0;
// Individually hidden functions, in addition to the range. A contiguous range can say "somewhere
// between here and there", which is enough to find a neighbourhood and no more; isolating a bug to
// the function that holds it needs to hide an arbitrary subset, so that a set can be shrunk until
// removing any one member brings the fault back.
std::vector<std::pair<std::uint32_t, std::uint32_t>> g_interp_set;

bool interpreted(std::uint32_t phys) noexcept {
    if (g_interp_lo != g_interp_hi && phys >= g_interp_lo && phys < g_interp_hi)
        return true;
    // Each entry is a whole function, not just its first address: a range hides everything inside
    // a function, including the resume points a non-local return lands on, and a set has to mean
    // the same thing or the two do not answer the same question.
    auto it =
        std::upper_bound(g_interp_set.begin(), g_interp_set.end(), phys,
                         [](std::uint32_t a, const std::pair<std::uint32_t, std::uint32_t>& e) {
                             return a < e.first;
                         });
    if (it == g_interp_set.begin())
        return false;
    --it;
    return phys < it->second;
}
}  // namespace

ReplayScope::ReplayScope(std::uint32_t fn_address, std::uint32_t assumed_mode) noexcept
    : fn_(fn_address), assumed_(assumed_mode), uncaught_(std::uncaught_exceptions()) {
#ifdef DREAM_DEV_INTERPRETER
    if (auto* r = ::dream::devinterp::replay())
        r->enter(fn_, assumed_);
#endif
}

ReplayScope::~ReplayScope() {
#ifdef DREAM_DEV_INTERPRETER
    // A non-local return unwinds through here. Running the interpreter while an exception is in
    // flight would be a second control transfer on top of the first, so the call is abandoned.
    if (std::uncaught_exceptions() != uncaught_)
        return;
    if (auto* r = ::dream::devinterp::replay())
        r->exit(fn_);
#endif
}

void set_interpret_range(std::uint32_t lo, std::uint32_t hi) noexcept {
    g_interp_lo = lo & 0x1FFFFFFFu;
    g_interp_hi = hi & 0x1FFFFFFFu;
}

void set_interpret_functions(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& fns) {
    g_interp_set.clear();
    g_interp_set.reserve(fns.size());
    for (const auto& [lo, hi] : fns)
        if (hi > lo)
            g_interp_set.push_back({lo & 0x1FFFFFFFu, hi & 0x1FFFFFFFu});
    std::sort(g_interp_set.begin(), g_interp_set.end());
}

GuestFn find_function(std::uint32_t address, ::dream::Memory* m) noexcept {
    const auto& t = table();
    const std::uint32_t phys = address & 0x1FFFFFFFu;
    if (interpreted(phys))
        return nullptr;
    auto it =
        std::lower_bound(t.begin(), t.end(), phys,
                         [](const FunctionEntry& e, std::uint32_t a) { return e.address < a; });
    for (; it != t.end() && it->address == phys; ++it) {
        if (!it->signature)
            return it->fn;
        if (!m)
            continue;
        bool same = true;
        for (std::uint32_t i = 0; same && i < it->signature_words; ++i)
            same = m->read16(address + 2 * i) == it->signature[i];
        if (same)
            return it->fn;
    }
    return nullptr;
}

namespace {
Hooks* g_hooks = nullptr;
}

void set_hooks(Hooks* h) noexcept {
    g_hooks = h;
}
Hooks* hooks() noexcept {
    return g_hooks;
}

namespace {
// The registered function whose range contains `address` and whose signature (if any) matches
// memory. Ranges can overlap (discovery keeps shared tails as their own functions), so the scan
// walks back from the first entry above the address.
const FunctionEntry* find_containing(std::uint32_t address, ::dream::Memory* m) noexcept {
    const auto& t = table();
    const std::uint32_t phys = address & 0x1FFFFFFFu;
    auto it =
        std::upper_bound(t.begin(), t.end(), phys,
                         [](std::uint32_t a, const FunctionEntry& e) { return a < e.address; });
    for (int n = 0; it != t.begin() && n < 1024; ++n) {
        --it;
        if (!it->end || phys < it->address || phys >= it->end)
            continue;
        if (!it->signature)
            return &*it;
        if (!m)
            continue;
        bool same = true;
        for (std::uint32_t i = 0; same && i < it->signature_words; ++i)
            same = m->read16(it->address + 2 * i) == it->signature[i];
        if (same)
            return &*it;
    }
    return nullptr;
}
}  // namespace

namespace {
bool trace_nonlocal() {
    static const bool on = std::getenv("DREAM_TRACE_NONLOCAL") != nullptr;
    return on;
}
}  // namespace

void nonlocal_return(Ctx& c, ::dream::Memory&, std::uint32_t pc) {
    if (trace_nonlocal())
        std::fprintf(stderr, "nonlocal: rts from pc 0x%08x to 0x%08x (r15 0x%08x)\n", c.pc, pc,
                     c.r[15]);
    c.pc = pc;
    throw NonLocalReturn{pc};
}

void resume_miss(Ctx& c, ::dream::Memory& m, std::uint32_t pc) {
    if (g_hooks) {
        g_hooks->on_resume(c, m, pc);
        return;
    }
    throw std::runtime_error("cannot resume translated code at " + hex(pc));
}

void resume_at(Ctx& c, ::dream::Memory& m, std::uint32_t pc) {
    if (trace_nonlocal()) {
        const FunctionEntry* e = find_containing(pc, &m);
        std::fprintf(stderr, "resume: 0x%08x -> %s 0x%08x (pr 0x%08x r15 0x%08x)\n", pc,
                     e ? (e->address == (pc & 0x1FFFFFFFu) ? "entry" : "inside") : "untranslated",
                     e ? e->address : 0u, c.pr, c.r[15]);
    }
    c.pc = pc;
    const std::uint32_t phys = pc & 0x1FFFFFFFu;
    if (!interpreted(phys)) {
        if (const FunctionEntry* e = find_containing(pc, &m)) {
            if (e->address == phys) {
                e->fn(c, m);
                return;
            }
            if (e->resume) {
                e->resume(c, m, pc);
                return;
            }
        }
    }
    resume_miss(c, m, pc);
}

namespace {
template <typename Start>
void run_guest_impl(Ctx& c, ::dream::Memory& m, Start start) {
    const std::uint32_t stop_pr = c.pr;
    std::uint32_t pc = 0;
    bool resumed = false;
    for (;;) {
        try {
            if (!resumed) {
                start();
                return;  // the entry function returned through PR, as translated code does
            }
            // After a non-local return the host stack no longer mirrors the guest's: every return
            // comes back here and continues at PR.
            resume_at(c, m, pc);
            if (c.pr == stop_pr)
                return;
            if (trace_nonlocal())
                std::fprintf(
                    stderr,
                    "run_guest: resumed code returned (pc 0x%08x), continuing at pr 0x%08x\n", c.pc,
                    c.pr);
            pc = c.pr;
        } catch (const NonLocalReturn& n) {
            if (n.pc == stop_pr)
                return;
            pc = n.pc;
            resumed = true;
        }
    }
}
}  // namespace

void run_guest(Ctx& c, ::dream::Memory& m, std::uint32_t entry) {
    run_guest_impl(c, m, [&] { call_indirect(c, m, entry); });
}

void run_guest(Ctx& c, ::dream::Memory& m, GuestFn entry) {
    run_guest_impl(c, m, [&] { entry(c, m); });
}

void call_indirect(Ctx& c, ::dream::Memory& m, std::uint32_t target) {
    if (GuestFn fn = find_function(target, &m)) {
        fn(c, m);
        return;
    }
    // A call or jump through a register that lands *inside* an already-translated function. That
    // is an ordinary thing for SH-4 code to do and it is not a missing translation: the block is
    // compiled, it simply is not a function entry and so has no row of its own in the table. The
    // function's resume entry can start at any of its block starts, which is the same door a
    // non-local return comes through.
    //
    // Without this, the only way to reach such a block was to name it in the game's TOML as an
    // extra discovery seed -- which gives it top rank, makes the enclosing function's branch to it
    // "foreign" (translator/src/analysis/discover.cpp), and truncates that function. Crazy Taxi's
    // display-list builder was split that way and produced corrupt texture control words: 152
    // texture decode failures against none. Reaching the block is the runtime's job, not the
    // config's.
    const std::uint32_t phys = target & 0x1FFFFFFFu;
    if (!interpreted(phys)) {
        if (const FunctionEntry* e = find_containing(target, &m); e && e->resume) {
            c.pc = target;
            // resume() dispatches on its own block starts and calls resume_miss for anything else,
            // so a target that is inside the range but not a block start still faults honestly
            // rather than running from the wrong place.
            e->resume(c, m, target);
            return;
        }
    }
    if (g_hooks) {
        g_hooks->on_untranslated(c, m, target);
        return;
    }
    throw std::runtime_error("untranslated call target " + hex(target) + " from pc " + hex(c.pc));
}

void deliver_irq(Ctx& c, ::dream::Memory& m) {
    if (g_hooks) {
        g_hooks->on_poll(c, m);
        return;
    }
    c.next_event = ~std::uint64_t{0};  // the bare harness never raises interrupts
}

void trapa(Ctx& c, ::dream::Memory& m, std::uint32_t imm) {
    if (g_hooks) {
        g_hooks->on_trapa(c, m, imm);
        return;
    }
    throw std::runtime_error("TRAPA #" + std::to_string(imm) + " at " + hex(c.pc) +
                             " (no exception model in the bare harness)");
}

bool rte(Ctx& c, ::dream::Memory& m) {
    if (g_hooks) {
        const bool unwind = g_hooks->in_exception_frame();
        g_hooks->on_rte(c, m);
        return unwind;
    }
    // The bare harness delivers no exceptions, so an RTE can only be a jump.
    write_sr(c, c.ssr);
    c.pc = c.spc;
    return false;
}

void unimplemented(Ctx& c, std::uint32_t raw, std::uint32_t pc) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "unimplemented instruction 0x%04x at 0x%08x", raw, pc);
    c.pc = pc;
    throw std::runtime_error(buf);
}

// DREAM_TRACE_RETURNS=path: one line per guest return with the rts address and the register
// state, from translated code built with --trace and from the dev interpreter alike, so the two
// runs of a title can be diffed to the first divergent return (docs/runtime-devinterp.md).
void trace_return(Ctx& c, ::dream::Memory&, std::uint32_t rts_pc) {
    static FILE* out = [] {
        const char* path = std::getenv("DREAM_TRACE_RETURNS");
        return path ? std::fopen(path, "w") : nullptr;
    }();
    if (!out)
        return;
    std::fprintf(out, "%08x", rts_pc);
    for (int i = 0; i < 16; ++i) std::fprintf(out, " %08x", c.r[i]);
    std::fprintf(out, " %08x %08x\n", c.pr, read_sr(c));
}

}  // namespace dream::sh4
