#include "dream/translator/analysis/discover.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>

#include "dream/translator/analysis/constprop.h"
#include "dream/translator/analysis/switch.h"
#include "dream/translator/sh4/decoder.h"

namespace dream::translator {

using sh4::Instr;
using sh4::Op;

namespace {

std::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}

class Discoverer {
public:
    Discoverer(const Image& img, const DiscoverOptions& opt) : img_(img), opt_(opt) {}

    DiscoverResult run() {
        for (std::uint32_t s : opt_.seeds) enqueue(s, "seed");
        drain();
        if (opt_.follow_pointers) {
            pointer_pass();
            drain();
        }
        if (opt_.aggressive_sweep) {
            sweep_pass();
            drain();
        }
        DiscoverResult r;
        for (const auto& [entry, f] : functions_) r.functions.push_back(f);
        r.code_bytes = code_.size() * 2;
        r.pointer_candidates = pointer_candidates_;
        r.pointer_accepted = pointer_accepted_;
        r.switches = switches_;
        r.switch_sites.assign(switch_sites_.begin(), switch_sites_.end());
        for (const auto& [addr, u] : unresolved_) {
            (void)addr;
            r.unresolved.push_back(u);
        }
        r.notes = notes_;
        return r;
    }

private:
    bool in_image(std::uint32_t a) const { return img_.contains(a, 2) && (a & 1) == 0; }

    // A target given through another RAM alias (P1 0x8C..., P2 0xAC... for an image modelled at
    // 0x0C...) names the same bytes: bring it into the image's alias so it can be a function here.
    std::uint32_t normalise(std::uint32_t a) const {
        const std::uint32_t phys = a & 0x1FFFFFFFu, base_phys = img_.base & 0x1FFFFFFFu;
        if ((a & 0x1C000000u) == 0x0C000000u && phys >= base_phys &&
            phys < base_phys + img_.bytes.size())
            return img_.base + (phys - base_phys);
        return a;
    }
    Instr at(std::uint32_t a) const { return sh4::decode(img_.read16(a)); }

    void enqueue(std::uint32_t entry, const char* origin) {
        if (!in_image(entry) || functions_.count(entry) || pending_.count(entry))
            return;
        pending_.insert(entry);
        work_.push_back({entry, origin});
    }

    void drain() {
        while (!work_.empty()) {
            auto [entry, origin] = work_.back();
            work_.pop_back();
            pending_.erase(entry);
            if (functions_.count(entry))
                continue;
            analyse_function(entry, origin);
        }
    }

    // The constant `reg` holds at `pc` when built inside the block (analysis/constprop.h); alias
    // bits OR'ed in from an unknown register are ignored because call targets are normalised.
    bool constant_in(std::uint32_t lo, std::uint32_t pc, unsigned reg, std::uint32_t& value) const {
        const auto v = constant_at(img_, lo, pc, reg, true);
        if (!v)
            return false;
        value = *v;
        return true;
    }

    // Every constant `reg` can hold at `pc`, following the graph back when the block itself does
    // not build it. False means "unknown on at least one path", which must not be confused with
    // "no targets": an unknown path can carry anything, so nothing may be seeded from it.
    //
    // Bounded three ways, because discovery runs this at thousands of sites: a visited set makes
    // loops terminate, a depth cap stops a long chain costing more than it returns, and a limit on
    // the number of distinct values keeps a jump-table-like merge from exploding.
    bool reaching_constants(std::uint32_t block, std::uint32_t pc, unsigned reg,
                            const std::map<std::uint32_t, std::set<std::uint32_t>>& preds,
                            const std::map<std::uint32_t, std::uint32_t>& block_end,
                            std::uint32_t entry, std::set<std::uint32_t>& out,
                            unsigned depth = 0) const {
        constexpr unsigned kMaxDepth = 16, kMaxValues = 8;
        if (depth > kMaxDepth || out.size() > kMaxValues)
            return false;
        // Built here? Then the predecessors cannot matter: this block's own definition wins.
        if (const auto v = constant_at(img_, block, pc, reg, true)) {
            out.insert(*v);
            return true;
        }
        const auto pit = preds.find(block);
        if (pit == preds.end() || pit->second.empty())
            return false;  // live in at the function entry: the caller decides, not us
        for (std::uint32_t p : pit->second) {
            if (p == block || p < entry)
                return false;  // a self-loop or an edge from outside: give up rather than guess
            const auto eit = block_end.find(p);
            if (eit == block_end.end())
                return false;
            if (!reaching_constants(p, eit->second, reg, preds, block_end, entry, out, depth + 1))
                return false;
        }
        return true;
    }

