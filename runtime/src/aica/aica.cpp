// AICA control half; see aica.h. Register semantics follow Flycast (aica.cpp, aica_mem.cpp,
// aica_if.cpp; GPL-2.0, ADR 1).
#include "dream/runtime/aica/aica.h"

namespace dream::aica {

Aica::Aica(sched::Scheduler& sched, holly::Intc& holly, mem::DcMemory& memory)
    : arm(*this),
      mixer(regs_.data(), memory.aram()),
      sched_(sched),
      holly_(holly),
      memory_(memory) {
    reset();
    event_ = sched_.add("aica.sample", [this](std::uint64_t, std::uint64_t) {
        sample_tick();
        sched_.request(event_, kSh4CyclesPerSample);
    });
    sched_.request(event_, kSh4CyclesPerSample);
}

void Aica::reset() {
    regs_.fill(0);
    regs_[kArmRst] = 1;  // the ARM starts held in reset; the BIOS or the title releases it
    timer_step_ = {1, 1, 1};
    timer_left_ = {1, 1, 1};
    arm.reset();
    arm.set_enabled(false);
    mixer.reset();
}

void Aica::set16(std::uint32_t offset, std::uint32_t value) noexcept {
    regs_[offset] = static_cast<std::uint8_t>(value);
    regs_[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

// Registers are 16 bits wide on 32-bit slots: a 32-bit read returns the low half, as Flycast does.
std::uint32_t Aica::reg_read(std::uint32_t offset, unsigned size) {
    offset &= 0x7FFFu;
    if (offset >= 0x2808u && offset < 0x2818u)
        mixer.common_reg_read(offset, size == 1);
    if (size == 1)
        return regs_[offset];
    const std::uint32_t o = offset & ~1u;
    return reg16(o);
}

void Aica::store(std::uint32_t offset, std::uint32_t value, unsigned size) {
    if (size == 1) {
        regs_[offset] = static_cast<std::uint8_t>(value);
        return;
    }
    set16(offset & ~1u, value & 0xFFFFu);
}

void Aica::reg_write(std::uint32_t offset, std::uint32_t value, unsigned size) {
    offset &= 0x7FFFu;
    const std::uint32_t slot = offset & ~3u;
    if (offset < 0x2000u) {  // 64 channels x 0x80 bytes
        ++channel_writes;
        store(offset, value, size);
        mixer.channel_reg_written(offset >> 7, offset & 0x7Fu, size);
        return;
    }
    if (offset >= 0x3000u) {  // DSP coefficients, programme and data
        ++dsp_writes;
        store(offset, value, size);
        if (offset >= 0x3400u && offset < 0x3C00u)
            mixer.dsp_program_written();
        return;
    }
    if (slot == 0x2804u) {  // RBP/RBL: the DSP ring buffer
        store(offset, value, size);
        mixer.ring_buffer_written();
        return;
    }
    switch (slot) {
        case kScieb:
            set16(kScieb, value & kIntMask);
            update_arm_interrupts();
            return;
        case kScipd:  // only the CPU-to-ARM bit is writable
            if (value & kIntScpu) {
                ++scpu_to_arm;
                set16(kScipd, reg16(kScipd) | kIntScpu);
                update_arm_interrupts();
            }
            return;
        case kScire:
            set16(kScipd, reg16(kScipd) & ~(value & kIntMask));
            update_arm_interrupts();
            return;
        case kMcieb:
            set16(kMcieb, value & kIntMask);
            update_sh4_interrupts();
            return;
        case kMcipd:
            if (value & kIntScpu) {
                ++scpu_to_sh4;
                set16(kMcipd, reg16(kMcipd) | kIntScpu);
                update_sh4_interrupts();
            }
            return;
        case kMcire:
            set16(kMcipd, reg16(kMcipd) & ~(value & kIntMask));
            update_sh4_interrupts();
            return;
        case kTimerA:
        case kTimerB:
        case kTimerC:
            store(offset, value, size);
            timer_written(static_cast<int>((slot - kTimerA) / 4));
            return;
        case 0x288C:  // DEXE/DDIR/DLG: the register-to-wave-memory DMA; counted, not modelled yet
            store(offset, value, size);
            if (value & 1u)
                ++internal_dma_requests;
            return;
        case kArmRst: {
            // Low byte ARMRST (bit 0 holds the core in reset), high byte VREG.
            if (size == 1) {
                regs_[offset] = static_cast<std::uint8_t>(value);
            } else {
                regs_[kArmRst] = static_cast<std::uint8_t>(value);
                regs_[kArmRst + 1] = static_cast<std::uint8_t>(value >> 8);
            }
            regs_[kArmRst] &= 1u;
            arm.set_enabled(regs_[kArmRst] == 0);
            return;
        }
        default:
            store(offset, value, size);
            return;
    }
}

std::uint32_t Aica::read(std::uint32_t addr, unsigned size) {
    return reg_read(addr - kRegBase, size);
}

void Aica::write(std::uint32_t addr, std::uint32_t value, unsigned size) {
    ++sh4_side_reg_writes;
    reg_write(addr - kRegBase, value, size);
}

std::uint32_t Aica::arm_reg_read(std::uint32_t offset, unsigned size) {
    offset &= 0x7FFFu;
    if ((offset & ~3u) == kIntReqL)
        return arm.intreq_l();
    if ((offset & ~3u) == kIntReqM)
        return arm.intreq_m();
    return reg_read(offset, size);
}

void Aica::arm_reg_write(std::uint32_t offset, std::uint32_t value, unsigned size) {
    offset &= 0x7FFFu;
    if ((offset & ~3u) == kIntReqL)
        return;  // read only
    if ((offset & ~3u) == kIntReqM) {
        if (value & 1u)
            arm.accept_interrupt();
        return;
    }
    ++arm_side_reg_writes;
    reg_write(offset, value, size);
}

// Timer registers: bits 0-7 count up once per 2^md samples (bits 8-10) and interrupt on wrap.
void Aica::timer_written(int i) {
    const std::uint32_t r = reg16(kTimerA + 4u * static_cast<std::uint32_t>(i));
    const int step = 1 << ((r >> 8) & 7u);
    if (step != timer_step_[static_cast<std::size_t>(i)]) {
        timer_step_[static_cast<std::size_t>(i)] = step;
        timer_left_[static_cast<std::size_t>(i)] = step;
    }
}

void Aica::step_timers() {
    static constexpr std::uint32_t kBits[3] = {kIntTimerA, kIntTimerB, kIntTimerC};
    for (std::size_t i = 0; i < 3; ++i) {
        if (--timer_left_[i] != 0)
            continue;
        timer_left_[i] = timer_step_[i];
        const std::uint32_t off = kTimerA + 4u * static_cast<std::uint32_t>(i);
        const std::uint32_t r = reg16(off);
        const std::uint32_t count = (r + 1u) & 0xFFu;
        set16(off, (r & 0xFF00u) | count);
        if (count == 0) {
            ++timer_irqs;
            set16(kScipd, reg16(kScipd) | kBits[i]);
            set16(kMcipd, reg16(kMcipd) | kBits[i]);
        }
    }
}

std::uint32_t Aica::level_of(unsigned bit) const noexcept {
    if (bit > 7)
        bit = 7;  // sources above bit 7 share bit 7's level
    const std::uint32_t mask = 1u << bit;
    std::uint32_t l = 0;
    if (regs_[kScilv0] & mask)
        l |= 1u;
    if (regs_[kScilv1] & mask)
        l |= 2u;
    if (regs_[kScilv2] & mask)
        l |= 4u;
    return l;
}

void Aica::update_arm_interrupts() {
    const std::uint32_t pending = reg16(kScieb) & reg16(kScipd);
    std::uint32_t level = 0;
    for (unsigned i = 0; i < 11 && pending; ++i) {  // lowest set bit wins
        if (pending & (1u << i)) {
            level = level_of(i);
            break;
        }
    }
    arm.interrupt_change(pending, level);
}

void Aica::update_sh4_interrupts() {
    const bool pending = (reg16(kMcieb) & reg16(kMcipd)) != 0;
    if (pending) {
        if (!holly_.raised(holly::Irq::AicaIrq)) {
            holly_.raise(holly::Irq::AicaIrq);
            ++sh4_irq_raises;
        }
    } else if (holly_.raised(holly::Irq::AicaIrq)) {
        holly_.clear(holly::Irq::AicaIrq);
    }
}

void Aica::sample_tick() {
    ++samples;
    arm.run(Arm7::kCyclesPerSample);
    step_timers();
    set16(kScipd, reg16(kScipd) | kIntSampleDone);
    set16(kMcipd, reg16(kMcipd) | kIntSampleDone);
    mixer.sample(last_left, last_right);
    if (on_sample)
        on_sample(last_left, last_right);
    update_arm_interrupts();
    update_sh4_interrupts();
}

}  // namespace dream::aica
