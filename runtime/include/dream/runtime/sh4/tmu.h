// SH-4 timer unit (WP2.2): three 32-bit down-counters on the peripheral clock (CPU/4) with a
// prescaler, underflow flag and interrupt enable. The model is Flycast's (GPL-2.0, ADR 1): the
// counter is derived from the virtual clock, so reads are exact and only underflows need events.
// Registers at 0xFFD80000: TOCR, TSTR, then TCOR/TCNT/TCR for channels 0-2, TCPR2.
#pragma once

#include <cstdint>

#include "dream/runtime/mem/dc_memory.h"
#include "dream/runtime/sched/scheduler.h"
#include "dream/runtime/sh4/intc.h"

namespace dream::sh4 {

class Tmu final : public mem::MmioHandler {
public:
    static constexpr std::uint32_t kBase = 0xFFD80000u;
    static constexpr std::uint32_t kEnd = 0xFFD80030u;

    Tmu(sched::Scheduler& sched, Intc& intc);
    void reset();

    std::uint32_t read(std::uint32_t addr, unsigned size) override;
    void write(std::uint32_t addr, std::uint32_t value, unsigned size) override;

    // Save states (state/state.h): this device's registers and internal state.
    void save_state(state::Writer& w) override;
    void load_state(state::Reader& r) override;
    std::uint32_t tcnt(unsigned ch);  // current counter value (reloads on underflow)
    std::uint16_t tcr(unsigned ch) const noexcept { return tcr_[ch]; }
    std::uint32_t tcor(unsigned ch) const noexcept { return tcor_[ch]; }
    std::uint8_t tstr() const noexcept { return tstr_; }

private:
    static constexpr std::uint16_t kUnf = 0x100, kUnie = 0x20;
    std::int64_t counter(unsigned ch) const noexcept;
    void set_counter(unsigned ch, std::int64_t value);
    void schedule(unsigned ch);
    void reload(unsigned ch, std::int64_t value);
    void update_mode(unsigned ch);
    void update_irq(unsigned ch);
    void on_event(unsigned ch);

    sched::Scheduler& sched_;
    Intc& intc_;
    int event_[3]{};
    std::uint32_t tcor_[3]{}, tcr_mode_[3]{};
    std::uint16_t tcr_[3]{};
    std::uint8_t tstr_ = 0, tocr_ = 0;
    unsigned shift_[3]{};
    bool running_[3]{};
    std::int64_t base_[3]{};
};

}  // namespace dream::sh4