    // Recursive descent inside one function: collects its reachable blocks, records call
    // targets as new function seeds, and returns the function extent.
    void analyse_function(std::uint32_t entry, const std::string& origin) {
        std::set<std::uint32_t> visited;
        std::vector<std::uint32_t> work{entry};
        std::set<std::uint32_t> block_starts{entry};  // every branch target followed locally
        std::uint32_t hi = entry;
        std::vector<std::uint32_t> calls;
        std::vector<std::uint32_t> tail_jumps;  // BRA out of the function body
        std::set<std::uint32_t> slots;          // delay-slot addresses: never instruction starts
        // The function's control-flow graph, built as the walk discovers it. Constant propagation
        // is block-local, so a callback loaded in one block and called in a later one cannot be
        // seen; these let a second pass ask the predecessors. Edges are recorded even when the
        // target was already visited, because the edge exists either way.
        std::map<std::uint32_t, std::set<std::uint32_t>> preds;
        std::map<std::uint32_t, std::uint32_t> block_end;
        // Indirect transfers the block-local answer could not resolve, retried after the walk
        // when the graph is complete.
        struct Pending {
            std::uint32_t pc, block, fn;
            unsigned reg;
            const char* op;
            bool pc_relative;  // BSRF: the constant is an offset from pc + 4
        };
        std::vector<Pending> pending;
        while (!work.empty()) {
            std::uint32_t pc = work.back();
            work.pop_back();
            const std::uint32_t block_start = pc;
            while (in_image(pc) && !visited.count(pc)) {
                if (pc - entry > opt_.max_function_bytes)
                    break;
                visited.insert(pc);
                code_.insert(pc);
                const Instr ins = at(pc);
                const bool delayed = sh4::has_delay_slot(ins.op);
                if (delayed) {
                    slots.insert(pc + 2);
                    slots_.insert(pc + 2);
                }
                const std::uint32_t next = pc + (delayed ? 4 : 2);
                if (delayed && in_image(pc + 2)) {
                    visited.insert(pc + 2);
                    code_.insert(pc + 2);
                }
                hi = std::max(hi, next);
                bool stop = false;
                switch (ins.op) {
                    case Op::BT:
                    case Op::BF:
                    case Op::BT_S:
                    case Op::BF_S: {
                        const std::uint32_t t = sh4::pcrel_target(ins, pc);
                        if (in_image(t)) {
                            if (foreign_target(entry, origin, t))
                                tail_jumps.push_back(t);
                            else {
                                work.push_back(t);
                                block_starts.insert(t);
                                preds[t].insert(block_start);
                            }
                        }
                        break;
                    }
                    case Op::BRA: {
                        const std::uint32_t t = sh4::pcrel_target(ins, pc);
                        if (in_image(t)) {
                            if (foreign_target(entry, origin, t))
                                tail_jumps.push_back(t);
                            else {
                                work.push_back(t);
                                block_starts.insert(t);
                                preds[t].insert(block_start);
                            }
                        }
                        stop = true;
                        break;
                    }
                    case Op::BSR: {
                        const std::uint32_t t = sh4::pcrel_target(ins, pc);
                        if (in_image(t))
                            calls.push_back(t);
                        break;
                    }
                    case Op::JSR: {
                        std::uint32_t t;
                        if (!constant_in(block_start, pc, ins.m, t))
                            pending.push_back({pc, block_start, entry, ins.m, "JSR", false});
                        else if (!in_image(normalise(t)))
                            note_unresolved(pc, entry, "JSR", kReasonOutOfImage);
                        else
                            calls.push_back(normalise(t));
                        break;
                    }
                    case Op::JMP:
                    case Op::BRAF: {
                        SwitchTable sw;
                        const std::uint32_t img_lo = img_.base;
                        const std::uint32_t img_hi =
                            img_.base + static_cast<std::uint32_t>(img_.bytes.size());
                        if (recover_switch(img_, block_start, pc, ins, img_lo, img_hi, sw)) {
                            if (switch_sites_.insert(pc).second)
                                ++switches_;
                            for (std::uint32_t t : sw.targets) {  // local cases
                                work.push_back(t);
                                block_starts.insert(t);
                                preds[t].insert(block_start);
                            }
                        } else if (ins.op == Op::JMP) {
                            std::uint32_t t;
                            if (!constant_in(block_start, pc, ins.m, t))
                                pending.push_back({pc, block_start, entry, ins.m, "JMP", false});
                            else if (!in_image(normalise(t)))
                                note_unresolved(pc, entry, "JMP", kReasonOutOfImage);
                            else
                                calls.push_back(normalise(t));
                        } else {
                            // BRAF with no table recovered. Unlike JMP above there is no constant
                            // fallback, so the target is lost even when it could be traced --
                            // measured at 351 of 642 sites across the corpus
                            // (corpus-scan-findings).
                            note_unresolved(pc, entry, "BRAF", kReasonNoTable);
                        }
                        stop = true;
                        break;
                    }
                    case Op::BSRF: {
                        // Position-independent call (SHC): target = literal + pc + 4.
                        std::uint32_t t;
                        if (!constant_in(block_start, pc, ins.m, t))
                            pending.push_back({pc, block_start, entry, ins.m, "BSRF", true});
                        else if (!in_image(normalise(pc + 4 + t)))
                            note_unresolved(pc, entry, "BSRF", kReasonOutOfImage);
                        else
                            calls.push_back(normalise(pc + 4 + t));
                        break;
                    }
                    case Op::RTS:
                    case Op::RTE:
                        stop = true;
                        break;
                    case Op::Invalid:
                        stop = true;  // data reached; the block ends here
                        break;
                    default:
                        break;
                }
                if (stop) {
                    block_end[block_start] = next;
                    break;
                }
                // A conditional branch continues into the instruction after it, which the target
                // side has already been recorded as reaching: that fallthrough is an edge too.
                if (ins.op == Op::BT || ins.op == Op::BF || ins.op == Op::BT_S ||
                    ins.op == Op::BF_S)
                    preds[next].insert(block_start);
                pc = next;
            }
            // A block that ran into already-visited code ends where it stopped.
            if (!block_end.count(block_start))
                block_end[block_start] = pc;
        }
        // Second pass: the graph is complete now, so ask the predecessors about everything the
        // block-local answer could not resolve.
        //
        // A register live into a block holds whatever its predecessors left there. Where they all
        // agree on one literal that is the value; where they disagree, every one of them is still
        // a real call target reached on a real path, so discovery takes the union rather than
        // giving up -- SHC picks between two callbacks with a conditional branch and joins at one
        // `jsr`, and dropping that finds neither of them. An unknown predecessor poisons the
        // whole answer, because a path we cannot see may carry anything.
        for (const Pending& q : pending) {
            std::set<std::uint32_t> targets;
            if (reaching_constants(q.block, q.pc, q.reg, preds, block_end, entry, targets) &&
                !targets.empty()) {
                bool any = false;
                for (std::uint32_t v : targets) {
                    const std::uint32_t t = normalise(q.pc_relative ? q.pc + 4 + v : v);
                    if (in_image(t)) {
                        calls.push_back(t);
                        any = true;
                    }
                }
                if (any)
                    continue;
                note_unresolved(q.pc, q.fn, q.op, kReasonOutOfImage);
            } else {
                note_unresolved(q.pc, q.fn, q.op, kReasonNoConstant);
            }
        }
        // Blocks that fell into an already-known function are not ours; trim to the first such
        // entry.
        for (std::uint32_t a : visited) {
            // An entry of lower confidence inside our range does not end us: a branch-derived
            // entry is shared tail code, and a pointer-pass entry is usually a switch-table case
            // label or a word that merely looks like an address (Crazy Taxi's kmiFBgetfreememEx
            // was cut into eleven fragments by one, another function was cut in a delay slot).
            // The two functions overlap and each emits its own copy; only an entry at least as
            // trustworthy as ours (a seed or a call target) is the next function.
            auto it = functions_.find(a);
            if (slots.count(a))
                continue;  // an "entry" in one of our delay slots is a mis-seed, not a function
            if (a > entry && it != functions_.end() && rank(it->second.origin) >= rank(origin))
                hi = std::min(hi, a);
        }
        // Include the trailing delay slot of the last instruction.
        DiscoveredFunction f{entry, std::max(hi, entry + 2), origin};
        functions_[entry] = f;
        // Blocks we reached that the trim left outside our extent are still reachable from us:
        // the emitter will tail-call them, so they must exist as functions.
        for (std::uint32_t b : block_starts)
            if (b >= f.end)
                tail_jumps.push_back(b);
        for (std::uint32_t c : calls) enqueue(c, "call");
        // A direct branch that leaves the function (shared tail code, as in libgcc's division
        // routines where __sdivsi3 branches into the middle of __udivsi3) makes its target a
        // function of its own: the emitter lowers the branch as a tail call to it.
        for (std::uint32_t t : tail_jumps) enqueue(t, "branch");
    }

