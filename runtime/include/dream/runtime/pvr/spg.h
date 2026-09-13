// Sync pulse generator (WP2.2): the video timing that drives VBlank-in/out and HBlank interrupts
// and the SPG_STATUS scanline the game reads. Timing follows Flycast's spg.cpp (GPL-2.0, ADR 1):
// one scheduler event per scanline, line length from SPG_LOAD and the pixel clock (13.5 MHz unless
// FB_R_CTRL.vclk_div selects 27 MHz). Registers at PVR base + 0xC8..0xE0 and SPG_STATUS at +0x10C.
#pragma once

#include <cstdint>
#include <functional>

#include "dream/runtime/holly/intc.h"
#include "dream/runtime/mem/dc_memory.h"
#include "dream/runtime/sched/scheduler.h"

namespace dream::pvr {

class Spg final : public mem::MmioHandler {
public:
    static constexpr std::uint32_t kPvrBase = 0x005F8000u;
    static constexpr std::uint32_t kRegLo = kPvrBase + 0xC8, kRegHi = kPvrBase + 0xE4;
    static constexpr std::uint32_t kStatus = kPvrBase + 0x10C;
    static constexpr std::uint64_t kPixelClock = 27'000'000;

    Spg(sched::Scheduler& sched, holly::Intc& holly);
    void reset();
    void set_vclk_div(bool full_clock);  // FB_R_CTRL bit 23 (WP2.3 writes it)

    std::uint32_t read(std::uint32_t addr, unsigned size) override;
    void write(std::uint32_t addr, std::uint32_t value, unsigned size) override;

    std::uint32_t scanline() const noexcept { return scanline_; }
    std::uint32_t lines() const noexcept { return ((spg_load >> 16) & 0x3FFu) + 1; }
    std::uint64_t line_cycles() const noexcept { return line_cycles_; }
    std::uint64_t frame_cycles() const noexcept { return line_cycles_ * lines(); }
    std::uint64_t frames() const noexcept { return frames_; }

    // Raised on the VBlank-out line after the interrupt: the Maple bus starts its
    // hardware-triggered transfer here.
    std::function<void()> on_vblank_out;

    std::uint32_t spg_hblank_int = 0x031D0000, spg_vblank_int = 0x00150104, spg_control = 0,
                  spg_hblank = 0x007E0345, spg_load = 0x01060359, spg_vblank = 0x00150104,
                  spg_width = 0x07F1933F;

private:
    void recalc();
    void on_line();

    sched::Scheduler& sched_;
    holly::Intc& holly_;
    int event_ = -1;
    bool vclk_full_ = false;
    std::uint64_t line_cycles_ = 0;
    std::uint32_t scanline_ = 0;
    bool vsync_ = false, fieldnum_ = false;
    std::uint64_t frames_ = 0;
};

}  // namespace dream::pvr
