// Dreamcast real-time clock (in the AICA block at 0x00710000): a 32-bit count of seconds since
// 1950-01-01 00:00 read as two 16-bit halves, writable after the enable register is set, as in
// Flycast's aica_if.cpp (GPL-2.0, ADR 1). The owner of the device advances it once per guest second
// with tick() so titles see time pass on the virtual clock (deterministic across runs).
#pragma once

#include <cstdint>

#include "dream/runtime/mem/dc_memory.h"

namespace dream::aica {

class Rtc final : public mem::MmioHandler {
public:
    static constexpr std::uint32_t kBase = 0x00710000u, kEnd = 0x00710100u;
    static constexpr std::uint32_t kSecondsHi = 0x0, kSecondsLo = 0x4, kEnable = 0x8;
    // Seconds between the Dreamcast epoch (1950-01-01) and the Unix epoch (1970-01-01).
    static constexpr std::uint32_t kUnixEpochOffset = (20u * 365u + 5u) * 24u * 60u * 60u;

    explicit Rtc(std::uint32_t seconds) noexcept : seconds_(seconds) {}

    std::uint32_t seconds() const noexcept { return seconds_; }
    void tick() noexcept { ++seconds_; }

    // Save states (state/state.h): this device's registers and internal state.
    void save_state(state::Writer& w) override;
    void load_state(state::Reader& r) override;
    std::uint32_t read(std::uint32_t addr, unsigned) override {
        switch (addr & 0xFFu) {
            case kSecondsHi:
                return (seconds_ >> 16) & 0xFFFFu;
            case kSecondsLo:
                return seconds_ & 0xFFFFu;
            case kEnable:
                return enable_;
            default:
                return 0;
        }
    }
    void write(std::uint32_t addr, std::uint32_t v, unsigned) override {
        switch (addr & 0xFFu) {
            case kSecondsHi:
                if (enable_) {
                    seconds_ = (seconds_ & 0xFFFFu) | ((v & 0xFFFFu) << 16);
                    enable_ = 0;  // the high half is the last write of a set; it re-locks
                }
                break;
            case kSecondsLo:
                if (enable_)
                    seconds_ = (seconds_ & 0xFFFF0000u) | (v & 0xFFFFu);
                break;
            case kEnable:
                enable_ = v & 1u;
                break;
            default:
                break;
        }
    }

private:
    std::uint32_t seconds_;
    std::uint32_t enable_ = 0;
};

}  // namespace dream::aica
