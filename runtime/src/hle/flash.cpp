#include "dream/runtime/hle/flash.h"

#include <cstring>

namespace dream::hle {
namespace {
constexpr char kMagic[16] = {'K', 'A', 'T', 'A', 'N', 'A', '_', 'F',
                             'L', 'A', 'S', 'H', '_', '_', '_', '_'};
constexpr std::uint32_t kBitmapBlocks = Flash::kBlock * 8;  // physical blocks one bitmap covers
}  // namespace

bool Flash::partition(unsigned id, std::uint32_t& offset, std::uint32_t& size) noexcept {
    switch (id) {
        case Factory:
            offset = 0x1A000;
            size = 0x2000;
            return true;
        case Reserved:
            offset = 0x18000;
            size = 0x2000;
            return true;
        case User:
            offset = 0x1C000;
            size = 0x4000;
            return true;
        case Game:
            offset = 0x10000;
            size = 0x8000;
            return true;
        case Unknown:
            offset = 0x00000;
            size = 0x10000;
            return true;
        default:
            return false;
    }
}

// CRC-16 over the block id and the 60 payload bytes (KallistiOS flashrom_calc_crc / redream).
std::uint16_t Flash::crc(const std::uint8_t* b) noexcept {
    int n = 0xFFFF;
    for (int i = 0; i < 62; ++i) {
        n ^= b[i] << 8;
        for (int c = 0; c < 8; ++c) n = (n & 0x8000) ? ((n << 1) ^ 4129) : (n << 1);
    }
    return static_cast<std::uint16_t>((~n) & 0xFFFF);
}

std::uint32_t Flash::user_blocks(std::uint32_t size) noexcept {
    // header + user blocks + one bitmap block per 512 user blocks
    const std::uint32_t total = size / kBlock;
    const std::uint32_t bitmaps = (total + kBitmapBlocks - 1) / kBitmapBlocks;
    return total - 1 - bitmaps;
}

void Flash::erase_partition(unsigned part) {
    std::uint32_t off = 0, size = 0;
    if (partition(part, off, size))
        std::memset(data_ + off, 0xFF, size);
}

void Flash::format(Language lang, Region region, Broadcast broadcast, const char* sysinfo) {
    std::memset(data_, 0xFF, kSize);
    // Factory partition: the system string twice, as the BIOS validates it. Both copies carry the
    // region, language and broadcast digits, and a title may read either, so both are stamped.
    // Offsets and encoding follow Flycast's fixUpDCFlash() (core/hw/flashrom/nvmem.cpp), which
    // writes '0' + the value at 0x1a002/3/4 and again at 0x1a0a2/3/4.
    for (std::uint32_t base : {0x1A000u, 0x1A0A0u}) {
        std::memcpy(data_ + base, sysinfo, 16);
        data_[base + 2] = static_cast<std::uint8_t>('0' + static_cast<unsigned>(region));
        data_[base + 3] = static_cast<std::uint8_t>('0' + static_cast<unsigned>(lang));
        data_[base + 4] = static_cast<std::uint8_t>('0' + static_cast<unsigned>(broadcast));
    }
    // The Reserved partition is left erased. It used to be zeroed here, with a comment claiming it
    // "reads as zeros"; that is not how flash behaves. An erased device reads all-ones, and the
    // usual way a title asks whether a block was ever written is to test it for 0xFF -- which a
    // zero-filled partition fails, so the title concludes it holds real data.
    for (unsigned part : {User, Game, Unknown}) {
        std::uint32_t off = 0, size = 0;
        partition(part, off, size);
        std::memset(data_ + off, 0xFF, size);
        std::memcpy(data_ + off, kMagic, 16);
        data_[off + 16] = static_cast<std::uint8_t>(part);
        data_[off + 17] = 0;  // version
        std::memset(data_ + off + 18, 0, kBlock - 18);
    }
    std::uint8_t syscfg[60] = {};
    // time_lo, time_hi (seconds since 1950; left zero), time_zone, lang, mono, autostart
    syscfg[4] = 0;
    syscfg[5] = static_cast<std::uint8_t>(lang);
    syscfg[6] = 0;  // stereo
    syscfg[7] = 1;  // auto start
    write_block(User, kSyscfgBlock, syscfg);
}

bool Flash::header_ok(std::uint32_t offset, unsigned part) const noexcept {
    return std::memcmp(data_ + offset, kMagic, 16) == 0 && data_[offset + 16] == part;
}

// Physical block ids count from 1 (0 is the header). A cleared bitmap bit means allocated; blocks
// are allocated linearly and the highest physical block carrying a logical id wins.
std::uint32_t Flash::lookup(std::uint32_t offset, std::uint32_t size,
                            std::uint32_t block_id) const noexcept {
    const std::uint32_t blocks = user_blocks(size);
    std::uint32_t bitmap_id = 1 + blocks;
    std::uint32_t result = 0;
    const std::uint8_t* bitmap = nullptr;
    for (std::uint32_t phys = 1; phys <= blocks; ++phys) {
        if (phys % kBitmapBlocks == 1)
            bitmap = data_ + offset + (bitmap_id++) * kBlock;
        const std::uint32_t bit = (phys - 1) % kBitmapBlocks;
        const bool allocated = !(bitmap[bit / 8] & (0x80u >> (bit % 8)));
        if (!allocated)
            break;
        const std::uint8_t* blk = data_ + offset + phys * kBlock;
        const std::uint32_t id = blk[0] | (blk[1] << 8);
        if (id == block_id)
            result = phys;
    }
    return result;
}

std::uint32_t Flash::alloc(std::uint32_t offset, std::uint32_t size) noexcept {
    const std::uint32_t blocks = user_blocks(size);
    std::uint32_t bitmap_id = 1 + blocks;
    std::uint8_t* bitmap = nullptr;
    for (std::uint32_t phys = 1; phys <= blocks; ++phys) {
        if (phys % kBitmapBlocks == 1)
            bitmap = data_ + offset + (bitmap_id++) * kBlock;
        const std::uint32_t bit = (phys - 1) % kBitmapBlocks;
        if (bitmap[bit / 8] & (0x80u >> (bit % 8))) {  // free
            bitmap[bit / 8] &= static_cast<std::uint8_t>(~(0x80u >> (bit % 8)));
            return phys;
        }
    }
    return 0;
}

bool Flash::read_block(unsigned part, std::uint32_t block_id, std::uint8_t out[60]) const {
    std::uint32_t off = 0, size = 0;
    if (!partition(part, off, size) || !header_ok(off, part))
        return false;
    const std::uint32_t phys = lookup(off, size, block_id);
    if (!phys)
        return false;
    std::memcpy(out, data_ + off + phys * kBlock + 2, 60);
    return true;
}

bool Flash::write_block(unsigned part, std::uint32_t block_id, const std::uint8_t in[60]) {
    std::uint32_t off = 0, size = 0;
    if (!partition(part, off, size) || !header_ok(off, part))
        return false;
    std::uint32_t phys = lookup(off, size, block_id);
    if (!phys)
        phys = alloc(off, size);
    if (!phys)
        return false;
    std::uint8_t* blk = data_ + off + phys * kBlock;
    blk[0] = static_cast<std::uint8_t>(block_id);
    blk[1] = static_cast<std::uint8_t>(block_id >> 8);
    std::memcpy(blk + 2, in, 60);
    const std::uint16_t c = crc(blk);
    blk[62] = static_cast<std::uint8_t>(c);
    blk[63] = static_cast<std::uint8_t>(c >> 8);
    return true;
}

}  // namespace dream::hle
