// Holly system block (0x005F6800..0x005F68FF): the channel-2 (TA) DMA that Kamui uses to submit
// display lists and textures, the Sort-DMA registers, the TA FIFO status and bus configuration
// registers, and the system-bus revision. Register masks and the transfer itself follow Flycast's
// sb.cpp and dmac.cpp DMAC_Ch2St (GPL-2.0, ADR 1). The transfer completes instantly, as in
// Flycast: data is fed to the PowerVR core or copied into VRAM, the DMAC channel is marked done,
// and the ch2-DMA-end interrupt is raised.
#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "dream/runtime/holly/intc.h"
#include "dream/runtime/mem/dc_memory.h"
#include "dream/runtime/pvr/core.h"
#include "dream/runtime/sh4/dmac.h"

namespace dream::holly {

class SysBlock final : public mem::MmioHandler {
public:
    static constexpr std::uint32_t kBase = 0x005F6800u, kEnd = 0x005F6900u;
    // Register offsets
    static constexpr std::uint32_t kC2dStat = 0x00, kC2dLen = 0x04, kC2dSt = 0x08, kSdStaw = 0x10,
                                   kSdBaaw = 0x14, kSdWlt = 0x18, kSdLas = 0x1C, kSdSt = 0x20,
                                   kDbReqM = 0x40, kBavlWc = 0x44, kC2dPryc = 0x48, kC2dMaxl = 0x4C,
                                   kSdDiv = 0x60, kTfRem = 0x80, kLmMode0 = 0x84, kLmMode1 = 0x88,
                                   kFfSt = 0x8C, kSfRes = 0x90, kSbRev = 0x9C, kRbSplt = 0xA0;
    static constexpr std::uint32_t kSoftResetKey = 0x7611u;

    SysBlock(sh4::Dmac& dmac, Intc& holly, mem::DcMemory& memory, pvr::Core& pvr);
    void reset() noexcept;

    std::uint32_t read(std::uint32_t addr, unsigned size) override;
    void write(std::uint32_t addr, std::uint32_t value, unsigned size) override;
    std::uint32_t reg(std::uint32_t offset) const noexcept { return regs_[offset >> 2]; }

    std::function<void()> on_soft_reset;  // SB_SFRES written with the reset key
    // Counters
    std::uint64_t ch2_transfers = 0, ch2_ta_bytes = 0, ch2_texture_bytes = 0, ch2_errors = 0,
                  sort_dma_starts = 0;
    // The registers at the first and the last refused start, for the launcher report.
    struct Refused {
        std::uint32_t dmaor = 0, src = 0, dst = 0, len = 0;
    } first_refused, last_refused;

private:
    void ch2_dma();
    sh4::Dmac& dmac_;
    Intc& holly_;
    mem::DcMemory& memory_;
    pvr::Core& pvr_;
    std::array<std::uint32_t, (kEnd - kBase) / 4> regs_{};
};

}  // namespace dream::holly
