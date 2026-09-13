#include "dream/runtime/holly/intc.h"

namespace dream::holly {

void Intc::reset() {
    istnrm = istext = isterr = 0;
    iml2nrm = iml2ext = iml2err = iml4nrm = iml4ext = iml4err = iml6nrm = iml6ext = iml6err = 0;
    recompute();
}

void Intc::raise(Irq irq) {
    const unsigned n = static_cast<unsigned>(irq);
    if (n < 32)
        istnrm |= 1u << n;
    else if (n < 64)
        istext |= 1u << (n - 32);
    else
        isterr |= 1u << (n - 64);
    recompute();
}

void Intc::clear(Irq irq) {
    const unsigned n = static_cast<unsigned>(irq);
    if (n < 32)
        istnrm &= ~(1u << n);
    else if (n < 64)
        istext &= ~(1u << (n - 32));
    else
        isterr &= ~(1u << (n - 64));
    recompute();
}

bool Intc::raised(Irq irq) const noexcept {
    const unsigned n = static_cast<unsigned>(irq);
    if (n < 32)
        return (istnrm >> n) & 1u;
    if (n < 64)
        return (istext >> (n - 32)) & 1u;
    return (isterr >> (n - 64)) & 1u;
}

// Level 6 masks feed IRL9, level 4 masks IRL11, level 2 masks IRL13 (Flycast holly_intc.cpp).
void Intc::recompute() {
    cpu_.set_pending(sh4::Irq::Irl9,
                     (istnrm & iml6nrm) || (istext & iml6ext) || (isterr & iml6err));
    cpu_.set_pending(sh4::Irq::Irl11,
                     (istnrm & iml4nrm) || (istext & iml4ext) || (isterr & iml4err));
    cpu_.set_pending(sh4::Irq::Irl13,
                     (istnrm & iml2nrm) || (istext & iml2ext) || (isterr & iml2err));
}

std::uint32_t Intc::read(std::uint32_t addr, unsigned) {
    switch (addr - kBase) {
        case 0x00: {
            // Bits 30 and 31 summarise the external and error registers.
            std::uint32_t v = istnrm & 0x3FFFFFFFu;
            if (istext)
                v |= 0x40000000u;
            if (isterr)
                v |= 0x80000000u;
            return v;
        }
        case 0x04:
            return istext;
        case 0x08:
            return isterr;
        case 0x10:
            return iml2nrm;
        case 0x14:
            return iml2ext;
        case 0x18:
            return iml2err;
        case 0x20:
            return iml4nrm;
        case 0x24:
            return iml4ext;
        case 0x28:
            return iml4err;
        case 0x30:
            return iml6nrm;
        case 0x34:
            return iml6ext;
        case 0x38:
            return iml6err;
        default:
            return 0;
    }
}

void Intc::write(std::uint32_t addr, std::uint32_t v, unsigned) {
    switch (addr - kBase) {
        case 0x00:
            istnrm &= ~v;
            break;  // write 1 to clear
        case 0x04:
            break;  // external sources are cleared at the device
        case 0x08:
            isterr &= ~v;
            break;
        case 0x10:
            iml2nrm = v;
            break;
        case 0x14:
            iml2ext = v;
            break;
        case 0x18:
            iml2err = v;
            break;
        case 0x20:
            iml4nrm = v;
            break;
        case 0x24:
            iml4ext = v;
            break;
        case 0x28:
            iml4err = v;
            break;
        case 0x30:
            iml6nrm = v;
            break;
        case 0x34:
            iml6ext = v;
            break;
        case 0x38:
            iml6err = v;
            break;
        default:
            return;
    }
    recompute();
}

}  // namespace dream::holly
