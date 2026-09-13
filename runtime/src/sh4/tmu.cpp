#include "dream/runtime/sh4/tmu.h"

#include <algorithm>

namespace dream::sh4 {

Tmu::Tmu(sched::Scheduler& sched, Intc& intc) : sched_(sched), intc_(intc) {
    for (unsigned ch = 0; ch < 3; ++ch)
        event_[ch] = sched_.add("tmu" + std::to_string(ch),
                                [this, ch](std::uint64_t, std::uint64_t) { on_event(ch); });
    reset();
}

void Tmu::reset() {
    for (unsigned ch = 0; ch < 3; ++ch) {
        tcor_[ch] = 0xFFFFFFFFu;
        tcr_[ch] = 0;
        tcr_mode_[ch] = 0xFFFF;
        shift_[ch] = 4;  // TPSC 0: peripheral clock / 4 = CPU / 16
        running_[ch] = false;
        base_[ch] = 0xFFFFFFFF;
        sched_.cancel(event_[ch]);
        update_mode(ch);
        set_counter(ch, 0xFFFFFFFF);
    }
    tstr_ = 0;
    tocr_ = 0;
}

// Counter value as a function of the clock: base minus elapsed prescaled ticks while running.
std::int64_t Tmu::counter(unsigned ch) const noexcept {
    if (!running_[ch])
        return base_[ch];
    return base_[ch] - static_cast<std::int64_t>(sched_.now() >> shift_[ch]);
}

void Tmu::set_counter(unsigned ch, std::int64_t value) {
    base_[ch] =
        running_[ch] ? value + static_cast<std::int64_t>(sched_.now() >> shift_[ch]) : value;
    schedule(ch);
}

void Tmu::schedule(unsigned ch) {
    if (!running_[ch]) {
        sched_.cancel(event_[ch]);
        return;
    }
    const std::int64_t cnt = counter(ch);
    // Underflow happens when the counter would pass below zero: (cnt + 1) prescaled ticks away,
    // plus the remainder of the current tick.
    const std::int64_t ticks = std::max<std::int64_t>(cnt + 1, 0);
    const std::uint64_t cycles = (static_cast<std::uint64_t>(ticks) << shift_[ch]) -
                                 (sched_.now() & ((1ull << shift_[ch]) - 1));
    sched_.request(event_[ch], std::min<std::uint64_t>(cycles, sched::kSh4Clock));
}

void Tmu::reload(unsigned ch, std::int64_t value) {
    tcr_[ch] |= kUnf;
    update_irq(ch);
    // The counter wraps to TCOR and keeps counting from there; value is -1 - overshoot.
    set_counter(ch, std::max<std::int64_t>(static_cast<std::int64_t>(tcor_[ch]) + value + 1, 0));
}

std::uint32_t Tmu::tcnt(unsigned ch) {
    const std::int64_t v = counter(ch);
    if (v < 0) {
        reload(ch, v);
        return static_cast<std::uint32_t>(counter(ch));
    }
    return static_cast<std::uint32_t>(v);
}

void Tmu::on_event(unsigned ch) {
    if (!running_[ch])
        return;
    const std::int64_t v = counter(ch);
    if (v < 0)
        reload(ch, v);
    else
        schedule(ch);  // clamped request: re-arm
}

void Tmu::update_irq(unsigned ch) {
    const Irq irq = ch == 0 ? Irq::Tmu0 : ch == 1 ? Irq::Tmu1 : Irq::Tmu2;
    intc_.set_pending(irq, (tcr_[ch] & kUnf) && (tcr_[ch] & kUnie));
}

void Tmu::update_mode(unsigned ch) {
    const std::uint32_t mode = tcr_[ch] & 7u;
    update_irq(ch);
    if (mode == tcr_mode_[ch])
        return;
    const std::int64_t cnt = counter(ch);
    tcr_mode_[ch] = mode;
    // TPSC 0..4: peripheral clock (CPU/4) divided by 4, 16, 64, 256, 1024.
    shift_[ch] = mode <= 4 ? 4 + 2 * mode : 4;
    set_counter(ch, cnt);
}

std::uint32_t Tmu::read(std::uint32_t addr, unsigned) {
    switch (addr - kBase) {
        case 0x00:
            return tocr_;
        case 0x04:
            return tstr_;
        case 0x08:
            return tcor_[0];
        case 0x0C:
            return tcnt(0);
        case 0x10:
            return tcr_[0];
        case 0x14:
            return tcor_[1];
        case 0x18:
            return tcnt(1);
        case 0x1C:
            return tcr_[1];
        case 0x20:
            return tcor_[2];
        case 0x24:
            return tcnt(2);
        case 0x28:
            return tcr_[2];
        default:
            return 0;
    }
}

void Tmu::write(std::uint32_t addr, std::uint32_t v, unsigned) {
    const std::uint32_t off = addr - kBase;
    switch (off) {
        case 0x00:
            tocr_ = static_cast<std::uint8_t>(v & 1u);
            return;
        case 0x04:
            tstr_ = static_cast<std::uint8_t>(v & 7u);
            for (unsigned ch = 0; ch < 3; ++ch) {
                const bool on = (tstr_ >> ch) & 1u;
                if (on == running_[ch])
                    continue;
                const std::int64_t cnt = counter(ch);
                running_[ch] = on;
                set_counter(ch, cnt);
            }
            return;
        default:
            break;
    }
    if (off >= 0x08 && off < 0x2C) {
        const unsigned ch = (off - 0x08) / 12;
        switch ((off - 0x08) % 12) {
            case 0:
                tcor_[ch] = v;
                return;
            case 4:
                set_counter(ch, static_cast<std::int64_t>(v));
                return;
            case 8: {
                // Writing 0 to UNF clears it; the other bits are plain configuration.
                const std::uint16_t keep_unf = static_cast<std::uint16_t>(tcr_[ch] & v & kUnf);
                tcr_[ch] =
                    static_cast<std::uint16_t>((v & (ch == 2 ? 0x02FFu : 0x00FFu)) | keep_unf);
                update_mode(ch);
                return;
            }
            default:
                return;
        }
    }
}

}  // namespace dream::sh4
