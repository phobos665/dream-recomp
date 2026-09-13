#include <string>

#include "dream/translator/config/game_config.h"

#include "doctest.h"

using dream::translator::GameConfig;
using dream::translator::parse_game_config;

namespace {
const char* kSample = R"(
[game]
id = "crazytaxi"
title = "Crazy Taxi"
region = "USA"

[disc]
sha1_1st_read = "abc"

[binary]
path = "extracted/fs/1ST_READ.BIN"
load_address = 0x8C010000
link_address = 0x0C010000
entry = 0x8C010000

[[relocations]]
source = 0x0C010100
size = 0x3F00
dest = 0x8C004000
entry = 0x8C004000

[functions]
extra = [0x0C08538C, 0x0C170C1A]
exclude = [0x0C08F4EA]
symbols = "symbols.tsv"
sweep = false

[hle]
"0x0C080684" = "libc.bfslu"

[hooks]
"0x0C010000" = "boot"
)";
}  // namespace

TEST_CASE("game config: the documented schema loads with resolved paths") {
    GameConfig c;
    std::string err;
    REQUIRE_MESSAGE(parse_game_config(kSample, "/games/crazytaxi", c, err), err);
    CHECK(c.id == "crazytaxi");
    CHECK(c.title == "Crazy Taxi");
    CHECK(c.region == "USA");
    CHECK(c.sha1_1st_read == "abc");
    CHECK(c.binary_path == std::filesystem::path("/games/crazytaxi/extracted/fs/1ST_READ.BIN"));
    CHECK(c.load_address == 0x8C010000u);
    CHECK(c.link_address == 0x0C010000u);
    CHECK(c.entry == 0x8C010000u);
    REQUIRE(c.relocations.size() == 1);
    CHECK(c.relocations[0].source == 0x0C010100u);
    CHECK(c.relocations[0].size == 0x3F00u);
    CHECK(c.relocations[0].dest == 0x8C004000u);
    CHECK(c.relocations[0].entries == std::vector<std::uint32_t>{0x8C004000u});
    CHECK(c.extra_functions == std::vector<std::uint32_t>{0x0C08538Cu, 0x0C170C1Au});
    CHECK(c.exclude == std::vector<std::uint32_t>{0x0C08F4EAu});
    REQUIRE(c.symbols.has_value());
    CHECK(*c.symbols == std::filesystem::path("/games/crazytaxi/symbols.tsv"));
    CHECK(c.pointers == true);
    CHECK(c.sweep == false);
    CHECK(c.hle.at(0x0C080684u) == "libc.bfslu");
    CHECK(c.hooks.at(0x0C010000u) == "boot");
}

TEST_CASE("game config: link address defaults to the load address") {
    GameConfig c;
    std::string err;
    REQUIRE(parse_game_config(R"(
[game]
id = "x"
title = "X"
[binary]
path = "1ST_READ.BIN"
load_address = 0x8C010000
entry = 0x8C010000
)",
                              ".", c, err));
    CHECK(c.link_address == 0x8C010000u);
    CHECK(c.relocations.empty());
    CHECK(!c.symbols.has_value());
}

TEST_CASE("game config: validation names the offending field") {
    GameConfig c;
    std::string err;
    CHECK(!parse_game_config("[game]\nid = \"x\"\ntitle = \"X\"\n", ".", c, err));
    CHECK(err.find("binary.path") != std::string::npos);
    CHECK(!parse_game_config(R"(
[game]
id = "x"
title = "X"
[binary]
path = "a"
load_address = 0x8C010000
link_address = 0x0C020000
entry = 0x8C010000
)",
                             ".", c, err));
    CHECK(err.find("alias the same RAM") != std::string::npos);
    CHECK(!parse_game_config(R"(
[game]
id = "bad id"
title = "X"
[binary]
path = "a"
load_address = 0x8C010000
entry = 0x8C010000
)",
                             ".", c, err));
    CHECK(err.find("game.id") != std::string::npos);
    CHECK(!parse_game_config("[game\n", ".", c, err));
    CHECK(err.find("TOML parse error") != std::string::npos);
}

TEST_CASE("game config: relocation overlay and no_fold flags default off and parse") {
    GameConfig c;
    std::string err;
    REQUIRE_MESSAGE(parse_game_config(kSample, "/games/crazytaxi", c, err), err);
    REQUIRE(c.relocations.size() == 1);
    CHECK_FALSE(c.relocations[0].no_fold);
    CHECK_FALSE(c.relocations[0].overlay);
    REQUIRE(parse_game_config(R"(
[game]
id = "x"
title = "X"
[binary]
path = "x.bin"
load_address = 0x8C010000
entry = 0x8C010000
[[relocations]]
source = 0x8C010D40
size = 0x20
dest = 0x8C00FA00
entries = [0x8C00FA00]
no_fold = true
overlay = true
[[relocations]]
source = 0x8C010D20
size = 0x10
dest = 0x8C00FA00
overlay = true
)",
                              ".", c, err));
    REQUIRE(c.relocations.size() == 2);
    CHECK(c.relocations[0].no_fold);
    CHECK(c.relocations[0].overlay);
    CHECK_FALSE(c.relocations[1].no_fold);
    CHECK(c.relocations[1].overlay);
}
