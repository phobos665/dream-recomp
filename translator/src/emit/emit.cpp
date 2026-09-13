#include "dream/translator/emit.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

#include "dream/translator/analysis/constprop.h"
#include "dream/translator/analysis/switch.h"
#include "dream/translator/sh4/decoder.h"

namespace dream::translator {

using sh4::Instr;
using sh4::Op;

std::string default_name(std::uint32_t entry) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "fn_%08x", entry);
    return buf;
}

namespace {

std::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}
std::string hexu(std::uint32_t v) {
    return hex(v) + "u";
}
std::string label(std::uint32_t a) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "L_%08x", a);
    return buf;
}
std::string R(unsigned n) {
    return "c.r[" + std::to_string(n) + "]";
}
std::string S32(const std::string& e) {
    return "static_cast<std::int32_t>(" + e + ")";
}
std::string U32(const std::string& e) {
    return "static_cast<std::uint32_t>(" + e + ")";
}
std::string I(std::int32_t v) {
    return v < 0 ? "(" + std::to_string(v) + ")" : std::to_string(v);
}

// FPSCR precision (PR) and transfer-size (SZ) bits at a program point (ADR 6), as a five-valued
// lattice per bit: a known constant, "same as at function entry", "entry toggled", or unknown.
// The entry-relative values let a function's net effect on the mode be summarised without knowing
// what its callers establish; callers then resolve the summary against their own state.
constexpr std::uint8_t kUnknown = 2;
constexpr std::uint8_t kEntry = 3;
constexpr std::uint8_t kEntryToggled = 4;

struct FpMode {
    std::uint8_t pr = kEntry, sz = kEntry;
    bool operator==(const FpMode&) const = default;
};

std::uint8_t merge_bit(std::uint8_t a, std::uint8_t b) {
    return a == b ? a : kUnknown;
}
FpMode merge(FpMode a, FpMode b) {
    return {merge_bit(a.pr, b.pr), merge_bit(a.sz, b.sz)};
}

std::uint8_t toggle_bit(std::uint8_t v) {
    switch (v) {
        case 0:
            return 1;
        case 1:
            return 0;
        case kEntry:
            return kEntryToggled;
        case kEntryToggled:
            return kEntry;
        default:
            return kUnknown;
    }
}

// Apply a callee's net effect (expressed relative to the callee's entry) to the caller's state.
std::uint8_t apply_effect(std::uint8_t state, std::uint8_t effect) {
    switch (effect) {
        case kEntry:
            return state;
        case kEntryToggled:
            return toggle_bit(state);
        case 0:
        case 1:
            return effect;
        default:
            return kUnknown;
    }
}
FpMode apply_summary(FpMode state, FpMode summary) {
    return {apply_effect(state.pr, summary.pr), apply_effect(state.sz, summary.sz)};
}

// Resolve an entry-relative bit against the concrete entry value (0, 1 or kUnknown).
std::uint8_t resolve_bit(std::uint8_t v, std::uint8_t entry) {
    switch (v) {
        case kEntry:
            return entry;
        case kEntryToggled:
            return entry == kUnknown ? kUnknown : static_cast<std::uint8_t>(entry ^ 1);
        default:
            return v;
    }
}

using Summaries = std::map<std::uint32_t, FpMode>;  // function entry -> net effect

class FunctionEmitter {
public:
    FunctionEmitter(const Image& img, const FunctionSpec& spec, const EmitOptions& opt,
                    const std::map<std::uint32_t, std::string>& known, const Summaries& summaries,
                    EmitResult& result)
        : img_(img),
          spec_(spec),
          opt_(opt),
          known_(known),
          summaries_(summaries),
          result_(result) {}

    // Net effect of this function on PR/SZ from entry to return, for callers' analyses.
    void set_written(const std::set<std::uint32_t>* w) { written_ = w; }

    // Pass 0 of emit_unit: every store whose address is a block-local constant inside the image
    // marks the word(s) it writes, so literal folding leaves them alone.
    void scan_stores(std::set<std::uint32_t>& out) {
        if (blocks_.empty())
            discover();
        for (std::uint32_t start : blocks_) {
            const std::uint32_t stop = block_end_.at(start);
            for (std::uint32_t pc = start; pc < stop; pc += 2) {
                const Instr i = at(pc);
                std::optional<std::uint32_t> base;
                std::uint32_t size = 0;
                std::int32_t disp = 0;
                bool needs_r0 = false;
                switch (i.op) {
                    case Op::MOV_B_S:
                        size = 1;
                        break;
                    case Op::MOV_W_S:
                        size = 2;
                        break;
                    case Op::MOV_L_S:
                    case Op::MOVCA_L:
                        size = 4;
                        break;
                    case Op::MOV_B_SD:
                        size = 1;
                        disp = -1;
                        break;
                    case Op::MOV_W_SD:
                        size = 2;
                        disp = -2;
                        break;
                    case Op::MOV_L_SD:
                        size = 4;
                        disp = -4;
                        break;
                    case Op::MOV_B_S_DISP0:
                        size = 1;
                        disp = i.imm;
                        break;
                    case Op::MOV_W_S_DISP0:
                        size = 2;
                        disp = i.imm * 2;
                        break;
                    case Op::MOV_L_S_DISP:
                        size = 4;
                        disp = i.imm * 4;
                        break;
                    case Op::MOV_B_S_R0:
                        size = 1;
                        needs_r0 = true;
                        break;
                    case Op::MOV_W_S_R0:
                        size = 2;
                        needs_r0 = true;
                        break;
                    case Op::MOV_L_S_R0:
                        size = 4;
                        needs_r0 = true;
                        break;
                    case Op::FMOV_STORE:
                    case Op::FMOV_STORE_R0:
                        size = 8;
                        needs_r0 = i.op == Op::FMOV_STORE_R0;
                        break;
                    case Op::FMOV_STORE_DEC:
                        size = 8;
                        disp = -8;
                        break;
                    default:
                        break;
                }
                if (!size)
                    continue;
                base = constant_at(img_, start, pc, i.n, false);
                if (!base)
                    continue;
                std::uint32_t addr = *base + static_cast<std::uint32_t>(disp);
                if (needs_r0) {
                    const auto r0 = constant_at(img_, start, pc, 0, false);
                    if (!r0)
                        continue;
                    addr += *r0;
                }
                addr = normalise(addr);
                for (std::uint32_t w = addr & ~3u; w < addr + size; w += 4)
                    if (img_.contains(w, 4))
                        out.insert(w);
            }
        }
    }

    FpMode summarise() {
        discover();
        analyse_fp_modes(/*relative=*/true);
        return summary_;
    }

    std::string run() {
        discover();
        analyse_fp_modes(/*relative=*/false);
        std::ostringstream out;
        const std::string name = spec_.name.empty() ? default_name(spec_.entry) : spec_.name;
        out << "// " << name << ": guest " << hex(spec_.entry) << ".." << hex(spec_.end) << "\n";
        collect_call_returns();
        // The body is the resume entry (docs/emitter-design.md, "Non-local returns"): entered with
        // resume_pc = 0 by the plain function, or at a block start / call-return address after a
        // non-local return. entry_pr is what an `rts` must return to; anything else is a non-local
        // return.
        out << "static void " << name << "__resume(Ctx& c, Memory& m, std::uint32_t resume_pc) {\n";
        out << "    [[maybe_unused]] const std::uint32_t entry_pr = c.pr;\n";
        // Poll points record the exact guest pc first: an interrupt handler that switches tasks
        // returns through RTE to SPC, and run_guest re-enters this function there.
        if (opt_.irq_checks)
            out << "    if (c.cycles >= c.next_event) { c.pc = " << hexu(spec_.entry)
                << "; deliver_irq(c, m); }\n";
        out << "    if (resume_pc) {\n        switch (resume_pc) {\n";
        for (std::uint32_t b : blocks_)
            out << "            case " << hexu(b) << ": goto " << label(b) << ";\n";
        for (std::uint32_t r : call_returns_)
            if (!blocks_.count(r))
                out << "            case " << hexu(r) << ": goto " << label(r) << ";\n";
        out << "            default: resume_miss(c, m, resume_pc); return;\n        }\n    }\n";
        for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
            const std::uint32_t start = *it;
            const std::uint32_t stop = block_end_.at(start);
            out << label(start) << ":\n";
            std::uint32_t pc = start;
            pending_cycles_ = 0;
            mode_ = block_mode_.at(start);
            out << "    // block " << hex(start)
                << " fp mode pr=" << (mode_.pr == kUnknown ? "?" : std::to_string(mode_.pr))
                << " sz=" << (mode_.sz == kUnknown ? "?" : std::to_string(mode_.sz)) << "\n";
            while (pc < stop) pc = emit_instruction(out, pc);
            flush_cycles(out);
            // A block ending in a conditional branch still falls through on the not-taken path,
            // so it needs the same continuation as one ending in a plain instruction (a loop
            // whose exit is the next function: Crazy Taxi's kmiFBgetfreememEx).
            if (!ends_in_transfer_.at(start) || is_conditional_branch(last_op(start))) {
                auto next = std::next(it);
                if (next == blocks_.end() || *next != stop) {
                    if (known_.count(normalise(stop))) {
                        // Discovery trimmed this function at another entry (a pointer seed
                        // landing one instruction into a prologue, or shared code): control
                        // continues in that function, so tail-call it.
                        out << "    // falls through into the function at " << hex(stop) << "\n";
                        out << "    " << call_target_expr(stop) << " return;\n";
                    } else {
                        // Control continues at `stop`, which no function of this unit starts at:
                        // hand it to the runtime (the miss handler interprets it in dev builds
                        // and it shows up in the untranslated-target log either way).
                        out << "    // falls off the end of the function range at " << hex(stop)
                            << "\n";
                        out << "    c.pc = " << hexu(stop) << "; call_indirect(c, m, " << hexu(stop)
                            << "); return;\n";
                    }
                }
            }
        }
        out << "}\n";
        out << "void " << name << "(Ctx& c, Memory& m) { ";
        if (opt_.replay_hooks) {
            // The mode this function was compiled for travels with the hook, so a development run
            // can check it against the mode the guest actually arrives in (ADR 6). A bit that was
            // inferred as unknown is not checked: those already get a runtime branch.
            const FpMode assumed = entry_mode(false);
            std::uint32_t bits = 0;
            if (assumed.pr != kUnknown)
                bits |= 0x1u | (static_cast<std::uint32_t>(assumed.pr) << 1);
            if (assumed.sz != kUnknown)
                bits |= 0x4u | (static_cast<std::uint32_t>(assumed.sz) << 3);
            out << "ReplayScope _replay(" << hexu(spec_.entry) << ", " << hexu(bits) << "); ";
        }
        out << name << "__resume(c, m, 0); }\n";
        // After the body, so every literal the lowering read has been recorded and a pool is not
        // mistaken for code that went missing.
        report_coverage();
        return out.str();
    }

