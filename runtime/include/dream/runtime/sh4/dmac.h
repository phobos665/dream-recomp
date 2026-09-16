// SH-4 on-chip DMA controller (0xFFA00000): the four channels' SAR/DAR/DMATCR/CHCR and DMAOR, with
// the register semantics Flycast's dmac.cpp implements (GPL-2.0, ADR 1). Two things matter to a
// Katana title: DMAOR/CHCR read back what was written (the DMA init routine polls DMAOR for the
// value it stored), and channel 2 is the DDT channel the Holly system block drives for TA and
// texture DMA (holly::SysBlock). Channel 0/1/3 manual (auto-request) transfers copy memory here.
#pragma once

#include <cstdint>
#include <functional>

#include "dream/runtime/mem/dc_memory.h"

namespace dream::sh4 {

class Dmac final : public mem::MmioHandler {
public:
    static constexpr std::uint32_t kBase = 0xFFA00000u, kEnd = 0xFFA00044u;
    static constexpr std::uint32_t kSar = 0x00, kDar = 0x04, kTcr = 0x08, kChcr = 0x0C,
                                   kChannelStride = 0x10, kDmaor = 0x40;
    // CHCR bits
    static constexpr std::uint32_t kDe = 1u << 0, kTe = 1u << 1, kIe = 1u << 2;
    // DMAOR bits
    static constexpr std::uint32_t kDme = 1u << 0, kNmif = 1u << 1, kAe = 1u << 2, kDdt = 1u << 15;

    explicit Dmac(mem::DcMemory& memory) : memory_(memory) {}
    void reset() noexcept;

    std::uint32_t read(std::uint32_t addr, unsigned size) override;
    void write(std::uint32_t addr, std::uint32_t value, unsigned size) override;

    // Save states (state/state.h): this device's registers and internal state.
    void save_state(state::Writer& w) override;
    void load_state(state::Reader& r) override;
    std::uint32_t sar(unsigned ch) const noexcept { return ch_[ch & 3].sar; }
    std::uint32_t dar(unsigned ch) const noexcept { return ch_[ch & 3].dar; }
    std::uint32_t tcr(unsigned ch) const noexcept { return ch_[ch & 3].tcr; }
    std::uint32_t chcr(unsigned ch) const noexcept { return ch_[ch & 3].chcr; }
    std::uint32_t dmaor() const noexcept { return dmaor_; }
    // DDT, priority mode and master enable exactly as Katana programs them (Flycast's DMAC_Ch2St
    // condition). A pending AE or NMIF does not block a start.
    bool ddt_ready() const noexcept { return (dmaor_ & 0xFFFF8201u) == 0x8201u; }

    // Device-side completion of a channel: TE set, count zeroed, transfer-end hook if IE is set.
    void complete(unsigned ch);
    void address_error() noexcept { dmaor_ |= kAe; }

    // Called when a channel finishes with IE set (the SH-4 INTC's DMTE0..3 sources).
    std::function<void(unsigned ch)> on_transfer_end;
    std::uint64_t manual_transfers = 0;

private:
    void manual_dma(unsigned ch);
    struct Channel {
        std::uint32_t sar = 0, dar = 0, tcr = 0, chcr = 0;
    };
    mem::DcMemory& memory_;
    Channel ch_[4]{};
    std::uint32_t dmaor_ = 0;
};

}  // namespace dream::sh4
