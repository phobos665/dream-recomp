// SH-4 interrupt controller (WP2.2): the sources that exist on a Dreamcast, their INTEVT codes,
// IPR priorities, and the selection rule against SR.IMASK and SR.BL. Devices set and clear pending
// bits; the system asks which interrupt, if any, to deliver. Registers are the on-chip INTC block
// at 0xFFD00000 (ICR, IPRA, IPRB, IPRC).
#pragma once

#include <cstdint>
#include <optional>

#include "dream/runtime/mem/dc_memory.h"
#include "dream/runtime/sh4/ctx.h"

namespace dream::sh4 {

enum class Irq : unsigned {
    Irl9,     // Holly level 9  (INTEVT 0x320)
    Irl11,    // Holly level 11 (0x360)
    Irl13,    // Holly level 13 (0x3A0)
    Tmu0,     // 0x400
    Tmu1,     // 0x420
    Tmu2,     // 0x440
    Dmte0,    // 0x640
    Dmte1,    // 0x660
    Dmte2,    // 0x680
    Dmte3,    // 0x6A0
    Dmae,     // 0x6C0
    ScifEri,  // 0x700
    ScifRxi,  // 0x720
    ScifBri,  // 0x740
    ScifTxi,  // 0x760
    Count
};

struct Selected {
    Irq irq;
    std::uint32_t intevt;
    unsigned priority;  // 1..15
};

class Intc final : public mem::MmioHandler {
public:
    static constexpr std::uint32_t kBase = 0xFFD00000u;

    void set_pending(Irq irq, bool pending) noexcept;
    bool pending(Irq irq) const noexcept { return (pending_ >> static_cast<unsigned>(irq)) & 1u; }
    bool any_pending() const noexcept { return pending_ != 0; }

    // The pending interrupt of highest priority that SR lets through (priority > IMASK, BL clear);
    // ties resolve in the fixed hardware order, IRL first.
    std::optional<Selected> select(std::uint32_t sr) const noexcept;
    unsigned priority(Irq irq) const noexcept;
    static std::uint32_t intevt(Irq irq) noexcept;

    // MMIO: ICR 0xFFD00000, IPRA 0xFFD00004, IPRB 0xFFD00008, IPRC 0xFFD0000C (16-bit).
    std::uint32_t read(std::uint32_t addr, unsigned size) override;
    void write(std::uint32_t addr, std::uint32_t value, unsigned size) override;
    std::uint16_t ipra = 0, iprb = 0, iprc = 0, icr = 0;

private:
    std::uint32_t pending_ = 0;
};

}  // namespace dream::sh4