    // A direct branch target that cannot belong to the function being walked: it lies before the
    // entry, or inside another known function.
    bool foreign_target(std::uint32_t entry, const std::string& origin, std::uint32_t t) const {
        if (t == entry)
            return false;
        if (t < entry)
            return true;
        // Only a function of at least our confidence counts as "another function" here: branch-
        // derived entries are shared tails that this function may legitimately reach too, and
        // pointer-pass entries inside a called function are case labels or look-alikes; treating
        // either as foreign fragments the caller into many tiny functions (Crazy Taxi lost five
        // recovered switch tables one way and a loop the other).
        auto it = functions_.upper_bound(t);
        if (it == functions_.begin())
            return false;
        const auto& f = std::prev(it)->second;
        return f.entry != entry && t < f.end && rank(f.origin) >= rank(origin);
    }

    // Confidence of a function's origin: configured seeds and call targets are certain, pointer
    // and sweep candidates are guesses, branch-derived entries are fragments of something else.
    static int rank(const std::string& origin) {
        if (origin == "branch")
            return 0;
        if (origin == "sweep")
            return 1;
        if (origin == "pointer")
            return 2;
        return 3;  // seed, call, config entries
    }

    // A pointer target is plausible code if its first instruction decodes, it is not inside a
    // known function, and a short forward scan reaches RTS/BRA/JMP without hitting undefined words.
    bool plausible_entry(std::uint32_t a) const {
        if (!in_image(a) || code_.count(a) || slots_.count(a))
            return false;
        std::uint32_t pc = a;
        for (int i = 0; i < 200 && in_image(pc); ++i, pc += 2) {
            const Instr ins = at(pc);
            if (ins.op == Op::Invalid)
                return false;
            if (ins.op == Op::RTS || ins.op == Op::BRA || ins.op == Op::JMP || ins.op == Op::RTE)
                return true;
        }
        return false;
    }

