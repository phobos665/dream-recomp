#include "dream/runtime/devinterp/interpreter.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "dream/runtime/sh4/abi.h"
#include "dream/runtime/sh4/ops.h"
#include "dream/translator/sh4/decoder.h"

// Every case below is the run-time form of the statement translator/src/emit/emit.cpp emits for the
// same opcode (lower(), lower_fp_mode(), emit_delayed()). Keep them in step: a divergence here is a
// divergence between the interpreter and the recompiled game.

namespace dream::devinterp {
namespace {

using sh4::Ctx;
using sh4::Instr;
using sh4::Op;

inline std::uint32_t u32(std::int32_t v) noexcept {
    return static_cast<std::uint32_t>(v);
}
inline std::int32_t s32(std::uint32_t v) noexcept {
    return static_cast<std::int32_t>(v);
}
inline std::uint32_t sx8(std::uint8_t v) noexcept {
    return u32(static_cast<std::int8_t>(v));
}
inline std::uint32_t sx16(std::uint16_t v) noexcept {
    return u32(static_cast<std::int16_t>(v));
}

// DREAM_DEVINTERP_TRACE=N prints the first N interpreted instructions (pc, word, disassembly, a
// few registers) to stderr: the tool for seeing what run-time-generated code actually did.
std::uint64_t trace_budget() {
    static const std::uint64_t budget = [] {
        const char* v = std::getenv("DREAM_DEVINTERP_TRACE");
        return v ? std::strtoull(v, nullptr, 0) : 0;
    }();
    return budget;
}

inline bool native_ok_for(const Options& opt, std::uint32_t t) noexcept {
    return opt.call_translated || (t >= opt.native_lo && t < opt.native_hi);
}

struct Machine {
    Ctx& c;
    ::dream::Memory& m;
    const Options& opt;
    Stats& st;
    const std::uint32_t return_to;

    Instr fetch(std::uint32_t pc) {
        const Instr ins = sh4::decode(m.read16(pc));
        if (st.instructions < trace_budget())
            std::fprintf(stderr,
                         "  interp %08x  %04x  %-28s r0=%08x r15=%08x pr=%08x sr=%08x spc=%08x "
                         "ssr=%08x\n",
                         pc, ins.raw, sh4::format(ins, pc).c_str(), c.r[0], c.r[15], c.pr,
                         sh4::read_sr(c), c.spc, c.ssr);
        return ins;
    }

    void poll() {
        if (c.cycles >= c.next_event)
            sh4::deliver_irq(c, m);
    }

