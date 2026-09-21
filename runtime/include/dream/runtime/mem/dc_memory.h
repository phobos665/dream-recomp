// The Dreamcast address space (WP2.1, docs/runtime-memory.md): main RAM with its mirrors, VRAM in
// both views, sound RAM, flash, boot ROM, on-chip RAM, the store queues with PREF flush through
// QACR, and an MMIO dispatch table the hardware cores of later work packages register into.
// Unhandled accesses go to a fault log instead of aborting, so a title keeps running while the
// log shows what it touched.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "dream/runtime/memory.h"

namespace dream::mem {

// A device on the bus. Sizes are 1, 2 or 4; 64-bit accesses arrive as two 32-bit ones.
class MmioHandler {
public:
    virtual ~MmioHandler() = default;
    virtual std::uint32_t read(std::uint32_t addr, unsigned size) = 0;
    virtual void write(std::uint32_t addr, std::uint32_t value, unsigned size) = 0;
    // A store-queue flush: 32 bytes as eight words. Default splits into word writes.
    virtual void write_burst(std::uint32_t addr, const std::uint32_t* words) {
        for (unsigned i = 0; i < 8; ++i) write(addr + 4 * i, words[i], 4);
    }
};

// A block of registers with no behaviour yet: reads return what was written. Used for on-chip
// modules a booting title programs but the runtime does not model (DMAC, BSC, UBC, CPG/WDT).
class RegisterFile final : public MmioHandler {
public:
    RegisterFile(std::uint32_t base, std::uint32_t words) : base_(base), regs_(words, 0) {}
    std::uint32_t read(std::uint32_t addr, unsigned) override {
        const std::uint32_t i = (addr - base_) >> 2;
        return i < regs_.size() ? regs_[i] : 0;
    }
    void write(std::uint32_t addr, std::uint32_t value, unsigned size) override {
        const std::uint32_t i = (addr - base_) >> 2;
        if (i >= regs_.size())
            return;
        if (size == 4) {
            regs_[i] = value;
        } else {  // merge a byte or halfword into its word (little-endian)
            const unsigned shift = (addr & 3u) * 8;
            const std::uint32_t mask = (size == 1 ? 0xFFu : 0xFFFFu) << shift;
            regs_[i] = (regs_[i] & ~mask) | ((value << shift) & mask);
        }
    }
    std::uint32_t& word(std::uint32_t offset) { return regs_[offset >> 2]; }

private:
    std::uint32_t base_;
    std::vector<std::uint32_t> regs_;
};

// A region whose writes are dropped and whose reads return zero: the instruction and operand
// cache address and data arrays at 0xF0000000..0xF7FFFFFF, which cache-maintenance loops write
// and nothing needs to model.
class NullRegion final : public MmioHandler {
public:
    std::uint32_t read(std::uint32_t, unsigned) override { return 0; }
    void write(std::uint32_t, std::uint32_t, unsigned) override {}
};

struct FaultRecord {
    std::uint32_t addr;
    unsigned size;
    bool write;
    std::uint64_t count;
};

// Unique (address, size, direction) triples with counts; bounded so a runaway loop cannot eat
// memory. This is the runtime half of the unreached-address report (docs/emitter-design.md).
class FaultLog {
public:
    static constexpr std::size_t kMaxUnique = 4096;
    void record(std::uint32_t addr, unsigned size, bool write);
    std::uint64_t total() const noexcept { return total_; }
    std::vector<FaultRecord> records() const;  // sorted by address
    std::string format() const;
    void clear();

private:
    std::unordered_map<std::uint64_t, FaultRecord> map_;
    std::uint64_t total_ = 0, dropped_ = 0;
};

class DcMemory final : public ::dream::Memory {
public:
    static constexpr std::uint32_t kRamSize = 16u << 20;
    static constexpr std::uint32_t kVramSize = 8u << 20;
    static constexpr std::uint32_t kAramSize = 2u << 20;
    static constexpr std::uint32_t kBiosSize = 2u << 20;
    static constexpr std::uint32_t kFlashSize = 128u << 10;
    static constexpr std::uint32_t kOcramSize = 8u << 10;

