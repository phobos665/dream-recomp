#include "dream/runtime/maple/maple.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace dream::maple {
namespace {
inline std::uint32_t bswap32(std::uint32_t v) noexcept {
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}
}  // namespace

void Payload::str(const char* s, std::size_t field) {
    const std::size_t n = std::strlen(s);
    for (std::size_t i = 0; i < field; ++i) u8(i < n ? static_cast<std::uint8_t>(s[i]) : ' ');
}

// ---- Controller --------------------------------------------------------------------------------

std::uint8_t axis_byte(float low, float high) noexcept {
    const float v = std::clamp(high - low, -1.0f, 1.0f);
    // 127.5 rather than 127, so -1 lands exactly on 0 and +1 on 255 while 0 still rounds to the
    // 0x80 the hardware calls centred.
    return static_cast<std::uint8_t>(std::lround(v * 127.5f + 127.5f));
}

std::uint8_t trigger_byte(float v) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

CardStatus MemoryCard::load(const std::string& path) {
    path_.clear();  // adopted only once the file is known to be a card
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return CardStatus::Missing;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec)
        return CardStatus::Unreadable;
    // Exact size or nothing. A card with a stray newline is not a card, and writing it back would
    // silently drop the byte; a file that is not a card at all must never be adopted, because the
    // next block write would rewrite it end to end.
    if (size != kImageSize)
        return CardStatus::WrongSize;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return CardStatus::Unreadable;
    in.read(reinterpret_cast<char*>(flash_.data()), static_cast<std::streamsize>(kImageSize));
    if (in.gcount() != static_cast<std::streamsize>(kImageSize))
        return CardStatus::Unreadable;
    path_ = path;
    return CardStatus::Ok;
}

bool MemoryCard::save() const {
    if (path_.empty())
        return false;
    std::ofstream out(path_, std::ios::binary);
    if (!out)
        return false;
    out.write(reinterpret_cast<const char*>(flash_.data()),
              static_cast<std::streamsize>(kImageSize));
    return static_cast<bool>(out);
}

bool MemoryCard::save_as(const std::string& path) {
    path_ = path;
    return save();
}

// docs/vmu-creation-study.md has the provenance of every value here: each was read out of two real
// cards rather than recalled, and a blank card built from this description differs from Flycast's
// own default image only in leftover junk that image carries.
void MemoryCard::format() {
    std::fill(flash_.begin(), flash_.end(), std::uint8_t{0});
    std::uint8_t* root = flash_.data() + 255 * kBlockSize;
    auto put16 = [](std::uint8_t* p, std::uint16_t v) {
        p[0] = static_cast<std::uint8_t>(v & 0xFF);
        p[1] = static_cast<std::uint8_t>(v >> 8);
    };
    for (unsigned i = 0; i < 16; ++i) root[i] = 0x55;  // the format marker formatted() looks for
    root[0x10] = 0x01;                                 // a custom volume colour follows
    root[0x11] = 0xFF;                                 // blue
    root[0x12] = 0xFF;                                 // green
    root[0x13] = 0xFF;                                 // red
    root[0x14] = 0x64;                                 // alpha, 100
    // A fixed BCD stamp rather than the host clock, and deliberately: two runs of the same build
    // must produce the same bytes, which is what the write-hash comparison in
    // docs/differential-harness.md depends on. It is also the date both reference cards carry.
    static constexpr std::uint8_t kStamp[8] = {0x19, 0x98, 0x11, 0x27, 0x00, 0x00, 0x59, 0x04};
    std::copy(std::begin(kStamp), std::end(kStamp), root + 0x30);
    // The geometry, which is also the 24 bytes Get Media Info hands back, so it is what makes the
    // card describe itself consistently.
    put16(root + 0x40, 255);  // last block
    put16(root + 0x42, 0);    // partition number
    put16(root + 0x44, 255);  // system area
    put16(root + 0x46, 254);  // FAT
    put16(root + 0x48, 1);    // one FAT block
    put16(root + 0x4A, 253);  // directory, growing downwards
    put16(root + 0x4C, 13);   // thirteen blocks of it
    root[0x4E] = 5;           // volume icon; a real format writes 5, a card-less reply claims 0
    put16(root + 0x50, 200);  // save area: VMU mini-games live at 200..230
    put16(root + 0x52, 31);
    root[0x56] = 0x80;  // both reference cards carry this; Flycast's own comment does not know why

    std::uint8_t* fat = flash_.data() + 254 * kBlockSize;
    for (unsigned b = 0; b <= 240; ++b) put16(fat + b * 2, 0xFFFC);  // free
    put16(fat + 241 * 2, 0xFFFA);                                    // directory's last block
    for (unsigned b = 242; b <= 253; ++b)
        put16(fat + b * 2, static_cast<std::uint16_t>(b - 1));  // 253 -> 252 -> ... -> 241
    put16(fat + 254 * 2, 0xFFFA);                               // the FAT block itself
    put16(fat + 255 * 2, 0xFFFA);                               // and the root
    // The directory (241..253) and the user area (0..199) stay zero: an empty card.
}