    void pointer_pass() {
        const std::uint32_t lo = img_.base,
                            hi = img_.base + static_cast<std::uint32_t>(img_.bytes.size());
        for (std::uint32_t a = lo; a + 4 <= hi; a += 4) {
            if (code_.count(a) || code_.count(a + 2))
                continue;  // inside code, not a pool word
            const std::uint32_t v = normalise(img_.read32(a));
            if (v < lo || v >= hi || (v & 1))
                continue;
            ++pointer_candidates_;
            if (!plausible_entry(v))
                continue;
            // require a prologue-like or otherwise ordinary first instruction; reject data-ish
            // words
            const Instr first = at(v);
            if (first.op == Op::NOP && at(v + 2).op == Op::NOP)
                continue;
            ++pointer_accepted_;
            enqueue(v, "pointer");
        }
    }

    // Linear sweep of unreached 4-byte-aligned gaps: a run that decodes to plausible code and ends
    // in RTS is taken as a function. Conservative by design.
    void sweep_pass() {
        const std::uint32_t lo = img_.base,
                            hi = img_.base + static_cast<std::uint32_t>(img_.bytes.size());
        for (std::uint32_t a = lo; a + 8 <= hi; a += 2) {
            if (code_.count(a))
                continue;
            const Instr first = at(a);
            // typical function starts
            const bool starts =
                (first.raw == 0x4F22) || (first.raw == 0x2FE6) || (first.raw & 0xFF0F) == 0x2F06;
            if (!starts)
                continue;
            if (!plausible_entry(a))
                continue;
            enqueue(a, "sweep");
            drain();
        }
    }