private:
    Instr at(std::uint32_t a) const { return sh4::decode(img_.read16(a)); }

    Op last_op(std::uint32_t start) const {
        Op op = Op::Invalid;
        for (std::uint32_t pc = start; pc < block_end_.at(start);) {
            const Instr i = at(pc);
            op = i.op;
            pc += sh4::has_delay_slot(i.op) ? 4 : 2;
        }
        return op;
    }
    static bool is_conditional_branch(Op op) {
        return op == Op::BT || op == Op::BF || op == Op::BT_S || op == Op::BF_S;
    }

    // Return addresses of the calls in this function (resume points for non-local returns).
    void collect_call_returns() {
        call_returns_.clear();
        for (std::uint32_t b : blocks_) {
            for (std::uint32_t pc = b; pc < block_end_.at(b);) {
                const Instr i = at(pc);
                const bool delayed = sh4::has_delay_slot(i.op);
                if ((i.op == Op::BSR || i.op == Op::JSR || i.op == Op::BSRF) && in_range(pc + 4) &&
                    !slot_labels_.count(pc + 4))
                    call_returns_.insert(pc + 4);
                pc += delayed ? 4 : 2;
            }
        }
    }
    bool in_range(std::uint32_t a) const { return a >= spec_.entry && a < spec_.end; }

    // Reachable blocks inside [entry, end): follow fallthrough and local branches only, so data
    // after the last RTS (literal pools) is never decoded as code.
    void discover() {
        std::vector<std::uint32_t> work{spec_.entry};
        std::set<std::uint32_t> seen;
        // Addresses that are the delay slot of some branch, and addresses decoding actually began
        // at. A branch may legally target a delay slot, and telling the two apart is what lets
        // decoding continue after such a branch pair instead of stopping at an address that was
        // only ever marked seen in passing.
        std::set<std::uint32_t> delay_slots, starts;
        while (!work.empty()) {
            std::uint32_t pc = work.back();
            work.pop_back();
            if (!in_range(pc))
                continue;
            if (seen.count(pc)) {
                // Already decoded, with one exception. A delay slot is marked seen when its branch
                // is decoded, so a *branch to that slot* finds it seen and is dropped, and the
                // instructions after the branch pair are never decoded at all. Entering through the
                // slot runs that one instruction and then continues after the pair, so that is
                // where decoding has to carry on.
                if (delay_slots.count(pc) && !starts.count(pc)) {
                    labels_.insert(pc);
                    work.push_back(pc + 2);
                }
                continue;
            }
            starts.insert(pc);
            const std::uint32_t start = pc;
            bool transfer = false;
            while (in_range(pc)) {
                if (pc != start && labels_.count(pc))
                    break;
                seen.insert(pc);
                const Instr ins = at(pc);
                const bool delayed = sh4::has_delay_slot(ins.op);
                const std::uint32_t next = pc + (delayed ? 4 : 2);
                if (delayed) {
                    seen.insert(pc + 2);
                    delay_slots.insert(pc + 2);
                }
                bool stop = true;
                switch (ins.op) {
                    case Op::BT:
                    case Op::BF:
                    case Op::BT_S:
                    case Op::BF_S: {
                        const std::uint32_t t = sh4::pcrel_target(ins, pc);
                        if (in_range(t)) {
                            labels_.insert(t);
                            work.push_back(t);
                        }
                        work.push_back(next);
                        break;
                    }
                    case Op::BRA: {
                        const std::uint32_t t = sh4::pcrel_target(ins, pc);
                        if (in_range(t)) {
                            labels_.insert(t);
                            work.push_back(t);
                        }
                        break;
                    }
                    case Op::RTS:
                    case Op::RTE:
                        break;
                    case Op::JMP:
                    case Op::BRAF: {
                        SwitchTable sw;
                        if (recover_switch(img_, start, pc, ins, spec_.entry, spec_.end, sw)) {
                            switches_[pc] = sw;
                            for (std::uint32_t t : sw.targets) {
                                labels_.insert(t);
                                work.push_back(t);
                            }
                        }
                        break;
                    }
                    case Op::Invalid:
                        // Data reached by fallthrough: the block ends with the fault the lowering
                        // emits for the undefined word; nothing after it is code.
                        break;
                    default:
                        stop = false;
                        break;
                }
                pc = next;
                if (stop) {
                    transfer = true;
                    break;
                }
            }
            blocks_.insert(start);
            block_end_[start] = pc;
            ends_in_transfer_[start] = transfer;
        }
        // Labels that land on a delay slot are not block starts: SH-4 lets a branch target the slot
        // instruction of another branch (it then executes as an ordinary instruction and falls
        // through). Those are emitted specially after the owning branch; everything else splits
        // its block at the label.
        std::set<std::uint32_t> boundary_labels;
        // Where control continues after entering through a delay slot. A branch that targets
        // another branch's slot executes that one instruction and then carries on at the
        // instruction *after* the branch pair, which is a path nothing else reaches: the owning
        // branch's own target is elsewhere and no other branch points there. Unless it is made a
        // block of its own, the instructions there are never decoded and never emitted, and the
        // fall-through lands in whichever block the emitter happened to write next. Crazy Taxi has
        // exactly this at 0x0c081204, where `bf` targets the slot of the `bra` that follows it, and
        // two instructions of a display-list builder went missing.
        std::set<std::uint32_t> after_slot;
        for (std::uint32_t start : blocks_) {
            const std::uint32_t stop = block_end_[start];
            std::uint32_t pc = start;
            while (pc < stop) {
                const Instr i = at(pc);
                const bool delayed = sh4::has_delay_slot(i.op);
                if (labels_.count(pc))
                    boundary_labels.insert(pc);
                if (delayed && labels_.count(pc + 2)) {
                    slot_labels_.insert(pc + 2);
                    after_slot.insert(pc + 4);
                }
                pc += delayed ? 4 : 2;
            }
        }
        for (std::uint32_t a : after_slot) {
            if (a < spec_.entry || a >= spec_.end || blocks_.count(a))
                continue;
            labels_.insert(a);
            boundary_labels.insert(a);
            ++result_.slot_fallthrough_blocks;
        }
        for (std::uint32_t l : labels_) {
            if (!boundary_labels.count(l) && !slot_labels_.count(l))
                slot_labels_.insert(l);  // defensive
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
                const std::uint32_t start = *it, stop = block_end_[start];
                for (auto lab = labels_.upper_bound(start); lab != labels_.end() && *lab < stop;
                     ++lab) {
                    if (slot_labels_.count(*lab))
                        continue;
                    const std::uint32_t split = *lab;
                    block_end_[start] = split;
                    const bool t = ends_in_transfer_[start];
                    ends_in_transfer_[start] = false;
                    blocks_.insert(split);
                    block_end_[split] = stop;
                    ends_in_transfer_[split] = t;
                    changed = true;
                    break;
                }
                if (changed)
                    break;
            }
        }
    }

    // ---- FPSCR PR/SZ data-flow --------------------------------------------------------------

    FpMode entry_mode(bool relative) const {
        if (relative)
            return {kEntry, kEntry};
        if (spec_.fpscr_entry == 0xFFFFFFFFu)
            return {kUnknown, kUnknown};
        return {spec_.entry_pr_unknown ? kUnknown
                                       : static_cast<std::uint8_t>((spec_.fpscr_entry >> 19) & 1),
                spec_.entry_sz_unknown ? kUnknown
                                       : static_cast<std::uint8_t>((spec_.fpscr_entry >> 20) & 1)};
    }

public:
    // Every direct call or tail jump to a function of this unit, with the entry-relative FPSCR
    // mode in force at that point. emit_unit joins these over the call graph to learn each callee's
    // entry mode instead of assuming the Katana default (a context-switch helper called under SZ=1
    // was emitted with 4-byte fmovs and mis-restored a task).
    void call_site_modes(std::vector<std::pair<std::uint32_t, FpMode>>& out) {
        discover();
        analyse_fp_modes(/*relative=*/true);
        for (std::uint32_t b : blocks_) {
            FpMode m = block_mode_[b];
            for (std::uint32_t pc = b; pc < block_end_[b]; pc += 2) {
                const Instr i = at(pc);
                std::uint32_t t = 0;
                bool call = false;
                switch (i.op) {
                    case Op::BSR:
                    case Op::BRA:
                        t = sh4::pcrel_target(i, pc);
                        call = true;
                        break;
                    case Op::JSR:
                    case Op::JMP:
                        call = constant_in_register(b, pc, i.m, t, true);
                        break;
                    case Op::BSRF: {
                        std::uint32_t k;
                        call = constant_in_register(b, pc, i.m, k, true);
                        t = pc + 4 + k;
                        break;
                    }
                    default:
                        break;
                }
                if (call) {
                    t = normalise(t);
                    if (known_.count(t) && t != spec_.entry)
                        out.push_back({t, m});
                }
                m = transfer(m, b, pc, i);
            }
        }
    }

