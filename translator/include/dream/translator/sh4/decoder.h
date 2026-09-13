// SH-4 instruction decoder (WP1.1).
//
// Every SH-4 instruction is one 16-bit word. decode() classifies a word into an operation and
// extracts its register/immediate fields; format() renders it in binutils objdump syntax, which is
// what tests/sh4/oracle/sh4_objdump.txt (all 65,536 words disassembled by sh-elf-objdump) is
// compared against. The decoder knows nothing about FPSCR: FMOV and friends are decoded to the
// single-precision spelling and the emitter picks the double/pair semantics from the FPSCR
// analysis (ADR 6). Instructions objdump accepts that are SH-4A only (LDC/STC SGR, DBR) are decoded
// too, flagged sh4a_only, and the translator rejects them.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace dream::sh4 {

enum class Op : std::uint8_t {
    Invalid = 0,
    // data transfer
    MOV_I,
    MOV_W_PCREL,
    MOV_L_PCREL,
    MOV,
    MOV_B_S,
    MOV_W_S,
    MOV_L_S,
    MOV_B_L,
    MOV_W_L,
    MOV_L_L,
    MOV_B_SD,
    MOV_W_SD,
    MOV_L_SD,
    MOV_B_LI,
    MOV_W_LI,
    MOV_L_LI,  // @-Rn store, @Rm+ load
    MOV_L_S_DISP,
    MOV_L_L_DISP,  // @(disp,Rn) 32-bit
    MOV_B_S_DISP0,
    MOV_W_S_DISP0,
    MOV_B_L_DISP0,
    MOV_W_L_DISP0,  // r0 with @(disp,Rn)
    MOV_B_S_R0,
    MOV_W_S_R0,
    MOV_L_S_R0,
    MOV_B_L_R0,
    MOV_W_L_R0,
    MOV_L_L_R0,  // @(r0,Rn)
    MOV_B_S_GBR,
    MOV_W_S_GBR,
    MOV_L_S_GBR,
    MOV_B_L_GBR,
    MOV_W_L_GBR,
    MOV_L_L_GBR,
    MOVA,
    MOVT,
    SWAP_B,
    SWAP_W,
    XTRCT,
    // arithmetic
    ADD,
    ADD_I,
    ADDC,
    ADDV,
    CMP_EQ_I,
    CMP_EQ,
    CMP_HS,
    CMP_GE,
    CMP_HI,
    CMP_GT,
    CMP_PZ,
    CMP_PL,
    CMP_STR,
    DIV1,
    DIV0S,
    DIV0U,
    DMULS_L,
    DMULU_L,
    DT,
    EXTS_B,
    EXTS_W,
    EXTU_B,
    EXTU_W,
    MAC_L,
    MAC_W,
    MUL_L,
    MULS_W,
    MULU_W,
    NEG,
    NEGC,
    SUB,
    SUBC,
    SUBV,
    // logic
    AND,
    AND_I,
    AND_B_GBR,
    NOT,
    OR,
    OR_I,
    OR_B_GBR,
    TAS_B,
    TST,
    TST_I,
    TST_B_GBR,
    XOR,
    XOR_I,
    XOR_B_GBR,
    // shifts
    ROTL,
    ROTR,
    ROTCL,
    ROTCR,
    SHAD,
    SHAL,
    SHAR,
    SHLD,
    SHLL,
    SHLL2,
    SHLL8,
    SHLL16,
    SHLR,
    SHLR2,
    SHLR8,
    SHLR16,
    // branches
    BF,
    BF_S,
    BT,
    BT_S,
    BRA,
    BRAF,
    BSR,
    BSRF,
    JMP,
    JSR,
    RTS,
    // system
    CLRMAC,
    CLRS,
    CLRT,
    LDC_SR,
    LDC_GBR,
    LDC_VBR,
    LDC_SSR,
    LDC_SPC,
    LDC_DBR,
    LDC_SGR,
    LDC_BANK,
    LDC_L_SR,
    LDC_L_GBR,
    LDC_L_VBR,
    LDC_L_SSR,
    LDC_L_SPC,
    LDC_L_DBR,
    LDC_L_SGR,
    LDC_L_BANK,
    LDS_MACH,
    LDS_MACL,
    LDS_PR,
    LDS_L_MACH,
    LDS_L_MACL,
    LDS_L_PR,
    LDTLB,
    MOVCA_L,
    NOP,
    OCBI,
    OCBP,
    OCBWB,
    PREF,
    RTE,
    SETS,
    SETT,
    SLEEP,
    STC_SR,
    STC_GBR,
    STC_VBR,
    STC_SSR,
    STC_SPC,
    STC_SGR,
    STC_DBR,
    STC_BANK,
    STC_L_SR,
    STC_L_GBR,
    STC_L_VBR,
    STC_L_SSR,
    STC_L_SPC,
    STC_L_SGR,
    STC_L_DBR,
    STC_L_BANK,
    STS_MACH,
    STS_MACL,
    STS_PR,
    STS_L_MACH,
    STS_L_MACL,
    STS_L_PR,
    TRAPA,
    // FPU
    FLDI0,
    FLDI1,
    FMOV,
    FMOV_LOAD,
    FMOV_STORE,
    FMOV_LOAD_INC,
    FMOV_STORE_DEC,
    FMOV_LOAD_R0,
    FMOV_STORE_R0,
    FLDS,
    FSTS,
    FABS,
    FNEG,
    FADD,
    FSUB,
    FMUL,
    FDIV,
    FMAC,
    FCMP_EQ,
    FCMP_GT,
    FLOAT,
    FTRC,
    FSQRT,
    FSRRA,
    FCNVDS,
    FCNVSD,
    FIPR,
    FTRV,
    FSCA,
    FSCHG,
    FRCHG,
    LDS_FPUL,
    LDS_FPSCR,
    LDS_L_FPUL,
    LDS_L_FPSCR,
    STS_FPUL,
    STS_FPSCR,
    STS_L_FPUL,
    STS_L_FPSCR,
    Count
};

// Which register fields a form uses; the decoder fills the generic n/m/imm slots accordingly.
struct Instr {
    Op op = Op::Invalid;
    std::uint16_t raw = 0;
    std::uint8_t n = 0;     // Rn / FRn / DRn field (bits 8..11)
    std::uint8_t m = 0;     // Rm / FRm field (bits 4..7)
    std::int32_t imm = 0;   // immediate or displacement, sign-extended where the ISA does, unscaled
    std::uint8_t bank = 0;  // Rn_BANK number for LDC/STC bank forms
    bool sh4a_only = false;

    bool valid() const noexcept { return op != Op::Invalid; }
};

Instr decode(std::uint16_t raw) noexcept;

// Mnemonic without operands, e.g. "mov.l", "bt.s", "fcmp/eq".
std::string_view mnemonic(Op op) noexcept;

// True for instructions followed by a delay slot (BRA, BSR, BRAF, BSRF, JMP, JSR, RTS, RTE, BT/S,
// BF/S).
bool has_delay_slot(Op op) noexcept;

// True for anything that transfers control (including conditional branches and TRAPA).
bool is_control_flow(Op op) noexcept;

// objdump-compatible text. `pc` is the address of the instruction word; PC-relative operands are
// rendered as absolute targets exactly as binutils does.
std::string format(const Instr& ins, std::uint32_t pc);

// Target of a PC-relative branch/load for the given pc, valid when the op is PC-relative.
std::uint32_t pcrel_target(const Instr& ins, std::uint32_t pc) noexcept;

}  // namespace dream::sh4
