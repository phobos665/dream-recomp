#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "dream/translator/sh4/decoder.h"

#include "doctest.h"

using namespace dream::sh4;

namespace {

struct OracleRow {
    std::uint16_t word;
    std::string text;
};

std::vector<OracleRow> load_oracle() {
    std::vector<OracleRow> rows;
    std::ifstream in(DREAM_SH4_ORACLE);
    REQUIRE_MESSAGE(in.good(), "oracle file missing: " DREAM_SH4_ORACLE);
    std::string line;
    while (std::getline(in, line)) {
        auto tab = line.find('\t');
        if (tab == std::string::npos)
            continue;
        rows.push_back({static_cast<std::uint16_t>(std::stoul(line.substr(0, tab), nullptr, 16)),
                        line.substr(tab + 1)});
    }
    return rows;
}

// binutils (2.43.1 in the KallistiOS toolchain) prints the target of BT/BF/BT.S/BF.S with the 8-bit
// displacement taken as unsigned, unlike the ISA (and unlike its own 12-bit BRA/BSR handling). The
// decoder keeps the correct signed semantics; only the comparison mirrors objdump.
std::string objdump_style(const Instr& ins, std::uint32_t pc) {
    switch (ins.op) {
        case Op::BT:
        case Op::BF:
        case Op::BT_S:
        case Op::BF_S: {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%s 0x%x", std::string(mnemonic(ins.op)).c_str(),
                          pc + 4 + (ins.raw & 0xFFu) * 2u);
            return buf;
        }
        default:
            return format(ins, pc);
    }
}

}  // namespace

TEST_CASE("every 16-bit word decodes and formats like sh-elf-objdump") {
    const auto rows = load_oracle();
    REQUIRE(rows.size() == 65536);
    int mismatches = 0, invalid_expected = 0, valid_expected = 0;
    for (const auto& row : rows) {
        const std::uint32_t pc = static_cast<std::uint32_t>(row.word) * 4u;  // oracle layout
        const Instr ins = decode(row.word);
        const bool oracle_invalid = row.text.rfind(".word", 0) == 0;
        if (oracle_invalid)
            ++invalid_expected;
        else
            ++valid_expected;
        const std::string ours = objdump_style(ins, pc);
        if (ours != row.text) {
            if (++mismatches <= 40) {
                MESSAGE("word " << std::hex << row.word << std::dec << ": ours '" << ours
                                << "' oracle '" << row.text << "'");
            }
        }
    }
    CHECK(mismatches == 0);
    CHECK(invalid_expected == 6503);
    CHECK(valid_expected == 65536 - 6503);
}

TEST_CASE("branch displacements are signed in the decoder") {
    // bf -2 (disp 0xff) loops back: target = pc + 4 - 4 = pc
    const Instr bf = decode(0x8BFF);
    CHECK(bf.op == Op::BF);
    CHECK(bf.imm == -1);
    CHECK(pcrel_target(bf, 0x8C010000) == 0x8C010002);
    const Instr bra = decode(0xAFFE);
    CHECK(bra.imm == -2);
    CHECK(pcrel_target(bra, 0x8C010000) == 0x8C010000);
    const Instr movl = decode(0xD105);
    CHECK(pcrel_target(movl, 0x8C010002) == 0x8C010018);  // (pc&~3)+4+5*4
}

TEST_CASE("delay slots and control flow classification") {
    CHECK(has_delay_slot(Op::BRA));
    CHECK(has_delay_slot(Op::RTS));
    CHECK(has_delay_slot(Op::BT_S));
    CHECK_FALSE(has_delay_slot(Op::BT));
    CHECK(is_control_flow(Op::BT));
    CHECK(is_control_flow(Op::TRAPA));
    CHECK_FALSE(is_control_flow(Op::ADD));
    CHECK(decode(0x403A).sh4a_only);        // ldc r0,sgr
    CHECK_FALSE(decode(0x003A).sh4a_only);  // stc sgr,r0 exists on SH-4
}

TEST_CASE("field extraction samples") {
    Instr i = decode(0x2FE6);  // mov.l r14,@-r15
    CHECK(i.op == Op::MOV_L_SD);
    CHECK(i.n == 15);
    CHECK(i.m == 14);
    i = decode(0x4F22);  // sts.l pr,@-r15
    CHECK(i.op == Op::STS_L_PR);
    CHECK(i.n == 15);
    i = decode(0x0092);  // stc r1_bank,r0
    CHECK(i.op == Op::STC_BANK);
    CHECK(i.bank == 1);
    CHECK(i.n == 0);
    i = decode(0xF5ED);  // fipr fv4,fv4
    CHECK(i.op == Op::FIPR);
    CHECK(i.n == 4);
    CHECK(i.m == 4);
    i = decode(0xFDFD);  // ftrv xmtrx,fv12
    CHECK(i.op == Op::FTRV);
    CHECK(i.n == 12);
    i = decode(0xE0FF);  // mov #-1,r0
    CHECK(i.op == Op::MOV_I);
    CHECK(i.imm == -1);
    i = decode(0x808F);  // mov.b r0,@(15,r8)
    CHECK(i.op == Op::MOV_B_S_DISP0);
    CHECK(i.m == 8);
    CHECK(i.imm == 15);
}
