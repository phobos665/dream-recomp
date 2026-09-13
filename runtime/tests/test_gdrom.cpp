#include <cstring>
#include <string>

#include "dream/runtime/gdrom/disc.h"

#include "doctest.h"

using namespace dream::gdrom;

namespace {
const char* kFixture = DREAM_DCDISC_FIXTURES "/synthetic.chd";
}

TEST_CASE("chd: the synthetic GD-ROM fixture has the layout dcdisc measured") {
    std::string err;
    auto disc = open_disc(kFixture, err);
    REQUIRE_MESSAGE(disc != nullptr, err);
    CHECK(disc->type() == DiscType::GdRom);
    REQUIRE(disc->tracks().size() == 3);
    CHECK(disc->tracks()[0].lba == 0);
    CHECK(disc->tracks()[0].sectors == 300);
    CHECK(!disc->tracks()[0].data);  // the synthetic disc puts an audio track first
    CHECK(disc->tracks()[1].lba == 300);
    CHECK(disc->tracks()[1].sectors == 60);
    CHECK(disc->tracks()[1].data);
    CHECK(disc->tracks()[2].lba == 45000);
    CHECK(disc->tracks()[2].sectors == 79);
    CHECK(disc->hd_track()->number == 3);
    CHECK(disc->leadout_lba() == 45079);
    // IP.BIN sits in the first sector of the game area.
    std::uint8_t sector[2048];
    REQUIRE(disc->read_user(45000, sector));
    CHECK_EQ(std::memcmp(sector, "SEGA SEGAKATANA ", 16), 0);
    // Primary volume descriptor at LBA 45016.
    REQUIRE(disc->read_user(45016, sector));
    CHECK(sector[0] == 1);
    CHECK_EQ(std::memcmp(sector + 1, "CD001", 5), 0);
    CHECK(!disc->read_user(45079, sector));  // past the last track
    CHECK(!disc->read_user(400, sector));    // gap between the areas
}

TEST_CASE("chd: the Katana TOC lists tracks per area with FADs") {
    std::string err;
    auto disc = open_disc(kFixture, err);
    REQUIRE(disc);
    std::uint32_t toc[102];
    disc->toc(0, toc);
    CHECK(toc[0] == (0x01u << 24 | 150u));             // track 1: audio, FAD 150
    CHECK(toc[1] == (0x41u << 24 | 450u));             // track 2: data, FAD 450
    CHECK(toc[2] == 0xFFFFFFFFu);                      // not in this area
    CHECK(toc[99] == (0x01u << 24 | (1u << 16)));      // first track 1
    CHECK(toc[100] == (0x41u << 24 | (2u << 16)));     // last track 2
    CHECK(toc[101] == (0x41u << 24 | (360u + 150u)));  // lead-out after track 2
    disc->toc(1, toc);
    CHECK(toc[0] == 0xFFFFFFFFu);
    CHECK(toc[2] == (0x41u << 24 | 45150u));
    CHECK(toc[99] == (0x41u << 24 | (3u << 16)));
    CHECK(toc[100] == (0x41u << 24 | (3u << 16)));
    CHECK(toc[101] == (0x41u << 24 | (45079u + 150u)));
}

TEST_CASE("disc: unsupported and missing images report errors") {
    std::string err;
    CHECK(open_disc("nothing.iso", err) == nullptr);
    CHECK(err.find("unsupported") != std::string::npos);
    CHECK(open_disc("/nonexistent/x.chd", err) == nullptr);
    CHECK(!err.empty());
}
