// Reading a memory card's file system for display, and keeping what a title draws on its screen.
#include <array>
#include <cstdint>
#include <vector>

#include "dream/runtime/maple/maple.h"
#include "dream/runtime/maple/vmu_fs.h"

#include "doctest.h"

using dream::maple::MemoryCard;

namespace {

constexpr std::size_t kBlock = MemoryCard::kBlockSize;

void put16(std::vector<std::uint8_t>& v, std::size_t off, std::uint16_t x) {
    v[off] = static_cast<std::uint8_t>(x & 0xFF);
    v[off + 1] = static_cast<std::uint8_t>(x >> 8);
}

// A formatted card with one two-block save at 199 -> 198, as a title would leave it.
std::vector<std::uint8_t> card_with_one_save() {
    MemoryCard card;
    card.format();
    std::vector<std::uint8_t> img = card.image();
    const std::size_t fat = 254 * kBlock;
    put16(img, fat + 199 * 2, 198);
    put16(img, fat + 198 * 2, 0xFFFA);
    std::uint8_t* entry = img.data() + 253 * kBlock;
    entry[0] = 0x33;  // a data file
    put16(img, 253 * kBlock + 2, 199);
    const char name[] = "RUSH2049    ";
    for (int i = 0; i < 12; ++i) entry[4 + i] = static_cast<std::uint8_t>(name[i]);
    put16(img, 253 * kBlock + 0x18, 2);
    // The header in the first block: a description, one icon, a palette and the bitmap, which runs
    // on into the second block of the chain.
    const std::size_t h = 199 * kBlock;
    const char desc[] = "RUSH 2049 SAVE  ";
    for (int i = 0; i < 16; ++i) img[h + i] = static_cast<std::uint8_t>(desc[i]);
    put16(img, h + 0x40, 1);
    put16(img, h + 0x60 + 2, 0xF0F0);                  // palette entry 1: opaque green
    img[h + 0x80] = 0x10;                              // pixel 0 is entry 1, pixel 1 is entry 0
    img[198 * kBlock + (0x80 + 500 - kBlock)] = 0x01;  // pixel 1001, in the chain's second block
    return img;
}

}  // namespace

TEST_CASE("a blank card lists nothing and has its whole save area free") {
    MemoryCard card;
    card.format();
    const auto c = dream::maple::read_vmu(card.image());
    CHECK(c.formatted);
    CHECK(c.user_blocks == 200);
    CHECK(c.free_blocks == 200);
    CHECK(c.files.empty());
}

TEST_CASE("an unformatted image is reported as such") {
    const std::vector<std::uint8_t> blank(MemoryCard::kImageSize, 0);
    CHECK_FALSE(dream::maple::read_vmu(blank).formatted);
    CHECK_FALSE(dream::maple::read_vmu(std::vector<std::uint8_t>(100, 0x55)).formatted);
}

TEST_CASE("a save is listed with its description and icon, following its chain") {
    const auto c = dream::maple::read_vmu(card_with_one_save());
    REQUIRE(c.files.size() == 1);
    const auto& f = c.files[0];
    CHECK(f.name == "RUSH2049");
    CHECK(f.description == "RUSH 2049 SAVE");
    CHECK_FALSE(f.game);
    CHECK(f.blocks == 2);
    CHECK(c.free_blocks == 198);
    REQUIRE(f.icon.size() == 32 * 32);
    CHECK(f.icon[0] == 0xFF00FF00u);  // opaque green, red in the low byte
    CHECK(f.icon[1] == 0u);           // entry 0: transparent black
    CHECK(f.icon[1001] == 0xFF00FF00u);
}

TEST_CASE("the card keeps what a title draws on its screen") {
    MemoryCard card;
    std::array<std::uint32_t, 2 + MemoryCard::kLcdBytes / 4> args{};
    args[0] = dream::maple::kLcd;
    for (std::size_t i = 2; i < args.size(); ++i)
        args[i] = 0x01020304u * static_cast<std::uint32_t>(i);
    dream::maple::Payload out;
    CHECK(card.handle(dream::maple::kBlockWrite, args.data(), args.size(), out) ==
          dream::maple::kDeviceReply);
    CHECK(card.screen_writes == 1);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(args.data() + 2);
    for (std::size_t i = 0; i < MemoryCard::kLcdBytes; ++i) CHECK(card.lcd()[i] == bytes[i]);
}