private:
    // Constant loaded into Rn just before `pc` within the block, if it can be seen. Recognises
    // the idiom `mov.l @(disp,pc),Rn ; lds Rn,fpscr` (possibly with unrelated instructions in
    // between) and `mov #imm,Rn`. Any other write to Rn in between gives up.
    bool constant_in_register(std::uint32_t block_start, std::uint32_t pc, unsigned reg,
                              std::uint32_t& value, bool alias_or = false) const {
        const auto v = constant_at(img_, block_start, pc, reg, alias_or);
        if (!v)
            return false;
        value = *v;
        return true;
    }

    static bool writes_rn(const Instr& i) {
        switch (i.op) {
            case Op::MOV_B_S:
            case Op::MOV_W_S:
            case Op::MOV_L_S:
            case Op::MOV_B_S_R0:
            case Op::MOV_W_S_R0:
            case Op::MOV_L_S_R0:
            case Op::MOV_L_S_DISP:
            case Op::CMP_EQ:
            case Op::CMP_HS:
            case Op::CMP_GE:
            case Op::CMP_HI:
            case Op::CMP_GT:
            case Op::CMP_STR:
            case Op::TST:
            case Op::DIV0S:
            case Op::MUL_L:
            case Op::MULS_W:
            case Op::MULU_W:
            case Op::DMULS_L:
            case Op::DMULU_L:
            case Op::CMP_PZ:
            case Op::CMP_PL:
            case Op::JSR:
            case Op::JMP:
            case Op::BRAF:
            case Op::BSRF:
            case Op::PREF:
            case Op::OCBI:
            case Op::OCBP:
            case Op::OCBWB:
            case Op::MOVCA_L:
            case Op::LDC_SR:
            case Op::LDC_GBR:
            case Op::LDC_VBR:
            case Op::LDC_SSR:
            case Op::LDC_SPC:
            case Op::LDC_DBR:
            case Op::LDC_SGR:
            case Op::LDC_BANK:
            case Op::LDS_MACH:
            case Op::LDS_MACL:
            case Op::LDS_PR:
            case Op::LDS_FPUL:
            case Op::LDS_FPSCR:
            case Op::TAS_B:
                return false;  // read Rn (or Rm) only
            default:
                return i.n != 0 ||
                       true;  // conservatively: anything else with an n field may write it
        }
    }

    FpMode transfer(FpMode m, std::uint32_t block_start, std::uint32_t pc, const Instr& i) const {
        switch (i.op) {
            case Op::FSCHG:
                m.sz = toggle_bit(m.sz);
                return m;
            case Op::LDS_FPSCR: {
                std::uint32_t v;
                if (constant_in_register(block_start, pc, i.m, v)) {
                    return {static_cast<std::uint8_t>((v >> 19) & 1),
                            static_cast<std::uint8_t>((v >> 20) & 1)};
                }
                return {kUnknown, kUnknown};
            }
            case Op::LDS_L_FPSCR:
                // `lds.l @r15+,fpscr` paired with a `sts.l fpscr,@-r15` in the same function is the
                // prologue/epilogue save-restore idiom: the mode returns to its entry value.
                if (i.m == 15 && saves_fpscr_)
                    return {kEntry, kEntry};
                return {kUnknown, kUnknown};
            case Op::BSR: {
                const std::uint32_t t = sh4::pcrel_target(i, pc);
                return call_effect(m, t, true);
            }
            case Op::JSR: {
                std::uint32_t t;
                if (constant_in_register(block_start, pc, i.m, t))
                    return call_effect(m, t, true);
                return m;  // unknown target: assume the callee preserves the mode
            }
            default:
                return m;
        }
    }

    // Effect of calling `target` on the mode. Known callee: its summary. Unknown callee: identity.
    FpMode call_effect(FpMode m, std::uint32_t target, bool) const {
        auto it = summaries_.find(target);
        if (it == summaries_.end())
            return m;
        return apply_summary(m, it->second);
    }

    // Successors of a block for the mode propagation: fallthrough and in-range branch targets.
    // A target that is a slot label (not a block start) continues in the block containing it.
    void push_successor(std::vector<std::uint32_t>& out, std::uint32_t t) const {
        if (!in_range(t))
            return;
        if (blocks_.count(t)) {
            out.push_back(t);
            return;
        }
        auto it = blocks_.upper_bound(t);
        if (it != blocks_.begin())
            out.push_back(*std::prev(it));
    }

    std::vector<std::uint32_t> successors(std::uint32_t start) const {
        std::vector<std::uint32_t> out;
        const std::uint32_t stop = block_end_.at(start);
        if (!ends_in_transfer_.at(start)) {
            if (blocks_.count(stop))
                out.push_back(stop);
            return out;
        }
        std::uint32_t last = stop - 2;
        Instr li = at(last);
        if (!sh4::is_control_flow(li.op) && last >= start + 2) {
            last -= 2;
            li = at(last);
        }
        switch (li.op) {
            case Op::BT:
            case Op::BF:
            case Op::BT_S:
            case Op::BF_S:
                push_successor(out, sh4::pcrel_target(li, last));
                if (blocks_.count(stop))
                    out.push_back(stop);
                break;
            case Op::BRA:
                push_successor(out, sh4::pcrel_target(li, last));
                break;
            case Op::BRAF:
            case Op::JMP: {
                auto it = switches_.find(last);
                if (it != switches_.end()) {
                    for (std::uint32_t t : it->second.targets) push_successor(out, t);
                }
                break;
            }
            default:
                break;
        }
        return out;
    }

    void analyse_fp_modes(bool relative) {
        saves_fpscr_ = false;
        for (std::uint32_t b : blocks_) {
            for (std::uint32_t pc = b; pc < block_end_[b]; pc += 2) {
                const Instr i = at(pc);
                if (i.op == Op::STS_L_FPSCR && i.n == 15)
                    saves_fpscr_ = true;
            }
        }
        for (std::uint32_t b : blocks_) block_mode_[b] = {kUnknown, kUnknown};
        block_mode_[spec_.entry] = entry_mode(relative);
        std::vector<std::uint32_t> work{spec_.entry};
        std::set<std::uint32_t> reached{spec_.entry};
        bool have_exit = false;
        FpMode exit_state{kUnknown, kUnknown};
        while (!work.empty()) {
            const std::uint32_t b = work.back();
            work.pop_back();
            if (!blocks_.count(b))
                continue;
            FpMode m = block_mode_[b];
            for (std::uint32_t pc = b; pc < block_end_[b]; pc += 2) m = transfer(m, b, pc, at(pc));
            // Exits: returns, and tail jumps to other functions (their effect applies before
            // return).
            if (ends_in_transfer_[b]) {
                const std::uint32_t stop = block_end_[b];
                std::uint32_t last = stop - 2;
                Instr li = at(last);
                if (!sh4::is_control_flow(li.op) && last >= b + 2) {
                    last -= 2;
                    li = at(last);
                }
                FpMode e = m;
                bool is_exit = false;
                switch (li.op) {
                    case Op::RTS:
                    case Op::RTE:
                        is_exit = true;
                        break;
                    case Op::BRA: {
                        const std::uint32_t t = sh4::pcrel_target(li, last);
                        if (!in_range(t)) {
                            e = call_effect(m, t, false);
                            is_exit = true;
                        }
                        break;
                    }
                    case Op::JMP: {
                        std::uint32_t t;
                        if (constant_in_register(b, last, li.m, t))
                            e = call_effect(m, t, false);
                        is_exit = true;
                        break;
                    }
                    case Op::BRAF:
                        break;  // computed jump within the function (switch), not an exit
                    default:
                        break;
                }
                if (is_exit) {
                    exit_state = have_exit ? merge(exit_state, e) : e;
                    have_exit = true;
                }
            }
            for (std::uint32_t succ : successors(b)) {
                const FpMode merged = reached.count(succ) ? merge(block_mode_[succ], m) : m;
                if (std::getenv("DREAM_EMIT_DEBUG")) {
                    std::fprintf(
                        stderr, "fp-edge %s -> %s : exit(pr=%u sz=%u) merged(pr=%u sz=%u)\n",
                        hex(b).c_str(), hex(succ).c_str(), m.pr, m.sz, merged.pr, merged.sz);
                }
                if (!reached.count(succ) || merged != block_mode_[succ]) {
                    block_mode_[succ] = merged;
                    reached.insert(succ);
                    work.push_back(succ);
                }
            }
        }
        summary_ = have_exit ? exit_state : FpMode{kEntry, kEntry};
    }

    void flush_cycles(std::ostringstream& out) {
        if (pending_cycles_) {
            out << "    c.cycles += " << pending_cycles_ << ";\n";
            pending_cycles_ = 0;
        }
    }

    // Bytes of the function's range that discovery never decoded. Some of it is legitimate: a
    // literal pool sits inside the function and is data, and a compiler may leave alignment
    // padding. A long run is not, and it means the emitted program is missing code that the guest
    // will nevertheless branch to. Nothing else notices that: the unit compiles, the tests pass,
    // and the wrong branch is taken at run time, which is how a branch to another branch's delay
    // slot cost fn_0c081200 141 of its 312 instructions.
    //
    // This needs no run and no reference implementation, which makes it the cheapest check the
    // translator has. It reports rather than fails, because a pool is indistinguishable from code
    // here and a false failure would be worse than a line of output.
    void report_coverage() {
        std::uint32_t covered = 0;
        for (std::uint32_t b : blocks_) covered += block_end_[b] - b;
        const std::uint32_t range = spec_.end - spec_.entry;
        result_.bytes_in_range += range;
        result_.bytes_decoded += covered;
        // Per-function figures double-count, because function ranges overlap: discovery gives a
        // caller a range that covers callees it falls into, and those callees are separately
        // emitted functions. Recording the intervals lets the union be taken at the end, which is
        // the only figure that answers "was this byte compiled".
        result_.function_ranges.push_back({spec_.entry, spec_.end});
        for (std::uint32_t b : blocks_) result_.decoded_runs.push_back({b, block_end_[b]});
        // Literal pools are data and belong to whichever function reads them, so they are recorded
        // globally too: a word one function reads as a constant is not missing code just because
        // another function's range happens to cover it.
        for (std::uint32_t a : literals_) result_.literal_words.push_back({a, a + 2});
    }

    void note(std::uint32_t pc, const Instr& ins, const std::string& what) {
        result_.warnings.push_back(hex(pc) + ": " + sh4::format(ins, pc) + ": " + what);
    }

    // Same alias rule as discovery: a constant target through another RAM alias of the image is
    // the function at the corresponding image address.
    std::uint32_t normalise(std::uint32_t a) const {
        const std::uint32_t phys = a & 0x1FFFFFFFu, base_phys = img_.base & 0x1FFFFFFFu;
        if ((a & 0x1C000000u) == 0x0C000000u && phys >= base_phys &&
            phys < base_phys + img_.bytes.size())
            return img_.base + (phys - base_phys);
        return a;
    }

    std::string call_target_expr(std::uint32_t target) const {
        auto it = known_.find(normalise(target));
        if (it != known_.end())
            return it->second + "(c, m);";
        return "call_indirect(c, m, " + hexu(target) + ");";
    }

    std::string irq_check_if_backedge(std::uint32_t target, std::uint32_t pc) const {
        // Braced: GCC's -Wmisleading-indentation rejects `if (x) f(); goto L;` on one line.
        return (opt_.irq_checks && target <= pc)
                   ? "if (c.cycles >= c.next_event) { c.pc = " + hexu(target) +
                         "; deliver_irq(c, m); } "
                   : "";
    }

    std::uint32_t emit_instruction(std::ostringstream& out, std::uint32_t pc) {
        const Instr ins = at(pc);
        ++result_.instructions;
        out << "    // " << hex(pc) << ": " << sh4::format(ins, pc) << "\n";
        const FpMode before = mode_;
        mode_ = transfer(mode_, current_block_start(pc), pc, ins);
        (void)before;
        if (sh4::has_delay_slot(ins.op)) {
            emit_delayed(out, pc, ins);
            return pc + 4;
        }
        if (ins.op == Op::BT || ins.op == Op::BF) {
            ++pending_cycles_;
            flush_cycles(out);
            const std::uint32_t t = sh4::pcrel_target(ins, pc);
            const std::string cond = ins.op == Op::BT ? "c.t" : "!c.t";
            if (in_range(t)) {
                out << "    if (" << cond << ") { c.cycles += 1; " << irq_check_if_backedge(t, pc)
                    << "goto " << label(t) << "; }\n";
            } else {
                out << "    if (" << cond << ") { c.cycles += 1; " << call_target_expr(t)
                    << " return; }\n";
            }
            return pc + 2;
        }
        ++pending_cycles_;
        out << "    " << lower(pc, ins) << "\n";
        return pc + 2;
    }

    void emit_delayed(std::ostringstream& out, std::uint32_t pc, const Instr& ins) {
        const Instr slot = at(pc + 2);
        ++result_.instructions;
        const bool illegal_slot =
            sh4::has_delay_slot(slot.op) || slot.op == Op::BT || slot.op == Op::BF;
        const std::string slot_code = illegal_slot
                                          ? "unimplemented(c, " + hexu(slot.raw) + ", " +
                                                hexu(pc + 2) + "); // illegal slot instruction"
                                          : lower(pc + 2, slot);
        auto emit_slot = [&] {
            out << "        // " << hex(pc + 2) << ": " << sh4::format(slot, pc + 2)
                << " (delay slot)\n";
            out << "        " << slot_code << "\n";
        };
        pending_cycles_ += 2;
        flush_cycles(out);
        out << "    {\n";
        switch (ins.op) {
            case Op::BT_S:
            case Op::BF_S: {
                const std::uint32_t t = sh4::pcrel_target(ins, pc);
                out << "        const bool taken = " << (ins.op == Op::BT_S ? "c.t" : "!c.t")
                    << ";\n";
                emit_slot();
                if (in_range(t)) {
                    out << "        if (taken) { " << irq_check_if_backedge(t, pc) << "goto "
                        << label(t) << "; }\n";
                } else {
                    out << "        if (taken) { " << call_target_expr(t) << " return; }\n";
                }
                break;
            }
            case Op::BRA: {
                const std::uint32_t t = sh4::pcrel_target(ins, pc);
                emit_slot();
                if (in_range(t)) {
                    out << "        " << irq_check_if_backedge(t, pc) << "goto " << label(t)
                        << ";\n";
                } else {
                    out << "        " << call_target_expr(t) << " return;\n";
                }
                break;
            }
            case Op::BSR: {
                const std::uint32_t t = sh4::pcrel_target(ins, pc);
                out << "        c.pr = " << hexu(pc + 4) << ";\n";
                emit_slot();
                out << "        c.pc = " << hexu(pc) << "; " << call_target_expr(t) << "\n";
                break;
            }
            case Op::BSRF:
            case Op::JSR: {
                // A target loaded from a literal (or immediate) in this block is called directly
                // when it is a function of this unit; otherwise the register value goes through
                // the function table at run time.
                std::uint32_t k = 0;
                const bool constant = constant_in_register(current_block_start(pc), pc, ins.m, k);
                const std::uint32_t direct = normalise(ins.op == Op::BSRF ? pc + 4 + k : k);
                if (constant && known_.count(direct)) {
                    out << "        c.pr = " << hexu(pc + 4) << ";\n";
                    emit_slot();
                    out << "        c.pc = " << hexu(pc) << "; " << call_target_expr(direct)
                        << "\n";
                    ++result_.direct_calls;
                    break;
                }
                if (ins.op == Op::BSRF)
                    out << "        const std::uint32_t target = " << hexu(pc + 4) << " + "
                        << R(ins.m) << ";\n";
                else
                    out << "        const std::uint32_t target = " << R(ins.m) << ";\n";
                out << "        c.pr = " << hexu(pc + 4) << ";\n";
                emit_slot();
                out << "        c.pc = " << hexu(pc) << "; call_indirect(c, m, target);\n";
                break;
            }
            case Op::BRAF:
            case Op::JMP: {
                if (ins.op == Op::BRAF)
                    out << "        const std::uint32_t target = " << hexu(pc + 4) << " + "
                        << R(ins.m) << ";\n";
                else
                    out << "        const std::uint32_t target = " << R(ins.m) << ";\n";
                emit_slot();
                auto it = switches_.find(pc);
                if (it != switches_.end()) {
                    ++result_.switches_recovered;
                    out << "        switch (target) {\n";
                    std::set<std::uint32_t> seen;
                    for (std::uint32_t t : it->second.targets) {
                        if (!seen.insert(t).second)
                            continue;
                        out << "            case " << hexu(t) << ": "
                            << irq_check_if_backedge(t, pc) << "goto " << label(t) << ";\n";
                    }
                    out << "            default: c.pc = " << hexu(pc)
                        << "; call_indirect(c, m, target); return;\n";
                    out << "        }\n";
                } else {
                    if (ins.op == Op::BRAF)
                        note(pc, ins,
                             "computed jump with no recovered table: emitted as tail call");
                    out << "        c.pc = " << hexu(pc)
                        << "; call_indirect(c, m, target); return;\n";
                }
                break;
            }
            case Op::RTS:
                // PR is read before the slot. A return to anywhere but the entry PR is a
                // non-local return (setjmp/longjmp, a task switch): the host frames are dropped
                // and run_guest re-enters the target. A resumed body returns to run_guest anyway.
                out << "        const std::uint32_t rts_target = c.pr;\n";
                emit_slot();
                if (opt_.trace)
                    out << "        trace_return(c, m, " << hexu(pc) << ");\n";
                out << "        if (!resume_pc && rts_target != entry_pr) nonlocal_return(c, m, "
                       "rts_target);\n";
                out << "        return;\n";
                break;
            case Op::RTE:
                emit_slot();
                // Closing a delivered exception: unwind to the delivery point. Otherwise the RTE
                // is a jump and control continues at SPC (docs/emitter-design.md).
                out << "        c.pc = " << hexu(pc)
                    << "; if (rte(c, m)) return; nonlocal_return(c, m, c.pc);\n";
                break;
            default:
                out << "        unimplemented(c, " << hexu(ins.raw) << ", " << hexu(pc) << ");\n";
                break;
        }
        out << "    }\n";
        if (slot_labels_.count(pc + 2)) {
            // Another branch targets this slot: entering here executes the slot instruction as an
            // ordinary instruction and falls through to pc+4. The not-taken path of the owning
            // branch skips this copy.
            out << "    goto " << label(pc) << "_skip;\n";
            out << label(pc + 2) << ":\n";
            out << "    " << slot_code << "\n";
            out << label(pc) << "_skip:;\n";
        }
        if (call_returns_.count(pc + 4) && !blocks_.count(pc + 4))
            out << label(pc + 4) << ":;\n";  // resume point after the call
    }

    // A pool word some instruction in the unit stores to (a run-time patched slot, like the
    // program-entry address the Katana stub writes before jumping) must be read at run time.
    bool patched(std::uint32_t addr) const { return written_ && written_->count(addr & ~3u); }
    // Addresses read as constants. A literal pool sits inside the function it serves, so those
    // bytes are data and must not be reported as code discovery failed to decode; without this the
    // coverage report is mostly pools and nobody reads it.
    std::set<std::uint32_t> literals_;
    std::string literal32(std::uint32_t addr) {
        literals_.insert(addr & ~3u);
        literals_.insert((addr & ~3u) + 2);
        if (opt_.fold_literals && img_.contains(addr, 4) && !patched(addr))
            return hexu(img_.read32(addr));
        return "m.read32(" + hexu(addr) + ")";
    }
    std::string literal16(std::uint32_t addr) {
        literals_.insert(addr & ~1u);
        if (opt_.fold_literals && img_.contains(addr, 2) && !patched(addr)) {
            const std::int32_t v = static_cast<std::int16_t>(img_.read16(addr));
            return U32(I(v));
        }
        return U32("static_cast<std::int16_t>(m.read16(" + hexu(addr) + "))");
    }

    static std::string ldsx(const std::string& r, const std::string& expr, unsigned bytes,
                            const std::string& cast) {
        // load with sign extension: r = (uint32)(int32)(castT)(expr)
        (void)bytes;
        return r + " = " + U32(S32("static_cast<" + cast + ">(" + expr + ")")) + ";";
    }

    std::uint32_t current_block_start(std::uint32_t pc) const {
        auto it = blocks_.upper_bound(pc);
        return it == blocks_.begin() ? spec_.entry : *std::prev(it);
    }

    // FP instruction under a known or unknown precision/size mode. When the mode is unknown the two
    // lowerings are emitted behind a runtime test of FPSCR (ADR 6 fallback (c)).
    std::string lower_fp(std::uint32_t pc, const Instr& ins) {
        const bool needs_pr = pr_dependent(ins.op), needs_sz = sz_dependent(ins.op);
        const FpMode entry = entry_mode(false);
        const std::uint8_t pr = resolve_bit(mode_.pr, entry.pr),
                           sz = resolve_bit(mode_.sz, entry.sz);
        if ((needs_pr && pr == kUnknown) || (needs_sz && sz == kUnknown)) {
            ++result_.fp_runtime_branches;
            note(pc, ins,
                 needs_pr ? "FPSCR.PR unknown here: runtime branch emitted"
                          : "FPSCR.SZ unknown here: runtime branch emitted");
            if (needs_pr) {
                return "if (c.fpscr & FPSCR_PR) { " +
                       lower_fp_mode(ins, 1, mode_.sz == kUnknown ? 0 : mode_.sz) + " } else { " +
                       lower_fp_mode(ins, 0, mode_.sz == kUnknown ? 0 : mode_.sz) + " }";
            }
            return "if (c.fpscr & FPSCR_SZ) { " + lower_fp_mode(ins, 0, 1) + " } else { " +
                   lower_fp_mode(ins, 0, 0) + " }";
        }
        return lower_fp_mode(ins, mode_.pr == kUnknown ? 0 : mode_.pr,
                             mode_.sz == kUnknown ? 0 : mode_.sz);
    }

    static bool pr_dependent(Op op) {
        switch (op) {
            case Op::FADD:
            case Op::FSUB:
            case Op::FMUL:
            case Op::FDIV:
            case Op::FCMP_EQ:
            case Op::FCMP_GT:
            case Op::FLOAT:
            case Op::FTRC:
            case Op::FSQRT:
                return true;
            default:
                return false;
        }
    }
    static bool sz_dependent(Op op) {
        switch (op) {
            case Op::FMOV:
            case Op::FMOV_LOAD:
            case Op::FMOV_STORE:
            case Op::FMOV_LOAD_INC:
            case Op::FMOV_STORE_DEC:
            case Op::FMOV_LOAD_R0:
            case Op::FMOV_STORE_R0:
                return true;
            default:
                return false;
        }
    }

    std::string lower_fp_mode(const Instr& ins, unsigned pr, unsigned sz) {
        const unsigned n = ins.n, m = ins.m;
        const std::string rn = R(n), rm = R(m), r0 = R(0);
        const std::string frn = "c.fr[" + std::to_string(n) + "]",
                          frm = "c.fr[" + std::to_string(m) + "]";
        const std::string drn = "get_dr(c, " + std::to_string(n & ~1u) + ")";
        const std::string drm = "get_dr(c, " + std::to_string(m & ~1u) + ")";
        auto set_drn = [&](const std::string& v) {
            return "set_dr(c, " + std::to_string(n & ~1u) + ", " + v + ");";
        };
        switch (ins.op) {
            case Op::FLDI0:
                return frn + " = 0.0f;";
            case Op::FLDI1:
                return frn + " = 1.0f;";
            case Op::FLDS:
                return "c.fpul = f2u(" + frn + ");";
            case Op::FSTS:
                return frn + " = u2f(c.fpul);";
            case Op::FABS:
                return frn + " = u2f(f2u(" + frn + ") & 0x7fffffffu);";
            case Op::FNEG:
                return frn + " = u2f(f2u(" + frn + ") ^ 0x80000000u);";
            case Op::FADD:
                return pr ? set_drn(drn + " + " + drm) : frn + " += " + frm + ";";
            case Op::FSUB:
                return pr ? set_drn(drn + " - " + drm) : frn + " -= " + frm + ";";
            case Op::FMUL:
                return pr ? set_drn(drn + " * " + drm) : frn + " *= " + frm + ";";
            case Op::FDIV:
                return pr ? set_drn(drn + " / " + drm) : frn + " /= " + frm + ";";
            case Op::FCMP_EQ:
                return "c.t = " + (pr ? drn + " == " + drm : frn + " == " + frm) + ";";
            case Op::FCMP_GT:
                return "c.t = " + (pr ? drn + " > " + drm : frn + " > " + frm) + ";";
            case Op::FMAC:
                return frn + " = fmac(c.fr[0], " + frm + ", " + frn + ");";
            case Op::FLOAT:
                return pr ? set_drn("static_cast<double>(static_cast<std::int32_t>(c.fpul))")
                          : frn + " = static_cast<float>(static_cast<std::int32_t>(c.fpul));";
            case Op::FTRC:
                return "c.fpul = ftrc(" + (pr ? drn : frn) + ");";
            case Op::FSQRT:
                return pr ? set_drn("std::sqrt(" + drn + ")") : frn + " = std::sqrt(" + frn + ");";
            case Op::FSRRA:
                return frn + " = 1.0f / std::sqrt(" + frn + ");";
            case Op::FCNVSD:
                return set_drn("static_cast<double>(u2f(c.fpul))");
            case Op::FCNVDS:
                return "c.fpul = f2u(static_cast<float>(" + drn + "));";
            case Op::FSCA:
                return "fsca(c, " + std::to_string(n & ~1u) + ");";
            case Op::FIPR:
                return "fipr(c, " + std::to_string(m) + ", " + std::to_string(n) + ");";
            case Op::FTRV:
                return "ftrv(c, " + std::to_string(n) + ");";
            case Op::FMOV:
                return sz ? "fmov_pair_load(c, " + std::to_string(n) + ", fmov_pair_store(c, " +
                                std::to_string(m) + "));"
                          : frn + " = " + frm + ";";
            case Op::FMOV_LOAD:
                return sz ? "fmov_pair_load(c, " + std::to_string(n) + ", read_pair(m, " + rm +
                                "));"
                          : frn + " = u2f(m.read32(" + rm + "));";
            case Op::FMOV_LOAD_INC:
                return sz ? "fmov_pair_load(c, " + std::to_string(n) + ", read_pair(m, " + rm +
                                ")); " + rm + " += 8;"
                          : frn + " = u2f(m.read32(" + rm + ")); " + rm + " += 4;";
            case Op::FMOV_LOAD_R0:
                return sz ? "fmov_pair_load(c, " + std::to_string(n) + ", read_pair(m, " + rm +
                                " + " + r0 + "));"
                          : frn + " = u2f(m.read32(" + rm + " + " + r0 + "));";
            case Op::FMOV_STORE:
                return sz ? "write_pair(m, " + rn + ", fmov_pair_store(c, " + std::to_string(m) +
                                "));"
                          : "m.write32(" + rn + ", f2u(" + frm + "));";
            case Op::FMOV_STORE_DEC:
                return sz ? rn + " -= 8; write_pair(m, " + rn + ", fmov_pair_store(c, " +
                                std::to_string(m) + "));"
                          : rn + " -= 4; m.write32(" + rn + ", f2u(" + frm + "));";
            case Op::FMOV_STORE_R0:
                return sz ? "write_pair(m, " + rn + " + " + r0 + ", fmov_pair_store(c, " +
                                std::to_string(m) + "));"
                          : "m.write32(" + rn + " + " + r0 + ", f2u(" + frm + "));";
            default:
                return "unimplemented(c, " + hexu(ins.raw) + ", 0u);";
        }
    }

    std::string lower(std::uint32_t pc, const Instr& ins) {
        const unsigned n = ins.n, m = ins.m;
        const std::int32_t imm = ins.imm;
        const std::string rn = R(n), rm = R(m), r0 = R(0);
        const std::string d = std::to_string(imm) + "u", d2 = std::to_string(imm * 2) + "u",
                          d4 = std::to_string(imm * 4) + "u";
        const std::string bank = "c.r_bank[" + std::to_string(ins.bank) + "]";
        switch (ins.op) {
            case Op::MOV_I:
                return rn + " = " + U32(I(imm)) + ";";
            case Op::MOV:
                return rn + " = " + rm + ";";
            case Op::MOV_W_PCREL:
                return rn + " = " + literal16(sh4::pcrel_target(ins, pc)) + ";";
            case Op::MOV_L_PCREL:
                return rn + " = " + literal32(sh4::pcrel_target(ins, pc)) + ";";
            case Op::MOVA:
                return r0 + " = " + hexu(sh4::pcrel_target(ins, pc)) + ";";
            case Op::MOVT:
                return rn + " = c.t;";
            case Op::MOV_B_S:
                return "m.write8(" + rn + ", static_cast<std::uint8_t>(" + rm + "));";
            case Op::MOV_W_S:
                return "m.write16(" + rn + ", static_cast<std::uint16_t>(" + rm + "));";
            case Op::MOV_L_S:
                return "m.write32(" + rn + ", " + rm + ");";
            case Op::MOV_B_L:
                return ldsx(rn, "m.read8(" + rm + ")", 1, "std::int8_t");
            case Op::MOV_W_L:
                return ldsx(rn, "m.read16(" + rm + ")", 2, "std::int16_t");
            case Op::MOV_L_L:
                return rn + " = m.read32(" + rm + ");";
            case Op::MOV_B_SD:
                return rn + " -= 1; m.write8(" + rn + ", static_cast<std::uint8_t>(" + rm + "));";
            case Op::MOV_W_SD:
                return rn + " -= 2; m.write16(" + rn + ", static_cast<std::uint16_t>(" + rm + "));";
            case Op::MOV_L_SD:
                return rn + " -= 4; m.write32(" + rn + ", " + rm + ");";
            case Op::MOV_B_LI:
                return ldsx(rn, "m.read8(" + rm + ")", 1, "std::int8_t") +
                       (n != m ? " " + rm + " += 1;" : "");
            case Op::MOV_W_LI:
                return ldsx(rn, "m.read16(" + rm + ")", 2, "std::int16_t") +
                       (n != m ? " " + rm + " += 2;" : "");
            case Op::MOV_L_LI:
                return rn + " = m.read32(" + rm + ");" + (n != m ? " " + rm + " += 4;" : "");
            case Op::MOV_L_S_DISP:
                return "m.write32(" + rn + " + " + d4 + ", " + rm + ");";
            case Op::MOV_L_L_DISP:
                return rn + " = m.read32(" + rm + " + " + d4 + ");";
            case Op::MOV_B_S_DISP0:
                return "m.write8(" + rm + " + " + d + ", static_cast<std::uint8_t>(" + r0 + "));";
            case Op::MOV_W_S_DISP0:
                return "m.write16(" + rm + " + " + d2 + ", static_cast<std::uint16_t>(" + r0 +
                       "));";
            case Op::MOV_B_L_DISP0:
                return ldsx(r0, "m.read8(" + rm + " + " + d + ")", 1, "std::int8_t");
            case Op::MOV_W_L_DISP0:
                return ldsx(r0, "m.read16(" + rm + " + " + d2 + ")", 2, "std::int16_t");
            case Op::MOV_B_S_R0:
                return "m.write8(" + rn + " + " + r0 + ", static_cast<std::uint8_t>(" + rm + "));";
            case Op::MOV_W_S_R0:
                return "m.write16(" + rn + " + " + r0 + ", static_cast<std::uint16_t>(" + rm +
                       "));";
            case Op::MOV_L_S_R0:
                return "m.write32(" + rn + " + " + r0 + ", " + rm + ");";
            case Op::MOV_B_L_R0:
                return ldsx(rn, "m.read8(" + rm + " + " + r0 + ")", 1, "std::int8_t");
            case Op::MOV_W_L_R0:
                return ldsx(rn, "m.read16(" + rm + " + " + r0 + ")", 2, "std::int16_t");
            case Op::MOV_L_L_R0:
                return rn + " = m.read32(" + rm + " + " + r0 + ");";
            case Op::MOV_B_S_GBR:
                return "m.write8(c.gbr + " + d + ", static_cast<std::uint8_t>(" + r0 + "));";
            case Op::MOV_W_S_GBR:
                return "m.write16(c.gbr + " + d2 + ", static_cast<std::uint16_t>(" + r0 + "));";
            case Op::MOV_L_S_GBR:
                return "m.write32(c.gbr + " + d4 + ", " + r0 + ");";
            case Op::MOV_B_L_GBR:
                return ldsx(r0, "m.read8(c.gbr + " + d + ")", 1, "std::int8_t");
            case Op::MOV_W_L_GBR:
                return ldsx(r0, "m.read16(c.gbr + " + d2 + ")", 2, "std::int16_t");
            case Op::MOV_L_L_GBR:
                return r0 + " = m.read32(c.gbr + " + d4 + ");";
            case Op::SWAP_B:
                return rn + " = (" + rm + " & 0xffff0000u) | ((" + rm + " & 0xffu) << 8) | ((" +
                       rm + " >> 8) & 0xffu);";
            case Op::SWAP_W:
                return rn + " = (" + rm + " << 16) | (" + rm + " >> 16);";
            case Op::XTRCT:
                return rn + " = (" + rm + " << 16) | (" + rn + " >> 16);";
            case Op::ADD:
                return rn + " += " + rm + ";";
            case Op::ADD_I:
                return rn + " += " + U32(I(imm)) + ";";
            case Op::ADDC:
                return rn + " = addc(c, " + rn + ", " + rm + ");";
            case Op::ADDV:
                return rn + " = addv(c, " + rn + ", " + rm + ");";
            case Op::CMP_EQ_I:
                return "c.t = " + r0 + " == " + U32(I(imm)) + ";";
            // A register compared with itself is how SH-4 code sets or clears T in one
            // instruction, and Charge 'N Blast does it (`cmp/hi r15,r15`). Emitted literally it is
            // `c.r[15] > c.r[15]`, which every compiler warns about and -Werror then rejects, so
            // a title using the idiom would not build at all. The answer the hardware gives is a
            // constant, so emit the constant.
            case Op::CMP_EQ:
                if (ins.n == ins.m)
                    return "c.t = true;  // " + rn + " compared with itself";
                return "c.t = " + rn + " == " + rm + ";";
            case Op::CMP_HS:
                if (ins.n == ins.m)
                    return "c.t = true;  // " + rn + " compared with itself";
                return "c.t = " + rn + " >= " + rm + ";";
            case Op::CMP_GE:
                if (ins.n == ins.m)
                    return "c.t = true;  // " + rn + " compared with itself";
                return "c.t = " + S32(rn) + " >= " + S32(rm) + ";";
            case Op::CMP_HI:
                if (ins.n == ins.m)
                    return "c.t = false;  // " + rn + " compared with itself";
                return "c.t = " + rn + " > " + rm + ";";
            case Op::CMP_GT:
                if (ins.n == ins.m)
                    return "c.t = false;  // " + rn + " compared with itself";
                return "c.t = " + S32(rn) + " > " + S32(rm) + ";";
            case Op::CMP_PZ:
                return "c.t = " + S32(rn) + " >= 0;";
            case Op::CMP_PL:
                return "c.t = " + S32(rn) + " > 0;";
            case Op::CMP_STR:
                return "c.t = cmp_str(" + rn + ", " + rm + ");";
            case Op::DIV1:
                return rn + " = div1(c, " + rn + ", " + rm + ");";
            case Op::DIV0S:
                return "div0s(c, " + rn + ", " + rm + ");";
            case Op::DIV0U:
                return "div0u(c);";
            case Op::DMULS_L:
                return "dmuls(c, " + rn + ", " + rm + ");";
            case Op::DMULU_L:
                return "dmulu(c, " + rn + ", " + rm + ");";
            case Op::DT:
                return rn + " -= 1; c.t = " + rn + " == 0;";
            case Op::EXTS_B:
                return rn + " = " + U32(S32("static_cast<std::int8_t>(" + rm + ")")) + ";";
            case Op::EXTS_W:
                return rn + " = " + U32(S32("static_cast<std::int16_t>(" + rm + ")")) + ";";
            case Op::EXTU_B:
                return rn + " = " + rm + " & 0xffu;";
            case Op::EXTU_W:
                return rn + " = " + rm + " & 0xffffu;";
            case Op::MAC_L:
                return "{ const std::int32_t a = " + S32("m.read32(" + rn + ")") + "; " + rn +
                       " += 4; const std::int32_t b = " + S32("m.read32(" + rm + ")") + "; " + rm +
                       " += 4; mac_l(c, a, b); }";
            case Op::MAC_W:
                return "{ const std::int16_t a = static_cast<std::int16_t>(m.read16(" + rn +
                       ")); " + rn +
                       " += 2; const std::int16_t b = static_cast<std::int16_t>(m.read16(" + rm +
                       ")); " + rm + " += 2; mac_w(c, a, b); }";
            case Op::MUL_L:
                return "c.macl = " + rn + " * " + rm + ";";
            case Op::MULS_W:
                return "c.macl = " +
                       U32(S32("static_cast<std::int16_t>(" + rn + ")") + " * " +
                           S32("static_cast<std::int16_t>(" + rm + ")")) +
                       ";";
            case Op::MULU_W:
                return "c.macl = (" + rn + " & 0xffffu) * (" + rm + " & 0xffffu);";
            case Op::NEG:
                return rn + " = 0u - " + rm + ";";
            case Op::NEGC:
                return rn + " = negc(c, " + rm + ");";
            case Op::SUB:
                return rn + " -= " + rm + ";";
            case Op::SUBC:
                return rn + " = subc(c, " + rn + ", " + rm + ");";
            case Op::SUBV:
                return rn + " = subv(c, " + rn + ", " + rm + ");";
            case Op::AND:
                return rn + " &= " + rm + ";";
            case Op::AND_I:
                return r0 + " &= " + d + ";";
            case Op::AND_B_GBR:
                return "{ const std::uint32_t a = c.gbr + " + r0 +
                       "; m.write8(a, static_cast<std::uint8_t>(m.read8(a) & " + d + ")); }";
            case Op::NOT:
                return rn + " = ~" + rm + ";";
            case Op::OR:
                return rn + " |= " + rm + ";";
            case Op::OR_I:
                return r0 + " |= " + d + ";";
            case Op::OR_B_GBR:
                return "{ const std::uint32_t a = c.gbr + " + r0 +
                       "; m.write8(a, static_cast<std::uint8_t>(m.read8(a) | " + d + ")); }";
            case Op::TAS_B:
                return "{ const std::uint8_t v = m.read8(" + rn + "); c.t = v == 0; m.write8(" +
                       rn + ", static_cast<std::uint8_t>(v | 0x80u)); }";
            case Op::TST:
                return "c.t = (" + rn + " & " + rm + ") == 0;";
            case Op::TST_I:
                return "c.t = (" + r0 + " & " + d + ") == 0;";
            case Op::TST_B_GBR:
                return "c.t = (m.read8(c.gbr + " + r0 + ") & " + d + ") == 0;";
            case Op::XOR:
                return rn + " ^= " + rm + ";";
            case Op::XOR_I:
                return r0 + " ^= " + d + ";";
            case Op::XOR_B_GBR:
                return "{ const std::uint32_t a = c.gbr + " + r0 +
                       "; m.write8(a, static_cast<std::uint8_t>(m.read8(a) ^ " + d + ")); }";
            case Op::ROTL:
                return "c.t = " + rn + " >> 31; " + rn + " = (" + rn + " << 1) | c.t;";
            case Op::ROTR:
                return "c.t = " + rn + " & 1u; " + rn + " = (" + rn + " >> 1) | (c.t << 31);";
            case Op::ROTCL:
                return rn + " = rotcl(c, " + rn + ");";
            case Op::ROTCR:
                return rn + " = rotcr(c, " + rn + ");";
            case Op::SHAD:
                return rn + " = shad(" + rn + ", " + rm + ");";
            case Op::SHLD:
                return rn + " = shld(" + rn + ", " + rm + ");";
            case Op::SHAL:
            case Op::SHLL:
                return "c.t = " + rn + " >> 31; " + rn + " <<= 1;";
            case Op::SHAR:
                return "c.t = " + rn + " & 1u; " + rn + " = " + U32(S32(rn) + " >> 1") + ";";
            case Op::SHLR:
                return "c.t = " + rn + " & 1u; " + rn + " >>= 1;";
            case Op::SHLL2:
                return rn + " <<= 2;";
            case Op::SHLL8:
                return rn + " <<= 8;";
            case Op::SHLL16:
                return rn + " <<= 16;";
            case Op::SHLR2:
                return rn + " >>= 2;";
            case Op::SHLR8:
                return rn + " >>= 8;";
            case Op::SHLR16:
                return rn + " >>= 16;";
            case Op::CLRMAC:
                return "c.mach = 0; c.macl = 0;";
            case Op::CLRS:
                return "c.sr &= ~SR_S;";
            case Op::CLRT:
                return "c.t = 0;";
            case Op::SETS:
                return "c.sr |= SR_S;";
            case Op::SETT:
                return "c.t = 1;";
            case Op::NOP:
                return "/* nop */";
            case Op::LDC_SR:
                return "write_sr(c, " + rm + ");";
            case Op::LDC_GBR:
                return "c.gbr = " + rm + ";";
            case Op::LDC_VBR:
                return "c.vbr = " + rm + ";";
            case Op::LDC_SSR:
                return "c.ssr = " + rm + ";";
            case Op::LDC_SPC:
                return "c.spc = " + rm + ";";
            case Op::LDC_DBR:
                return "c.dbr = " + rm + ";";
            case Op::LDC_BANK:
                return bank + " = " + rm + ";";
            case Op::LDC_L_SR:
                return "write_sr(c, m.read32(" + rm + ")); " + rm + " += 4;";
            case Op::LDC_L_GBR:
                return "c.gbr = m.read32(" + rm + "); " + rm + " += 4;";
            case Op::LDC_L_VBR:
                return "c.vbr = m.read32(" + rm + "); " + rm + " += 4;";
            case Op::LDC_L_SSR:
                return "c.ssr = m.read32(" + rm + "); " + rm + " += 4;";
            case Op::LDC_L_SPC:
                return "c.spc = m.read32(" + rm + "); " + rm + " += 4;";
            case Op::LDC_L_DBR:
                return "c.dbr = m.read32(" + rm + "); " + rm + " += 4;";
            case Op::LDC_L_BANK:
                return bank + " = m.read32(" + rm + "); " + rm + " += 4;";
            case Op::LDS_MACH:
                return "c.mach = " + rm + ";";
            case Op::LDS_MACL:
                return "c.macl = " + rm + ";";
            case Op::LDS_PR:
                return "c.pr = " + rm + ";";
            case Op::LDS_L_MACH:
                return "c.mach = m.read32(" + rm + "); " + rm + " += 4;";
            case Op::LDS_L_MACL:
                return "c.macl = m.read32(" + rm + "); " + rm + " += 4;";
            case Op::LDS_L_PR:
                return "c.pr = m.read32(" + rm + "); " + rm + " += 4;";
            case Op::STC_SR:
                return rn + " = read_sr(c);";
            case Op::STC_GBR:
                return rn + " = c.gbr;";
            case Op::STC_VBR:
                return rn + " = c.vbr;";
            case Op::STC_SSR:
                return rn + " = c.ssr;";
            case Op::STC_SPC:
                return rn + " = c.spc;";
            case Op::STC_SGR:
                return rn + " = c.sgr;";
            case Op::STC_DBR:
                return rn + " = c.dbr;";
            case Op::STC_BANK:
                return rn + " = " + bank + ";";
            case Op::STC_L_SR:
                return rn + " -= 4; m.write32(" + rn + ", read_sr(c));";
            case Op::STC_L_GBR:
                return rn + " -= 4; m.write32(" + rn + ", c.gbr);";
            case Op::STC_L_VBR:
                return rn + " -= 4; m.write32(" + rn + ", c.vbr);";
            case Op::STC_L_SSR:
                return rn + " -= 4; m.write32(" + rn + ", c.ssr);";
            case Op::STC_L_SPC:
                return rn + " -= 4; m.write32(" + rn + ", c.spc);";
            case Op::STC_L_SGR:
                return rn + " -= 4; m.write32(" + rn + ", c.sgr);";
            case Op::STC_L_DBR:
                return rn + " -= 4; m.write32(" + rn + ", c.dbr);";
            case Op::STC_L_BANK:
                return rn + " -= 4; m.write32(" + rn + ", " + bank + ");";
            case Op::STS_MACH:
                return rn + " = c.mach;";
            case Op::STS_MACL:
                return rn + " = c.macl;";
            case Op::STS_PR:
                return rn + " = c.pr;";
            case Op::STS_L_MACH:
                return rn + " -= 4; m.write32(" + rn + ", c.mach);";
            case Op::STS_L_MACL:
                return rn + " -= 4; m.write32(" + rn + ", c.macl);";
            case Op::STS_L_PR:
                return rn + " -= 4; m.write32(" + rn + ", c.pr);";
            case Op::TRAPA:
                return "c.pc = " + hexu(pc) + "; trapa(c, m, " + d + ");";
            case Op::MOVCA_L:
                return "m.write32(" + rn + ", " + r0 + ");";
            case Op::PREF:
                return "if ((" + rn + " & 0xfc000000u) == 0xe0000000u) m.sq_flush(" + rn +
                       "); /* else cache hint */";
            case Op::OCBI:
            case Op::OCBP:
            case Op::OCBWB:
                return "/* cache op: no-op on the host */";
            case Op::FRCHG:
                return "frchg(c);";
            case Op::FSCHG:
                return "c.fpscr ^= FPSCR_SZ;";
            case Op::LDS_FPUL:
                return "c.fpul = " + rm + ";";
            case Op::STS_FPUL:
                return rn + " = c.fpul;";
            case Op::LDS_L_FPUL:
                return "c.fpul = m.read32(" + rm + "); " + rm + " += 4;";
            case Op::STS_L_FPUL:
                return rn + " -= 4; m.write32(" + rn + ", c.fpul);";
            case Op::STS_FPSCR:
                return rn + " = c.fpscr;";
            case Op::STS_L_FPSCR:
                return rn + " -= 4; m.write32(" + rn + ", c.fpscr);";
            case Op::LDS_FPSCR:
                return "write_fpscr(c, " + rm + ");";
            case Op::LDS_L_FPSCR:
                return "write_fpscr(c, m.read32(" + rm + ")); " + rm + " += 4;";
            case Op::FLDI0:
            case Op::FLDI1:
            case Op::FLDS:
            case Op::FSTS:
            case Op::FABS:
            case Op::FNEG:
            case Op::FADD:
            case Op::FSUB:
            case Op::FMUL:
            case Op::FDIV:
            case Op::FCMP_EQ:
            case Op::FCMP_GT:
            case Op::FMAC:
            case Op::FLOAT:
            case Op::FTRC:
            case Op::FSQRT:
            case Op::FSRRA:
            case Op::FCNVSD:
            case Op::FCNVDS:
            case Op::FSCA:
            case Op::FIPR:
            case Op::FTRV:
            case Op::FMOV:
            case Op::FMOV_LOAD:
            case Op::FMOV_LOAD_INC:
            case Op::FMOV_LOAD_R0:
            case Op::FMOV_STORE:
            case Op::FMOV_STORE_DEC:
            case Op::FMOV_STORE_R0:
                return lower_fp(pc, ins);
            default:
                ++result_.unlowered;
                note(pc, ins, "not lowered yet");
                return "unimplemented(c, " + hexu(ins.raw) + ", " + hexu(pc) + ");";
        }
    }

    const Image& img_;
    const FunctionSpec& spec_;
    const EmitOptions& opt_;
    const std::map<std::uint32_t, std::string>& known_;
    const Summaries& summaries_;
    EmitResult& result_;
    FpMode summary_{kEntry, kEntry};
    bool saves_fpscr_ = false;
    const std::set<std::uint32_t>* written_ = nullptr;  // image words stored to by unit code
    std::set<std::uint32_t> blocks_;
    std::set<std::uint32_t> labels_;
    std::set<std::uint32_t> call_returns_;
    std::map<std::uint32_t, std::uint32_t> block_end_;
    std::map<std::uint32_t, bool> ends_in_transfer_;
    std::map<std::uint32_t, FpMode> block_mode_;
    std::set<std::uint32_t> slot_labels_;
    std::map<std::uint32_t, SwitchTable> switches_;
    FpMode mode_;
    unsigned pending_cycles_ = 0;
};

}  // namespace

