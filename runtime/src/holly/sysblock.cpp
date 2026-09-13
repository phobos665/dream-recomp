#include "dream/runtime/holly/sysblock.h"

namespace dream::holly {

SysBlock::SysBlock(sh4::Dmac& dmac, Intc& holly, mem::DcMemory& memory, pvr::Core& pvr)
    : dmac_(dmac), holly_(holly), memory_(memory), pvr_(pvr) {
    reset();
}

void SysBlock::reset() noexcept {
    regs_.fill(0);
    regs_[kC2dStat >> 2] = 0x10000000u;
    regs_[kSdStaw >> 2] = regs_[kSdBaaw >> 2] = 0x08000000u;
    regs_[kTfRem >> 2] = 8;      // TA FIFO empty: eight 32-byte slots free
    regs_[kSbRev >> 2] = 0x0Bu;  // system bus revision
}

std::uint32_t SysBlock::read(std::uint32_t addr, unsigned) {
    const std::uint32_t off = addr - kBase;
    if (off == kSfRes)
        return 0;  // write-only
    return off < kEnd - kBase ? regs_[off >> 2] : 0;
}

void SysBlock::write(std::uint32_t addr, std::uint32_t v, unsigned) {
    const std::uint32_t off = addr - kBase;
    if (off >= kEnd - kBase)
        return;
    std::uint32_t& r = regs_[off >> 2];
    switch (off) {
        case kC2dStat:
            r = (v & 0x03FFFFE0u) | 0x10000000u;
            break;
        case kC2dLen:
            r = v & 0x00FFFFE0u;
            break;
        case kC2dSt:
            if (v & 1u)
                ch2_dma();
            break;
        case kSdStaw:
        case kSdBaaw:
            r = (v & 0x07FFFFE0u) | 0x08000000u;
            break;
        case kSdWlt:
        case kSdLas:
        case kDbReqM:
        case kLmMode0:
        case kLmMode1:
            r = v & 1u;
            break;
        case kSdSt:
            if (v & 1u)
                ++sort_dma_starts;  // Sort-DMA is not modelled; the start bit reads back clear
            break;
        case kBavlWc:
            r = v & 0x1Fu;
            break;
        case kC2dPryc:
            r = v & 0xFu;
            break;
        case kC2dMaxl:
            r = v & 3u;
            break;
        case kRbSplt:
            r = v & 0x80000000u;
            break;
        case kSfRes:
            if (v == kSoftResetKey && on_soft_reset)
                on_soft_reset();
            break;
        case kSdDiv:
        case kTfRem:
        case kFfSt:
        case kSbRev:
            break;  // read-only
        default:
            r = v;
            break;
    }
}

void SysBlock::ch2_dma() {
    std::uint32_t src = dmac_.sar(2) & 0x1FFFFFE0u;
    const std::uint32_t stat = regs_[kC2dStat >> 2];
    const std::uint32_t dst = stat & 0x01FFFFE0u;
    const std::uint32_t len = regs_[kC2dLen >> 2];
    const auto refuse = [&] {
        last_refused = Refused{dmac_.dmaor(), dmac_.sar(2), stat, len};
        if (ch2_errors++ == 0)
            first_refused = last_refused;
    };
    if (!dmac_.ddt_ready()) {  // DMAOR must have DDT and DME set with no error flags
        refuse();
        return;
    }
    if ((src >> 26) != 3) {  // the source must be system RAM
        refuse();
        dmac_.address_error();
        holly_.raise(Irq::Ch2Dma);
        return;
    }
    if ((dst & 0x01000000u) == 0) {
        // TA FIFO (or YUV converter) path: 32-byte parameter chunks, all to the same FIFO address.
        std::uint32_t words[8];
        for (std::uint32_t off = 0; off + 32 <= len; off += 32) {
            for (unsigned k = 0; k < 8; ++k) words[k] = memory_.read32(src + off + 4 * k);
            pvr_.write_burst(pvr::Core::kFifoBase + dst, words);
        }
        ch2_ta_bytes += len;
    } else {
        // Texture path: the 64-bit or the 32-bit VRAM view, chosen by LMMODE0/1 for the two
        // texture-area mirrors; the destination register advances past the data.
        const bool path64 =
            (stat & 0x02000000u) ? regs_[kLmMode1 >> 2] == 0 : regs_[kLmMode0 >> 2] == 0;
        const std::uint32_t view = (dst & 0x00FFFFFFu) | (path64 ? 0x04000000u : 0x05000000u);
        for (std::uint32_t off = 0; off < len; off += 4)
            memory_.write32(view + off, memory_.read32(src + off));
        regs_[kC2dStat >> 2] = 0x10000000u | ((dst + len) & 0x03FFFFE0u);
        ch2_texture_bytes += len;
    }
    dmac_.complete(2);
    regs_[kC2dLen >> 2] = 0;
    ++ch2_transfers;
    holly_.raise(Irq::Ch2Dma);
}

}  // namespace dream::holly
