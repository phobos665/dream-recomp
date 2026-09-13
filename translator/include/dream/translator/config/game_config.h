// Per-game configuration (ADR 14): one TOML file under games/<id>/ describing the disc, the
// executable's load and link addresses, relocated regions, extra discovery seeds, symbol names and
// HLE/hook overrides. Schema in docs/game-config.md.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dream::translator {

struct Relocation {
    std::uint32_t source = 0;  // link-space address of the first byte copied
    std::uint32_t size = 0;
    std::uint32_t dest = 0;              // address the copy executes at
    std::vector<std::uint32_t> entries;  // discovery seeds inside the copy (`entry` or `entries`)
    bool no_fold =
        false;  // read every literal at run time: the copy is a template the program patches
    bool overlay = false;  // other code occupies `dest` at other times: dispatch checks the bytes
};

struct GameConfig {
    std::filesystem::path dir;  // directory of the config file; relative paths resolve against it

    std::string id, title, region, product;
    std::string sha1_1st_read, chd_sha1;
    std::optional<std::filesystem::path>
        disc_image;  // [disc] image, resolved; the GDI/CHD to mount

    std::filesystem::path binary_path;  // resolved
    std::uint32_t load_address = 0, link_address = 0, entry = 0;

    std::vector<Relocation> relocations;
    std::vector<std::uint32_t> extra_functions, exclude;
    std::optional<std::filesystem::path> symbols;  // resolved
    bool pointers = true, sweep = true;

    std::map<std::uint32_t, std::string> hle;    // guest address -> runtime handler name
    std::map<std::uint32_t, std::string> hooks;  // guest address -> hook name
};

// Loads and validates. On failure returns false and sets `error` to one line naming the field.
bool load_game_config(const std::filesystem::path& path, GameConfig& out, std::string& error);

// Parses from text (for tests and tools); `dir` is what relative paths resolve against.
bool parse_game_config(const std::string& text, const std::filesystem::path& dir, GameConfig& out,
                       std::string& error);

}  // namespace dream::translator