// A formatted card has its system area in the last block, starting with a run of 0x55.
bool MemoryCard::formatted() const noexcept {
    const std::uint8_t* root = flash_.data() + 0xFF * kBlockSize;
    for (unsigned i = 0; i < 16; ++i)
        if (root[i] != 0x55)
            return false;
    return true;
}

Reply MemoryCard::handle(std::uint8_t command, const std::uint32_t* args, std::size_t nwords,
                         Payload& out) {
    switch (command) {
        case kDeviceRequest:
        case kAllStatusReq: {
            out.u32(functions());
            // Function data, most significant function first: clock, screen, storage. The storage
            // word says 256 blocks of 512 bytes, read and written a block at a time.
            out.u32(0x403F7E7Eu);
            out.u32(0x00100500u);
            out.u32(0x00410F00u);
            out.u8(0xFF);  // area code
            out.u8(0);     // connector direction
            out.str("Visual Memory", 30);
            out.str("Produced By or Under License From SEGA ENTERPRISES,LTD.", 60);
            out.u16(0x007Cu);  // standby current
            out.u16(0x0082u);  // maximum current
            if (command == kAllStatusReq) {
                const char* extra =
                    "Version 1.005,1999/04/15,315-6208-03,SEGA Visual Memory System BIOS Produced "
                    "by ";
                for (const char* p = extra; *p; ++p) out.u8(static_cast<std::uint8_t>(*p));
                while (out.bytes().size() % 4) out.u8(0);
                return kDeviceStatusAll;
            }
            return kDeviceStatus;
        }
        case kGetMediaInfo: {
            if (nwords < 1)
                return kFileError;
            // The function selector arrives in the frame's own order, but the block and phase
            // word that follows it is big-endian. Swapping both reads the selector as 2 rather
            // than the storage function and every request is refused.
            if (args[0] != kStorage)
                return kUnknownFunction;
            ++media_info_reads;
            out.u32(kStorage);
            if (formatted()) {
                // The card's own system area describes its geometry.
                const std::uint8_t* info = flash_.data() + 0xFF * kBlockSize + 0x40;
                for (unsigned i = 0; i < 24; ++i) out.u8(info[i]);
            } else {
                // A blank card reports the standard geometry so the game can format it.
                out.u16(0x00FF);  // last block
                out.u16(0x0000);  // partition
                out.u16(0x00FF);  // system area block
                out.u16(0x00FE);  // file allocation table block
                out.u16(0x0001);  // allocation table blocks
                out.u16(0x00FD);  // directory block
                out.u16(0x000D);  // directory blocks
                out.u8(0);        // volume icon
                out.u8(0);
                out.u16(0x00C8);  // first save block
                out.u16(0x001F);  // save blocks
                out.u32(0);
            }
            return kDataTransfer;
        }
        case kBlockRead: {
            if (nwords < 2)
                return kFileError;
            if (args[0] != kStorage)
                return kUnknownFunction;
            const std::uint32_t block = bswap32(args[1]) & 0xFFFFu;
            if (block >= kBlocks)
                return kFileError;
            ++block_reads;
            out.u32(kStorage);
            out.u32(args[1]);  // the request word is echoed back unchanged
            const std::uint8_t* p = flash_.data() + block * kBlockSize;
            for (std::size_t i = 0; i < kBlockSize; ++i) out.u8(p[i]);
            return kDataTransfer;
        }
        case kBlockWrite: {
            if (nwords < 2)
                return kFileError;
            // Titles also draw on the card's little screen; Crazy Taxi sends it an icon every few
            // frames. The screen is not modelled, but the write is accepted so the title does not
            // decide the device is broken.
            if (args[0] == kLcd) {
                ++screen_writes;
                return kDeviceReply;
            }
            if (args[0] != kStorage)
                return kUnknownFunction;
            const std::uint32_t request = bswap32(args[1]);
            const std::uint32_t block = request & 0xFFFFu;
            const std::uint32_t phase = (request >> 16) & 0xFFu;
            // A block is written in four phases of an eighth of a kilobyte each.
            const std::size_t offset = block * kBlockSize + phase * (kBlockSize / 4);
            const std::size_t length = (nwords - 2) * 4;
            if (offset + length > kImageSize)
                return kFileError;
            std::memcpy(flash_.data() + offset, args + 2, length);
            ++block_writes;
            if (!save())
                return kFileError;
            return kDeviceReply;
        }
        case kGetLastError:
            return kDeviceReply;  // the write completed when it was accepted
        case kSetCondition:
            return kDeviceReply;  // the screen and the beeper are not modelled
        default:
            return kUnknownCommand;
    }
}

