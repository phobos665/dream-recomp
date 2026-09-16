// ARM7DI core of the AICA sound processor (WP2.5, ADR 10): the interpreter that runs the sound
// driver a title uploads to sound RAM. The decode/execute table is Flycast's copy of the
// VisualBoyAdvance ARM interpreter (arm7_ops.inc, GPL-2.0); this class is the glue around it:
// register banks, mode switches, exceptions, the memory bus and the e68k-style interrupt latch that
// feeds the FIQ pin, as in Flycast's arm7.cpp / arm_mem.cpp. No Thumb: the ARM7DI has none.
#pragma once

#include <cstdint>

namespace dream::state {
class Writer;
class Reader;
}  // namespace dream::state

namespace dream::aica {

// What the ARM7 sees: 2 MB of sound RAM mirrored through 0x007FFFFF and the AICA registers at
// 0x00800000 (the block the SH-4 sees at 0x00700000 plus the ARM-only INTREQ registers).
class ArmBus {
public:
    virtual ~ArmBus() = default;
    virtual std::uint8_t* aram() noexcept = 0;
    virtual std::uint32_t arm_reg_read(std::uint32_t offset, unsigned size) = 0;  // offset < 0x8000
    virtual void arm_reg_write(std::uint32_t offset, std::uint32_t value, unsigned size) = 0;
};

// Register file layout (Flycast's Arm7Reg): r0-r15, CPSR, SPSR, then the banked copies.
enum ArmReg : unsigned {
    RN_LR = 14,
    RN_PC = 15,
    RN_CPSR = 16,
    RN_SPSR = 17,
    R13_IRQ = 18,
    R14_IRQ = 19,
    SPSR_IRQ = 20,
    R13_USR = 26,
    R14_USR = 27,
    R13_SVC = 28,
    R14_SVC = 29,
    SPSR_SVC = 30,
    R13_ABT = 31,
    R14_ABT = 32,
    SPSR_ABT = 33,
    R13_UND = 34,
    R14_UND = 35,
    SPSR_UND = 36,
    R8_FIQ = 37,
    R9_FIQ = 38,
    R10_FIQ = 39,
    R11_FIQ = 40,
    R12_FIQ = 41,
    R13_FIQ = 42,
    R14_FIQ = 43,
    SPSR_FIQ = 44,
    RN_PSR_FLAGS = 45,
    R15_ARM_NEXT = 46,
    INTR_PEND = 47,
    CYCL_CNT = 48,
    RN_SCRATCH = 49,
    RN_ARM_REG_COUNT
};

union RegPair {
    struct Bytes {
        std::uint8_t B0, B1, B2, B3;
    } B;
    struct Halves {
        std::uint16_t W0, W1;
    } W;
    std::uint32_t I;
};

class Arm7 {
public:
    static constexpr std::uint32_t kAramMask = 0x1FFFFFu;  // 2 MB
    static constexpr int kClockHz = 22'579'200;            // 512 cycles per 44.1 kHz sample
    static constexpr int kCyclesPerSample = kClockHz / 44100;

    explicit Arm7(ArmBus& bus) noexcept;

    void reset();
    // ARMRST: releasing the core from reset resets it first; holding it stops execution.
    void set_enabled(bool on);
    bool enabled() const noexcept { return enabled_; }
    // Executes instructions until `cycles` of the 22.58 MHz clock are used up (the table's own
    // cycle estimates), or immediately returns when the core is held in reset.
    void run(int cycles);

    // e68k interrupt latch (Flycast arm_mem.cpp): the AICA's SCIEB & SCIPD output and its level L
    // are held until the driver acknowledges by writing INTREQ M bit 0; while held, FIQ is
    // asserted.
    void interrupt_change(std::uint32_t pending_bits, std::uint32_t level);
    void accept_interrupt();
    std::uint32_t intreq_l() const noexcept { return e68k_reg_l_; }
    std::uint32_t intreq_m() const noexcept { return e68k_reg_m_; }

    // State, for tests and reports.
    std::uint32_t reg(unsigned n) const noexcept { return r_[n].I; }
    void set_reg(unsigned n, std::uint32_t v) noexcept { r_[n].I = v; }
    std::uint32_t next_pc() const noexcept { return r_[R15_ARM_NEXT].I; }
    std::uint32_t cpsr();
    int mode() const noexcept { return mode_; }
    bool fiq_enabled() const noexcept { return fiq_enable_; }

    // Save states (state/state.h): this device's registers and internal state.
    void save_state(state::Writer& w);
    void load_state(state::Reader& r);
    std::uint64_t instructions = 0, fiqs = 0, swis = 0, undefined_ops = 0;
    std::uint64_t fiqs_by_level[8] = {};

private:
    template <typename T>
    T read(std::uint32_t addr);
    template <typename T>
    void write(std::uint32_t addr, T value);
    std::uint32_t fetch(std::uint32_t addr);

    void switch_mode(int mode, bool save_state);
    void update_cpsr();
    void update_flags();
    void update_intc();
    void swi();
    void undefined();
    void fiq();
    void step();

    ArmBus& bus_;
    RegPair r_[RN_ARM_REG_COUNT]{};
    bool n_flag_ = false, z_flag_ = false, c_flag_ = false, v_flag_ = false;
    bool irq_enable_ = true, fiq_enable_ = false, enabled_ = false;
    int mode_ = 0x13;
    int ticks_ = 0;
    // e68k latch
    bool aica_interr_ = false, e68k_out_ = false;
    std::uint32_t aica_reg_l_ = 0, e68k_reg_l_ = 0, e68k_reg_m_ = 0;
};

}  // namespace dream::aica
