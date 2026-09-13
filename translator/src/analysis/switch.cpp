#include "dream/translator/analysis/switch.h"

namespace dream::translator {

using sh4::Instr;
using sh4::Op;

namespace {

Instr at(const Image& img, std::uint32_t a) {
    return sh4::decode(img.read16(a));
}

// Constant in `reg` established before `pc` inside the block, via mova / mov.l @(disp,pc) / mov
// #imm, possibly copied through `mov Rx,Ry`.
bool constant_in(const Image& img, std::uint32_t lo, std::uint32_t pc, unsigned reg,
                 std::uint32_t& value, int depth = 0) {
    if (depth > 3)
        return false;
    for (std::uint32_t p = pc; p > lo;) {
        p -= 2;
        const Instr i = at(img, p);
        switch (i.op) {
            case Op::MOVA:
                if (reg == 0) {
                    value = sh4::pcrel_target(i, p);
                    return true;
                }
                break;
            case Op::MOV_L_PCREL:
                if (i.n == reg) {
                    const std::uint32_t a = sh4::pcrel_target(i, p);
                    if (!img.contains(a, 4))
                        return false;
                    value = img.read32(a);
                    return true;
                }
                break;
            case Op::MOV_I:
                if (i.n == reg) {
                    value = static_cast<std::uint32_t>(i.imm);
                    return true;
                }
                break;
            case Op::MOV:
                if (i.n == reg)
                    return constant_in(img, lo, p, i.m, value, depth + 1);
                break;
            default:
                break;
        }
        // Any other definition of reg ends the search.
        if (i.n == reg) {
            switch (i.op) {
                case Op::MOV_B_S:
                case Op::MOV_W_S:
                case Op::MOV_L_S:
                case Op::MOV_L_S_DISP:
                case Op::MOV_B_S_R0:
                case Op::MOV_W_S_R0:
                case Op::MOV_L_S_R0:
                case Op::CMP_EQ:
                case Op::CMP_HS:
                case Op::CMP_GE:
                case Op::CMP_HI:
                case Op::CMP_GT:
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
                case Op::MOVCA_L:
                case Op::TAS_B:
                case Op::LDC_SR:
                case Op::LDC_GBR:
                case Op::LDC_VBR:
                case Op::LDS_PR:
                    break;  // reads only
                default:
                    return false;
            }
        }
        if (sh4::has_delay_slot(i.op) || i.op == Op::BT || i.op == Op::BF)
            return false;
    }
    return false;
}

}  // namespace

bool recover_switch(const Image& img, std::uint32_t block_start, std::uint32_t jump_pc,
                    const Instr& jump, std::uint32_t range_lo, std::uint32_t range_hi,
                    SwitchTable& out, unsigned max_entries) {
    if (jump.op != Op::BRAF && jump.op != Op::JMP)
        return false;
    unsigned jreg = jump.m;
    std::uint32_t p = jump_pc;
    bool table_relative = false;
    unsigned base_reg_for_add = 16;
    // Walk back to the definition of the jump register.
    Instr def{};
    std::uint32_t def_pc = 0;
    bool found = false;
    for (int steps = 0; steps < 12 && p > block_start; ++steps) {
        p -= 2;
        const Instr i = at(img, p);
        if (i.n != jreg)
            continue;
        if (i.op == Op::ADD && !table_relative) {
            // jmp form with table-relative entries: `mov.l @(r0,Rb),J ; add Rb,J ; jmp @J`
            table_relative = true;
            base_reg_for_add = i.m;
            continue;
        }
        if (i.op == Op::MOV_B_L_R0 || i.op == Op::MOV_W_L_R0 || i.op == Op::MOV_L_L_R0) {
            def = i;
            def_pc = p;
            found = true;
            break;
        }
        if (i.op == Op::MOV) {  // copied from another register: follow it
            jreg = i.m;
            continue;
        }
        if (i.op == Op::SHLL || i.op == Op::SHLL2 || i.op == Op::SHLL8 || i.op == Op::SHLL16 ||
            i.op == Op::AND_I) {
            break;  // scaled index with no table load: try the code-table idiom below
        }
        return false;  // some other definition: not a table load
    }
    if (!found) {
        // Code-table idiom: `and #mask,r0 ; shll2 r0 ; braf r0` jumps into a run of `bra target;
        // nop` stubs laid out straight after the jump. Entries are code at jump_pc + 4 + k*stride.
        if (jump.op != Op::BRAF)
            return false;
        unsigned stride = 0;
        int mask = -1;
        std::uint32_t q = jump_pc;
        for (int steps = 0; steps < 8 && q > block_start; ++steps) {
            q -= 2;
            const Instr i = at(img, q);
            if (i.op == Op::SHLL2 && i.n == jump.m && stride == 0) {
                stride = 4;
                continue;
            }
            if (i.op == Op::SHLL && i.n == jump.m && stride == 0) {
                stride = 2;
                continue;
            }
            if (i.op == Op::AND_I && jump.m == 0 && stride != 0) {
                mask = i.imm;
                break;
            }
            if (i.op == Op::MOV && i.n == jump.m)
                continue;
            if (i.n == jump.m && i.op != Op::CMP_EQ && i.op != Op::TST)
                break;
        }
        // Scaling in a predecessor block (typically the delay slot of the branch that reaches this
        // jump): fall back to the stub-table shape with a 4-byte stride and no bound, accepting
        // entries while they look like `bra x; nop` or `nop; nop` stubs.
        bool unbounded_stubs = false;
        if (stride == 0) {
            if (jump_pc != block_start)
                return false;
            stride = 4;
            unbounded_stubs = true;
        }
        out.table = jump_pc + 4;
        out.entry_size = static_cast<std::uint8_t>(stride);
        out.relative_to_pc = true;
        out.relative_to_table = false;
        out.targets.clear();
        const unsigned count = mask >= 0 ? static_cast<unsigned>(mask) + 1 : max_entries;
        for (unsigned k = 0; k < count; ++k) {
            const std::uint32_t t = jump_pc + 4 + k * stride;
            if (t < range_lo || t >= range_hi || !img.contains(t, 2))
                break;
            const Instr ti = at(img, t);
            if (mask < 0) {
                const bool stub = ti.op == Op::BRA || (ti.op == Op::NOP && img.contains(t + 2, 2) &&
                                                       at(img, t + 2).op == Op::NOP);
                if (!stub)
                    break;
                if (unbounded_stubs && k >= 32)
                    break;
            }
            if (ti.op == Op::Invalid)
                break;
            out.targets.push_back(t);
        }
        return !out.targets.empty();
    }

    out.entry_size = def.op == Op::MOV_B_L_R0 ? 1 : def.op == Op::MOV_W_L_R0 ? 2 : 4;
    // Address = r0 + Rm: one of them is the table base. Try Rm first (GCC: `mova tbl,r0` gives r0
    // as base with the scaled index in Rm; SHC: `mov.l tbl,Rm` with the index in r0).
    std::uint32_t base = 0;
    bool have_base = false;
    if (def.m != 0 && constant_in(img, block_start, def_pc, def.m, base))
        have_base = true;
    if (!have_base && constant_in(img, block_start, def_pc, 0, base))
        have_base = true;
    if (!have_base)
        return false;
    if (table_relative && base_reg_for_add != def.m && base_reg_for_add != 0) {
        std::uint32_t b2;
        if (constant_in(img, block_start, def_pc, base_reg_for_add, b2))
            base = b2;
    }
    out.table = base;
    out.relative_to_pc = jump.op == Op::BRAF;
    out.relative_to_table = table_relative;
    out.targets.clear();

    for (unsigned k = 0; k < max_entries; ++k) {
        const std::uint32_t ea = base + k * out.entry_size;
        if (!img.contains(ea, out.entry_size))
            break;
        std::int32_t entry;
        if (out.entry_size == 4)
            entry = static_cast<std::int32_t>(img.read32(ea));
        else if (out.entry_size == 2)
            entry = static_cast<std::int16_t>(img.read16(ea));
        else
            entry = static_cast<std::int8_t>(img.bytes[ea - img.base]);
        std::uint32_t target;
        if (out.relative_to_pc)
            target = jump_pc + 4 + static_cast<std::uint32_t>(entry);
        else if (out.relative_to_table)
            target = base + static_cast<std::uint32_t>(entry);
        else
            target = static_cast<std::uint32_t>(entry);
        if (target < range_lo || target >= range_hi || (target & 1))
            break;
        // A table entry that lands on the table itself is a data word, not a target.
        if (target >= base && target < base + (k + 1) * out.entry_size)
            break;
        out.targets.push_back(target);
    }
    return !out.targets.empty();
}

}  // namespace dream::translator