EmitResult emit_unit(const Image& image, const std::vector<FunctionSpec>& functions,
                     const EmitOptions& options) {
    EmitResult result;
    std::map<std::uint32_t, std::string> known;
    std::vector<FunctionSpec> specs = functions;
    for (auto& f : specs) {
        if (f.name.empty())
            f.name = default_name(f.entry);
        known[f.entry] = f.name;
    }
    std::ostringstream src, hdr;
    hdr << "// Generated by dream-translate. Do not edit.\n#pragma once\n#include "
           "\"dream/runtime/memory.h\"\n"
        << "#include \"dream/runtime/sh4/ctx.h\"\n\nnamespace dream::gen {\n";
    src << "// Generated by dream-translate from image base " << hex(image.base)
        << ". Do not edit.\n"
        << "#include <cstdint>\n#include \"dream/runtime/memory.h\"\n#include "
           "\"dream/runtime/sh4/abi.h\"\n"
        << "#include \"dream/runtime/sh4/ctx.h\"\n#include \"dream/runtime/sh4/ops.h\"\n\n"
        << "namespace dream::gen {\nusing namespace dream::sh4;\nusing dream::Memory;\n\n";
    for (const auto& f : specs) {
        hdr << "void " << f.name << "(dream::sh4::Ctx& c, dream::Memory& m);\n";
        src << "void " << f.name << "(Ctx& c, Memory& m);\n";
        src << "static void " << f.name
            << "__resume(Ctx& c, Memory& m, std::uint32_t resume_pc);\n";
    }
    src << "\n";
    // Pass 0: pool words the unit itself writes (run-time patched literals) are never folded.
    std::set<std::uint32_t> written;
    {
        Summaries none;
        for (const auto& f : specs) {
            EmitResult scratch;
            FunctionEmitter fe(image, f, options, known, none, scratch);
            fe.scan_stores(written);
        }
    }
    result.patched_words = written.size();
    // Callee summaries: iterate to a fixpoint over the call graph. A summary that keeps changing
    // (recursion through a toggle) is widened to unknown after a few rounds.
    Summaries summaries;
    std::map<std::uint32_t, int> changes;
    for (int round = 0; round < 8; ++round) {
        bool changed = false;
        for (const auto& f : specs) {
            EmitResult scratch;
            FunctionEmitter fe(image, f, options, known, summaries, scratch);
            fe.set_written(&written);
            FpMode sm = fe.summarise();
            auto it = summaries.find(f.entry);
            if (it == summaries.end() || it->second != sm) {
                if (++changes[f.entry] > 4)
                    sm = {kUnknown, kUnknown};
                summaries[f.entry] = sm;
                changed = true;
            }
        }
        if (!changed)
            break;
    }
    for (const auto& [entry, sm] : summaries) {
        if (sm.pr == kUnknown || sm.sz == kUnknown)
            ++result.unknown_summaries;
    }
    // Entry modes: seed the functions nobody calls directly with their spec's default, then push
    // the mode at each call site into the callee and join (disagreement -> unknown) to a fixpoint.
    {
        std::map<std::uint32_t, std::vector<std::pair<std::uint32_t, FpMode>>> sites;
        std::set<std::uint32_t> called;
        for (const auto& f : specs) {
            EmitResult scratch;
            FunctionEmitter fe(image, f, options, known, summaries, scratch);
            fe.set_written(&written);
            fe.call_site_modes(sites[f.entry]);
            for (const auto& [t, m] : sites[f.entry]) called.insert(t);
        }
        auto default_of = [](const FunctionSpec& f) {
            return FpMode{static_cast<std::uint8_t>((f.fpscr_entry >> 19) & 1),
                          static_cast<std::uint8_t>((f.fpscr_entry >> 20) & 1)};
        };
        auto resolve_abs = [](std::uint8_t rel, std::uint8_t e) -> std::uint8_t {
            switch (rel) {
                case 0:
                case 1:
                    return rel;
                case kEntry:
                    return e;
                case kEntryToggled:
                    return e == kUnknown ? kUnknown : static_cast<std::uint8_t>(e ^ 1u);
                default:
                    return kUnknown;
            }
        };
        auto join = [](std::uint8_t a, std::uint8_t b) -> std::uint8_t {
            return a == b ? a : kUnknown;
        };
        std::map<std::uint32_t, FpMode> entry_abs;
        std::map<std::uint32_t, const FunctionSpec*> by_entry;
        for (const auto& f : specs) {
            by_entry[f.entry] = &f;
            if (called.count(f.entry))
                continue;
            // Nothing in the image calls this one, so it is entered from outside the analysis: an
            // interrupt vector, the program's own entry, or an indirect call. An interrupt arrives
            // in whatever floating-point mode the code it interrupted was using, which is not a
            // property of the handler and cannot be deduced from the image. Assuming the reset mode
            // here is a guess, and a wrong guess compiles `fmov` to move the wrong number of bytes
            // and walk its pointer at the wrong rate; unknown costs a runtime branch and is always
            // right (ADR 6 fallback (c)).
            entry_abs[f.entry] = FpMode{kUnknown, kUnknown};
        }
        for (int round = 0; round < 32; ++round) {
            bool changed = false;
            for (const auto& [caller, list] : sites) {
                auto ce = entry_abs.find(caller);
                if (ce == entry_abs.end())
                    continue;
                for (const auto& [callee, rel] : list) {
                    const FpMode abs{resolve_abs(rel.pr, ce->second.pr),
                                     resolve_abs(rel.sz, ce->second.sz)};
                    auto it = entry_abs.find(callee);
                    if (it == entry_abs.end()) {
                        entry_abs[callee] = abs;
                        changed = true;
                    } else {
                        const FpMode joined{join(it->second.pr, abs.pr),
                                            join(it->second.sz, abs.sz)};
                        if (joined != it->second) {
                            it->second = joined;
                            changed = true;
                        }
                    }
                }
            }
            if (!changed)
                break;
        }
        for (auto& f : specs) {
            auto it = entry_abs.find(f.entry);
            if (it == entry_abs.end())
                continue;  // only reachable through a cycle: keep the default
            const FpMode d = default_of(f);
            const FpMode e = it->second;
            if (e.pr == kUnknown || e.sz == kUnknown)
                ++result.entries_unknown;
            else if (e != d)
                ++result.entries_inferred;
            f.entry_pr_unknown = e.pr == kUnknown;
            f.entry_sz_unknown = e.sz == kUnknown;
            if (e.pr != kUnknown)
                f.fpscr_entry =
                    (f.fpscr_entry & ~(1u << 19)) | (static_cast<std::uint32_t>(e.pr) << 19);
            if (e.sz != kUnknown)
                f.fpscr_entry =
                    (f.fpscr_entry & ~(1u << 20)) | (static_cast<std::uint32_t>(e.sz) << 20);
        }
    }
    for (const auto& f : specs) {
        FunctionEmitter fe(image, f, options, known, summaries, result);
        fe.set_written(&written);
        src << fe.run() << "\n";
    }
    src << "namespace {\n";
    if (options.overlay) {
        // Up to 32 instruction words from the function's own extent (never its literal pool,
        // which a no_fold copy patches at run time): the runtime dispatches to this translation
        // only while RAM still holds them.
        const std::uint32_t image_end = image.base + static_cast<std::uint32_t>(image.bytes.size());
        for (const auto& f : specs) {
            src << "const std::uint16_t kSig_" << f.name << "[] = {";
            const std::uint32_t stop = std::min({f.end, f.entry + 64u, image_end});
            for (std::uint32_t a = f.entry; a < stop; a += 2) src << hexu(image.read16(a)) << ", ";
            src << "};\n";
        }
    }
    src << "const FunctionEntry kTable[] = {\n";
    for (const auto& f : specs) {
        src << "    {" << hexu(f.entry) << ", &" << f.name;
        if (options.overlay)
            src << ", kSig_" << f.name << ", sizeof kSig_" << f.name << " / sizeof(std::uint16_t)";
        else
            src << ", nullptr, 0";
        src << ", " << hexu(f.end) << ", &" << f.name << "__resume},\n";
    }
    src << "};\nstruct Registrar {\n    Registrar() { register_functions(kTable, sizeof kTable / "
           "sizeof kTable[0]); }\n} registrar;\n"
        << "}  // namespace\n}  // namespace dream::gen\n";
    hdr << "}  // namespace dream::gen\n";
    result.source = src.str();
    result.header = hdr.str();
    return result;
}

}  // namespace dream::translator
