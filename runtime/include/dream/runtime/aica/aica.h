// AICA sound processor, control half (WP2.5, ADR 10): the 32 KB register block at 0x00700000 as the
// SH-4 sees it and at 0x00800000 as the ARM7 sees it, the three timers, the two interrupt
// controllers (SCIEB/SCIPD for the ARM through the e68k latch, MCIEB/MCIPD for the SH-4 through
// Holly's external AICA line), the ARM reset register and the 44.1 kHz sample tick that clocks the
// ARM7 and the timers on the virtual clock. Modelled on Flycast's aica.cpp / aica_mem.cpp /
// aica_if.cpp (GPL-2.0, ADR 1). Channel and DSP registers are stored and read back; sound
// generation is the second half of the work package.
#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "dream/runtime/aica/arm7.h"
#include "dream/runtime/aica/mixer.h"
#include "dream/runtime/holly/intc.h"
#include "dream/runtime/mem/dc_memory.h"
#include "dream/runtime/sched/scheduler.h"

namespace dream::aica {

class Aica final : public mem::MmioHandler, public ArmBus {
public:
    static constexpr std::uint32_t kRegBase = 0x00700000u, kRegEnd = 0x00708000u;
    static constexpr std::uint64_t kSh4CyclesPerSample = 200'000'000 / 44100;  // 4535
    // Register offsets (16-bit registers on 32-bit slots)
    static constexpr std::uint32_t kTimerA = 0x2890, kTimerB = 0x2894, kTimerC = 0x2898,
                                   kScieb = 0x289C, kScipd = 0x28A0, kScire = 0x28A4,
                                   kScilv0 = 0x28A8, kScilv1 = 0x28AC, kScilv2 = 0x28B0,
                                   kMcieb = 0x28B4, kMcipd = 0x28B8, kMcire = 0x28BC,
                                   kArmRst = 0x2C00, kIntReqL = 0x2D00, kIntReqM = 0x2D04;
    // Interrupt bits shared by SCIEB/SCIPD/SCIRE and MCIEB/MCIPD/MCIRE
    static constexpr std::uint32_t kIntMidiIn = 1u << 3, kIntDmaEnd = 1u << 4, kIntScpu = 1u << 5,
                                   kIntTimerA = 1u << 6, kIntTimerB = 1u << 7, kIntTimerC = 1u << 8,
                                   kIntMidiOut = 1u << 9, kIntSampleDone = 1u << 10,
                                   kIntMask = 0x7FFu;

    Aica(sched::Scheduler& sched, holly::Intc& holly, mem::DcMemory& memory);
    void reset();

    // SH-4 side (mem::MmioHandler)
    std::uint32_t read(std::uint32_t addr, unsigned size) override;
    void write(std::uint32_t addr, std::uint32_t value, unsigned size) override;
    // ARM side (ArmBus)
    std::uint8_t* aram() noexcept override { return memory_.aram(); }
    std::uint32_t arm_reg_read(std::uint32_t offset, unsigned size) override;
    void arm_reg_write(std::uint32_t offset, std::uint32_t value, unsigned size) override;

    // Advances one 44.1 kHz sample: the ARM7 runs its share of cycles, timers step, SAMPLE_DONE
    // is raised, both interrupt controllers are re-evaluated and the mixer produces one stereo
    // sample, handed to `on_sample` when set. Scheduled on the virtual clock; callable by tests.
    void sample_tick();
    std::function<void(std::int16_t left, std::int16_t right)> on_sample;
    std::int16_t last_left = 0, last_right = 0;

    std::uint32_t reg16(std::uint32_t offset) const noexcept {
        return regs_[offset] | (static_cast<std::uint32_t>(regs_[offset + 1]) << 8);
    }
    bool arm_in_reset() const noexcept { return (regs_[kArmRst] & 1u) != 0; }

private:
    std::array<std::uint8_t, 0x8000> regs_{};  // before arm/mixer: they are built on it

public:
    Arm7 arm;
    Mixer mixer;
    std::uint64_t samples = 0, timer_irqs = 0, sh4_irq_raises = 0, channel_writes = 0,
                  dsp_writes = 0, internal_dma_requests = 0, scpu_to_arm = 0, scpu_to_sh4 = 0,
                  arm_side_reg_writes = 0, sh4_side_reg_writes = 0;

private:
    std::uint32_t reg_read(std::uint32_t offset, unsigned size);
    void reg_write(std::uint32_t offset, std::uint32_t value, unsigned size);
    void store(std::uint32_t offset, std::uint32_t value, unsigned size);
    void set16(std::uint32_t offset, std::uint32_t value) noexcept;
    void timer_written(int i);
    void step_timers();
    void update_arm_interrupts();
    void update_sh4_interrupts();
    std::uint32_t level_of(unsigned bit) const noexcept;

    sched::Scheduler& sched_;
    holly::Intc& holly_;
    mem::DcMemory& memory_;
    std::array<int, 3> timer_step_{1, 1, 1}, timer_left_{1, 1, 1};
    int event_ = -1;
};

}  // namespace dream::aica