    DcMemory();

    std::uint8_t* ram() noexcept { return ram_.get(); }
    std::uint8_t* vram() noexcept { return vram_.get(); }
    std::uint8_t* aram() noexcept { return aram_.get(); }
    std::uint8_t* bios() noexcept { return bios_.get(); }
    std::uint8_t* flash() noexcept { return flash_.get(); }
    FaultLog& faults() noexcept { return faults_; }
    // Called before every device (MMIO) access: the runtime advances the virtual clock to the
    // guest's cycle count here, so a timer or drive status read sees the present rather than the
    // last interrupt poll (Katana's syTmrGetCount busy-waits on TMU0 between polls).
    std::function<void()> on_device_access;
    // Development write watch: stores that touch [watch_lo, watch_hi) call on_watch_write with
    // the guest address and value (the launcher's DREAM_WATCH_WRITE).
    std::uint32_t watch_lo = 1, watch_hi = 0;
    std::function<void(std::uint32_t addr, std::uint32_t value, unsigned size)> on_watch_write;

    // Development aid: a rolling hash of every guest store. Two runs of the same program that
    // agree on this did the same thing, whether one was translated and the other interpreted, so
    // comparing it frame by frame finds where a translated run first goes wrong
    // (docs/differential-harness.md). Off by default, and one predictable branch when on.
    bool hash_writes = false;
    std::uint64_t write_hash = 0, writes_hashed = 0;
    // Once the hashes say which frame diverged, the individual writes in a window around it say
    // which one: [log_from, log_to) are reported with their index, so two runs can be diffed line
    // for line down to the store that differs.
    std::uint64_t log_from = ~0ull, log_to = 0;
    std::function<void(std::uint64_t index, std::uint32_t addr, std::uint64_t value, unsigned size)>
        on_hashed_write;
    // A stored pointer carries the segment the code was running in, and the same guest function
    // reached through P1 and through P2 pushes a different return address on real hardware. A
    // statically translated build cannot reproduce that, and it is not a bug: the runtime masks
    // the segment off every access anyway. Masking the top three bits of a 32-bit value removes
    // the whole class of false difference, at the cost of not seeing a difference that lives only
    // in those bits.
    bool hash_mask_segment = false;

    // A journal of guest stores that can be undone, so the same guest function can be run twice
    // from the same memory: once as translated code and once through the interpreter, and the two
    // compared (docs/differential-harness.md). Device writes cannot be undone and must not be
    // repeated, so one sets `journal_saw_device` and the comparison is abandoned for that call.
    struct WriteRecord {
        std::uint32_t addr;
        std::uint64_t old_value, new_value;
        unsigned size;
    };
    bool journaling = false;
    bool journal_saw_device = false;
    std::vector<WriteRecord> journal;
    // Puts back what the journalled stores overwrote, newest first, and clears the journal.
    void undo_journal();
    // Applies the journal again, oldest first, for putting a rolled-back run back as it was.
    void redo_journal();
    void note_write(std::uint32_t addr, std::uint64_t value, unsigned size) noexcept {
        constexpr std::uint64_t kPrime = 0x100000001B3ull;
        if (hash_mask_segment && size == 4)
            value &= 0x1FFFFFFFull;
        if (writes_hashed >= log_from && writes_hashed < log_to && on_hashed_write)
            on_hashed_write(writes_hashed, addr, value, size);
        write_hash = (write_hash ^ (addr & 0x1FFFFFFFu)) * kPrime;
        write_hash = (write_hash ^ value) * kPrime;
        write_hash = (write_hash ^ size) * kPrime;
        ++writes_hashed;
    }
    // True when either the hash or the watch is armed, so the cold paths can skip the per-word
    // loop entirely.
    bool tracing_writes() const noexcept {
        return hash_writes || (on_watch_write && watch_lo < watch_hi);
    }
    // Every path that writes guest RAM reports through here, not just DcMemory::store. The hash
    // and the watch were originally wired into the store path alone, which left store-queue
    // bursts invisible to both: a run could differ from another in 32 bytes at a time and the
    // write hash would call the two identical. Anything that memcpys into `t.bytes` must call
    // this (docs/write-coverage.md).
    void note_ram_write(std::uint32_t addr, std::uint64_t value, unsigned size) noexcept {
        if (hash_writes)
            note_write(addr, value, size);
        if ((addr & 0x1FFFFFFFu) >= watch_lo && (addr & 0x1FFFFFFFu) < watch_hi && on_watch_write)
            on_watch_write(addr, static_cast<std::uint32_t>(value), size);
    }

