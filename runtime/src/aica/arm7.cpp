// ARM7DI glue around the VisualBoyAdvance/Flycast decode table (arm7_ops.inc). Structure and
// register model follow Flycast's core/hw/arm7/arm7.cpp and arm_mem.cpp (GPL-2.0, ADR 1).
#include "dream/runtime/aica/arm7.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dream::aica {

namespace {
using u32 = std::uint32_t;
using s32 = std::int32_t;
using u16 = std::uint16_t;
using s16 = std::int16_t;
using u8 = std::uint8_t;
using s8 = std::int8_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;

// Population counts for LDM/STM register lists.
struct BitsSet {
    u8 t[256];
    constexpr BitsSet() : t{} {
        for (unsigned i = 0; i < 256; ++i) {
            unsigned n = 0;
            for (unsigned j = 0; j < 8; ++j)
                if (i & (1u << j))
                    ++n;
            t[i] = static_cast<u8>(n);
        }
    }
};
constexpr BitsSet kBitsSet{};
}  // namespace

Arm7::Arm7(ArmBus& bus) noexcept : bus_(bus) {}

template <typename T>
T Arm7::read(std::uint32_t addr) {
    addr &= 0x00FFFFFFu;
    if (addr < 0x800000u) {
        T v;
        std::memcpy(&v, bus_.aram() + (addr & (kAramMask - (sizeof(T) - 1))), sizeof(T));
        if (sizeof(T) == 4 && (addr & 3u)) {  // unaligned word loads rotate, as on the ARM7
            const unsigned sf = (addr & 3u) * 8;
            const std::uint32_t w = static_cast<std::uint32_t>(v);
            return static_cast<T>((w >> sf) | (w << (32 - sf)));
        }
        return v;
    }
    return static_cast<T>(bus_.arm_reg_read(addr & 0x7FFFu, sizeof(T)));
}

template <typename T>
void Arm7::write(std::uint32_t addr, T value) {
    addr &= 0x00FFFFFFu;
    if (addr < 0x800000u) {
        std::memcpy(bus_.aram() + (addr & (kAramMask - (sizeof(T) - 1))), &value, sizeof(T));
        return;
    }
    bus_.arm_reg_write(addr & 0x7FFFu, static_cast<std::uint32_t>(value), sizeof(T));
}

std::uint32_t Arm7::fetch(std::uint32_t addr) {
    std::uint32_t v;
    std::memcpy(&v, bus_.aram() + (addr & kAramMask & ~3u), 4);
    return v;
}

void Arm7::update_cpsr() {
    std::uint32_t cpsr = r_[RN_CPSR].I & 0x40u;
    cpsr |= (n_flag_ ? 0x80000000u : 0u) | (z_flag_ ? 0x40000000u : 0u) |
            (c_flag_ ? 0x20000000u : 0u) | (v_flag_ ? 0x10000000u : 0u);
    if (!fiq_enable_)
        cpsr |= 0x40u;
    if (!irq_enable_)
        cpsr |= 0x80u;
    cpsr = (cpsr & ~0x1Fu) | (static_cast<std::uint32_t>(mode_) & 0x1Fu);
    r_[RN_CPSR].I = cpsr;
}

std::uint32_t Arm7::cpsr() {
    update_cpsr();
    return r_[RN_CPSR].I;
}

void Arm7::update_flags() {
    const std::uint32_t cpsr = r_[RN_CPSR].I;
    n_flag_ = (cpsr >> 31) & 1u;
    z_flag_ = (cpsr >> 30) & 1u;
    c_flag_ = (cpsr >> 29) & 1u;
    v_flag_ = (cpsr >> 28) & 1u;
    irq_enable_ = !(cpsr & 0x80u);
    fiq_enable_ = !(cpsr & 0x40u);
    update_intc();
}

void Arm7::update_intc() {
    r_[INTR_PEND].I = (e68k_out_ && fiq_enable_) ? 1u : 0u;
}

namespace {
void swap(std::uint32_t& a, std::uint32_t& b) noexcept {
    const std::uint32_t t = b;
    b = a;
    a = t;
}
}  // namespace

