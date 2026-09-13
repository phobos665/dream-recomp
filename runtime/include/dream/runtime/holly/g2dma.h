// G2 bus DMA (WP2.5): the four Holly channels at 0x005F7800 that move blocks between system RAM
// and the G2 devices, of which the AICA (sound RAM at 0x00800000 and its registers) is the one
// titles use: Katana's sound library uploads driver commands, samples and streamed ADX blocks this
// way. Register set per channel: STAG (G2 address), STAR (system RAM address), LEN (bit 31: clear
// EN when done), DIR (1: G2 to RAM), TSEL (hardware trigger, not modelled), EN, ST, SUSP. As in
// Flycast's aica_if.cpp Write_SB_ADST / Write_DmaStart (GPL-2.0, ADR 1): the copy is immediate,
// the end-of-DMA interrupt follows after the transfer time on the 25 MHz 16-bit bus.
#pragma once

#include <array>
#include <cstdint>

#include "dream/runtime/holly/intc.h"
#include "dream/runtime/mem/dc_memory.h"
#include "dream/runtime/sched/scheduler.h"

namespace dream::holly {

class G2Dma final : public mem::MmioHandler {
public:
    static constexpr std::uint32_t kBase = 0x005F7800u, kEnd = 0x005F7900u;
    static constexpr unsigned kChannels = 4;  // AICA, EXT1, EXT2, DEV
    // Per-channel register offsets
    static constexpr std::uint32_t kStag = 0x00, kStar = 0x04, kLen = 0x08, kDir = 0x0C,
                                   kTsel = 0x10, kEn = 0x14, kSt = 0x18, kSusp = 0x1C;
    static constexpr std::uint32_t kG2Id = 0x80;  // reads 0x12 (Flycast)

    G2Dma(sched::Scheduler& sched, Intc& holly, mem::DcMemory& memory);

    std::uint32_t read(std::uint32_t addr, unsigned size) override;
    void write(std::uint32_t addr, std::uint32_t value, unsigned size) override;

    std::uint32_t reg(unsigned channel, std::uint32_t offset) const noexcept {
        return regs_[channel * 8 + offset / 4];
    }
    std::uint64_t transfers = 0, bytes = 0, refused = 0, to_aica_ram = 0;

private:
    void start(unsigned channel);
    void finish(unsigned channel);

    sched::Scheduler& sched_;
    Intc& holly_;
    mem::DcMemory& memory_;
    std::array<std::uint32_t, 0x100 / 4> regs_{};
    std::array<int, kChannels> events_{-1, -1, -1, -1};
};

}  // namespace dream::holly
