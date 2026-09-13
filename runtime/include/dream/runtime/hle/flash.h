// Katana flash ROM layout (WP2.6): five partitions, three of them block-allocated with a header,
// 64-byte user blocks and free bitmaps, the way the system libraries (and KallistiOS) read them.
// Block handling follows redream/Flycast (GPL-2.0, ADR 1). The bytes live in DcMemory::flash().
#pragma once

#include <cstdint>
#include <string>

namespace dream::hle {

enum class Language : std::uint8_t { Japanese = 0, English, German, French, Spanish, Italian };

class Flash {
public:
    static constexpr std::uint32_t kSize = 128u << 10;
    static constexpr std::uint32_t kBlock = 64;
    enum Partition : unsigned { Factory = 0, Reserved, User, Game, Unknown, Count };
    static constexpr std::uint32_t kSyscfgBlock = 0x05;

    explicit Flash(std::uint8_t* data) : data_(data) {}

    // Partition offset and size in bytes; false for an unknown id.
    static bool partition(unsigned id, std::uint32_t& offset, std::uint32_t& size) noexcept;

    // Erases everything, writes the factory strings and formats the block partitions, then stores a
    // system-configuration block with the given language. `sysinfo` is the 16-byte factory string
    // (Flycast's default "00000Dreamcast  ").
    void format(Language lang, const char* sysinfo = "00000Dreamcast  ");

    // Block-allocated partitions: logical block id -> 60 bytes of payload (crc handled here).
    bool read_block(unsigned part, std::uint32_t block_id, std::uint8_t out60[60]) const;
    bool write_block(unsigned part, std::uint32_t block_id, const std::uint8_t in60[60]);
    void erase_partition(unsigned part);

    // Raw access the syscalls use. Writes only clear bits, as flash does.
    std::uint8_t read8(std::uint32_t offset) const noexcept { return data_[offset & (kSize - 1)]; }
    void program8(std::uint32_t offset, std::uint8_t v) noexcept {
        data_[offset & (kSize - 1)] &= v;
    }

    static std::uint16_t crc(const std::uint8_t* block62) noexcept;

private:
    static std::uint32_t user_blocks(std::uint32_t size) noexcept;
    bool header_ok(std::uint32_t offset, unsigned part) const noexcept;
    std::uint32_t lookup(std::uint32_t offset, std::uint32_t size,
                         std::uint32_t block_id) const noexcept;
    std::uint32_t alloc(std::uint32_t offset, std::uint32_t size) noexcept;
    std::uint8_t* data_;
};

}  // namespace dream::hle