void Arm7::switch_mode(int mode, bool save_state) {
    update_cpsr();
    switch (mode_) {
        case 0x10:
        case 0x1F:
            r_[R13_USR].I = r_[13].I;
            r_[R14_USR].I = r_[14].I;
            r_[RN_SPSR].I = r_[RN_CPSR].I;
            break;
        case 0x11:
            swap(r_[R8_FIQ].I, r_[8].I);
            swap(r_[R9_FIQ].I, r_[9].I);
            swap(r_[R10_FIQ].I, r_[10].I);
            swap(r_[R11_FIQ].I, r_[11].I);
            swap(r_[R12_FIQ].I, r_[12].I);
            r_[R13_FIQ].I = r_[13].I;
            r_[R14_FIQ].I = r_[14].I;
            r_[SPSR_FIQ].I = r_[RN_SPSR].I;
            break;
        case 0x12:
            r_[R13_IRQ].I = r_[13].I;
            r_[R14_IRQ].I = r_[14].I;
            r_[SPSR_IRQ].I = r_[RN_SPSR].I;
            break;
        case 0x13:
            r_[R13_SVC].I = r_[13].I;
            r_[R14_SVC].I = r_[14].I;
            r_[SPSR_SVC].I = r_[RN_SPSR].I;
            break;
        case 0x17:
            r_[R13_ABT].I = r_[13].I;
            r_[R14_ABT].I = r_[14].I;
            r_[SPSR_ABT].I = r_[RN_SPSR].I;
            break;
        case 0x1B:
            r_[R13_UND].I = r_[13].I;
            r_[R14_UND].I = r_[14].I;
            r_[SPSR_UND].I = r_[RN_SPSR].I;
            break;
        default:
            break;
    }
    const std::uint32_t cpsr = r_[RN_CPSR].I;
    const std::uint32_t spsr = r_[RN_SPSR].I;
    switch (mode) {
        case 0x10:
        case 0x1F:
            r_[13].I = r_[R13_USR].I;
            r_[14].I = r_[R14_USR].I;
            r_[RN_CPSR].I = spsr;
            break;
        case 0x11:
            swap(r_[8].I, r_[R8_FIQ].I);
            swap(r_[9].I, r_[R9_FIQ].I);
            swap(r_[10].I, r_[R10_FIQ].I);
            swap(r_[11].I, r_[R11_FIQ].I);
            swap(r_[12].I, r_[R12_FIQ].I);
            r_[13].I = r_[R13_FIQ].I;
            r_[14].I = r_[R14_FIQ].I;
            r_[RN_SPSR].I = save_state ? cpsr : r_[SPSR_FIQ].I;
            break;
        case 0x12:
            r_[13].I = r_[R13_IRQ].I;
            r_[14].I = r_[R14_IRQ].I;
            r_[RN_CPSR].I = spsr;
            r_[RN_SPSR].I = save_state ? cpsr : r_[SPSR_IRQ].I;
            break;
        case 0x13:
            r_[13].I = r_[R13_SVC].I;
            r_[14].I = r_[R14_SVC].I;
            r_[RN_CPSR].I = spsr;
            r_[RN_SPSR].I = save_state ? cpsr : r_[SPSR_SVC].I;
            break;
        case 0x17:
            r_[13].I = r_[R13_ABT].I;
            r_[14].I = r_[R14_ABT].I;
            r_[RN_CPSR].I = spsr;
            r_[RN_SPSR].I = save_state ? cpsr : r_[SPSR_ABT].I;
            break;
        case 0x1B:
            r_[13].I = r_[R13_UND].I;
            r_[14].I = r_[R14_UND].I;
            r_[RN_CPSR].I = spsr;
            r_[RN_SPSR].I = save_state ? cpsr : r_[SPSR_UND].I;
            break;
        default:
            // An illegal mode leaves the processor in an unrecoverable state: hold it.
            enabled_ = false;
            break;
    }
    mode_ = mode;
    update_flags();
    update_cpsr();
}

// SWI and undefined-instruction traps happen after the fetch advanced R15_ARM_NEXT to the next
// instruction, which is exactly the return address (LR = pc + 4). Flycast's interpreter adds 4
// again here; its recompiler path, where R15_ARM_NEXT still holds the faulting pc, is the one
// normally exercised, so the table's convention is followed, not the interpreter glue's.
void Arm7::swi() {
    ++swis;
    const std::uint32_t pc = r_[R15_ARM_NEXT].I;
    switch_mode(0x13, true);
    r_[14].I = pc;
    irq_enable_ = false;
    r_[R15_ARM_NEXT].I = 0x08;
}

void Arm7::undefined() {
    ++undefined_ops;
    const std::uint32_t pc = r_[R15_ARM_NEXT].I;
    switch_mode(0x1B, true);
    r_[14].I = pc;
    irq_enable_ = false;
    r_[R15_ARM_NEXT].I = 0x04;
}

// FIQ is taken between instructions: R15_ARM_NEXT is the interrupted instruction, and the ARM
// convention (LR = that address + 4, returned to with `subs pc, lr, #4`) needs the +4 here.
void Arm7::fiq() {
    ++fiqs;
    ++fiqs_by_level[e68k_reg_l_ & 7u];
    const std::uint32_t pc = r_[R15_ARM_NEXT].I + 4;
    switch_mode(0x11, true);
    r_[14].I = pc;
    irq_enable_ = false;
    fiq_enable_ = false;
    update_intc();
    r_[R15_ARM_NEXT].I = 0x1C;
}

