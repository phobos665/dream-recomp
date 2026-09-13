// Maple bus (WP2.4, headless part): the DMA engine at 0x005F6C00 that walks the game's transfer
// descriptors, hands each frame to the device on that port, and writes the reply frames back
// into RAM on the virtual clock, plus a standard controller. Protocol and device responses follow
// Flycast's hw/maple (GPL-2.0, ADR 1) and the Katana Maple specification's frame layout.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dream/runtime/holly/intc.h"
#include "dream/runtime/mem/dc_memory.h"
#include "dream/runtime/sched/scheduler.h"

namespace dream::maple {

enum Command : std::uint8_t {
    kDeviceRequest = 0x01,
    kAllStatusReq = 0x02,
    kDeviceReset = 0x03,
    kDeviceKill = 0x04,
    kGetCondition = 0x09,
    kGetMediaInfo = 0x0A,
    kBlockRead = 0x0B,
    kBlockWrite = 0x0C,
    kGetLastError = 0x0D,
    kSetCondition = 0x0E,
};
enum Reply : std::uint8_t {
    kDeviceStatus = 0x05,
    kDeviceStatusAll = 0x06,
    kDeviceReply = 0x07,
    kDataTransfer = 0x08,
    kFileError = 0xFB,
    kTransmitAgain = 0xFC,
    kUnknownCommand = 0xFD,
    kUnknownFunction = 0xFE,
};
enum Function : std::uint32_t {
    kInput = 0x01000000,
    kStorage = 0x02000000,
    kLcd = 0x04000000,
    kClock = 0x08000000,
    kVibration = 0x00010000,
};

// Payload builder for reply frames (words after the header, little-endian bytes).
class Payload {
public:
    void u8(std::uint8_t v) { bytes_.push_back(v); }
    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v));
        u8(static_cast<std::uint8_t>(v >> 8));
    }
    void u32(std::uint32_t v) {
        u16(static_cast<std::uint16_t>(v));
        u16(static_cast<std::uint16_t>(v >> 16));
    }
    void str(const char* s, std::size_t field);  // space-padded fixed-width string
    const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
};

class Device {
public:
    virtual ~Device() = default;
    virtual std::uint32_t functions() const noexcept = 0;
    // Handles one frame: `args` are the words after the frame header. Returns the reply code and
    // fills `out` with the reply payload (a multiple of 4 bytes).
    virtual Reply handle(std::uint8_t command, const std::uint32_t* args, std::size_t nwords,
                         Payload& out) = 0;
};

// The standard controller. Buttons are active-low (1 = released) in Katana's order:
// C B A Start Up Down Left Right Z Y X D Up2 Down2 Left2 Right2 (bits 0..15).
struct ControllerState {
    std::uint16_t buttons = 0xFFFF;
    std::uint8_t rtrigger = 0, ltrigger = 0;
    std::uint8_t joy_x = 0x80, joy_y = 0x80, joy2_x = 0x80, joy2_y = 0x80;
};
enum Button : std::uint16_t {
    kC = 1 << 0,
    kB = 1 << 1,
    kA = 1 << 2,
    kStart = 1 << 3,
    kUp = 1 << 4,
    kDown = 1 << 5,
    kLeft = 1 << 6,
    kRight = 1 << 7,
    kZ = 1 << 8,
    kY = 1 << 9,
    kX = 1 << 10,
    kD = 1 << 11,
};

class Controller final : public Device {
public:
    ControllerState state;
    std::uint32_t functions() const noexcept override { return kInput; }
    Reply handle(std::uint8_t command, const std::uint32_t* args, std::size_t nwords,
                 Payload& out) override;
    std::uint64_t condition_reads = 0;
};

// A visual memory unit, as far as a game is concerned: 128 KB of flash in 256 blocks of 512
// bytes, read and written a block at a time, with a small screen and a clock the game may also
// address. Games use it to save; Crazy Taxi asks for 23 free blocks and refuses to record anything
// without one.
//
// The image is the same 128 KB layout every Dreamcast tool uses, so a save made in an emulator can
// be used directly. Writes go back to the file so progress survives a run. Never commit one: it is
// the owner's data (docs/owner-tasks.md).
class MemoryCard final : public Device {
public:
    static constexpr std::size_t kBlocks = 256, kBlockSize = 512;
    static constexpr std::size_t kImageSize = kBlocks * kBlockSize;

    // Loads an image. A missing file leaves the card unformatted rather than absent, which is what
    // a blank card does. Returns false only when the file exists but cannot be read.
    bool load(const std::string& path);
    // Writes the image back; called automatically after each block write when a path is set.
    bool save() const;

    std::uint32_t functions() const noexcept override { return kStorage | kLcd | kClock; }
    Reply handle(std::uint8_t command, const std::uint32_t* args, std::size_t nwords,
                 Payload& out) override;

    bool formatted() const noexcept;
    const std::vector<std::uint8_t>& image() const noexcept { return flash_; }
    std::uint64_t block_reads = 0, block_writes = 0, media_info_reads = 0, screen_writes = 0;

private:
    std::vector<std::uint8_t> flash_ = std::vector<std::uint8_t>(kImageSize, 0);
    std::string path_;
};

class Bus final : public mem::MmioHandler {
public:
    static constexpr std::uint32_t kBase = 0x005F6C00u, kEnd = 0x005F6D00u;
    static constexpr unsigned kPorts = 4;

    Bus(sched::Scheduler& sched, holly::Intc& holly, mem::DcMemory& memory);
    void reset();
    void attach(unsigned port, std::unique_ptr<Device> device);
    // Each controller port carries up to five expansion slots, addressed by one bit each, where
    // memory cards and rumble packs live. Slot 0 is where a memory card normally sits.
    static constexpr unsigned kExpansionSlots = 5;
    void attach_expansion(unsigned port, unsigned slot, std::unique_ptr<Device> device);
    // The device a frame's recipient byte names: bit 5 is the main device, bits 0 to 4 the slots.
    Device* device_for(unsigned port, std::uint8_t recipient) const noexcept;
    Device* device(unsigned port) const noexcept { return ports_[port].get(); }

    // Called by the SPG at VBlank-out: starts the transfer when the hardware trigger is selected.
    void vblank();

    std::uint32_t read(std::uint32_t addr, unsigned size) override;
    void write(std::uint32_t addr, std::uint32_t value, unsigned size) override;

    // Registers
    std::uint32_t mdstar = 0, mdtsel = 0, mden = 0, mdst = 0, msys = 0x3A980000, mshtcl = 0,
                  mdapro = 0x00007F00, mmsel = 1;
    // Diagnostics
    std::uint64_t transfers = 0, frames = 0, no_device = 0;

private:
    void start_dma();
    void complete();
    struct Pending {
        std::uint32_t addr;
        std::vector<std::uint32_t> words;
    };
    sched::Scheduler& sched_;
    holly::Intc& holly_;
    mem::DcMemory& memory_;
    std::array<std::unique_ptr<Device>, kPorts> ports_;
    std::array<std::array<std::unique_ptr<Device>, kExpansionSlots>, kPorts> expansion_;

public:
    // Frames seen per (recipient byte, command), for bring-up: which addresses a title probes and
    // what it asks them.
    std::array<std::uint64_t, 256> frames_by_recipient{};
    std::array<std::uint64_t, 256> frames_by_command{};

private:
    std::vector<Pending> out_;
    int event_ = -1;
    bool trigger_pending_reset_ = false;
};

}  // namespace dream::maple
