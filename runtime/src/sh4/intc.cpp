#include "dream/runtime/sh4/intc.h"

namespace dream::sh4 {

void Intc::set_pending(Irq irq, bool p) noexcept {
    const std::uint32_t bit = 1u << static_cast<unsigned>(irq);
    pending_ = p ? (pending_ | bit) : (pending_ & ~bit);
}

std::uint32_t Intc::intevt(Irq irq) noexcept {
    switch (irq) {
        case Irq::Irl9:
            return 0x320;
        case Irq::Irl11:
            return 0x360;
        case Irq::Irl13:
            return 0x3A0;
        case Irq::Tmu0:
            return 0x400;
        case Irq::Tmu1:
            return 0x420;
        case Irq::Tmu2:
            return 0x440;
        case Irq::Dmte0:
            return 0x640;
        case Irq::Dmte1:
            return 0x660;
        case Irq::Dmte2:
            return 0x680;
        case Irq::Dmte3:
            return 0x6A0;
        case Irq::Dmae:
            return 0x6C0;
        case Irq::ScifEri:
            return 0x700;
        case Irq::ScifRxi:
            return 0x720;
        case Irq::ScifBri:
            return 0x740;
        case Irq::ScifTxi:
            return 0x760;
        default:
            return 0;
    }
}

// IRL levels 9/11/13 have the fixed priorities 6/4/2 (15 - level); the on-chip modules take theirs
// from the IPR nibbles: IPRA TMU0 15:12, TMU1 11:8, TMU2 7:4; IPRC DMAC 11:8, SCIF 7:4.
unsigned Intc::priority(Irq irq) const noexcept {
    switch (irq) {
        case Irq::Irl9:
            return 6;
        case Irq::Irl11:
            return 4;
        case Irq::Irl13:
            return 2;
        case Irq::Tmu0:
            return (ipra >> 12) & 0xF;
        case Irq::Tmu1:
            return (ipra >> 8) & 0xF;
        case Irq::Tmu2:
            return (ipra >> 4) & 0xF;
        case Irq::Dmte0:
        case Irq::Dmte1:
        case Irq::Dmte2:
        case Irq::Dmte3:
        case Irq::Dmae:
            return (iprc >> 8) & 0xF;
        case Irq::ScifEri:
        case Irq::ScifRxi:
        case Irq::ScifBri:
        case Irq::ScifTxi:
            return (iprc >> 4) & 0xF;
        default:
            return 0;
    }
}

std::optional<Selected> Intc::select(std::uint32_t sr) const noexcept {
    if (pending_ == 0 || (sr & SR_BL))
        return std::nullopt;
    const unsigned imask = (sr & SR_IMASK) >> 4;
    std::optional<Selected> best;
    for (unsigned i = 0; i < static_cast<unsigned>(Irq::Count); ++i) {
        if (!((pending_ >> i) & 1u))
            continue;
        const Irq irq = static_cast<Irq>(i);
        const unsigned pr = priority(irq);
        if (pr == 0 || pr <= imask)
            continue;
        if (!best || pr > best->priority)
            best = Selected{irq, intevt(irq), pr};
    }
    return best;
}

std::uint32_t Intc::read(std::uint32_t addr, unsigned) {
    switch (addr - kBase) {
        case 0x0:
            return icr;
        case 0x4:
            return ipra;
        case 0x8:
            return iprb;
        case 0xC:
            return iprc;
        default:
            return 0;
    }
}

void Intc::write(std::uint32_t addr, std::uint32_t v, unsigned) {
    switch (addr - kBase) {
        case 0x0:
            icr = static_cast<std::uint16_t>(v & 0xC380u);
            break;
        case 0x4:
            ipra = static_cast<std::uint16_t>(v);
            break;
        case 0x8:
            iprb = static_cast<std::uint16_t>(v & 0xFFF0u);
            break;
        case 0xC:
            iprc = static_cast<std::uint16_t>(v);
            break;
        default:
            break;
    }
}

}  // namespace dream::sh4
