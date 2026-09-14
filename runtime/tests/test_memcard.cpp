// The memory card: making a blank one, and refusing to destroy a file that is not one.
//
// The second half matters more than the first. `--vmu` writes the whole 128 KB back after every
// block write, so pointing it at the wrong path used to truncate that file to 131,072 bytes the
// moment the title saved. A mistyped path should cost an error message, not somebody's data.
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "dream/runtime/maple/maple.h"

#include "doctest.h"

using dream::maple::CardStatus;
using dream::maple::MemoryCard;

namespace {

// One directory per call, numbered rather than named after the process: getpid() is not portable
// and these tests run on every CI host.
std::filesystem::path temp_dir() {
    static int n = 0;
    const auto d =
        std::filesystem::temp_directory_path() / ("dream-memcard-" + std::to_string(n++));
    std::error_code ec;
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d, ec);
    return d;
}

std::uint16_t le16(const std::vector<std::uint8_t>& v, std::size_t off) {
    return static_cast<std::uint16_t>(v[off] | (v[off + 1] << 8));
}

constexpr std::size_t kRoot = 255 * MemoryCard::kBlockSize;
constexpr std::size_t kFat = 254 * MemoryCard::kBlockSize;

}  // namespace

TEST_CASE("a formatted blank card has the geometry the console expects") {
    MemoryCard c;
    CHECK_FALSE(c.formatted());  // a zeroed card is not formatted
    c.format();
    CHECK(c.formatted());
    const auto& v = c.image();
    REQUIRE(v.size() == MemoryCard::kImageSize);

    for (unsigned i = 0; i < 16; ++i) CHECK(v[kRoot + i] == 0x55);  // the marker formatted() reads

    // The 24 bytes at root+0x40 are what Get Media Info returns, so they are what makes the card
    // self-describing. Values from docs/vmu-creation-study.md, verified against two real cards.
    CHECK(le16(v, kRoot + 0x40) == 255);  // total size, last block
    CHECK(le16(v, kRoot + 0x42) == 0);    // partition
    CHECK(le16(v, kRoot + 0x44) == 255);  // system area block
    CHECK(le16(v, kRoot + 0x46) == 254);  // FAT block
    CHECK(le16(v, kRoot + 0x48) == 1);    // one FAT block
    CHECK(le16(v, kRoot + 0x4A) == 253);  // directory starts here
    CHECK(le16(v, kRoot + 0x4C) == 13);   // and runs for thirteen blocks
    CHECK(le16(v, kRoot + 0x50) == 200);  // save area
    CHECK(le16(v, kRoot + 0x52) == 31);
}

TEST_CASE("the FAT marks the user area free and the system blocks taken") {
    MemoryCard c;
    c.format();
    const auto& v = c.image();
    // Everything a save could use is free...
    for (unsigned b = 0; b <= 240; ++b)
        CHECK_MESSAGE(le16(v, kFat + b * 2) == 0xFFFC, "block " << b << " should be free");
    // ...the directory is a descending chain ending at 241...
    CHECK(le16(v, kFat + 241 * 2) == 0xFFFA);
    for (unsigned b = 242; b <= 253; ++b)
        CHECK_MESSAGE(le16(v, kFat + b * 2) == b - 1, "directory chain at " << b);
    // ...and the FAT and root blocks are allocated to themselves.
    CHECK(le16(v, kFat + 254 * 2) == 0xFFFA);
    CHECK(le16(v, kFat + 255 * 2) == 0xFFFA);
}

TEST_CASE("a blank card has an empty directory and an empty user area") {
    MemoryCard c;
    c.format();
    const auto& v = c.image();
    for (std::size_t i = 241 * MemoryCard::kBlockSize; i < 254 * MemoryCard::kBlockSize; ++i)
        REQUIRE(v[i] == 0);
    for (std::size_t i = 0; i < 200 * MemoryCard::kBlockSize; ++i) REQUIRE(v[i] == 0);
}

TEST_CASE("loading says which of the four things happened") {
    const auto dir = temp_dir();
    MemoryCard c;

    // Missing is not an error, and must not create anything: the caller decides whether to.
    const auto missing = dir / "nothing-here.bin";
    std::filesystem::remove(missing);
    CHECK(c.load(missing.string()) == CardStatus::Missing);
    CHECK_FALSE(std::filesystem::exists(missing));

    // A real card loads.
    const auto good = dir / "good.bin";
    {
        MemoryCard blank;
        blank.format();
        std::ofstream o(good, std::ios::binary);
        o.write(reinterpret_cast<const char*>(blank.image().data()),
                static_cast<std::streamsize>(MemoryCard::kImageSize));
    }
    CHECK(c.load(good.string()) == CardStatus::Ok);
    CHECK(c.formatted());
}

TEST_CASE("a file that is not a card is refused, and left exactly as it was") {
    // The whole point. This used to be accepted and then truncated to 131,072 bytes on the first
    // block write, which is how a mistyped path destroyed a file.
    const auto dir = temp_dir();
    const auto victim = dir / "important.toml";
    const std::string content = "[game]\nid = \"mygame\"\n";
    {
        std::ofstream o(victim, std::ios::binary);
        o << content;
    }
    MemoryCard c;
    CHECK(c.load(victim.string()) == CardStatus::WrongSize);
    // Refused, so nothing was remembered to save back to...
    CHECK_FALSE(c.save());
    // ...and the file is untouched, byte for byte.
    std::ifstream in(victim, std::ios::binary);
    const std::string after((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(after == content);
    CHECK(std::filesystem::file_size(victim) == content.size());
}

TEST_CASE("a card one byte off is still refused") {
    // 131,073 bytes is a card with a stray newline, and writing it back would silently drop the
    // byte. Exact size or nothing.
    const auto dir = temp_dir();
    for (std::size_t size : {MemoryCard::kImageSize - 1, MemoryCard::kImageSize + 1}) {
        const auto p = dir / ("off-" + std::to_string(size) + ".bin");
        {
            std::ofstream o(p, std::ios::binary);
            const std::vector<char> bytes(size, '\0');
            o.write(bytes.data(), static_cast<std::streamsize>(size));
        }
        MemoryCard c;
        CHECK(c.load(p.string()) == CardStatus::WrongSize);
        CHECK(std::filesystem::file_size(p) == size);
    }
}

TEST_CASE("a formatted card survives the round trip through a file") {
    const auto dir = temp_dir();
    const auto p = dir / "round.bin";
    MemoryCard a;
    a.format();
    CHECK(a.save_as(p.string()));
    CHECK(std::filesystem::file_size(p) == MemoryCard::kImageSize);
    MemoryCard b;
    CHECK(b.load(p.string()) == CardStatus::Ok);
    CHECK(b.formatted());
    CHECK(b.image() == a.image());
}