    const Image& img_;
    const DiscoverOptions& opt_;
    std::map<std::uint32_t, DiscoveredFunction> functions_;
    std::set<std::uint32_t> code_;
    std::set<std::uint32_t> pending_;
    std::vector<std::pair<std::uint32_t, std::string>> work_;
    std::set<std::uint32_t> slots_;  // delay slots of every walked branch
    std::size_t pointer_candidates_ = 0, pointer_accepted_ = 0, switches_ = 0;
    std::set<std::uint32_t> switch_sites_;
    // Keyed by address so a site walked more than once -- functions can share tails -- is recorded
    // once rather than per visit.
    std::map<std::uint32_t, UnresolvedIndirect> unresolved_;

    void note_unresolved(std::uint32_t pc, std::uint32_t fn, const char* op, const char* reason) {
        unresolved_.emplace(pc, UnresolvedIndirect{pc, fn, op, reason});
    }
    std::vector<std::string> notes_;
};

}  // namespace

DiscoverResult discover(const Image& image, const DiscoverOptions& options) {
    return Discoverer(image, options).run();
}

std::string to_json(const DiscoverResult& r, const Image& image) {
    std::ostringstream o;
    o << "{\n  \"image_base\": \"" << hex(image.base)
      << "\",\n  \"image_size\": " << image.bytes.size() << ",\n  \"code_bytes\": " << r.code_bytes
      << ",\n  \"pointer_candidates\": " << r.pointer_candidates
      << ",\n  \"pointer_accepted\": " << r.pointer_accepted << ",\n  \"functions\": [\n";
    for (std::size_t i = 0; i < r.functions.size(); ++i) {
        const auto& f = r.functions[i];
        o << "    {\"entry\": \"" << hex(f.entry) << "\", \"end\": \"" << hex(f.end)
          << "\", \"size\": " << (f.end - f.entry) << ", \"origin\": \"" << f.origin << "\"}"
          << (i + 1 < r.functions.size() ? "," : "") << "\n";
    }
    o << "  ],\n  \"switch_sites\": [";
    for (std::size_t i = 0; i < r.switch_sites.size(); ++i)
        o << (i ? ", " : "") << "\"" << hex(r.switch_sites[i]) << "\"";
    // Every control transfer through a register whose target could not be established. This is the
    // measurable form of a discovery gap: a heuristic is worth writing when it shortens this list,
    // and worth keeping when it shortens it without changing behaviour.
    o << "],\n  \"unresolved_indirect\": [\n";
    for (std::size_t i = 0; i < r.unresolved.size(); ++i) {
        const auto& u = r.unresolved[i];
        o << "    {\"address\": \"" << hex(u.address) << "\", \"op\": \"" << u.op
          << "\", \"in_function\": \"" << hex(u.in_function) << "\", \"reason\": \"" << u.reason
          << "\"}" << (i + 1 < r.unresolved.size() ? "," : "") << "\n";
    }
    o << "  ]\n}\n";
    return o.str();
}

std::vector<FunctionSpec> to_specs(const DiscoverResult& r) {
    std::vector<FunctionSpec> out;
    for (const auto& f : r.functions) out.push_back({f.entry, f.end, default_name(f.entry)});
    return out;
}

bool load_functions_json(const std::string& path, std::vector<FunctionSpec>& out,
                         std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // Minimal parse of the shape to_json writes: {"entry": "0x..", "end": "0x..", ...}
    std::size_t pos = 0;
    while ((pos = text.find("\"entry\"", pos)) != std::string::npos) {
        auto q1 = text.find("0x", pos);
        auto e = std::strtoul(text.c_str() + q1, nullptr, 16);
        auto p2 = text.find("\"end\"", pos);
        auto q2 = text.find("0x", p2);
        auto en = std::strtoul(text.c_str() + q2, nullptr, 16);
        FunctionSpec f;
        f.entry = static_cast<std::uint32_t>(e);
        f.end = static_cast<std::uint32_t>(en);
        f.name = default_name(f.entry);
        auto pn = text.find("\"name\"", pos);
        auto next_entry = text.find("\"entry\"", pos + 7);
        if (pn != std::string::npos && (next_entry == std::string::npos || pn < next_entry)) {
            auto s1 = text.find('"', text.find(':', pn)) + 1;
            auto s2 = text.find('"', s1);
            f.name = text.substr(s1, s2 - s1);
        }
        out.push_back(f);
        pos = pos + 7;
    }
    return !out.empty();
}

}  // namespace dream::translator