Reply Controller::handle(std::uint8_t command, const std::uint32_t* args, std::size_t nwords,
                         Payload& out) {
    switch (command) {
        case kDeviceRequest:
        case kAllStatusReq: {
            // Fixed device status: function codes, function data, area, direction, names, power.
            out.u32(kInput);
            out.u32(0xFE060F00u);  // input capabilities: 4 analogue axes, X Y A B Start and D-pad
            out.u32(0);
            out.u32(0);
            out.u8(0xFF);  // area code: all regions
            out.u8(0);     // connector direction
            out.str("Dreamcast Controller", 30);
            out.str("Produced By or Under License From SEGA ENTERPRISES,LTD.", 60);
            out.u16(0x01AE);  // standby current, 43 mA
            out.u16(0x01F4);  // maximum current, 50 mA
            if (command == kAllStatusReq) {
                const char* extra =
                    "Version 1.010,1998/09/28,315-6211-AB   ,Analog Module : The 4th Edition.5/8  "
                    "+DF";
                for (const char* p = extra; *p; ++p) out.u8(static_cast<std::uint8_t>(*p));
                while (out.bytes().size() % 4) out.u8(0);
                return kDeviceStatusAll;
            }
            return kDeviceStatus;
        }
        case kGetCondition: {
            if (nwords < 1 || args[0] != kInput)
                return kUnknownFunction;
            ++condition_reads;
            out.u32(kInput);
            out.u16(state.buttons | 0xF901u);  // D-pad 2, C, D and Z are absent on the standard pad
            out.u8(state.rtrigger);
            out.u8(state.ltrigger);
            out.u8(state.joy_x);
            out.u8(state.joy_y);
            out.u8(state.joy2_x);
            out.u8(state.joy2_y);
            return kDataTransfer;
        }
        case kDeviceReset:
        case kDeviceKill:
            return kDeviceReply;
        default:
            return kUnknownCommand;
    }
}

// ---- Bus -----------------------------------------------------------------------------------

Bus::Bus(sched::Scheduler& sched, holly::Intc& holly, mem::DcMemory& memory)
    : sched_(sched), holly_(holly), memory_(memory) {
    event_ = sched_.add("maple-dma", [this](std::uint64_t, std::uint64_t) { complete(); });
    reset();
}

void Bus::reset() {
    mdstar = mdtsel = mden = mdst = mshtcl = 0;
    msys = 0x3A980000;
    mdapro = 0x00007F00;
    mmsel = 1;
    out_.clear();
    trigger_pending_reset_ = false;
    sched_.cancel(event_);
}

void Bus::attach(unsigned port, std::unique_ptr<Device> device) {
    ports_[port & 3] = std::move(device);
}

void Bus::attach_expansion(unsigned port, unsigned slot, std::unique_ptr<Device> device) {
    if (slot < kExpansionSlots)
        expansion_[port & 3][slot] = std::move(device);
}

// The recipient byte names one device on the port: bit 5 is the controller itself and bits 0 to 4
// are its expansion slots, so a memory card in the first slot is addressed as 0x01.
Device* Bus::device_for(unsigned port, std::uint8_t recipient) const noexcept {
    port &= 3;
    if (recipient & 0x20u)
        return ports_[port].get();
    for (unsigned slot = 0; slot < kExpansionSlots; ++slot)
        if (recipient & (1u << slot))
            return expansion_[port][slot].get();
    return nullptr;
}

void Bus::vblank() {
    if (!(mden & 1))
        return;
    if (mdtsel == 1) {  // hardware trigger
        if (!trigger_pending_reset_) {
            mdst = 1;
            start_dma();
            if ((msys >> 12) & 1)  // trigger reset is manual (SB_MSHTCL)
                trigger_pending_reset_ = true;
        }
    } else {
        trigger_pending_reset_ = false;
    }
}

