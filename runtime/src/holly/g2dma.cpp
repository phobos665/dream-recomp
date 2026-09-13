// G2 bus DMA; see g2dma.h.
#include "dream/runtime/holly/g2dma.h"

namespace dream::holly {

namespace {
constexpr Irq kDoneIrq[G2Dma::kChannels] = {Irq::AicaDma, Irq::Ext1Dma, Irq::Ext2Dma, Irq::DevDma};
constexpr std::uint64_t kCyclesPerByte = 200'000'000 / 2 / 25'000'000;  // 16 bits at 25 MHz
}  // namespace

G2Dma::G2Dma(sched::Scheduler& sched, Intc& holly, mem::DcMemory& memory)
    : sched_(sched), holly_(holly), memory_(memory) {
    regs_[kG2Id / 4] = 0x12;
    for (unsigned ch = 0; ch < kChannels; ++ch) {
        regs_[ch * 8 + kSusp / 4] = 0x10;  // idle: "DMA stopped" bit
        events_[ch] = sched_.add("g2dma", [this, ch](std::uint64_t, std::uint64_t) { finish(ch); });
    }
}

std::uint32_t G2Dma::read(std::uint32_t addr, unsigned) {
    const std::uint32_t off = (addr - kBase) & 0xFCu;
    return regs_[off / 4];
}

void G2Dma::write(std::uint32_t addr, std::uint32_t value, unsigned) {
    const std::uint32_t off = (addr - kBase) & 0xFCu;
    if (off >= 0x80) {  // ID (read only), time-outs, protection: stored
        if (off != kG2Id)
            regs_[off / 4] = value;
        return;
    }
    const unsigned ch = off / 0x20;
    switch (off & 0x1Cu) {
        case kSt:
            if ((value & 1u) && !(regs_[ch * 8 + kSt / 4] & 1u))
                start(ch);
            return;
        case kSusp:
            regs_[ch * 8 + kSusp / 4] = (regs_[ch * 8 + kSusp / 4] & 0x10u) | (value & 1u);
            return;
        case kLen:
            regs_[ch * 8 + kLen / 4] = value & 0x9FFFFFFFu;
            return;
        case kEn:
        case kDir:
        case kTsel:
            regs_[off / 4] = value & 1u;
            return;
        default:  // STAG, STAR
            regs_[off / 4] = value & 0x1FFFFFE0u;
            return;
    }
}

void G2Dma::start(unsigned ch) {
    std::uint32_t* r = &regs_[ch * 8];
    if (r[kEn / 4] != 1u)
        return;
    std::uint32_t src = r[kStar / 4], dst = r[kStag / 4];
    const std::uint32_t len = r[kLen / 4] & 0x7FFFFFFFu;
    // STAR must lie in system RAM (or VRAM); STAG on the G2 bus (area 0 devices) or in RAM.
    const bool src_ok = ((src >> 26) & 7u) == 3u || ((src >> 26) & 7u) == 1u;
    const bool dst_ok = ((dst >> 26) & 7u) == 0u || ((dst >> 26) & 7u) == 3u;
    if (!src_ok || !dst_ok || len == 0) {
        ++refused;
        r[kSt / 4] = 0;
        r[kEn / 4] = 0;
        holly_.raise(Irq::ErrAicaIllegalAddr);
        return;
    }
    if (r[kDir / 4] == 1u)
        std::swap(src, dst);
    for (std::uint32_t i = 0; i < len; i += 4) memory_.write32(dst + i, memory_.read32(src + i));
    ++transfers;
    bytes += len;
    if ((dst & 0x1F000000u) == 0x00000000u && (dst & 0x00800000u))
        to_aica_ram += len;
    r[kSt / 4] = 1u;  // in progress until the end-of-DMA interrupt
    r[kSusp / 4] &= ~0x10u;
    const std::uint64_t cycles = len * kCyclesPerByte;
    if (cycles <= 512)
        finish(ch);
    else
        sched_.request(events_[ch], cycles);
}

void G2Dma::finish(unsigned ch) {
    std::uint32_t* r = &regs_[ch * 8];
    const std::uint32_t len = r[kLen / 4] & 0x7FFFFFFFu;
    if (r[kLen / 4] & 0x80000000u)
        r[kEn / 4] = 0;
    r[kStar / 4] += len;
    r[kStag / 4] += len;
    r[kLen / 4] = 0;
    r[kSt / 4] = 0;
    r[kSusp / 4] |= 0x10u;
    holly_.raise(kDoneIrq[ch]);
}

}  // namespace dream::holly
