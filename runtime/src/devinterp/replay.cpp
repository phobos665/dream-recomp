// See replay.h.
#include "dream/runtime/devinterp/replay.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "dream/runtime/devinterp/interpreter.h"
#include "dream/runtime/sh4/abi.h"

namespace dream::devinterp {

namespace {
Replay* g_replay = nullptr;

// Formats a disagreement so the line says which value came from which engine, in that order.
std::string differs(const char* name, std::uint64_t translated, std::uint64_t interpreted) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s: translated %016llx, interpreted %016llx", name,
                  static_cast<unsigned long long>(translated),
                  static_cast<unsigned long long>(interpreted));
    return buf;
}
}  // namespace

Replay* replay() noexcept {
    return g_replay;
}
void set_replay(Replay* r) noexcept {
    g_replay = r;
}

bool Replay::wanted(std::uint32_t function) const {
    if (only.empty())
        return true;
    return std::find(only.begin(), only.end(), function) != only.end();
}

void Replay::enter(std::uint32_t function, std::uint32_t assumed_mode) {
    // Checked on every call, whether or not this function is being compared: it costs two tests and
    // it answers the question the comparison only hints at.
    if (!replaying_ && assumed_mode != 0) {
        const bool pr_known = (assumed_mode & 1u) != 0, sz_known = (assumed_mode & 4u) != 0;
        const unsigned pr = (assumed_mode >> 1) & 1u, sz = (assumed_mode >> 3) & 1u;
        const unsigned real_pr = (ctx_.fpscr >> 19) & 1u, real_sz = (ctx_.fpscr >> 20) & 1u;
        if ((pr_known && pr != real_pr) || (sz_known && sz != real_sz)) {
            ++mode_mismatches;
            if (std::find(mode_mismatch_functions.begin(), mode_mismatch_functions.end(),
                          function) == mode_mismatch_functions.end()) {
                mode_mismatch_functions.push_back(function);
                std::fprintf(stderr,
                             "replay: fn_%08x compiled for pr=%u sz=%u, entered with pr=%u sz=%u\n",
                             function, pr, sz, real_pr, real_sz);
            }
        }
    }
    return enter_impl(function);
}

void Replay::enter_impl(std::uint32_t function) {
    // The interpreted replay calls translated functions natively, and their hooks would otherwise
    // start a comparison inside a comparison and throw away the journal being compared.
    if (replaying_)
        return;
    ++depth_;
    // The first function a run enters is the program's entry point, which does not return until the
    // run is over. A comparison waiting for it would never complete and would journal the whole
    // run, so one that grows past a bound is abandoned and the next call entered is compared
    // instead. That is what makes "compare everything" reach the functions that matter: the deep,
    // short ones.
    if (active_ != 0 && memory_.journal.size() > kJournalLimit) {
        ++skipped_too_long;
        // Remember it, so the run stops trying the same never-returning function over and over.
        // A handful of attempts blacklists the entry point and the main loop, and everything short
        // enough to compare then gets a turn.
        too_long_.insert(active_);
        memory_.journaling = false;
        memory_.journal.clear();
        active_ = 0;
    }
    if (active_ != 0 || !wanted(function))
        return;  // already inside one under comparison, or not asked for
    if (too_long_.count(function))
        return;  // known not to return within the journal's bound
    active_ = function;
    entry_ = ctx_;
    entry_interrupts_ = interrupts ? *interrupts : 0;
    memory_.journal.clear();
    memory_.journal_saw_device = false;
    memory_.journaling = true;
}