void Arm7::reset() {
    aica_interr_ = false;
    aica_reg_l_ = 0;
    e68k_out_ = false;
    e68k_reg_l_ = 0;
    e68k_reg_m_ = 0;
    enabled_ = false;
    std::memset(r_, 0, sizeof r_);
    mode_ = 0x13;
    r_[13].I = 0x03007F00u;
    r_[R13_IRQ].I = 0x03007FA0u;
    r_[R13_SVC].I = 0x03007FE0u;
    irq_enable_ = true;
    fiq_enable_ = false;
    n_flag_ = z_flag_ = c_flag_ = v_flag_ = false;
    update_intc();
    r_[RN_CPSR].I |= 0x40u;
    update_cpsr();
    r_[R15_ARM_NEXT].I = 0;
    r_[15].I = 4;
    ticks_ = 0;
}

void Arm7::set_enabled(bool on) {
    if (!enabled_ && on)
        reset();
    enabled_ = on;
}

void Arm7::interrupt_change(std::uint32_t pending_bits, std::uint32_t level) {
    aica_interr_ = pending_bits != 0;
    if (aica_interr_)
        aica_reg_l_ = level;
    if (!e68k_out_ && aica_interr_) {
        e68k_out_ = true;
        e68k_reg_l_ = aica_reg_l_;
        update_intc();
    }
}

void Arm7::accept_interrupt() {
    e68k_out_ = false;
    if (aica_interr_) {  // the next pending source latches immediately
        e68k_out_ = true;
        e68k_reg_l_ = aica_reg_l_;
    }
    update_intc();
}

namespace {
// DREAM_ARM7_WATCH=pc[,count]: print the registers the first `count` times the core is about to
// execute `pc` (development aid for reading a sound driver's behaviour).
struct Watch {
    std::uint32_t pc = 0xFFFFFFFFu;
    int left = 0;
    Watch() {
        if (const char* v = std::getenv("DREAM_ARM7_WATCH")) {
            pc = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 0));
            const char* comma = std::strchr(v, ',');
            left = comma ? std::atoi(comma + 1) : 8;
        }
    }
};
}  // namespace

void Arm7::run(int cycles) {
    if (!enabled_)
        return;
    static Watch watch;
    ticks_ -= cycles;
    while (ticks_ < 0 && enabled_) {
        if (r_[INTR_PEND].I)
            fiq();
        if (r_[R15_ARM_NEXT].I == watch.pc && watch.left > 0) {
            --watch.left;
            std::fprintf(stderr, "arm7 @%08x:", watch.pc);
            for (int i = 0; i < 15; ++i) std::fprintf(stderr, " r%d=%08x", i, r_[i].I);
            std::fprintf(stderr, " mode=%02x\n", mode_);
        }
        r_[15].I = r_[R15_ARM_NEXT].I + 8;
        ++instructions;
        step();
    }
}

// The decode table expects the VisualBoyAdvance names; map them onto this object.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wparentheses"
#pragma GCC diagnostic ignored "-Wunused-value"
#endif
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100 4127 4189 4244 4245 4267 4310 4389 4456 4457 4458 4459 4701 4702)
#endif
#define reg r_
#define armNextPC r_[R15_ARM_NEXT].I
#define armMode mode_
#define N_FLAG n_flag_
#define Z_FLAG z_flag_
#define C_FLAG c_flag_
#define V_FLAG v_flag_
#define CPUSwitchMode(m, s) switch_mode((m), (s))
#define CPUSoftwareInterrupt(c) swi()
#define CPUUndefinedException() undefined()
#define CPUUpdateFlags() update_flags()
#define CPUUpdateCPSR() update_cpsr()
#define CPUReadMemoryQuick(a) fetch(a)
#define CPUReadMemory(a) read<u32>(a)
#define CPUReadHalfWord(a) read<u16>(a)
#define CPUReadHalfWordSigned(a) static_cast<s16>(read<u16>(a))
#define CPUReadByte(a) read<u8>(a)
#define CPUWriteMemory(a, v) write<u32>((a), (v))
#define CPUWriteHalfWord(a, v) write<u16>((a), static_cast<u16>(v))
#define CPUWriteByte(a, v) write<u8>((a), static_cast<u8>(v))
#define CPUUpdateTicksAccessSeq32(a) 1
#define CPUUpdateTicksAccess32(a) 1
#define CPUUpdateTicksAccess16(a) 1
#define cpuBitsSet kBitsSet.t
#define verify(x) ((void)0)

void Arm7::step() {
    int& clockTicks = ticks_;
#include "arm7_ops.inc"
}

#undef reg
#undef armNextPC
#undef armMode
#undef N_FLAG
#undef Z_FLAG
#undef C_FLAG
#undef V_FLAG
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

}  // namespace dream::aica
