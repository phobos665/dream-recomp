// What is on a memory card, read from the card's own file system: the free space, and one entry
// per file with the description and icon the console's file manager would show.
//
// Read-only and from the 128 KB image alone, so a settings screen can list a card without the
// title's help. The layout is the one MemoryCard::format() writes (docs/vmu-creation-study.md):
// the system block at 255, the allocation table at 254, the directory growing down from 253.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dream::maple {

struct VmuFile {
    std::string name;         // the twelve-character directory name, trailing spaces dropped
    std::string description;  // the file header's short description, printable ASCII only
    bool game = false;        // a mini-game for the card itself rather than a save
    unsigned blocks = 0;
    // The first frame of the file's icon, 32x32, RGBA with red in the low byte. Empty when the
    // file carries no icon or its header could not be read.
    std::vector<std::uint32_t> icon;
};

struct VmuContents {
    bool formatted = false;
    unsigned free_blocks = 0;  // of the save area
    unsigned user_blocks = 0;  // the save area's size, 200 on a standard card
    std::vector<VmuFile> files;
};

constexpr unsigned kVmuIconSize = 32;

// Anything malformed -- a bad chain, an out-of-range block -- ends that file's reading rather than
// the listing: a damaged save is still worth seeing in the list.
VmuContents read_vmu(const std::vector<std::uint8_t>& image);

}  // namespace dream::maple