void Replay::exit(std::uint32_t function) {
    if (replaying_)
        return;
    if (depth_ > 0)
        --depth_;
    if (active_ != function)
        return;
    active_ = 0;
    memory_.journaling = false;

    // An interrupt delivered while the function ran put the handler's stores into the journal, and
    // the replay does not deliver one. The two would differ for a reason that is not a bug.
    if (interrupts && *interrupts != entry_interrupts_) {
        ++skipped_interrupt;
        memory_.journal.clear();
        return;
    }
    // A device write cannot be undone and must not be repeated, so this call cannot be compared.
    if (memory_.journal_saw_device) {
        ++skipped_device;
        memory_.journal.clear();
        return;
    }
    // A function that did not come back to where it was called from was unwound by a non-local
    // return, and the interpreter would have to be unwound the same way to match.
    if (ctx_.pr != entry_.pr) {
        ++skipped_nonlocal;
        memory_.journal.clear();
        return;
    }

    const sh4::Ctx translated_exit = ctx_;
    const std::vector<mem::DcMemory::WriteRecord> translated_writes = memory_.journal;

    // Back to the memory the function started with, then run it again through the interpreter from
    // the same context.
    memory_.undo_journal();
    memory_.journal.clear();
    memory_.journal_saw_device = false;
    memory_.journaling = true;

    ctx_ = entry_;
    ctx_.pc = function;
    replaying_ = true;
    Options opt;
    opt.call_translated = true;  // callees are not what is under test here
    opt.catch_nonlocal = false;
    opt.max_instructions = 2'000'000;
    Stop stop = Stop::StepLimit;
    bool unwound = false;
    try {
        if (self_check) {
            // The same translated function again, from the same context and the same memory.
            if (sh4::GuestFn fn = sh4::find_function(function, &memory_)) {
                fn(ctx_, memory_);
                stop = Stop::Returned;
            }
        } else {
            stop = run(ctx_, memory_, entry_.pr, opt, nullptr);
        }
    } catch (const sh4::NonLocalReturn&) {
        // The replay unwound where the translated run did not, or did so differently. Either way
        // the comparison is void, and the exception must not escape into the real run, whose stack
        // is not the one it was thrown for.
        unwound = true;
    }
    replaying_ = false;
    memory_.journaling = false;

    if (unwound || stop != Stop::Returned || memory_.journal_saw_device) {
        // The interpreted run did something the comparison cannot account for. Put memory back the
        // way the translated run left it and move on.
        ++skipped_nonlocal;
        memory_.undo_journal();
        memory_.journal = translated_writes;
        memory_.redo_journal();
        memory_.journal.clear();
        ctx_ = translated_exit;
        return;
    }

    compare(function, translated_exit, translated_writes);
    ++compared;

    // Whatever the comparison found, the run continues as the translated code left it: undo the
    // interpreter's stores, put the translated ones back, and restore its exit context.
    memory_.undo_journal();
    memory_.journal = translated_writes;
    memory_.redo_journal();
    memory_.journal.clear();
    ctx_ = translated_exit;
}

void Replay::compare(std::uint32_t function, const sh4::Ctx& translated,
                     const std::vector<mem::DcMemory::WriteRecord>& translated_writes) {
    const sh4::Ctx& interpreted = ctx_;
    auto note = [&](std::string what) { divergences_.push_back({function, std::move(what)}); };
    const std::size_t before = divergences_.size();

    for (unsigned i = 0; i < 16; ++i)
        if (translated.r[i] != interpreted.r[i]) {
            char name[8];
            std::snprintf(name, sizeof name, "r%u", i);
            note(differs(name, translated.r[i], interpreted.r[i]));
        }
    for (unsigned i = 0; i < 16; ++i) {
        std::uint32_t a, b;
        std::memcpy(&a, &translated.fr[i], 4);
        std::memcpy(&b, &interpreted.fr[i], 4);
        if (a != b) {
            char name[8];
            std::snprintf(name, sizeof name, "fr%u", i);
            note(differs(name, a, b));
        }
    }
    if (translated.t != interpreted.t)
        note(differs("t", translated.t, interpreted.t));
    if (translated.macl != interpreted.macl)
        note(differs("macl", translated.macl, interpreted.macl));
    if (translated.mach != interpreted.mach)
        note(differs("mach", translated.mach, interpreted.mach));
    if (translated.fpul != interpreted.fpul)
        note(differs("fpul", translated.fpul, interpreted.fpul));
    if (translated.fpscr != interpreted.fpscr)
        note(differs("fpscr", translated.fpscr, interpreted.fpscr));
    if (translated.gbr != interpreted.gbr)
        note(differs("gbr", translated.gbr, interpreted.gbr));

    // The stores, in order. A different count is itself the finding, so report the first place the
    // two sequences part rather than every later consequence of it.
    const std::vector<mem::DcMemory::WriteRecord>& iw = memory_.journal;
    const std::size_t n = std::min(translated_writes.size(), iw.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto& a = translated_writes[i];
        const auto& b = iw[i];
        if (a.addr != b.addr || a.size != b.size || a.new_value != b.new_value) {
            char buf[224];
            std::snprintf(buf, sizeof buf,
                          "store %zu: translated %u bytes of %016llx to %08x, interpreted %u bytes "
                          "of %016llx to %08x",
                          i, a.size, static_cast<unsigned long long>(a.new_value), a.addr, b.size,
                          static_cast<unsigned long long>(b.new_value), b.addr);
            note(buf);
            break;
        }
    }
    if (translated_writes.size() != iw.size()) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "store count: translated %zu, interpreted %zu",
                      translated_writes.size(), iw.size());
        note(buf);
    }

    if (divergences_.size() != before && stop_on_divergence) {
        // The entry floating-point mode is printed with every disagreement, because the emitter
        // decides the size of an `fmov` from a static guess at it: a function entered in a mode
        // other than the one assumed moves the wrong number of bytes and walks its pointer at the
        // wrong rate (ADR 6).
        std::fprintf(stderr,
                     "replay: fn_%08x disagrees with the interpreter (entry fpscr %08x, pr=%u "
                     "sz=%u)\n",
                     function, entry_.fpscr, (entry_.fpscr >> 19) & 1u, (entry_.fpscr >> 20) & 1u);
        for (std::size_t i = before; i < divergences_.size(); ++i)
            std::fprintf(stderr, "  %s\n", divergences_[i].what.c_str());
    }
}

}  // namespace dream::devinterp
