#include "dream/runtime/pvr/spg.h"

namespace dream::pvr {

Spg::Spg(sched::Scheduler& sched, holly::Intc& holly) : sched_(sched), holly_(holly) {
    event_ = sched_.add("spg-line", [this](std::uint64_t, std::uint64_t) { on_line(); });
    reset();
}

void Spg::reset() {
    spg_hblank_int = 0x031D0000;
    spg_vblank_int = 0x00150104;
    spg_control = 0;
    spg_hblank = 0x007E0345;
    spg_load = 0x01060359;
    spg_vblank = 0x00150104;
    spg_width = 0x07F1933F;
    scanline_ = 0;
    vsync_ = fieldnum_ = false;
    frames_ = 0;
    recalc();
}

void Spg::set_vclk_div(bool full) {
    vclk_full_ = full;
    recalc();
}

void Spg::recalc() {
    const std::uint64_t pixel_clock = vclk_full_ ? kPixelClock : kPixelClock / 2;
    const std::uint64_t hcount = (spg_load & 0x3FFu) + 1;  // SPG_LOAD: hcount 9:0, vcount 25:16
    line_cycles_ = sched::kSh4Clock * hcount / pixel_clock;
    if (spg_control & 0x10u)  // interlace
        line_cycles_ /= 2;
    if (line_cycles_ == 0)
        line_cycles_ = 1;
    sched_.request(event_, line_cycles_);
}

void Spg::on_line() {
    scanline_ = (scanline_ + 1) % lines();
    const std::uint32_t vblank_in = spg_vblank_int & 0x3FFu;
    const std::uint32_t vblank_out = (spg_vblank_int >> 16) & 0x3FFu;
    const std::uint32_t vstart = spg_vblank & 0x3FFu;
    const std::uint32_t vbend = (spg_vblank >> 16) & 0x3FFu;
    if (scanline_ == vblank_in)
        holly_.raise(holly::Irq::VBlankIn);
    if (scanline_ == vblank_out) {
        holly_.raise(holly::Irq::VBlankOut);
        if (on_vblank_out)
            on_vblank_out();
    }
    if (scanline_ == vstart)
        vsync_ = true;
    if (scanline_ == vbend)
        vsync_ = false;
    switch ((spg_hblank_int >> 12) & 3u) {
        case 0:
            if (scanline_ == ((spg_hblank_int >> 16) & 0x3FFu))
                holly_.raise(holly::Irq::HBlank);
            break;
        case 2:
            holly_.raise(holly::Irq::HBlank);
            break;
        default:
            break;
    }
    if (scanline_ == 0) {
        fieldnum_ = (spg_control & 0x10u) ? !fieldnum_ : false;
        ++frames_;
    }
    sched_.request(event_, line_cycles_);
}

std::uint32_t Spg::read(std::uint32_t addr, unsigned) {
    if (addr == kStatus) {
        // scanline 9:0, fieldnum 10, blank 11, hsync 12, vsync 13
        const std::uint32_t vstart = spg_vblank & 0x3FFu;
        const bool blank = vsync_ || scanline_ >= vstart;
        return scanline_ | (fieldnum_ ? 1u << 10 : 0) | (blank ? 1u << 11 : 0) |
               (vsync_ ? 1u << 13 : 0);
    }
    switch (addr - kPvrBase) {
        case 0xC8:
            return spg_hblank_int;
        case 0xCC:
            return spg_vblank_int;
        case 0xD0:
            return spg_control;
        case 0xD4:
            return spg_hblank;
        case 0xD8:
            return spg_load;
        case 0xDC:
            return spg_vblank;
        case 0xE0:
            return spg_width;
        default:
            return 0;
    }
}

void Spg::write(std::uint32_t addr, std::uint32_t v, unsigned) {
    switch (addr - kPvrBase) {
        case 0xC8:
            spg_hblank_int = v;
            break;
        case 0xCC:
            spg_vblank_int = v;
            break;
        case 0xD0:
            spg_control = v;
            recalc();
            break;
        case 0xD4:
            spg_hblank = v;
            break;
        case 0xD8:
            spg_load = v;
            recalc();
            break;
        case 0xDC:
            spg_vblank = v;
            break;
        case 0xE0:
            spg_width = v;
            break;
        default:
            break;
    }
}

}  // namespace dream::pvr