    // ---- FPU: the mode is read from FPSCR at run time (the emitter resolves it statically) ----
    void exec_fp(std::uint32_t pc, const Instr& ins) {
        using namespace sh4;
        const unsigned n = ins.n, mm = ins.m;
        const bool pr = (c.fpscr & FPSCR_PR) != 0, sz = (c.fpscr & FPSCR_SZ) != 0;
        float& frn = c.fr[n];
        float& frm = c.fr[mm];
        const unsigned dn = n & ~1u, dm = mm & ~1u;
        std::uint32_t& rn = c.r[n];
        std::uint32_t& rm = c.r[mm];
        const std::uint32_t r0 = c.r[0];
        switch (ins.op) {
            case Op::FLDI0:
                frn = 0.0f;
                break;
            case Op::FLDI1:
                frn = 1.0f;
                break;
            case Op::FLDS:
                c.fpul = f2u(frn);
                break;
            case Op::FSTS:
                frn = u2f(c.fpul);
                break;
            case Op::FABS:
                frn = u2f(f2u(frn) & 0x7fffffffu);
                break;
            case Op::FNEG:
                frn = u2f(f2u(frn) ^ 0x80000000u);
                break;
            case Op::FADD:
                if (pr)
                    set_dr(c, dn, get_dr(c, dn) + get_dr(c, dm));
                else
                    frn += frm;
                break;
            case Op::FSUB:
                if (pr)
                    set_dr(c, dn, get_dr(c, dn) - get_dr(c, dm));
                else
                    frn -= frm;
                break;
            case Op::FMUL:
                if (pr)
                    set_dr(c, dn, get_dr(c, dn) * get_dr(c, dm));
                else
                    frn *= frm;
                break;
            case Op::FDIV:
                if (pr)
                    set_dr(c, dn, get_dr(c, dn) / get_dr(c, dm));
                else
                    frn /= frm;
                break;
            case Op::FCMP_EQ:
                c.t = pr ? (get_dr(c, dn) == get_dr(c, dm)) : (frn == frm);
                break;
            case Op::FCMP_GT:
                c.t = pr ? (get_dr(c, dn) > get_dr(c, dm)) : (frn > frm);
                break;
            case Op::FMAC:
                frn = fmac(c.fr[0], frm, frn);
                break;
            case Op::FLOAT:
                if (pr)
                    set_dr(c, dn, static_cast<double>(static_cast<std::int32_t>(c.fpul)));
                else
                    frn = static_cast<float>(static_cast<std::int32_t>(c.fpul));
                break;
            case Op::FTRC:
                c.fpul = pr ? ftrc(get_dr(c, dn)) : ftrc(frn);
                break;
            case Op::FSQRT:
                if (pr)
                    set_dr(c, dn, std::sqrt(get_dr(c, dn)));
                else
                    frn = std::sqrt(frn);
                break;
            case Op::FSRRA:
                frn = 1.0f / std::sqrt(frn);
                break;
            case Op::FCNVSD:
                set_dr(c, dn, static_cast<double>(u2f(c.fpul)));
                break;
            case Op::FCNVDS:
                c.fpul = f2u(static_cast<float>(get_dr(c, dn)));
                break;
            case Op::FSCA:
                fsca(c, dn);
                break;
            case Op::FIPR:
                fipr(c, mm, n);
                break;
            case Op::FTRV:
                ftrv(c, n);
                break;
            case Op::FMOV:
                if (sz)
                    fmov_pair_load(c, n, fmov_pair_store(c, mm));
                else
                    frn = frm;
                break;
            case Op::FMOV_LOAD:
                if (sz)
                    fmov_pair_load(c, n, read_pair(m, rm));
                else
                    frn = u2f(m.read32(rm));
                break;
            case Op::FMOV_LOAD_INC:
                if (sz) {
                    fmov_pair_load(c, n, read_pair(m, rm));
                    rm += 8;
                } else {
                    frn = u2f(m.read32(rm));
                    rm += 4;
                }
                break;
            case Op::FMOV_LOAD_R0:
                if (sz)
                    fmov_pair_load(c, n, read_pair(m, rm + r0));
                else
                    frn = u2f(m.read32(rm + r0));
                break;
            case Op::FMOV_STORE:
                if (sz)
                    write_pair(m, rn, fmov_pair_store(c, mm));
                else
                    m.write32(rn, f2u(frm));
                break;
            case Op::FMOV_STORE_DEC:
                if (sz) {
                    rn -= 8;
                    write_pair(m, rn, fmov_pair_store(c, mm));
                } else {
                    rn -= 4;
                    m.write32(rn, f2u(frm));
                }
                break;
            case Op::FMOV_STORE_R0:
                if (sz)
                    write_pair(m, rn + r0, fmov_pair_store(c, mm));
                else
                    m.write32(rn + r0, f2u(frm));
                break;
            default:
                sh4::unimplemented(c, ins.raw, pc);
        }
    }