    // Development aid: an access to an address that is in no region at all, called before it is
    // recorded in the fault log. The *first* one is where a run started going wrong; every access
    // after it is the guest following whatever garbage the first one returned, so catching it here
    // rather than reading the fault log afterwards is the difference between seeing the cause and
    // seeing the millionth symptom.
    std::function<void(std::uint32_t addr, unsigned size, bool write)> on_unmapped;

    // Devices. Physical ranges are [lo, hi) within the 512 MB physical space (area 0 registers,
    // area 4 TA FIFO); P4 ranges are virtual addresses at 0xE4000000 and above (on-chip modules).
    void map_mmio(std::uint32_t phys_lo, std::uint32_t phys_hi, MmioHandler* h);
    void map_p4(std::uint32_t lo, std::uint32_t hi, MmioHandler* h);

    // CCN exception registers the interrupt model writes and handlers read back through memory:
    // TRA 0xFF000020, EXPEVT 0xFF000024, INTEVT 0xFF000028.
    std::uint32_t tra = 0, expevt = 0, intevt = 0;

    // Store-queue area control registers (CCN QACR0/1 at 0xFF000038/0xFF00003C); bits 4:2 select
    // the 64 MB area the flushed data lands in.
    void set_qacr(unsigned n, std::uint32_t v) noexcept { qacr_[n & 1] = v & 0x1Cu; }
    std::uint32_t qacr(unsigned n) const noexcept { return qacr_[n & 1]; }

    // Memory interface
    std::uint8_t read8(std::uint32_t a) override;
    std::uint16_t read16(std::uint32_t a) override;
    std::uint32_t read32(std::uint32_t a) override;
    std::uint64_t read64(std::uint32_t a) override;
    void write8(std::uint32_t a, std::uint8_t v) override;
    void write16(std::uint32_t a, std::uint16_t v) override;
    void write32(std::uint32_t a, std::uint32_t v) override;
    void write64(std::uint32_t a, std::uint64_t v) override;
    void sq_write32(std::uint32_t a, std::uint32_t v) override;
    void sq_flush(std::uint32_t a) override;

    // The 32-bit VRAM view interleaves the two 4 MB banks word by word onto the 64-bit view.
    static std::uint32_t vram_map32(std::uint32_t offset32) noexcept;

private:
    struct Target {
        enum Kind { kNone, kBytes, kMmio } kind = kNone;
        std::uint8_t* bytes = nullptr;  // kBytes: host pointer to the first byte
        MmioHandler* mmio = nullptr;    // kMmio
        std::uint32_t mmio_addr = 0;    // address handed to the handler
    };
    Target resolve(std::uint32_t a, unsigned size, bool write);
    template <typename T>
    T load(std::uint32_t a);
    template <typename T>
    void store(std::uint32_t a, T v);

    struct Range {
        std::uint32_t lo, hi;
        MmioHandler* h;
    };
    MmioHandler* find(const std::vector<Range>& t, std::uint32_t a) const noexcept;

    std::unique_ptr<std::uint8_t[]> ram_, vram_, aram_, bios_, flash_, ocram_;
    std::vector<Range> phys_mmio_, p4_mmio_;
    std::uint32_t sq_[2][8]{};
    std::uint32_t qacr_[2]{};
    std::uint32_t ccn_[16]{};  // PTEH..QACR1 word registers at 0xFF000000 + 4n (CCR, TRA, EXPEVT,
                               // INTEVT, QACR aliased)
    FaultLog faults_;
};

}  // namespace dream::mem
