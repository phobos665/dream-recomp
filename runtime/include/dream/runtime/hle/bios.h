// BIOS syscall HLE (WP2.6): the vectors at 0x8C0000B0..E0 point at native functions registered
// in the function table, so a game's `jsr` through a vector lands here without any translated
// BIOS code. Semantics follow Flycast's reios (GPL-2.0, ADR 1) and the KallistiOS headers.
// GD-ROM reads complete on the virtual clock with Flycast's rate model (1.8 MB/s for large
// transfers), so games that stream from disc see plausible progress rather than instant data.
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "dream/runtime/gdrom/disc.h"
#include "dream/runtime/hle/flash.h"
#include "dream/runtime/system.h"

namespace dream::hle {

// Return values of the GD-ROM syscalls (gd_return_value in reios).
enum GdcStatus : std::int32_t {
    GDC_ERR = -1,
    GDC_OK = 0,
    GDC_BUSY = 1,
    GDC_COMPLETE = 2,
    GDC_CONTINUE = 3
};

class Bios {
public:
    static constexpr std::uint32_t kVecSystem = 0x8C0000B0, kVecFont = 0x8C0000B4,
                                   kVecFlash = 0x8C0000B8, kVecGd = 0x8C0000BC,
                                   kVecGd2 = 0x8C0000C0, kVecMisc = 0x8C0000E0;
    // Where the vectors point; native handlers are registered at these guest addresses.
    static constexpr std::uint32_t kHookSystem = 0x8C001000, kHookFont = 0x8C001002,
                                   kHookFlash = 0x8C001004, kHookGd = 0x8C001006,
                                   kHookMisc = 0x8C001008, kHookGd2 = 0x8C00100A;
    static constexpr std::uint32_t kSysInfoBlock = 0x8C000068;

    explicit Bios(System& sys);
    ~Bios();

    void attach_disc(gdrom::Disc* disc) noexcept { disc_ = disc; }
    // Formats the flash (if unformatted) and writes the vector table and handlers.
    void install(Language lang = Language::English);
    // Registers and memory as after the real BIOS handed over to 1ST_READ.BIN at boot_addr; loads
    // IP.BIN from the disc's high-density area to 0x8C008000 when a disc is attached.
    void setup_boot(std::uint32_t boot_addr);

    // Statistics for the run report.
    std::map<std::string, std::uint64_t> counts;
    std::uint64_t sectors_read = 0;

    // Syscall implementations (public for tests; the handlers call them).
    void sys_system(sh4::Ctx& c, ::dream::Memory& m);
    void sys_font(sh4::Ctx& c, ::dream::Memory& m);
    void sys_flashrom(sh4::Ctx& c, ::dream::Memory& m);
    void sys_gdrom(sh4::Ctx& c, ::dream::Memory& m);
    void sys_misc(sh4::Ctx& c, ::dream::Memory& m);

    Flash& flash() noexcept { return flash_; }

private:
    struct Gd {
        std::int32_t status = GDC_OK;
        std::uint32_t command = 0;
        std::uint32_t params[4] = {};
        std::uint32_t result[4] = {};
        std::uint32_t last_request = 0, next_request = 1;
        // DMA read in progress
        std::uint32_t read_sector = 0, read_remaining = 0, read_total = 0, read_dest = 0;
        // drive mode (REQ_MODE / SET_MODE)
        std::uint32_t speed = 0, standby = 0xE10, read_flags = 0x19, read_retry = 8;
        std::uint32_t callback = 0, callback_arg = 0;
        std::uint32_t sector_mode[4] = {};
    } gd_;

    void gd_request(sh4::Ctx& c, ::dream::Memory& m);
    void gd_exec(::dream::Memory& m);
    void gd_start_read();
    void gd_tick();
    std::uint64_t gd_ticks() const noexcept;
    void write_sector(::dream::Memory& m, std::uint32_t dest, const std::uint8_t* data);

    System& sys_;
    Flash flash_;
    gdrom::Disc* disc_ = nullptr;
    int gd_event_ = -1;
};

}  // namespace dream::hle