    // ---- everything that is not a delayed branch (BT/BF are handled by the caller) ----
    void exec(std::uint32_t pc, const Instr& ins) {
        using namespace sh4;
        const unsigned n = ins.n, mm = ins.m;
        const std::int32_t imm = ins.imm;
        std::uint32_t& rn = c.r[n];
        std::uint32_t& rm = c.r[mm];
        std::uint32_t& r0 = c.r[0];
        const std::uint32_t d = u32(imm), d2 = u32(imm * 2), d4 = u32(imm * 4);
        std::uint32_t& bank = c.r_bank[ins.bank];
        switch (ins.op) {
            // moves and loads/stores
            case Op::MOV_I:
                rn = u32(imm);
                break;
            case Op::MOV:
                rn = rm;
                break;
            case Op::MOV_W_PCREL:
                rn = sx16(m.read16(pcrel_target(ins, pc)));
                break;
            case Op::MOV_L_PCREL:
                rn = m.read32(pcrel_target(ins, pc));
                break;
            case Op::MOVA:
                r0 = pcrel_target(ins, pc);
                break;
            case Op::MOVT:
                rn = c.t;
                break;
            case Op::MOV_B_S:
                m.write8(rn, static_cast<std::uint8_t>(rm));
                break;
            case Op::MOV_W_S:
                m.write16(rn, static_cast<std::uint16_t>(rm));
                break;
            case Op::MOV_L_S:
                m.write32(rn, rm);
                break;
            case Op::MOV_B_L:
                rn = sx8(m.read8(rm));
                break;
            case Op::MOV_W_L:
                rn = sx16(m.read16(rm));
                break;
            case Op::MOV_L_L:
                rn = m.read32(rm);
                break;
            case Op::MOV_B_SD:
                rn -= 1;
                m.write8(rn, static_cast<std::uint8_t>(rm));
                break;
            case Op::MOV_W_SD:
                rn -= 2;
                m.write16(rn, static_cast<std::uint16_t>(rm));
                break;
            case Op::MOV_L_SD:
                rn -= 4;
                m.write32(rn, rm);
                break;
            case Op::MOV_B_LI: {
                const std::uint32_t v = sx8(m.read8(rm));
                if (n != mm)
                    rm += 1;
                rn = v;
                break;
            }
            case Op::MOV_W_LI: {
                const std::uint32_t v = sx16(m.read16(rm));
                if (n != mm)
                    rm += 2;
                rn = v;
                break;
            }
            case Op::MOV_L_LI: {
                const std::uint32_t v = m.read32(rm);
                if (n != mm)
                    rm += 4;
                rn = v;
                break;
            }
            case Op::MOV_L_S_DISP:
                m.write32(rn + d4, rm);
                break;
            case Op::MOV_L_L_DISP:
                rn = m.read32(rm + d4);
                break;
            case Op::MOV_B_S_DISP0:
                m.write8(rm + d, static_cast<std::uint8_t>(r0));
                break;
            case Op::MOV_W_S_DISP0:
                m.write16(rm + d2, static_cast<std::uint16_t>(r0));
                break;
            case Op::MOV_B_L_DISP0:
                r0 = sx8(m.read8(rm + d));
                break;
            case Op::MOV_W_L_DISP0:
                r0 = sx16(m.read16(rm + d2));
                break;
            case Op::MOV_B_S_R0:
                m.write8(rn + r0, static_cast<std::uint8_t>(rm));
                break;
            case Op::MOV_W_S_R0:
                m.write16(rn + r0, static_cast<std::uint16_t>(rm));
                break;
            case Op::MOV_L_S_R0:
                m.write32(rn + r0, rm);
                break;
            case Op::MOV_B_L_R0:
                rn = sx8(m.read8(rm + r0));
                break;
            case Op::MOV_W_L_R0:
                rn = sx16(m.read16(rm + r0));
                break;
            case Op::MOV_L_L_R0:
                rn = m.read32(rm + r0);
                break;
            case Op::MOV_B_S_GBR:
                m.write8(c.gbr + d, static_cast<std::uint8_t>(r0));
                break;
            case Op::MOV_W_S_GBR:
                m.write16(c.gbr + d2, static_cast<std::uint16_t>(r0));
                break;
            case Op::MOV_L_S_GBR:
                m.write32(c.gbr + d4, r0);
                break;
            case Op::MOV_B_L_GBR:
                r0 = sx8(m.read8(c.gbr + d));
                break;
            case Op::MOV_W_L_GBR:
                r0 = sx16(m.read16(c.gbr + d2));
                break;
            case Op::MOV_L_L_GBR:
                r0 = m.read32(c.gbr + d4);
                break;
            case Op::SWAP_B:
                rn = (rm & 0xffff0000u) | ((rm & 0xffu) << 8) | ((rm >> 8) & 0xffu);
                break;
            case Op::SWAP_W:
                rn = (rm << 16) | (rm >> 16);
                break;
            case Op::XTRCT:
                rn = (rm << 16) | (rn >> 16);
                break;

            // arithmetic
            case Op::ADD:
                rn += rm;
                break;
            case Op::ADD_I:
                rn += u32(imm);
                break;
            case Op::ADDC:
                rn = addc(c, rn, rm);
                break;
            case Op::ADDV:
                rn = addv(c, rn, rm);
                break;
            case Op::CMP_EQ_I:
                c.t = r0 == u32(imm);
                break;
            case Op::CMP_EQ:
                c.t = rn == rm;
                break;
            case Op::CMP_HS:
                c.t = rn >= rm;
                break;
            case Op::CMP_GE:
                c.t = s32(rn) >= s32(rm);
                break;
            case Op::CMP_HI:
                c.t = rn > rm;
                break;
            case Op::CMP_GT:
                c.t = s32(rn) > s32(rm);
                break;
            case Op::CMP_PZ:
                c.t = s32(rn) >= 0;
                break;
            case Op::CMP_PL:
                c.t = s32(rn) > 0;
                break;
            case Op::CMP_STR:
                c.t = cmp_str(rn, rm);
                break;
            case Op::DIV1:
                rn = div1(c, rn, rm);
                break;
            case Op::DIV0S:
                div0s(c, rn, rm);
                break;
            case Op::DIV0U:
                div0u(c);
                break;
            case Op::DMULS_L:
                dmuls(c, rn, rm);
                break;
            case Op::DMULU_L:
                dmulu(c, rn, rm);
                break;
            case Op::DT:
                rn -= 1;
                c.t = rn == 0;
                break;
            case Op::EXTS_B:
                rn = sx8(static_cast<std::uint8_t>(rm));
                break;
            case Op::EXTS_W:
                rn = sx16(static_cast<std::uint16_t>(rm));
                break;
            case Op::EXTU_B:
                rn = rm & 0xffu;
                break;
            case Op::EXTU_W:
                rn = rm & 0xffffu;
                break;
            case Op::MAC_L: {
                const std::int32_t a = s32(m.read32(rn));
                rn += 4;
                const std::int32_t b = s32(m.read32(rm));
                rm += 4;
                mac_l(c, a, b);
                break;
            }
            case Op::MAC_W: {
                const std::int16_t a = static_cast<std::int16_t>(m.read16(rn));
                rn += 2;
                const std::int16_t b = static_cast<std::int16_t>(m.read16(rm));
                rm += 2;
                mac_w(c, a, b);
                break;
            }
            case Op::MUL_L:
                c.macl = rn * rm;
                break;
            case Op::MULS_W:
                c.macl = u32(s32(sx16(static_cast<std::uint16_t>(rn))) *
                             s32(sx16(static_cast<std::uint16_t>(rm))));
                break;
            case Op::MULU_W:
                c.macl = (rn & 0xffffu) * (rm & 0xffffu);
                break;
            case Op::NEG:
                rn = 0u - rm;
                break;
            case Op::NEGC:
                rn = negc(c, rm);
                break;
            case Op::SUB:
                rn -= rm;
                break;
            case Op::SUBC:
                rn = subc(c, rn, rm);
                break;
            case Op::SUBV:
                rn = subv(c, rn, rm);
                break;

            // logic
            case Op::AND:
                rn &= rm;
                break;
            case Op::AND_I:
                r0 &= d;
                break;
            case Op::AND_B_GBR: {
                const std::uint32_t a = c.gbr + r0;
                m.write8(a, static_cast<std::uint8_t>(m.read8(a) & d));
                break;
            }
            case Op::NOT:
                rn = ~rm;
                break;
            case Op::OR:
                rn |= rm;
                break;
            case Op::OR_I:
                r0 |= d;
                break;
            case Op::OR_B_GBR: {
                const std::uint32_t a = c.gbr + r0;
                m.write8(a, static_cast<std::uint8_t>(m.read8(a) | d));
                break;
            }
            case Op::TAS_B: {
                const std::uint8_t v = m.read8(rn);
                c.t = v == 0;
                m.write8(rn, static_cast<std::uint8_t>(v | 0x80u));
                break;
            }
            case Op::TST:
                c.t = (rn & rm) == 0;
                break;
            case Op::TST_I:
                c.t = (r0 & d) == 0;
                break;
            case Op::TST_B_GBR:
                c.t = (m.read8(c.gbr + r0) & d) == 0;
                break;
            case Op::XOR:
                rn ^= rm;
                break;
            case Op::XOR_I:
                r0 ^= d;
                break;
            case Op::XOR_B_GBR: {
                const std::uint32_t a = c.gbr + r0;
                m.write8(a, static_cast<std::uint8_t>(m.read8(a) ^ d));
                break;
            }

            // shifts and rotates
            case Op::ROTL:
                c.t = rn >> 31;
                rn = (rn << 1) | c.t;
                break;
            case Op::ROTR:
                c.t = rn & 1u;
                rn = (rn >> 1) | (c.t << 31);
                break;
            case Op::ROTCL:
                rn = rotcl(c, rn);
                break;
            case Op::ROTCR:
                rn = rotcr(c, rn);
                break;
            case Op::SHAD:
                rn = shad(rn, rm);
                break;
            case Op::SHLD:
                rn = shld(rn, rm);
                break;
            case Op::SHAL:
            case Op::SHLL:
                c.t = rn >> 31;
                rn <<= 1;
                break;
            case Op::SHAR:
                c.t = rn & 1u;
                rn = u32(s32(rn) >> 1);
                break;
            case Op::SHLR:
                c.t = rn & 1u;
                rn >>= 1;
                break;
            case Op::SHLL2:
                rn <<= 2;
                break;
            case Op::SHLL8:
                rn <<= 8;
                break;
            case Op::SHLL16:
                rn <<= 16;
                break;
            case Op::SHLR2:
                rn >>= 2;
                break;
            case Op::SHLR8:
                rn >>= 8;
                break;
            case Op::SHLR16:
                rn >>= 16;
                break;

            // system
            case Op::CLRMAC:
                c.mach = 0;
                c.macl = 0;
                break;
            case Op::CLRS:
                c.sr &= ~SR_S;
                break;
            case Op::CLRT:
                c.t = 0;
                break;
            case Op::SETS:
                c.sr |= SR_S;
                break;
            case Op::SETT:
                c.t = 1;
                break;
            case Op::NOP:
                break;
            case Op::LDC_SR:
                write_sr(c, rm);
                break;
            case Op::LDC_GBR:
                c.gbr = rm;
                break;
            case Op::LDC_VBR:
                c.vbr = rm;
                break;
            case Op::LDC_SSR:
                c.ssr = rm;
                break;
            case Op::LDC_SPC:
                c.spc = rm;
                break;
            case Op::LDC_DBR:
                c.dbr = rm;
                break;
            case Op::LDC_BANK:
                bank = rm;
                break;
            case Op::LDC_L_SR:
                write_sr(c, m.read32(rm));
                rm += 4;
                break;
            case Op::LDC_L_GBR:
                c.gbr = m.read32(rm);
                rm += 4;
                break;
            case Op::LDC_L_VBR:
                c.vbr = m.read32(rm);
                rm += 4;
                break;
            case Op::LDC_L_SSR:
                c.ssr = m.read32(rm);
                rm += 4;
                break;
            case Op::LDC_L_SPC:
                c.spc = m.read32(rm);
                rm += 4;
                break;
            case Op::LDC_L_DBR:
                c.dbr = m.read32(rm);
                rm += 4;
                break;
            case Op::LDC_L_BANK:
                bank = m.read32(rm);
                rm += 4;
                break;
            case Op::LDS_MACH:
                c.mach = rm;
                break;
            case Op::LDS_MACL:
                c.macl = rm;
                break;
            case Op::LDS_PR:
                c.pr = rm;
                break;
            case Op::LDS_L_MACH:
                c.mach = m.read32(rm);
                rm += 4;
                break;
            case Op::LDS_L_MACL:
                c.macl = m.read32(rm);
                rm += 4;
                break;
            case Op::LDS_L_PR:
                c.pr = m.read32(rm);
                rm += 4;
                break;
            case Op::STC_SR:
                rn = read_sr(c);
                break;
            case Op::STC_GBR:
                rn = c.gbr;
                break;
            case Op::STC_VBR:
                rn = c.vbr;
                break;
            case Op::STC_SSR:
                rn = c.ssr;
                break;
            case Op::STC_SPC:
                rn = c.spc;
                break;
            case Op::STC_SGR:
                rn = c.sgr;
                break;
            case Op::STC_DBR:
                rn = c.dbr;
                break;
            case Op::STC_BANK:
                rn = bank;
                break;
            case Op::STC_L_SR:
                rn -= 4;
                m.write32(rn, read_sr(c));
                break;
            case Op::STC_L_GBR:
                rn -= 4;
                m.write32(rn, c.gbr);
                break;
            case Op::STC_L_VBR:
                rn -= 4;
                m.write32(rn, c.vbr);
                break;
            case Op::STC_L_SSR:
                rn -= 4;
                m.write32(rn, c.ssr);
                break;
            case Op::STC_L_SPC:
                rn -= 4;
                m.write32(rn, c.spc);
                break;
            case Op::STC_L_SGR:
                rn -= 4;
                m.write32(rn, c.sgr);
                break;
            case Op::STC_L_DBR:
                rn -= 4;
                m.write32(rn, c.dbr);
                break;
            case Op::STC_L_BANK:
                rn -= 4;
                m.write32(rn, bank);
                break;
            case Op::STS_MACH:
                rn = c.mach;
                break;
            case Op::STS_MACL:
                rn = c.macl;
                break;
            case Op::STS_PR:
                rn = c.pr;
                break;
            case Op::STS_L_MACH:
                rn -= 4;
                m.write32(rn, c.mach);
                break;
            case Op::STS_L_MACL:
                rn -= 4;
                m.write32(rn, c.macl);
                break;
            case Op::STS_L_PR:
                rn -= 4;
                m.write32(rn, c.pr);
                break;
            case Op::TRAPA:
                c.pc = pc;
                sh4::trapa(c, m, d);
                break;
            case Op::MOVCA_L:
                m.write32(rn, r0);
                break;
            case Op::PREF:
                if ((rn & 0xfc000000u) == 0xe0000000u)
                    m.sq_flush(rn);
                break;
            case Op::OCBI:
            case Op::OCBP:
            case Op::OCBWB:
                break;  // cache operations: no-op on the host
            case Op::SLEEP:
                // Wait for the next event: advance the clock to it and poll. Not something the
                // emitter lowers; here it keeps a sleeping guest making progress.
                if (c.next_event == ~std::uint64_t{0})
                    sh4::unimplemented(c, ins.raw, pc);
                if (c.next_event > c.cycles)
                    c.cycles = c.next_event;
                sh4::deliver_irq(c, m);
                break;

            // FPU control
            case Op::FRCHG:
                frchg(c);
                break;
            case Op::FSCHG:
                c.fpscr ^= FPSCR_SZ;
                break;
            case Op::LDS_FPUL:
                c.fpul = rm;
                break;
            case Op::STS_FPUL:
                rn = c.fpul;
                break;
            case Op::LDS_L_FPUL:
                c.fpul = m.read32(rm);
                rm += 4;
                break;
            case Op::STS_L_FPUL:
                rn -= 4;
                m.write32(rn, c.fpul);
                break;
            case Op::STS_FPSCR:
                rn = c.fpscr;
                break;
            case Op::STS_L_FPSCR:
                rn -= 4;
                m.write32(rn, c.fpscr);
                break;
            case Op::LDS_FPSCR:
                write_fpscr(c, rm);
                break;
            case Op::LDS_L_FPSCR:
                write_fpscr(c, m.read32(rm));
                rm += 4;
                break;

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
                exec_fp(pc, ins);
                break;

            default:
                // Delayed branches never reach here; BT/BF are handled by run(); LDTLB, the SH-4A
                // forms and undefined words are faults, as in the emitter.
                sh4::unimplemented(c, ins.raw, pc);
        }
    }
};

}  // namespace

Stop run(Ctx& c, ::dream::Memory& m, std::uint32_t return_to, const Options& opt, Stats* stats) {
    Stats local;
    Stats& st = stats ? *stats : local;
    ++st.runs;
    Machine mc{c, m, opt, st, return_to};
    mc.poll();  // a translated function polls at entry; so does an interpreted one
    // Calls translated code; true when it ended in a non-local return that redirected c.pc.
    auto native = [&](sh4::GuestFn fn) -> bool {
        try {
            fn(c, m);
            return false;
        } catch (const sh4::NonLocalReturn& n) {
            if (!opt.catch_nonlocal)
                throw;
            c.pc = n.pc;
            return true;
        }
    };
    for (;;) {
        if (opt.max_instructions && st.instructions >= opt.max_instructions)
            return Stop::StepLimit;
        const std::uint32_t pc = c.pc;
        const Instr ins = mc.fetch(pc);
        ++st.instructions;
        c.cycles += 1;

        if (!sh4::has_delay_slot(ins.op)) {
            if (ins.op == Op::BT || ins.op == Op::BF) {
                const std::uint32_t t = sh4::pcrel_target(ins, pc);
                const bool taken = ins.op == Op::BT ? c.t != 0 : c.t == 0;
                if (!taken) {
                    c.pc = pc + 2;
                    continue;
                }
                c.cycles += 1;
                if (t == return_to)
                    return Stop::Returned;
                c.pc = t;
                if (t <= pc)
                    mc.poll();
                continue;
            }
            mc.exec(pc, ins);
            c.pc = pc + 2;
            continue;
        }

        // Delayed branch: the branch's operands are read before the slot runs (target, condition,
        // PR for calls), the slot executes at pc+2, then control transfers. Mirrors emit_delayed().
        const Instr slot = mc.fetch(pc + 2);
        ++st.instructions;
        c.cycles += 1;
        enum class Flow { Jump, Call, Ret, Rte } flow = Flow::Jump;
        std::uint32_t t = 0;
        bool taken = true;
        switch (ins.op) {
            case Op::BT_S:
                taken = c.t != 0;
                t = sh4::pcrel_target(ins, pc);
                break;
            case Op::BF_S:
                taken = c.t == 0;
                t = sh4::pcrel_target(ins, pc);
                break;
            case Op::BRA:
                t = sh4::pcrel_target(ins, pc);
                break;
            case Op::BRAF:
                t = pc + 4 + c.r[ins.m];
                break;
            case Op::JMP:
                t = c.r[ins.m];
                break;
            case Op::BSR:
                t = sh4::pcrel_target(ins, pc);
                c.pr = pc + 4;
                flow = Flow::Call;
                break;
            case Op::BSRF:
                t = pc + 4 + c.r[ins.m];
                c.pr = pc + 4;
                flow = Flow::Call;
                break;
            case Op::JSR:
                t = c.r[ins.m];
                c.pr = pc + 4;
                flow = Flow::Call;
                break;
            case Op::RTS:
                t = c.pr;
                flow = Flow::Ret;
                break;
            case Op::RTE:
                flow = Flow::Rte;
                break;
            default:
                sh4::unimplemented(c, ins.raw, pc);
        }
        if (sh4::has_delay_slot(slot.op) || slot.op == Op::BT || slot.op == Op::BF)
            sh4::unimplemented(c, slot.raw, pc + 2);  // illegal slot instruction, as the emitter
        mc.exec(pc + 2, slot);

        if (flow == Flow::Rte) {
            // rte() restores SR from SSR and sets PC = SPC through the runtime hook. Whether that
            // ends this run depends on why the code is executing: inside a delivered exception the
            // handler is done and the delivering frame resumes; outside one (Katana's startup stub
            // enters the program with an RTE) it is a jump, and interpretation continues at SPC.
            c.pc = pc;
            if (sh4::rte(c, m))
                return Stop::Rte;
            t = c.pc;
            flow = Flow::Jump;
        }
        switch (flow) {
            case Flow::Rte:
                return Stop::Rte;  // unreachable: handled above
            case Flow::Jump: {
                if (!taken) {
                    c.pc = pc + 4;
                    break;
                }
                // An RTE used as a jump is not a return through PR: it goes to SPC even when SPC
                // happens to equal the caller's return address (both zero in an uninitialised
                // context, where "returning" would hide the real fault of executing address 0).
                if (t == return_to && ins.op != Op::RTE)
                    return Stop::Returned;
                const bool computed = ins.op == Op::JMP || ins.op == Op::BRAF || ins.op == Op::RTE;
                if (computed && native_ok_for(opt, t)) {
                    if (sh4::GuestFn fn = sh4::find_function(t, &m)) {
                        // Tail call into translated code: it returns to its PR, which is where the
                        // interpreted function would have returned to.
                        ++st.native_calls;
                        c.pc = t;
                        if (native(fn)) {
                            mc.poll();
                            break;
                        }
                        if (c.pr == return_to)
                            return Stop::Returned;
                        c.pc = c.pr;
                        mc.poll();
                        break;
                    }
                }
                c.pc = t;
                if (t <= pc)
                    mc.poll();
                break;
            }
            case Flow::Call:
                if (native_ok_for(opt, t)) {
                    if (sh4::GuestFn fn = sh4::find_function(t, &m)) {
                        ++st.native_calls;
                        c.pc = pc;
                        if (!native(fn))
                            c.pc = pc + 4;
                        mc.poll();
                        break;
                    }
                }
                c.pc = t;  // interpret the callee; its RTS comes back through PR
                break;
            case Flow::Ret:
                sh4::trace_return(c, m, pc);
                if (t == return_to)
                    return Stop::Returned;
                c.pc = t;
                mc.poll();
                break;
        }
    }
}

}  // namespace dream::devinterp
