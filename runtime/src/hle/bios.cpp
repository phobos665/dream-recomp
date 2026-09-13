#include "dream/runtime/hle/bios.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dream/runtime/sh4/abi.h"
#include "dream/runtime/sh4/ops.h"

namespace dream::hle {

// Guest time a syscall costs. The real BIOS entry runs a few hundred instructions before it
// answers; games calibrate their polling loops against that (Crazy Taxi retries a GD-ROM status
// check 120 times, which at zero cost per call expires before a 4096-cycle sector read lands).
constexpr std::uint64_t kSyscallCycles = 100, kGdSyscallCycles = 300;

namespace {

Bios* g_bios = nullptr;  // the handlers below are plain functions in the guest function table

void hook_system(sh4::Ctx& c, ::dream::Memory& m) {
    g_bios->sys_system(c, m);
}
void hook_font(sh4::Ctx& c, ::dream::Memory& m) {
    g_bios->sys_font(c, m);
}
void hook_flash(sh4::Ctx& c, ::dream::Memory& m) {
    g_bios->sys_flashrom(c, m);
}
void hook_gd(sh4::Ctx& c, ::dream::Memory& m) {
    g_bios->sys_gdrom(c, m);
}
void hook_misc(sh4::Ctx& c, ::dream::Memory& m) {
    g_bios->sys_misc(c, m);
}

const sh4::FunctionEntry kHooks[] = {
    {Bios::kHookSystem, hook_system}, {Bios::kHookFont, hook_font}, {Bios::kHookFlash, hook_flash},
    {Bios::kHookGd, hook_gd},         {Bios::kHookMisc, hook_misc}, {Bios::kHookGd2, hook_gd},
};

// GD-ROM command codes (KallistiOS dc/cdrom.h, reios gdrom_hle.h)
enum : std::uint32_t {
    CMD_PIOREAD = 0x10,
    CMD_DMAREAD = 0x11,
    CMD_GETTOC = 0x12,
    CMD_GETTOC2 = 0x13,
    CMD_PLAY = 0x14,
    CMD_PLAY2 = 0x15,
    CMD_PAUSE = 0x16,
    CMD_RELEASE = 0x17,
    CMD_INIT = 0x18,
    CMD_READABORT = 0x19,
    CMD_OPEN = 0x1A,
    CMD_SEEK = 0x1B,
    CMD_DMA_READ_REQ = 0x1C,
    CMD_GETQINFO = 0x1D,
    CMD_REQ_MODE = 0x1E,
    CMD_SET_MODE = 0x1F,
    CMD_SCAN = 0x20,
    CMD_STOP = 0x21,
    CMD_GETSCD = 0x22,
    CMD_REQ_SES = 0x23,
    CMD_REQ_STAT = 0x24,
    CMD_PIOREADREQ = 0x25,
    CMD_MULTI_DMAREAD = 0x26,
    CMD_MULTI_PIOREAD = 0x27,
    CMD_GET_VERSION = 0x28,
};
// r7 of the GD-ROM vector (r6 == 0)
enum : std::uint32_t {
    FN_REQ_CMD = 0,
    FN_GET_CMD_STAT,
    FN_EXEC_SERVER,
    FN_INIT_SYSTEM,
    FN_GET_DRV_STAT,
    FN_G1_DMA_END,
    FN_REQ_DMA_TRANS,
    FN_CHECK_DMA_TRANS,
    FN_READ_ABORT,
    FN_RESET,
    FN_CHANGE_DATA_TYPE,
    FN_SET_PIO_CALLBACK,
    FN_REQ_PIO_TRANS,
    FN_CHECK_PIO_TRANS,
};
enum : std::uint32_t { DRV_BUSY = 0, DRV_PAUSE = 1, DRV_STANDBY = 2, DRV_PLAY = 3, DRV_NODISC = 7 };

}  // namespace

Bios::Bios(System& sys) : sys_(sys), flash_(sys.memory.flash()) {
    gd_event_ = sys_.sched.add("gdrom-hle", [this](std::uint64_t, std::uint64_t) { gd_tick(); });
}

Bios::~Bios() {
    if (g_bios == this)
        g_bios = nullptr;
}

void Bios::install(Language lang) {
    g_bios = this;
    static bool registered = false;
    if (!registered) {
        sh4::register_functions(kHooks, sizeof kHooks / sizeof kHooks[0]);
        registered = true;
    }
    if (std::memcmp(sys_.memory.flash() + 0x1A005, "Dreamcast  ", 11) != 0)
        flash_.format(lang);
    auto& m = sys_.memory;
    m.write32(kVecSystem, kHookSystem);
    m.write32(kVecFont, kHookFont);
    m.write32(kVecFlash, kHookFlash);
    m.write32(kVecGd, kHookGd);
    m.write32(kVecGd2, kHookGd2);
    m.write32(kVecMisc, kHookMisc);
    gd_ = Gd{};
}

void Bios::setup_boot(std::uint32_t boot_addr) {
    auto& c = sys_.ctx;
    auto& m = sys_.memory;
    // The BIOS work area as the real one leaves it (Flycast reios_setup_state): erased, then the
    // vectors, then the SYSINFO block.
    std::memset(m.ram(), 0xFF, 0x10000);
    install();
    sh4::Ctx fresh{};
    c = fresh;
    c.r[0] = 0xAC0005D8;
    c.r[1] = 0x00000009;
    c.r[2] = 0xAC00940C;
    c.r[4] = 0xAC008300;
    c.r[5] = 0xF4000000;
    c.r[6] = 0xF4002000;
    c.r[7] = 0x00000070;
    c.r[15] = 0x8D000000;
    c.gbr = 0x8C000000;
    c.vbr = 0x8C000000;
    c.dbr = 0x8C000010;
    c.ssr = 0x40000001;
    c.spc = 0x8C000776;
    c.sgr = 0x8D000000;
    c.pr = 0xAC00043C;
    c.pc = boot_addr;
    sh4::write_sr(c, 0x400000F1);  // MD, IMASK 15, T
    sh4::write_fpscr(c, 0x00040001);
    c.next_event = 0;
    // SYSINFO_INIT's block, so games that skip the call still find the id.
    sh4::Ctx tmp = c;
    tmp.r[7] = 0;
    sys_system(tmp, m);
    // IP.BIN: 16 sectors from the start of the game area to 0x8C008000.
    if (disc_) {
        if (const gdrom::Track* hd = disc_->hd_track()) {
            std::uint8_t sector[2048];
            for (std::uint32_t i = 0; i < 16; ++i) {
                if (!disc_->read_user(hd->lba + i, sector))
                    break;
                write_sector(m, 0x8C008000 + i * 2048, sector);
            }
        }
    }
}

void Bios::write_sector(::dream::Memory& m, std::uint32_t dest, const std::uint8_t* data) {
    if ((dest & 0x1C000000u) == 0x0C000000u) {
        std::memcpy(sys_.memory.ram() + (dest & (mem::DcMemory::kRamSize - 1)), data, 2048);
        return;
    }
    for (std::uint32_t i = 0; i < 2048; i += 4) {
        std::uint32_t w;
        std::memcpy(&w, data + i, 4);
        m.write32(dest + i, w);
    }
}

// ---- SYSINFO (0x8C0000B0): r7 = function --------------------------------------------------------

void Bios::sys_system(sh4::Ctx& c, ::dream::Memory& m) {
    c.cycles += kSyscallCycles;
    ++counts["sysinfo"];
    switch (c.r[7]) {
        case 0: {  // SYSINFO_INIT: id (8 bytes at flash 0x1A056) + properties (5 at 0x1A000)
            std::uint8_t data[24] = {};
            for (unsigned i = 0; i < 8; ++i) data[i] = flash_.read8(0x1A056 + i);
            for (unsigned i = 0; i < 5; ++i) data[8 + i] = flash_.read8(0x1A000 + i);
            for (unsigned i = 0; i < 24; ++i) m.write8(kSysInfoBlock + i, data[i]);
            c.r[0] = 0;
            break;
        }
        case 2:  // SYSINFO_ICON: r4 icon number, r5 704-byte buffer
            c.r[0] = c.r[4] > 9 ? 0xFFFFFFFFu : 704u;
            break;
        case 3:  // SYSINFO_ID
            c.r[0] = kSysInfoBlock;
            break;
        default:
            c.r[0] = 0xFFFFFFFFu;
            ++counts["sysinfo.unknown"];
            break;
    }
}

// ---- ROMFONT (0x8C0000B4): r1 = function --------------------------------------------------------

void Bios::sys_font(sh4::Ctx& c, ::dream::Memory&) {
    c.cycles += kSyscallCycles;
    ++counts["font"];
    switch (c.r[1]) {
        case 0:
            c.r[0] = 0xA0100020u;
            break;  // FONTROM_ADDRESS: in the (empty) boot ROM area
        case 1:
        case 2:
            c.r[0] = 0;
            break;  // LOCK / UNLOCK
        default:
            c.r[0] = 0xFFFFFFFFu;
            break;
    }
}

// ---- FLASHROM (0x8C0000B8): r7 = function
// --------------------------------------------------------

void Bios::sys_flashrom(sh4::Ctx& c, ::dream::Memory& m) {
    c.cycles += kSyscallCycles;
    ++counts["flashrom"];
    switch (c.r[7]) {
        case 0: {  // FLASHROM_INFO: r4 partition, r5 -> {offset, size}
            std::uint32_t off = 0, size = 0;
            if (Flash::partition(c.r[4], off, size)) {
                m.write32(c.r[5], off);
                m.write32(c.r[5] + 4, size);
                c.r[0] = 0;
            } else {
                c.r[0] = 0xFFFFFFFFu;
            }
            break;
        }
        case 1: {  // FLASHROM_READ: r4 offset, r5 dest, r6 size
            for (std::uint32_t i = 0; i < c.r[6]; ++i)
                m.write8(c.r[5] + i, flash_.read8(c.r[4] + i));
            c.r[0] = 0;
            break;
        }
        case 2: {  // FLASHROM_WRITE: r4 offset, r5 src, r6 size; returns bytes written
            for (std::uint32_t i = 0; i < c.r[6]; ++i)
                flash_.program8(c.r[4] + i, m.read8(c.r[5] + i));
            c.r[0] = c.r[6];
            break;
        }
        case 3: {  // FLASHROM_DELETE: r4 offset inside the partition to erase
            std::uint32_t off = 0, size = 0;
            c.r[0] = 0xFFFFFFFFu;
            for (unsigned p = 0; p < Flash::Count; ++p) {
                if (Flash::partition(p, off, size) && c.r[4] >= off && c.r[4] < off + size) {
                    flash_.erase_partition(p);
                    c.r[0] = 0;
                }
            }
            break;
        }
        default:
            c.r[0] = 0xFFFFFFFFu;
            ++counts["flashrom.unknown"];
            break;
    }
}

// ---- GD-ROM and misc (0x8C0000BC / C0): r6 = 0 GD-ROM (r7 function), r6 = -1 misc ---------------

std::uint64_t Bios::gd_ticks() const noexcept {
    // Flycast's rate model: large transfers at the GD-ROM's 1.8 MB/s (a 5-sector batch per
    // million cycles), small ones at the G1 bus rate.
    const std::uint64_t len = static_cast<std::uint64_t>(gd_.read_remaining) * 2048;
    return len > 10240 ? 1'000'000 : std::max<std::uint64_t>(len * 2, 1000);
}

void Bios::gd_start_read() {
    gd_.read_sector = (gd_.params[0] & 0xFFFFFFu) - gdrom::kFadOffset;
    gd_.read_remaining = gd_.read_total = gd_.params[1];
    gd_.read_dest = gd_.params[2];
    gd_.result[2] = gd_.result[3] = 0;
    sys_.sched.request(gd_event_, gd_ticks());
}

void Bios::gd_tick() {
    if (gd_.status != GDC_BUSY || gd_.read_remaining == 0)
        return;
    const std::uint32_t batch = std::min<std::uint32_t>(gd_.read_remaining, 5);
    std::uint8_t sector[2048];
    for (std::uint32_t i = 0; i < batch; ++i) {
        if (disc_ && disc_->read_user(gd_.read_sector, sector))
            write_sector(sys_.memory, gd_.read_dest, sector);
        else
            ++counts["gdrom.read_miss"];
        ++gd_.read_sector;
        gd_.read_dest += 2048;
        ++sectors_read;
    }
    gd_.read_remaining -= batch;
    gd_.result[2] = (gd_.read_total - gd_.read_remaining) * 2048;
    if (gd_.read_remaining == 0) {
        gd_.status = GDC_COMPLETE;
        // The real BIOS raises the G1 DMA-end interrupt, which a title can hook through
        // FN_G1_DMA_END; Crazy Taxi behaves the same either way (measured 2026-09-12).
        sys_.holly.raise(holly::Irq::GdromDma);
    } else {
        sys_.sched.request(gd_event_, gd_ticks());
    }
}

void Bios::gd_exec(::dream::Memory& m) {
    switch (gd_.command) {
        case CMD_INIT:
            gd_.callback = 0;
            gd_.read_remaining = 0;
            gd_.status = GDC_COMPLETE;
            break;
        case CMD_DMAREAD:
        case CMD_PIOREAD:
            if (gd_.read_remaining == 0 && gd_.read_total == 0)
                gd_start_read();
            return;          // completes from the scheduler
        case CMD_GETTOC2: {  // params: area, dest
            std::uint32_t toc[102];
            if (disc_)
                disc_->toc(gd_.params[0], toc);
            else
                for (auto& w : toc) w = 0xFFFFFFFFu;
            for (unsigned i = 0; i < 102; ++i) m.write32(gd_.params[1] + 4 * i, toc[i]);
            gd_.result[2] = 102 * 4;
            gd_.status = GDC_COMPLETE;
            break;
        }
        case CMD_REQ_SES: {  // params: session, dest (6 bytes: status, 0, first track, start FAD
                             // 24-bit)
            const std::uint32_t dest = gd_.params[1];
            std::uint32_t first = 1, fad = gdrom::kFadOffset;
            if (disc_ && gd_.params[0] == 2 && disc_->tracks().size() >= 3) {
                first = 3;
                fad = disc_->tracks()[2].lba + gdrom::kFadOffset;
            } else if (disc_ && gd_.params[0] == 0) {
                first = static_cast<std::uint32_t>(disc_->tracks().size());
                fad = disc_->leadout_lba() + gdrom::kFadOffset;
            }
            m.write8(dest, DRV_PAUSE);
            m.write8(dest + 1, 0);
            m.write8(dest + 2, static_cast<std::uint8_t>(first));
            m.write8(dest + 3, static_cast<std::uint8_t>(fad >> 16));
            m.write8(dest + 4, static_cast<std::uint8_t>(fad >> 8));
            m.write8(dest + 5, static_cast<std::uint8_t>(fad));
            gd_.result[2] = 6;
            gd_.status = GDC_COMPLETE;
            break;
        }
        case CMD_REQ_MODE: {  // params: dest -> {speed, standby, read_flags, read_retry}
            const std::uint32_t dest = gd_.params[0];
            m.write32(dest, gd_.speed);
            m.write32(dest + 4, gd_.standby);
            m.write32(dest + 8, gd_.read_flags);
            m.write32(dest + 12, gd_.read_retry);
            gd_.result[2] = 0xA;
            gd_.status = GDC_COMPLETE;
            break;
        }
        case CMD_SET_MODE:
            gd_.speed = gd_.params[0];
            gd_.standby = gd_.params[1];
            gd_.read_flags = gd_.params[2];
            gd_.read_retry = gd_.params[3];
            gd_.result[2] = 0xA;
            gd_.status = GDC_COMPLETE;
            break;
        case CMD_GET_VERSION: {  // params: dest (16 bytes), 0
            static const char ver[] = "GDC Version 1.10 1999-03-31 ";
            const std::uint32_t dest = gd_.params[0];
            for (unsigned i = 0; i < 16; ++i) m.write8(dest + i, static_cast<std::uint8_t>(ver[i]));
            m.write8(dest + 15, 0x02);
            gd_.status = GDC_COMPLETE;
            break;
        }
        case CMD_GETSCD: {  // subcode: params format, size, dest; report "no audio" cleanly
            const std::uint32_t dest = gd_.params[2],
                                size = std::min<std::uint32_t>(gd_.params[1], 100);
            for (std::uint32_t i = 0; i < size; ++i) m.write8(dest + i, 0);
            if (size >= 2) {
                m.write8(dest, 0);  // format
                m.write8(dest + 1, DRV_PAUSE);
            }
            gd_.result[2] = size;
            gd_.status = GDC_COMPLETE;
            break;
        }
        case CMD_PLAY:
        case CMD_PLAY2:
        case CMD_PAUSE:
        case CMD_RELEASE:
        case CMD_SEEK:
        case CMD_STOP:
        case CMD_SCAN:
            ++counts["gdrom.cdda"];  // CD audio is not modelled (Crazy Taxi streams ADX from data)
            gd_.status = GDC_COMPLETE;
            break;
        default:
            ++counts["gdrom.unknown_cmd"];
            gd_.result[0] = 5;  // GDC_ERR_ILLEGALREQUEST
            gd_.status = GDC_ERR;
            break;
    }
}

void Bios::gd_request(sh4::Ctx& c, ::dream::Memory& m) {
    if (gd_.status != GDC_OK) {  // one command at a time; 0 means "try again"
        c.r[0] = 0;
        return;
    }
    gd_.command = c.r[4];
    for (unsigned i = 0; i < 4; ++i) gd_.params[i] = c.r[5] ? m.read32(c.r[5] + 4 * i) : 0;
    gd_.result[0] = gd_.result[1] = gd_.result[2] = gd_.result[3] = 0;
    gd_.read_total = gd_.read_remaining = 0;
    gd_.status = GDC_BUSY;
    gd_.last_request = gd_.next_request++;
    c.r[0] = gd_.last_request;
}

void Bios::sys_gdrom(sh4::Ctx& c, ::dream::Memory& m) {
    c.cycles += kGdSyscallCycles;
    if (c.r[6] == 0xFFFFFFFFu) {  // misc functions share the vector
        ++counts["misc"];
        switch (c.r[7]) {
            case 0:
                c.r[0] = 0;
                break;  // MISC_INIT
            case 1:
                c.r[0] = 0;
                break;  // MISC_SETVECTOR (r4 vector, r5 handler): not used by Katana titles
            default:
                c.r[0] = 0xFFFFFFFFu;
                break;
        }
        return;
    }
    ++counts["gdrom"];
    static const bool trace = std::getenv("DREAM_TRACE_GDROM") != nullptr;
    const std::uint32_t fn = c.r[7], a4 = c.r[4], a5 = c.r[5];
    struct Report {
        bool on;
        sh4::Ctx& c;
        ::dream::Memory& m;
        Bios& b;
        std::uint32_t fn, a4, a5;
        ~Report() {
            if (!on)
                return;
            std::fprintf(stderr,
                         "gdrom fn %2u r4 %08x r5 %08x -> r0 %08x  [status %d cmd %u remaining %u "
                         "total %u req %u]",
                         fn, a4, a5, c.r[0], b.gd_.status, b.gd_.command, b.gd_.read_remaining,
                         b.gd_.read_total, b.gd_.last_request);
            if (fn == FN_GET_DRV_STAT && a4)
                std::fprintf(stderr, " drv %u type %u", m.read32(a4), m.read32(a4 + 4));
            if (fn == FN_GET_CMD_STAT && a5)
                std::fprintf(stderr, " res %u %u %u %u", m.read32(a5), m.read32(a5 + 4),
                             m.read32(a5 + 8), m.read32(a5 + 12));
            if (fn == FN_REQ_CMD && a5)
                std::fprintf(stderr, " params %08x %08x %08x %08x", m.read32(a5), m.read32(a5 + 4),
                             m.read32(a5 + 8), m.read32(a5 + 12));
            std::fputc('\n', stderr);
        }
    } report{trace, c, m, *this, fn, a4, a5};
    switch (c.r[7]) {
        case FN_REQ_CMD:
            gd_request(c, m);
            break;
        case FN_GET_CMD_STAT: {  // r4 request id, r5 -> {error, error1, size, wait}
            // The drive works in real time: a read whose completion lies before the guest clock
            // is complete when asked, whether or not an interrupt poll has run the scheduler.
            if (sys_.sched.now() < c.cycles)
                sys_.sched.advance_to(c.cycles);
            if (c.r[4] != gd_.last_request) {
                c.r[0] = GDC_OK;
                break;
            }
            if (c.r[5]) {
                for (unsigned i = 0; i < 4; ++i) m.write32(c.r[5] + 4 * i, gd_.result[i]);
            }
            c.r[0] = static_cast<std::uint32_t>(gd_.status);
            if (gd_.status == GDC_COMPLETE || gd_.status == GDC_ERR)
                gd_.status = GDC_OK;  // reported once, then the queue is free again
            break;
        }
        case FN_EXEC_SERVER:
            if (gd_.status == GDC_BUSY) {
                // Let the read make progress up to the guest clock before answering.
                if (sys_.sched.now() < c.cycles)
                    sys_.sched.advance_to(c.cycles);
                if (gd_.status == GDC_BUSY)
                    gd_exec(m);
            }
            c.r[0] = 0;
            break;
        case FN_INIT_SYSTEM:
            gd_ = Gd{};
            c.r[0] = 0;
            break;
        case FN_GET_DRV_STAT:  // r4 -> {status, disc type}
            if (sys_.sched.now() < c.cycles)
                sys_.sched.advance_to(c.cycles);
            // PAUSE whenever a disc is present: BUSY describes the drive mechanism, not a queued
            // syscall command, and PLAY is CD audio, which is not modelled (Flycast's HLE agrees;
            // Crazy Taxi behaves the same under either reading, measured 2026-09-12).
            m.write32(c.r[4], disc_ ? DRV_PAUSE : DRV_NODISC);
            m.write32(c.r[4] + 4, disc_ ? static_cast<std::uint32_t>(disc_->type()) : 0);
            c.r[0] = GDC_OK;
            break;
        case FN_G1_DMA_END:  // r4 callback, r5 arg
            gd_.callback = c.r[4];
            gd_.callback_arg = c.r[5];
            sys_.holly.clear(holly::Irq::GdromDma);
            c.r[0] = GDC_OK;
            break;
        case FN_READ_ABORT:
            if (c.r[4] == gd_.last_request) {
                gd_.read_remaining = 0;
                sys_.sched.cancel(gd_event_);
                gd_.status = GDC_OK;
                c.r[0] = GDC_OK;
            } else {
                c.r[0] = static_cast<std::uint32_t>(GDC_ERR);
            }
            break;
        case FN_RESET:
            gd_.status = GDC_OK;
            gd_.last_request = 0xFFFFFFFFu;
            c.r[0] = GDC_OK;
            break;
        case FN_CHANGE_DATA_TYPE:  // r4 -> 4 words of sector mode
            for (unsigned i = 0; i < 4; ++i) gd_.sector_mode[i] = m.read32(c.r[4] + 4 * i);
            c.r[0] = GDC_OK;
            break;
        case FN_SET_PIO_CALLBACK:
            gd_.callback = c.r[4];
            gd_.callback_arg = c.r[5];
            c.r[0] = GDC_OK;
            break;
        case FN_REQ_DMA_TRANS:
        case FN_CHECK_DMA_TRANS:
        case FN_REQ_PIO_TRANS:
        case FN_CHECK_PIO_TRANS:
            ++counts["gdrom.streaming_unsupported"];
            c.r[0] = static_cast<std::uint32_t>(GDC_ERR);
            break;
        default:
            ++counts["gdrom.unknown_fn"];
            c.r[0] = static_cast<std::uint32_t>(GDC_ERR);
            break;
    }
}

// ---- system misc (0x8C0000E0): r4 = function ---------------------------------------------------

void Bios::sys_misc(sh4::Ctx& c, ::dream::Memory&) {
    c.cycles += kSyscallCycles;
    ++counts["sysmisc"];
    switch (c.r[4]) {
        case 0:
            c.r[0] = 0x00C0BEBCu;
            break;  // normal init; border colour the BIOS leaves behind
        case 2:
            c.r[0] = disc_ ? 0 : 0xFFFFFFFFu;
            break;  // check disc
        default:
            c.r[0] = 0;
            break;  // exit to menu: nothing sensible to do
    }
}

}  // namespace dream::hle
