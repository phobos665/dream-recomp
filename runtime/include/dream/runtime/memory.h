// Guest memory interface used by emitted code (docs/emitter-design.md).
//
// Emitted functions receive a Memory& and call these accessors with guest virtual addresses.
// The full Dreamcast map (RAM mirrors, VRAM views, MMIO dispatch, store queues) is WP2.1; the
// BareMemory implementation below is the harness's RAM-only substitute and traps on anything else.
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace dream {

class Memory {
public:
    virtual ~Memory() = default;
    virtual std::uint8_t read8(std::uint32_t addr) = 0;
    virtual std::uint16_t read16(std::uint32_t addr) = 0;
    virtual std::uint32_t read32(std::uint32_t addr) = 0;
    virtual std::uint64_t read64(std::uint32_t addr) = 0;
    virtual void write8(std::uint32_t addr, std::uint8_t v) = 0;
    virtual void write16(std::uint32_t addr, std::uint16_t v) = 0;
    virtual void write32(std::uint32_t addr, std::uint32_t v) = 0;
    virtual void write64(std::uint32_t addr, std::uint64_t v) = 0;
    // Store queue: writes to 0xE0000000..0xE3FFFFFF buffer in the SQ; PREF flushes 32 bytes.
    virtual void sq_write32(std::uint32_t addr, std::uint32_t v) = 0;
    virtual void sq_flush(std::uint32_t addr) = 0;
};

// 16 MB of main RAM at every alias of area 3 (0x0C000000 physical), nothing else. Accesses outside
// RAM throw, which is what the harness wants: a translated test program touching MMIO is a bug.
class BareMemory final : public Memory {
public:
    static constexpr std::uint32_t kRamSize = 16u * 1024 * 1024;

    BareMemory() : ram_(new std::uint8_t[kRamSize]()) {}

    std::uint8_t* ram() noexcept { return ram_.get(); }

    std::uint8_t read8(std::uint32_t a) override { return ram_[idx(a)]; }
    std::uint16_t read16(std::uint32_t a) override {
        std::uint16_t v;
        std::memcpy(&v, ram_.get() + idx(a), 2);
        return v;
    }
    std::uint32_t read32(std::uint32_t a) override {
        std::uint32_t v;
        std::memcpy(&v, ram_.get() + idx(a), 4);
        return v;
    }
    std::uint64_t read64(std::uint32_t a) override {
        std::uint64_t v;
        std::memcpy(&v, ram_.get() + idx(a), 8);
        return v;
    }
    void write8(std::uint32_t a, std::uint8_t v) override { ram_[idx(a)] = v; }
    void write16(std::uint32_t a, std::uint16_t v) override {
        std::memcpy(ram_.get() + idx(a), &v, 2);
    }
    void write32(std::uint32_t a, std::uint32_t v) override {
        std::memcpy(ram_.get() + idx(a), &v, 4);
    }
    void write64(std::uint32_t a, std::uint64_t v) override {
        std::memcpy(ram_.get() + idx(a), &v, 8);
    }
    void sq_write32(std::uint32_t a, std::uint32_t v) override { sq_[(a >> 2) & 15] = v; }
    void sq_flush(std::uint32_t) override {}  // no QACR mapping in the bare harness

private:
    std::uint32_t idx(std::uint32_t a) const {
        // Any P0/P1/P2/P3 alias of area 3 lands in RAM; RAM itself is mirrored across the 64 MB
        // area.
        if (((a & 0x1C000000u) != 0x0C000000u)) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "BareMemory: access outside RAM at 0x%08x", a);
            throw std::out_of_range(buf);
        }
        return a & (kRamSize - 1);
    }

    std::unique_ptr<std::uint8_t[]> ram_;
    std::uint32_t sq_[16]{};
};

}  // namespace dream
