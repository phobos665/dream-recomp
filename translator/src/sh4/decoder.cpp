#include "dream/translator/sh4/decoder.h"

#include <array>
#include <cstdio>

namespace dream::sh4 {
namespace {

// Field extraction recipe for a table entry.
enum class Fields : std::uint8_t {
    None,
    N,       // Rn in bits 8..11
    M,       // Rm in bits 8..11 (single-register forms that name their register "m")
    NM,      // Rn bits 8..11, Rm bits 4..7
    NMD4,    // + 4-bit unsigned displacement
    MD4_R0,  // 1000 00xx mmmm dddd: register in bits 4..7, disp bits 0..3
    NI8,     // Rn + signed 8-bit immediate
    NU8,     // Rn + unsigned 8-bit displacement (PC-relative loads)
    I8,      // signed 8-bit immediate (R0 implicit)
    U8,      // unsigned 8-bit immediate / displacement
    S8,      // signed 8-bit branch displacement
    S12,     // signed 12-bit branch displacement
    BANK_N,  // 0000nnnn1bbb0010 / 0100nnnn1bbb0011: Rn bits 8..11, bank bits 4..6
    BANK_M,  // 0100mmmm1bbb1110 / 0100mmmm1bbb0111: Rm bits 8..11, bank bits 4..6
    FIPR,    // FVn bits 10..11, FVm bits 8..9
    FTRV,    // FVn bits 10..11
};

struct Entry {
    std::uint16_t mask;
    std::uint16_t value;
    Op op;
    Fields fields;
    bool sh4a = false;
};

// Order matters only where masks overlap; more specific entries come first.
constexpr Entry kTable[] = {
    // ---- 0000 ----
    {0xFFFF, 0x0008, Op::CLRT, Fields::None},
    {0xFFFF, 0x0018, Op::SETT, Fields::None},
    {0xFFFF, 0x0028, Op::CLRMAC, Fields::None},
    {0xFFFF, 0x0038, Op::LDTLB, Fields::None},
    {0xFFFF, 0x0048, Op::CLRS, Fields::None},
    {0xFFFF, 0x0058, Op::SETS, Fields::None},
    {0xFFFF, 0x0009, Op::NOP, Fields::None},
    {0xFFFF, 0x0019, Op::DIV0U, Fields::None},
    {0xFFFF, 0x000B, Op::RTS, Fields::None},
    {0xFFFF, 0x001B, Op::SLEEP, Fields::None},
    {0xFFFF, 0x002B, Op::RTE, Fields::None},
    {0xF0FF, 0x0002, Op::STC_SR, Fields::N},
    {0xF0FF, 0x0012, Op::STC_GBR, Fields::N},
    {0xF0FF, 0x0022, Op::STC_VBR, Fields::N},
    {0xF0FF, 0x0032, Op::STC_SSR, Fields::N},
    {0xF0FF, 0x0042, Op::STC_SPC, Fields::N},
    {0xF0FF, 0x003A, Op::STC_SGR, Fields::N},
    {0xF0FF, 0x00FA, Op::STC_DBR, Fields::N},
    {0xF08F, 0x0082, Op::STC_BANK, Fields::BANK_N},
    {0xF0FF, 0x0003, Op::BSRF, Fields::M},
    {0xF0FF, 0x0023, Op::BRAF, Fields::M},
    {0xF0FF, 0x0083, Op::PREF, Fields::N},
    {0xF0FF, 0x0093, Op::OCBI, Fields::N},
    {0xF0FF, 0x00A3, Op::OCBP, Fields::N},
    {0xF0FF, 0x00B3, Op::OCBWB, Fields::N},
    {0xF0FF, 0x00C3, Op::MOVCA_L, Fields::N},
    {0xF00F, 0x0004, Op::MOV_B_S_R0, Fields::NM},
    {0xF00F, 0x0005, Op::MOV_W_S_R0, Fields::NM},
    {0xF00F, 0x0006, Op::MOV_L_S_R0, Fields::NM},
    {0xF00F, 0x0007, Op::MUL_L, Fields::NM},
    {0xF0FF, 0x0029, Op::MOVT, Fields::N},
    {0xF0FF, 0x000A, Op::STS_MACH, Fields::N},
    {0xF0FF, 0x001A, Op::STS_MACL, Fields::N},
    {0xF0FF, 0x002A, Op::STS_PR, Fields::N},
    {0xF0FF, 0x005A, Op::STS_FPUL, Fields::N},
    {0xF0FF, 0x006A, Op::STS_FPSCR, Fields::N},
    {0xF00F, 0x000C, Op::MOV_B_L_R0, Fields::NM},
    {0xF00F, 0x000D, Op::MOV_W_L_R0, Fields::NM},
    {0xF00F, 0x000E, Op::MOV_L_L_R0, Fields::NM},
    {0xF00F, 0x000F, Op::MAC_L, Fields::NM},
    // ---- 0001 / 0101 ----
    {0xF000, 0x1000, Op::MOV_L_S_DISP, Fields::NMD4},
    {0xF000, 0x5000, Op::MOV_L_L_DISP, Fields::NMD4},
    // ---- 0010 ----
    {0xF00F, 0x2000, Op::MOV_B_S, Fields::NM},
    {0xF00F, 0x2001, Op::MOV_W_S, Fields::NM},
    {0xF00F, 0x2002, Op::MOV_L_S, Fields::NM},
    {0xF00F, 0x2004, Op::MOV_B_SD, Fields::NM},
    {0xF00F, 0x2005, Op::MOV_W_SD, Fields::NM},
    {0xF00F, 0x2006, Op::MOV_L_SD, Fields::NM},
    {0xF00F, 0x2007, Op::DIV0S, Fields::NM},
    {0xF00F, 0x2008, Op::TST, Fields::NM},
    {0xF00F, 0x2009, Op::AND, Fields::NM},
    {0xF00F, 0x200A, Op::XOR, Fields::NM},
    {0xF00F, 0x200B, Op::OR, Fields::NM},
    {0xF00F, 0x200C, Op::CMP_STR, Fields::NM},
    {0xF00F, 0x200D, Op::XTRCT, Fields::NM},
    {0xF00F, 0x200E, Op::MULU_W, Fields::NM},
    {0xF00F, 0x200F, Op::MULS_W, Fields::NM},
    // ---- 0011 ----
    {0xF00F, 0x3000, Op::CMP_EQ, Fields::NM},
    {0xF00F, 0x3002, Op::CMP_HS, Fields::NM},
    {0xF00F, 0x3003, Op::CMP_GE, Fields::NM},
    {0xF00F, 0x3004, Op::DIV1, Fields::NM},
    {0xF00F, 0x3005, Op::DMULU_L, Fields::NM},
    {0xF00F, 0x3006, Op::CMP_HI, Fields::NM},
    {0xF00F, 0x3007, Op::CMP_GT, Fields::NM},
    {0xF00F, 0x3008, Op::SUB, Fields::NM},
    {0xF00F, 0x300A, Op::SUBC, Fields::NM},
    {0xF00F, 0x300B, Op::SUBV, Fields::NM},
    {0xF00F, 0x300C, Op::ADD, Fields::NM},
    {0xF00F, 0x300D, Op::DMULS_L, Fields::NM},
    {0xF00F, 0x300E, Op::ADDC, Fields::NM},
    {0xF00F, 0x300F, Op::ADDV, Fields::NM},
    // ---- 0100 ----
    {0xF0FF, 0x4000, Op::SHLL, Fields::N},
    {0xF0FF, 0x4010, Op::DT, Fields::N},
    {0xF0FF, 0x4020, Op::SHAL, Fields::N},
    {0xF0FF, 0x4001, Op::SHLR, Fields::N},
    {0xF0FF, 0x4011, Op::CMP_PZ, Fields::N},
    {0xF0FF, 0x4021, Op::SHAR, Fields::N},
    {0xF0FF, 0x4002, Op::STS_L_MACH, Fields::N},
    {0xF0FF, 0x4012, Op::STS_L_MACL, Fields::N},
    {0xF0FF, 0x4022, Op::STS_L_PR, Fields::N},
    {0xF0FF, 0x4052, Op::STS_L_FPUL, Fields::N},
    {0xF0FF, 0x4062, Op::STS_L_FPSCR, Fields::N},
    {0xF0FF, 0x4003, Op::STC_L_SR, Fields::N},
    {0xF0FF, 0x4013, Op::STC_L_GBR, Fields::N},
    {0xF0FF, 0x4023, Op::STC_L_VBR, Fields::N},
    {0xF0FF, 0x4033, Op::STC_L_SSR, Fields::N},
    {0xF0FF, 0x4043, Op::STC_L_SPC, Fields::N},
    {0xF0FF, 0x4032, Op::STC_L_SGR, Fields::N},
    {0xF0FF, 0x40F2, Op::STC_L_DBR, Fields::N},
    {0xF08F, 0x4083, Op::STC_L_BANK, Fields::BANK_N},
    {0xF0FF, 0x4004, Op::ROTL, Fields::N},
    {0xF0FF, 0x4024, Op::ROTCL, Fields::N},
    {0xF0FF, 0x4005, Op::ROTR, Fields::N},
    {0xF0FF, 0x4015, Op::CMP_PL, Fields::N},
    {0xF0FF, 0x4025, Op::ROTCR, Fields::N},
    {0xF0FF, 0x4006, Op::LDS_L_MACH, Fields::M},
    {0xF0FF, 0x4016, Op::LDS_L_MACL, Fields::M},
    {0xF0FF, 0x4026, Op::LDS_L_PR, Fields::M},
    {0xF0FF, 0x4056, Op::LDS_L_FPUL, Fields::M},
    {0xF0FF, 0x4066, Op::LDS_L_FPSCR, Fields::M},
    {0xF0FF, 0x4007, Op::LDC_L_SR, Fields::M},
    {0xF0FF, 0x4017, Op::LDC_L_GBR, Fields::M},
    {0xF0FF, 0x4027, Op::LDC_L_VBR, Fields::M},
    {0xF0FF, 0x4037, Op::LDC_L_SSR, Fields::M},
    {0xF0FF, 0x4047, Op::LDC_L_SPC, Fields::M},
    {0xF0FF, 0x4036, Op::LDC_L_SGR, Fields::M, true},
    {0xF0FF, 0x40F6, Op::LDC_L_DBR, Fields::M},
    {0xF08F, 0x4087, Op::LDC_L_BANK, Fields::BANK_M},
    {0xF0FF, 0x4008, Op::SHLL2, Fields::N},
    {0xF0FF, 0x4018, Op::SHLL8, Fields::N},
    {0xF0FF, 0x4028, Op::SHLL16, Fields::N},
    {0xF0FF, 0x4009, Op::SHLR2, Fields::N},
    {0xF0FF, 0x4019, Op::SHLR8, Fields::N},
    {0xF0FF, 0x4029, Op::SHLR16, Fields::N},
    {0xF0FF, 0x400A, Op::LDS_MACH, Fields::M},
    {0xF0FF, 0x401A, Op::LDS_MACL, Fields::M},
    {0xF0FF, 0x402A, Op::LDS_PR, Fields::M},
    {0xF0FF, 0x405A, Op::LDS_FPUL, Fields::M},
    {0xF0FF, 0x406A, Op::LDS_FPSCR, Fields::M},
    {0xF0FF, 0x400B, Op::JSR, Fields::M},
    {0xF0FF, 0x401B, Op::TAS_B, Fields::N},
    {0xF0FF, 0x402B, Op::JMP, Fields::M},
    {0xF00F, 0x400C, Op::SHAD, Fields::NM},
    {0xF00F, 0x400D, Op::SHLD, Fields::NM},
    {0xF0FF, 0x400E, Op::LDC_SR, Fields::M},
    {0xF0FF, 0x401E, Op::LDC_GBR, Fields::M},
    {0xF0FF, 0x402E, Op::LDC_VBR, Fields::M},
    {0xF0FF, 0x403E, Op::LDC_SSR, Fields::M},
    {0xF0FF, 0x404E, Op::LDC_SPC, Fields::M},
    {0xF0FF, 0x403A, Op::LDC_SGR, Fields::M, true},
    {0xF0FF, 0x40FA, Op::LDC_DBR, Fields::M},
    {0xF08F, 0x408E, Op::LDC_BANK, Fields::BANK_M},
    {0xF00F, 0x400F, Op::MAC_W, Fields::NM},
    // ---- 0110 ----
    {0xF00F, 0x6000, Op::MOV_B_L, Fields::NM},
    {0xF00F, 0x6001, Op::MOV_W_L, Fields::NM},
    {0xF00F, 0x6002, Op::MOV_L_L, Fields::NM},
    {0xF00F, 0x6003, Op::MOV, Fields::NM},
    {0xF00F, 0x6004, Op::MOV_B_LI, Fields::NM},
    {0xF00F, 0x6005, Op::MOV_W_LI, Fields::NM},
    {0xF00F, 0x6006, Op::MOV_L_LI, Fields::NM},
    {0xF00F, 0x6007, Op::NOT, Fields::NM},
    {0xF00F, 0x6008, Op::SWAP_B, Fields::NM},
    {0xF00F, 0x6009, Op::SWAP_W, Fields::NM},
    {0xF00F, 0x600A, Op::NEGC, Fields::NM},
    {0xF00F, 0x600B, Op::NEG, Fields::NM},
    {0xF00F, 0x600C, Op::EXTU_B, Fields::NM},
    {0xF00F, 0x600D, Op::EXTU_W, Fields::NM},
    {0xF00F, 0x600E, Op::EXTS_B, Fields::NM},
    {0xF00F, 0x600F, Op::EXTS_W, Fields::NM},
    // ---- 0111 ----
    {0xF000, 0x7000, Op::ADD_I, Fields::NI8},
    // ---- 1000 ----
    {0xFF00, 0x8000, Op::MOV_B_S_DISP0, Fields::MD4_R0},
    {0xFF00, 0x8100, Op::MOV_W_S_DISP0, Fields::MD4_R0},
    {0xFF00, 0x8400, Op::MOV_B_L_DISP0, Fields::MD4_R0},
    {0xFF00, 0x8500, Op::MOV_W_L_DISP0, Fields::MD4_R0},
    {0xFF00, 0x8800, Op::CMP_EQ_I, Fields::I8},
    {0xFF00, 0x8900, Op::BT, Fields::S8},
    {0xFF00, 0x8B00, Op::BF, Fields::S8},
    {0xFF00, 0x8D00, Op::BT_S, Fields::S8},
    {0xFF00, 0x8F00, Op::BF_S, Fields::S8},
    // ---- 1001 / 1010 / 1011 ----
    {0xF000, 0x9000, Op::MOV_W_PCREL, Fields::NU8},
    {0xF000, 0xA000, Op::BRA, Fields::S12},
    {0xF000, 0xB000, Op::BSR, Fields::S12},
    // ---- 1100 ----
    {0xFF00, 0xC000, Op::MOV_B_S_GBR, Fields::U8},
    {0xFF00, 0xC100, Op::MOV_W_S_GBR, Fields::U8},
    {0xFF00, 0xC200, Op::MOV_L_S_GBR, Fields::U8},
    {0xFF00, 0xC300, Op::TRAPA, Fields::U8},
    {0xFF00, 0xC400, Op::MOV_B_L_GBR, Fields::U8},
    {0xFF00, 0xC500, Op::MOV_W_L_GBR, Fields::U8},
    {0xFF00, 0xC600, Op::MOV_L_L_GBR, Fields::U8},
    {0xFF00, 0xC700, Op::MOVA, Fields::U8},
    {0xFF00, 0xC800, Op::TST_I, Fields::U8},
    {0xFF00, 0xC900, Op::AND_I, Fields::U8},
    {0xFF00, 0xCA00, Op::XOR_I, Fields::U8},
    {0xFF00, 0xCB00, Op::OR_I, Fields::U8},
    {0xFF00, 0xCC00, Op::TST_B_GBR, Fields::U8},
    {0xFF00, 0xCD00, Op::AND_B_GBR, Fields::U8},
    {0xFF00, 0xCE00, Op::XOR_B_GBR, Fields::U8},
    {0xFF00, 0xCF00, Op::OR_B_GBR, Fields::U8},
    // ---- 1101 / 1110 ----
    {0xF000, 0xD000, Op::MOV_L_PCREL, Fields::NU8},
    {0xF000, 0xE000, Op::MOV_I, Fields::NI8},
    // ---- 1111 FPU ----
    {0xFFFF, 0xF3FD, Op::FSCHG, Fields::None},
    {0xFFFF, 0xFBFD, Op::FRCHG, Fields::None},
    {0xF3FF, 0xF1FD, Op::FTRV, Fields::FTRV},
    {0xF1FF, 0xF0FD, Op::FSCA, Fields::N},
    {0xF0FF, 0xF0ED, Op::FIPR, Fields::FIPR},
    {0xF1FF, 0xF0AD, Op::FCNVSD, Fields::N},
    {0xF1FF, 0xF0BD, Op::FCNVDS, Fields::N},
    {0xF0FF, 0xF00D, Op::FSTS, Fields::N},
    {0xF0FF, 0xF01D, Op::FLDS, Fields::N},
    {0xF0FF, 0xF02D, Op::FLOAT, Fields::N},
    {0xF0FF, 0xF03D, Op::FTRC, Fields::N},
    {0xF0FF, 0xF04D, Op::FNEG, Fields::N},
    {0xF0FF, 0xF05D, Op::FABS, Fields::N},
    {0xF0FF, 0xF06D, Op::FSQRT, Fields::N},
    {0xF0FF, 0xF07D, Op::FSRRA, Fields::N},
    {0xF0FF, 0xF08D, Op::FLDI0, Fields::N},
    {0xF0FF, 0xF09D, Op::FLDI1, Fields::N},
    {0xF00F, 0xF000, Op::FADD, Fields::NM},
    {0xF00F, 0xF001, Op::FSUB, Fields::NM},
    {0xF00F, 0xF002, Op::FMUL, Fields::NM},
    {0xF00F, 0xF003, Op::FDIV, Fields::NM},
    {0xF00F, 0xF004, Op::FCMP_EQ, Fields::NM},
    {0xF00F, 0xF005, Op::FCMP_GT, Fields::NM},
    {0xF00F, 0xF006, Op::FMOV_LOAD_R0, Fields::NM},
    {0xF00F, 0xF007, Op::FMOV_STORE_R0, Fields::NM},
    {0xF00F, 0xF008, Op::FMOV_LOAD, Fields::NM},
    {0xF00F, 0xF009, Op::FMOV_LOAD_INC, Fields::NM},
    {0xF00F, 0xF00A, Op::FMOV_STORE, Fields::NM},
    {0xF00F, 0xF00B, Op::FMOV_STORE_DEC, Fields::NM},
    {0xF00F, 0xF00C, Op::FMOV, Fields::NM},
    {0xF00F, 0xF00E, Op::FMAC, Fields::NM},
};

struct Lookup {
    std::array<std::uint8_t, 65536> index{};  // 0 = invalid, else 1 + table index
    Lookup() {
        for (std::uint32_t raw = 0; raw < 65536; ++raw) {
            for (std::size_t i = 0; i < std::size(kTable); ++i) {
                if ((raw & kTable[i].mask) == kTable[i].value) {
                    index[raw] = static_cast<std::uint8_t>(i + 1);
                    break;
                }
            }
        }
    }
};

const Lookup& lookup() {
    static const Lookup table;
    return table;
}

constexpr std::int32_t sext(std::uint32_t v, unsigned bits) noexcept {
    const std::uint32_t sign = 1u << (bits - 1);
    return static_cast<std::int32_t>((v ^ sign) - sign);
}

}  // namespace

Instr decode(std::uint16_t raw) noexcept {
    static_assert(std::size(kTable) < 255);
    Instr ins;
    ins.raw = raw;
    const std::uint8_t idx = lookup().index[raw];
    if (idx == 0) {
        return ins;
    }
    const Entry& e = kTable[idx - 1];
    ins.op = e.op;
    ins.sh4a_only = e.sh4a;
    const std::uint8_t hi = static_cast<std::uint8_t>((raw >> 8) & 0xF);
    const std::uint8_t mid = static_cast<std::uint8_t>((raw >> 4) & 0xF);
    switch (e.fields) {
        case Fields::None:
            break;
        case Fields::N:
            ins.n = hi;
            break;
        case Fields::M:
            ins.m = hi;
            break;
        case Fields::NM:
            ins.n = hi;
            ins.m = mid;
            break;
        case Fields::NMD4:
            ins.n = hi;
            ins.m = mid;
            ins.imm = raw & 0xF;
            break;
        case Fields::MD4_R0:
            ins.m = mid;
            ins.imm = raw & 0xF;
            break;
        case Fields::NI8:
            ins.n = hi;
            ins.imm = sext(raw & 0xFF, 8);
            break;
        case Fields::NU8:
            ins.n = hi;
            ins.imm = raw & 0xFF;
            break;
        case Fields::I8:
            ins.imm = sext(raw & 0xFF, 8);
            break;
        case Fields::U8:
            ins.imm = raw & 0xFF;
            break;
        case Fields::S8:
            ins.imm = sext(raw & 0xFF, 8);
            break;
        case Fields::S12:
            ins.imm = sext(raw & 0xFFF, 12);
            break;
        case Fields::BANK_N:
            ins.n = hi;
            ins.bank = static_cast<std::uint8_t>(mid & 7);
            break;
        case Fields::BANK_M:
            ins.m = hi;
            ins.bank = static_cast<std::uint8_t>(mid & 7);
            break;
        case Fields::FIPR:
            ins.n = static_cast<std::uint8_t>(((raw >> 10) & 3) * 4);
            ins.m = static_cast<std::uint8_t>(((raw >> 8) & 3) * 4);
            break;
        case Fields::FTRV:
            ins.n = static_cast<std::uint8_t>(((raw >> 10) & 3) * 4);
            break;
    }
    return ins;
}

std::string_view mnemonic(Op op) noexcept {
    switch (op) {
        case Op::Invalid:
            return ".word";
        case Op::MOV_I:
        case Op::MOV:
            return "mov";
        case Op::MOV_W_PCREL:
        case Op::MOV_W_S:
        case Op::MOV_W_L:
        case Op::MOV_W_SD:
        case Op::MOV_W_LI:
        case Op::MOV_W_S_DISP0:
        case Op::MOV_W_L_DISP0:
        case Op::MOV_W_S_R0:
        case Op::MOV_W_L_R0:
        case Op::MOV_W_S_GBR:
        case Op::MOV_W_L_GBR:
            return "mov.w";
        case Op::MOV_L_PCREL:
        case Op::MOV_L_S:
        case Op::MOV_L_L:
        case Op::MOV_L_SD:
        case Op::MOV_L_LI:
        case Op::MOV_L_S_DISP:
        case Op::MOV_L_L_DISP:
        case Op::MOV_L_S_R0:
        case Op::MOV_L_L_R0:
        case Op::MOV_L_S_GBR:
        case Op::MOV_L_L_GBR:
            return "mov.l";
        case Op::MOV_B_S:
        case Op::MOV_B_L:
        case Op::MOV_B_SD:
        case Op::MOV_B_LI:
        case Op::MOV_B_S_DISP0:
        case Op::MOV_B_L_DISP0:
        case Op::MOV_B_S_R0:
        case Op::MOV_B_L_R0:
        case Op::MOV_B_S_GBR:
        case Op::MOV_B_L_GBR:
            return "mov.b";
        case Op::MOVA:
            return "mova";
        case Op::MOVT:
            return "movt";
        case Op::SWAP_B:
            return "swap.b";
        case Op::SWAP_W:
            return "swap.w";
        case Op::XTRCT:
            return "xtrct";
        case Op::ADD:
        case Op::ADD_I:
            return "add";
        case Op::ADDC:
            return "addc";
        case Op::ADDV:
            return "addv";
        case Op::CMP_EQ_I:
        case Op::CMP_EQ:
            return "cmp/eq";
        case Op::CMP_HS:
            return "cmp/hs";
        case Op::CMP_GE:
            return "cmp/ge";
        case Op::CMP_HI:
            return "cmp/hi";
        case Op::CMP_GT:
            return "cmp/gt";
        case Op::CMP_PZ:
            return "cmp/pz";
        case Op::CMP_PL:
            return "cmp/pl";
        case Op::CMP_STR:
            return "cmp/str";
        case Op::DIV1:
            return "div1";
        case Op::DIV0S:
            return "div0s";
        case Op::DIV0U:
            return "div0u";
        case Op::DMULS_L:
            return "dmuls.l";
        case Op::DMULU_L:
            return "dmulu.l";
        case Op::DT:
            return "dt";
        case Op::EXTS_B:
            return "exts.b";
        case Op::EXTS_W:
            return "exts.w";
        case Op::EXTU_B:
            return "extu.b";
        case Op::EXTU_W:
            return "extu.w";
        case Op::MAC_L:
            return "mac.l";
        case Op::MAC_W:
            return "mac.w";
        case Op::MUL_L:
            return "mul.l";
        case Op::MULS_W:
            return "muls.w";
        case Op::MULU_W:
            return "mulu.w";
        case Op::NEG:
            return "neg";
        case Op::NEGC:
            return "negc";
        case Op::SUB:
            return "sub";
        case Op::SUBC:
            return "subc";
        case Op::SUBV:
            return "subv";
        case Op::AND:
        case Op::AND_I:
            return "and";
        case Op::AND_B_GBR:
            return "and.b";
        case Op::NOT:
            return "not";
        case Op::OR:
        case Op::OR_I:
            return "or";
        case Op::OR_B_GBR:
            return "or.b";
        case Op::TAS_B:
            return "tas.b";
        case Op::TST:
        case Op::TST_I:
            return "tst";
        case Op::TST_B_GBR:
            return "tst.b";
        case Op::XOR:
        case Op::XOR_I:
            return "xor";
        case Op::XOR_B_GBR:
            return "xor.b";
        case Op::ROTL:
            return "rotl";
        case Op::ROTR:
            return "rotr";
        case Op::ROTCL:
            return "rotcl";
        case Op::ROTCR:
            return "rotcr";
        case Op::SHAD:
            return "shad";
        case Op::SHAL:
            return "shal";
        case Op::SHAR:
            return "shar";
        case Op::SHLD:
            return "shld";
        case Op::SHLL:
            return "shll";
        case Op::SHLL2:
            return "shll2";
        case Op::SHLL8:
            return "shll8";
        case Op::SHLL16:
            return "shll16";
        case Op::SHLR:
            return "shlr";
        case Op::SHLR2:
            return "shlr2";
        case Op::SHLR8:
            return "shlr8";
        case Op::SHLR16:
            return "shlr16";
        case Op::BF:
            return "bf";
        case Op::BF_S:
            return "bf.s";
        case Op::BT:
            return "bt";
        case Op::BT_S:
            return "bt.s";
        case Op::BRA:
            return "bra";
        case Op::BRAF:
            return "braf";
        case Op::BSR:
            return "bsr";
        case Op::BSRF:
            return "bsrf";
        case Op::JMP:
            return "jmp";
        case Op::JSR:
            return "jsr";
        case Op::RTS:
            return "rts";
        case Op::CLRMAC:
            return "clrmac";
        case Op::CLRS:
            return "clrs";
        case Op::CLRT:
            return "clrt";
        case Op::LDC_SR:
        case Op::LDC_GBR:
        case Op::LDC_VBR:
        case Op::LDC_SSR:
        case Op::LDC_SPC:
        case Op::LDC_DBR:
        case Op::LDC_SGR:
        case Op::LDC_BANK:
            return "ldc";
        case Op::LDC_L_SR:
        case Op::LDC_L_GBR:
        case Op::LDC_L_VBR:
        case Op::LDC_L_SSR:
        case Op::LDC_L_SPC:
        case Op::LDC_L_DBR:
        case Op::LDC_L_SGR:
        case Op::LDC_L_BANK:
            return "ldc.l";
        case Op::LDS_MACH:
        case Op::LDS_MACL:
        case Op::LDS_PR:
        case Op::LDS_FPUL:
        case Op::LDS_FPSCR:
            return "lds";
        case Op::LDS_L_MACH:
        case Op::LDS_L_MACL:
        case Op::LDS_L_PR:
        case Op::LDS_L_FPUL:
        case Op::LDS_L_FPSCR:
            return "lds.l";
        case Op::LDTLB:
            return "ldtlb";
        case Op::MOVCA_L:
            return "movca.l";
        case Op::NOP:
            return "nop";
        case Op::OCBI:
            return "ocbi";
        case Op::OCBP:
            return "ocbp";
        case Op::OCBWB:
            return "ocbwb";
        case Op::PREF:
            return "pref";
        case Op::RTE:
            return "rte";
        case Op::SETS:
            return "sets";
        case Op::SETT:
            return "sett";
        case Op::SLEEP:
            return "sleep";
        case Op::STC_SR:
        case Op::STC_GBR:
        case Op::STC_VBR:
        case Op::STC_SSR:
        case Op::STC_SPC:
        case Op::STC_SGR:
        case Op::STC_DBR:
        case Op::STC_BANK:
            return "stc";
        case Op::STC_L_SR:
        case Op::STC_L_GBR:
        case Op::STC_L_VBR:
        case Op::STC_L_SSR:
        case Op::STC_L_SPC:
        case Op::STC_L_SGR:
        case Op::STC_L_DBR:
        case Op::STC_L_BANK:
            return "stc.l";
        case Op::STS_MACH:
        case Op::STS_MACL:
        case Op::STS_PR:
        case Op::STS_FPUL:
        case Op::STS_FPSCR:
            return "sts";
        case Op::STS_L_MACH:
        case Op::STS_L_MACL:
        case Op::STS_L_PR:
        case Op::STS_L_FPUL:
        case Op::STS_L_FPSCR:
            return "sts.l";
        case Op::TRAPA:
            return "trapa";
        case Op::FLDI0:
            return "fldi0";
        case Op::FLDI1:
            return "fldi1";
        case Op::FMOV:
        case Op::FMOV_LOAD:
        case Op::FMOV_STORE:
        case Op::FMOV_LOAD_INC:
        case Op::FMOV_STORE_DEC:
        case Op::FMOV_LOAD_R0:
        case Op::FMOV_STORE_R0:
            return "fmov";
        case Op::FLDS:
            return "flds";
        case Op::FSTS:
            return "fsts";
        case Op::FABS:
            return "fabs";
        case Op::FNEG:
            return "fneg";
        case Op::FADD:
            return "fadd";
        case Op::FSUB:
            return "fsub";
        case Op::FMUL:
            return "fmul";
        case Op::FDIV:
            return "fdiv";
        case Op::FMAC:
            return "fmac";
        case Op::FCMP_EQ:
            return "fcmp/eq";
        case Op::FCMP_GT:
            return "fcmp/gt";
        case Op::FLOAT:
            return "float";
        case Op::FTRC:
            return "ftrc";
        case Op::FSQRT:
            return "fsqrt";
        case Op::FSRRA:
            return "fsrra";
        case Op::FCNVDS:
            return "fcnvds";
        case Op::FCNVSD:
            return "fcnvsd";
        case Op::FIPR:
            return "fipr";
        case Op::FTRV:
            return "ftrv";
        case Op::FSCA:
            return "fsca";
        case Op::FSCHG:
            return "fschg";
        case Op::FRCHG:
            return "frchg";
        case Op::Count:
            break;
    }
    return "?";
}

bool has_delay_slot(Op op) noexcept {
    switch (op) {
        case Op::BRA:
        case Op::BSR:
        case Op::BRAF:
        case Op::BSRF:
        case Op::JMP:
        case Op::JSR:
        case Op::RTS:
        case Op::RTE:
        case Op::BT_S:
        case Op::BF_S:
            return true;
        default:
            return false;
    }
}

bool is_control_flow(Op op) noexcept {
    switch (op) {
        case Op::BF:
        case Op::BT:
        case Op::TRAPA:
            return true;
        default:
            return has_delay_slot(op);
    }
}

std::uint32_t pcrel_target(const Instr& ins, std::uint32_t pc) noexcept {
    switch (ins.op) {
        case Op::BT:
        case Op::BF:
        case Op::BT_S:
        case Op::BF_S:
        case Op::BRA:
        case Op::BSR:
            return pc + 4 + static_cast<std::uint32_t>(ins.imm) * 2;
        case Op::MOV_W_PCREL:
            return pc + 4 + static_cast<std::uint32_t>(ins.imm) * 2;
        case Op::MOV_L_PCREL:
        case Op::MOVA:
            return (pc & ~3u) + 4 + static_cast<std::uint32_t>(ins.imm) * 4;
        default:
            return 0;
    }
}

namespace {

std::string r(unsigned i) {
    return "r" + std::to_string(i);
}
std::string fr(unsigned i) {
    return "fr" + std::to_string(i);
}
std::string dr(unsigned i) {
    return "dr" + std::to_string(i);
}
std::string fv(unsigned i) {
    return "fv" + std::to_string(i);
}
std::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%x", v);
    return buf;
}
std::string dec(std::int32_t v) {
    return std::to_string(v);
}

const char* ctrl_name(Op op) {
    switch (op) {
        case Op::LDC_SR:
        case Op::LDC_L_SR:
        case Op::STC_SR:
        case Op::STC_L_SR:
            return "sr";
        case Op::LDC_GBR:
        case Op::LDC_L_GBR:
        case Op::STC_GBR:
        case Op::STC_L_GBR:
            return "gbr";
        case Op::LDC_VBR:
        case Op::LDC_L_VBR:
        case Op::STC_VBR:
        case Op::STC_L_VBR:
            return "vbr";
        case Op::LDC_SSR:
        case Op::LDC_L_SSR:
        case Op::STC_SSR:
        case Op::STC_L_SSR:
            return "ssr";
        case Op::LDC_SPC:
        case Op::LDC_L_SPC:
        case Op::STC_SPC:
        case Op::STC_L_SPC:
            return "spc";
        case Op::LDC_SGR:
        case Op::LDC_L_SGR:
        case Op::STC_SGR:
        case Op::STC_L_SGR:
            return "sgr";
        case Op::LDC_DBR:
        case Op::LDC_L_DBR:
        case Op::STC_DBR:
        case Op::STC_L_DBR:
            return "dbr";
        case Op::LDS_MACH:
        case Op::LDS_L_MACH:
        case Op::STS_MACH:
        case Op::STS_L_MACH:
            return "mach";
        case Op::LDS_MACL:
        case Op::LDS_L_MACL:
        case Op::STS_MACL:
        case Op::STS_L_MACL:
            return "macl";
        case Op::LDS_PR:
        case Op::LDS_L_PR:
        case Op::STS_PR:
        case Op::STS_L_PR:
            return "pr";
        case Op::LDS_FPUL:
        case Op::LDS_L_FPUL:
        case Op::STS_FPUL:
        case Op::STS_L_FPUL:
            return "fpul";
        case Op::LDS_FPSCR:
        case Op::LDS_L_FPSCR:
        case Op::STS_FPSCR:
        case Op::STS_L_FPSCR:
            return "fpscr";
        default:
            return "?";
    }
}

}  // namespace

