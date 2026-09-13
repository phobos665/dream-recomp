#include "dream/runtime/sh4/dmac.h"

namespace dream::sh4 {

void Dmac::reset() noexcept {
    for (auto& c : ch_) c = {};
    dmaor_ = 0;
}

std::uint32_t Dmac::read(std::uint32_t addr, unsigned) {
    const std::uint32_t off = addr - kBase;
    if (off == kDmaor)
        return dmaor_;
    if (off >= kDmaor)
        return 0;
    const Channel& c = ch_[off / kChannelStride];
    switch (off % kChannelStride) {
        case kSar:
            return c.sar;
        case kDar:
            return c.dar;
        case kTcr:
            return c.tcr;
        default:
            return c.chcr;
    }
}

void Dmac::write(std::uint32_t addr, std::uint32_t value, unsigned) {
    const std::uint32_t off = addr - kBase;
    if (off == kDmaor) {
        // NMIF and AE are flags the software can only clear (write 0); the rest is stored as
        // written within the defined bits.
        dmaor_ = (value & 0xFFFF8201u) | (dmaor_ & value & (kNmif | kAe));
        return;
    }
    if (off >= kDmaor)
        return;
    const unsigned n = off / kChannelStride;
    Channel& c = ch_[n];
    switch (off % kChannelStride) {
        case kSar:
            c.sar = value;
            return;
        case kDar:
            c.dar = value;
            return;
        case kTcr:
            c.tcr = value & 0x00FFFFFFu;
            return;
        default:
            break;
    }
    // CHCR: channels 2 and 3 have no AL/RL bits; TE is cleared by writing 0 and unaffected by 1.
    const std::uint32_t mask = n < 2 ? 0xFF0FFFF7u : 0xFF0AFFF7u;
    const std::uint32_t te = c.chcr & value & kTe;
    c.chcr = (value & mask & ~kTe) | te;
    // Auto-request (RS = 4) with the channel and the controller enabled starts a manual copy.
    if (!(c.chcr & kTe) && (c.chcr & kDe) && (dmaor_ & kDme) && ((c.chcr >> 8) & 0xF) == 4)
        manual_dma(n);
}

void Dmac::manual_dma(unsigned n) {
    Channel& c = ch_[n];
    static constexpr std::uint32_t kSizes[8] = {8, 1, 2, 4, 32, 0, 0, 0};
    const std::uint32_t ts = kSizes[(c.chcr >> 4) & 7];
    if (ts == 0)
        return;
    const auto step = [](unsigned mode, std::uint32_t size) -> std::int32_t {
        return mode == 1   ? static_cast<std::int32_t>(size)
               : mode == 2 ? -static_cast<std::int32_t>(size)
                           : 0;
    };
    const std::int32_t sinc = step((c.chcr >> 12) & 3, ts), dinc = step((c.chcr >> 14) & 3, ts);
    std::uint32_t count = c.tcr ? c.tcr : 0x01000000u;
    for (; count; --count) {
        for (std::uint32_t k = 0; k < ts; k += 4) {
            if (ts < 4) {
                if (ts == 1)
                    memory_.write8(c.dar + k, memory_.read8(c.sar + k));
                else
                    memory_.write16(c.dar + k, memory_.read16(c.sar + k));
            } else {
                memory_.write32(c.dar + k, memory_.read32(c.sar + k));
            }
        }
        c.sar += static_cast<std::uint32_t>(sinc);
        c.dar += static_cast<std::uint32_t>(dinc);
    }
    ++manual_transfers;
    complete(n);
}

void Dmac::complete(unsigned n) {
    Channel& c = ch_[n & 3];
    c.chcr |= kTe;
    c.tcr = 0;
    if ((c.chcr & kIe) && on_transfer_end)
        on_transfer_end(n & 3);
}

}  // namespace dream::sh4
