// Holly interrupt controller (WP2.2): the system ASIC collects its sources into three status
// registers (normal, external, error) and routes them to the SH-4's IRL levels 9, 11 and 13 through
// per-level mask registers. Register block at 0x005F6900 (SB_ISTNRM .. SB_IML6ERR).
#pragma once

#include <cstdint>

#include "dream/runtime/mem/dc_memory.h"
#include "dream/runtime/sh4/intc.h"

namespace dream::holly {

// Bit numbers in SB_ISTNRM (0-31), SB_ISTEXT (32+) and SB_ISTERR (64+).
enum class Irq : unsigned {
    RenderDoneVideo = 0,
    RenderDoneIsp = 1,
    RenderDone = 2,
    VBlankIn = 3,
    VBlankOut = 4,
    HBlank = 5,
    YuvDma = 6,
    OpaqueDone = 7,
    OpaqueModDone = 8,
    TransDone = 9,
    TransModDone = 10,
    MapleDma = 12,
    MapleVbOver = 13,
    GdromDma = 14,
    AicaDma = 15,
    Ext1Dma = 16,
    Ext2Dma = 17,
    DevDma = 18,
    Ch2Dma = 19,
    PvrDma = 20,
    PunchThruDone = 21,
    // external
    GdromCmd = 32,
    AicaIrq = 33,
    ModemIrq = 34,
    ExtDevIrq = 35,
    // errors (a few; the rest are raised by number)
    ErrRenderIspOutOfCache = 64,
    ErrRenderHazard = 65,
    ErrTaIllegalParam = 66,
    ErrTaFifoOverflow = 67,
    ErrMapleIllegalAddr = 72,
    ErrMapleOverrun = 73,
    ErrMapleFifo = 74,
    ErrMapleIllegalCmd = 75,
    ErrG1IllegalAddr = 76,
    ErrG1Overrun = 77,
    ErrG1Protect = 78,
    ErrAicaIllegalAddr = 79,
    ErrSh4If = 95,
};

class Intc final : public mem::MmioHandler {
public:
    static constexpr std::uint32_t kBase = 0x005F6900u;
    static constexpr std::uint32_t kEnd = 0x005F6940u;

    explicit Intc(sh4::Intc& cpu) : cpu_(cpu) {}
    void reset();

    void raise(Irq irq);
    void clear(Irq irq);  // for external sources, which the device lowers itself
    bool raised(Irq irq) const noexcept;

    std::uint32_t read(std::uint32_t addr, unsigned size) override;
    void write(std::uint32_t addr, std::uint32_t value, unsigned size) override;

    std::uint32_t istnrm = 0, istext = 0, isterr = 0;
    std::uint32_t iml2nrm = 0, iml2ext = 0, iml2err = 0;
    std::uint32_t iml4nrm = 0, iml4ext = 0, iml4err = 0;
    std::uint32_t iml6nrm = 0, iml6ext = 0, iml6err = 0;

private:
    void recompute();
    sh4::Intc& cpu_;
};

}  // namespace dream::holly