std::string format(const Instr& ins, std::uint32_t pc) {
    const std::string mn{mnemonic(ins.op)};
    const unsigned n = ins.n, m = ins.m;
    const std::int32_t imm = ins.imm;
    auto two = [&](const std::string& a, const std::string& b) { return mn + " " + a + "," + b; };
    auto one = [&](const std::string& a) { return mn + " " + a; };
    switch (ins.op) {
        case Op::Invalid: {
            char buf[16];
            std::snprintf(buf, sizeof buf, ".word 0x%04x", ins.raw);
            return buf;
        }
        // register/register and memory forms
        case Op::MOV:
        case Op::ADD:
        case Op::ADDC:
        case Op::ADDV:
        case Op::CMP_EQ:
        case Op::CMP_HS:
        case Op::CMP_GE:
        case Op::CMP_HI:
        case Op::CMP_GT:
        case Op::CMP_STR:
        case Op::DIV1:
        case Op::DIV0S:
        case Op::DMULS_L:
        case Op::DMULU_L:
        case Op::EXTS_B:
        case Op::EXTS_W:
        case Op::EXTU_B:
        case Op::EXTU_W:
        case Op::MUL_L:
        case Op::MULS_W:
        case Op::MULU_W:
        case Op::NEG:
        case Op::NEGC:
        case Op::SUB:
        case Op::SUBC:
        case Op::SUBV:
        case Op::AND:
        case Op::NOT:
        case Op::OR:
        case Op::TST:
        case Op::XOR:
        case Op::SHAD:
        case Op::SHLD:
        case Op::SWAP_B:
        case Op::SWAP_W:
        case Op::XTRCT:
            return two(r(m), r(n));
        case Op::MOV_B_S:
        case Op::MOV_W_S:
        case Op::MOV_L_S:
            return two(r(m), "@" + r(n));
        case Op::MOV_B_SD:
        case Op::MOV_W_SD:
        case Op::MOV_L_SD:
            return two(r(m), "@-" + r(n));
        case Op::MOV_B_L:
        case Op::MOV_W_L:
        case Op::MOV_L_L:
            return two("@" + r(m), r(n));
        case Op::MOV_B_LI:
        case Op::MOV_W_LI:
        case Op::MOV_L_LI:
            return two("@" + r(m) + "+", r(n));
        case Op::MOV_B_S_R0:
        case Op::MOV_W_S_R0:
        case Op::MOV_L_S_R0:
            return two(r(m), "@(r0," + r(n) + ")");
        case Op::MOV_B_L_R0:
        case Op::MOV_W_L_R0:
        case Op::MOV_L_L_R0:
            return two("@(r0," + r(m) + ")", r(n));
        case Op::MOV_L_S_DISP:
            return two(r(m), "@(" + dec(imm * 4) + "," + r(n) + ")");
        case Op::MOV_L_L_DISP:
            return two("@(" + dec(imm * 4) + "," + r(m) + ")", r(n));
        case Op::MOV_B_S_DISP0:
            return two("r0", "@(" + dec(imm) + "," + r(m) + ")");
        case Op::MOV_W_S_DISP0:
            return two("r0", "@(" + dec(imm * 2) + "," + r(m) + ")");
        case Op::MOV_B_L_DISP0:
            return two("@(" + dec(imm) + "," + r(m) + ")", "r0");
        case Op::MOV_W_L_DISP0:
            return two("@(" + dec(imm * 2) + "," + r(m) + ")", "r0");
        case Op::MOV_B_S_GBR:
            return two("r0", "@(" + dec(imm) + ",gbr)");
        case Op::MOV_W_S_GBR:
            return two("r0", "@(" + dec(imm * 2) + ",gbr)");
        case Op::MOV_L_S_GBR:
            return two("r0", "@(" + dec(imm * 4) + ",gbr)");
        case Op::MOV_B_L_GBR:
            return two("@(" + dec(imm) + ",gbr)", "r0");
        case Op::MOV_W_L_GBR:
            return two("@(" + dec(imm * 2) + ",gbr)", "r0");
        case Op::MOV_L_L_GBR:
            return two("@(" + dec(imm * 4) + ",gbr)", "r0");
        case Op::MOV_I:
        case Op::ADD_I:
            return two("#" + dec(imm), r(n));
        case Op::CMP_EQ_I:
            return two("#" + dec(imm), "r0");
        case Op::TST_I:
        case Op::AND_I:
        case Op::XOR_I:
        case Op::OR_I:
            return two("#" + dec(imm), "r0");
        case Op::TST_B_GBR:
        case Op::AND_B_GBR:
        case Op::XOR_B_GBR:
        case Op::OR_B_GBR:
            return two("#" + dec(imm), "@(r0,gbr)");
        case Op::TRAPA:
            return one("#" + dec(imm));
        case Op::MOV_W_PCREL:
        case Op::MOV_L_PCREL:
            return two(hex(pcrel_target(ins, pc)), r(n));
        case Op::MOVA:
            return two(hex(pcrel_target(ins, pc)), "r0");
        case Op::MOVT:
        case Op::DT:
        case Op::CMP_PZ:
        case Op::CMP_PL:
        case Op::ROTL:
        case Op::ROTR:
        case Op::ROTCL:
        case Op::ROTCR:
        case Op::SHAL:
        case Op::SHAR:
        case Op::SHLL:
        case Op::SHLL2:
        case Op::SHLL8:
        case Op::SHLL16:
        case Op::SHLR:
        case Op::SHLR2:
        case Op::SHLR8:
        case Op::SHLR16:
            return one(r(n));
        case Op::BRAF:
        case Op::BSRF:
            return one(r(m));
        case Op::JMP:
        case Op::JSR:
            return one("@" + r(m));
        case Op::PREF:
        case Op::OCBI:
        case Op::OCBP:
        case Op::OCBWB:
        case Op::TAS_B:
            return one("@" + r(n));
        case Op::MOVCA_L:
            return two("r0", "@" + r(n));
        case Op::MAC_L:
        case Op::MAC_W:
            return two("@" + r(m) + "+", "@" + r(n) + "+");
        case Op::BT:
        case Op::BF:
        case Op::BT_S:
        case Op::BF_S:
        case Op::BRA:
        case Op::BSR:
            return one(hex(pcrel_target(ins, pc)));
        case Op::RTS:
        case Op::RTE:
        case Op::NOP:
        case Op::CLRMAC:
        case Op::CLRS:
        case Op::CLRT:
        case Op::SETS:
        case Op::SETT:
        case Op::DIV0U:
        case Op::LDTLB:
        case Op::SLEEP:
        case Op::FSCHG:
        case Op::FRCHG:
            return mn;
        // control registers
        case Op::STC_SR:
        case Op::STC_GBR:
        case Op::STC_VBR:
        case Op::STC_SSR:
        case Op::STC_SPC:
        case Op::STC_SGR:
        case Op::STC_DBR:
        case Op::STS_MACH:
        case Op::STS_MACL:
        case Op::STS_PR:
        case Op::STS_FPUL:
        case Op::STS_FPSCR:
            return two(ctrl_name(ins.op), r(n));
        case Op::STC_BANK:
            return two(r(ins.bank) + "_bank", r(n));
        case Op::STC_L_SR:
        case Op::STC_L_GBR:
        case Op::STC_L_VBR:
        case Op::STC_L_SSR:
        case Op::STC_L_SPC:
        case Op::STC_L_SGR:
        case Op::STC_L_DBR:
        case Op::STS_L_MACH:
        case Op::STS_L_MACL:
        case Op::STS_L_PR:
        case Op::STS_L_FPUL:
        case Op::STS_L_FPSCR:
            return two(ctrl_name(ins.op), "@-" + r(n));
        case Op::STC_L_BANK:
            return two(r(ins.bank) + "_bank", "@-" + r(n));
        case Op::LDC_SR:
        case Op::LDC_GBR:
        case Op::LDC_VBR:
        case Op::LDC_SSR:
        case Op::LDC_SPC:
        case Op::LDC_SGR:
        case Op::LDC_DBR:
        case Op::LDS_MACH:
        case Op::LDS_MACL:
        case Op::LDS_PR:
        case Op::LDS_FPUL:
        case Op::LDS_FPSCR:
            return two(r(m), ctrl_name(ins.op));
        case Op::LDC_BANK:
            return two(r(m), r(ins.bank) + "_bank");
        case Op::LDC_L_SR:
        case Op::LDC_L_GBR:
        case Op::LDC_L_VBR:
        case Op::LDC_L_SSR:
        case Op::LDC_L_SPC:
        case Op::LDC_L_SGR:
        case Op::LDC_L_DBR:
        case Op::LDS_L_MACH:
        case Op::LDS_L_MACL:
        case Op::LDS_L_PR:
        case Op::LDS_L_FPUL:
        case Op::LDS_L_FPSCR:
            return two("@" + r(m) + "+", ctrl_name(ins.op));
        case Op::LDC_L_BANK:
            return two("@" + r(m) + "+", r(ins.bank) + "_bank");
        // FPU
        case Op::FADD:
        case Op::FSUB:
        case Op::FMUL:
        case Op::FDIV:
        case Op::FCMP_EQ:
        case Op::FCMP_GT:
        case Op::FMOV:
            return two(fr(m), fr(n));
        case Op::FMAC:
            return mn + " fr0," + fr(m) + "," + fr(n);
        case Op::FMOV_LOAD:
            return two("@" + r(m), fr(n));
        case Op::FMOV_LOAD_INC:
            return two("@" + r(m) + "+", fr(n));
        case Op::FMOV_LOAD_R0:
            return two("@(r0," + r(m) + ")", fr(n));
        case Op::FMOV_STORE:
            return two(fr(m), "@" + r(n));
        case Op::FMOV_STORE_DEC:
            return two(fr(m), "@-" + r(n));
        case Op::FMOV_STORE_R0:
            return two(fr(m), "@(r0," + r(n) + ")");
        case Op::FSTS:
        case Op::FLOAT:
            return two("fpul", fr(n));
        case Op::FLDS:
        case Op::FTRC:
            return two(fr(n), "fpul");
        case Op::FABS:
        case Op::FNEG:
        case Op::FSQRT:
        case Op::FSRRA:
        case Op::FLDI0:
        case Op::FLDI1:
            return one(fr(n));
        case Op::FCNVSD:
        case Op::FSCA:
            return two("fpul", dr(n));
        case Op::FCNVDS:
            return two(dr(n), "fpul");
        case Op::FIPR:
            return two(fv(m), fv(n));
        case Op::FTRV:
            return two("xmtrx", fv(n));
        case Op::Count:
            break;
    }
    return mn;
}

}  // namespace dream::sh4