// Descriptor list at SB_MDSTAR (Katana Maple spec / Flycast maple_DoDma): each entry is a header
// word (bit 31 last, 17:16 port, 10:8 pattern, 7:0 frame words - 1), the receive address, then the
// frame for pattern START. Frame header: command | recipient << 8 | sender << 16 | words << 24.
void Bus::start_dma() {
    if (mdst == 0 || (mden & 1) == 0)
        return;
    ++transfers;
    out_.clear();
    std::uint32_t addr = mdstar & 0x1FFFFFE0u;
    std::uint64_t bits_in = 0, bits_out = 0;
    const bool swap = mmsel == 0;
    auto rd = [&](std::uint32_t a) {
        const std::uint32_t v = memory_.read32(a);
        return swap ? bswap32(v) : v;
    };
    for (unsigned guard = 0; guard < 1024; ++guard) {
        const std::uint32_t h1 = memory_.read32(addr);
        const std::uint32_t recv = memory_.read32(addr + 4) & 0x1FFFFFE0u;
        const bool last = (h1 >> 31) != 0;
        const std::uint32_t words = (h1 & 0xFFu) + 1;
        const unsigned pattern = (h1 >> 8) & 7u;
        const unsigned port = (h1 >> 16) & 3u;
        if (pattern == 0) {  // START
            const std::uint32_t frame = rd(addr + 8);
            const std::uint8_t command = static_cast<std::uint8_t>(frame);
            const std::uint8_t reci = static_cast<std::uint8_t>(frame >> 8);
            const std::uint8_t send = static_cast<std::uint8_t>(frame >> 16);
            std::vector<std::uint32_t> args;
            for (std::uint32_t i = 1; i < words; ++i) args.push_back(rd(addr + 8 + 4 * i));
            ++frames;
            bits_in += (words * 4 + 3) * 8;
            ++frames_by_recipient[reci];
            ++frames_by_command[command];
            Device* dev = device_for(port, reci);
            // A main device's reply says which of its expansion slots are occupied: that is how a
            // title discovers a memory card without probing every address.
            std::uint8_t sender = reci;
            if (reci & 0x20u)
                for (unsigned slot = 0; slot < kExpansionSlots; ++slot)
                    if (expansion_[port][slot])
                        sender |= static_cast<std::uint8_t>(1u << slot);
            if (dev) {
                Payload out;
                const Reply reply = dev->handle(command, args.data(), args.size(), out);
                const auto& b = out.bytes();
                std::vector<std::uint32_t> resp;
                resp.push_back(static_cast<std::uint32_t>(reply) |
                               (static_cast<std::uint32_t>(send) << 8) |
                               (static_cast<std::uint32_t>(sender) << 16) |
                               (static_cast<std::uint32_t>(b.size() / 4) << 24));
                for (std::size_t i = 0; i + 4 <= b.size(); i += 4) {
                    std::uint32_t w;
                    std::memcpy(&w, b.data() + i, 4);
                    resp.push_back(w);
                }
                if (swap)
                    for (auto& w : resp) w = bswap32(w);
                bits_out += (resp.size() * 4 + 3) * 8;
                out_.push_back({recv, std::move(resp)});
            } else {
                ++no_device;
                out_.push_back({recv, {0xFFFFFFFFu}});  // no response: the bus times out
            }
            addr += (2 + words) * 4;
        } else {
            addr += 4;  // RESET, SDCKB occupy/cancel, NOP: one word each
            bits_in += 8;
        }
        if (last)
            break;
    }
    // 2 Mb/s from the console, ~740 kb/s from the devices (Flycast's measured figures).
    const std::uint64_t cycles =
        bits_in * sched::kSh4Clock / 2'000'000 + bits_out * sched::kSh4Clock / 740'000;
    sched_.request(event_, cycles ? cycles : 1);
}

void Bus::complete() {
    if (mden & 1) {
        for (const auto& p : out_) {
            if (p.addr == 0)
                continue;
            for (std::size_t i = 0; i < p.words.size(); ++i)
                memory_.write32(p.addr + static_cast<std::uint32_t>(4 * i), p.words[i]);
        }
        holly_.raise(holly::Irq::MapleDma);
    }
    out_.clear();
    mdst = 0;
}

std::uint32_t Bus::read(std::uint32_t addr, unsigned) {
    switch (addr - kBase) {
        case 0x04:
            return mdstar;
        case 0x10:
            return mdtsel;
        case 0x14:
            return mden;
        case 0x18:
            return mdst;
        case 0x80:
            return msys;
        case 0x88:
            return mshtcl;
        case 0x8C:
            return mdapro;
        case 0xE8:
            return mmsel;
        default:
            return 0;
    }
}

void Bus::write(std::uint32_t addr, std::uint32_t v, unsigned) {
    switch (addr - kBase) {
        case 0x04:
            mdstar = v & 0x1FFFFFE0u;
            break;
        case 0x10:
            mdtsel = v & 1u;
            break;
        case 0x14:
            mden = v & 1u;
            if (!mden)
                mdst = 0;
            break;
        case 0x18:
            if ((v & 1u) && (mden & 1u) && mdst == 0) {
                mdst = 1;
                start_dma();
            }
            break;
        case 0x80:
            msys = v;
            break;
        case 0x84:
            break;  // SB_MST: status of the trigger reset, read-only in effect
        case 0x88:
            if (v & 1u)
                trigger_pending_reset_ = false;
            break;
        case 0x8C:
            if ((v >> 16) == 0x6155u)
                mdapro = v & 0x00007F7Fu;
            break;
        case 0xE8:
            mmsel = v & 1u;
            break;
        default:
            break;
    }
}

}  // namespace dream::maple
