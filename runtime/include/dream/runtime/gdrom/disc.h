// Disc images for the GD-ROM HLE (WP2.6): GDI track sets and CHD (through libchdr), read at the
// sector level. Layout conventions are the ones tools/dcdisc worked out and tested on real dumps:
// chdman stores GD-ROM inter-track gaps as PAD frames at the end of the preceding track and CD
// audio big-endian; a GDI's high-density area starts at LBA 45000.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace dream::gdrom {

constexpr std::uint32_t kFadOffset = 150;    // FAD = LBA + 150
constexpr std::uint32_t kHdAreaLba = 45000;  // first LBA of a GD-ROM's high-density area
constexpr std::uint32_t kUserBytes = 2048;

struct Track {
    unsigned number = 0;
    std::uint32_t lba = 0;      // first sector
    std::uint32_t sectors = 0;  // genuine sectors (gaps excluded)
    std::uint32_t sector_size = 2352;
    bool data = false;
    std::uint32_t end_lba() const noexcept { return lba + sectors; }
};

enum class DiscType : std::uint32_t { CdRom = 0x10, CdRomXa = 0x20, GdRom = 0x80 };

class Disc {
public:
    virtual ~Disc() = default;
    const std::vector<Track>& tracks() const noexcept { return tracks_; }
    DiscType type() const noexcept { return type_; }
    const std::string& path() const noexcept { return path_; }
    const Track* track_for(std::uint32_t lba) const noexcept;
    const Track* hd_track() const noexcept;  // first data track of the game area
    std::uint32_t leadout_lba() const noexcept;

    // Stored bytes of one sector (track sector_size of them). False outside every track.
    virtual bool read_raw(std::uint32_t lba, std::uint8_t* out) = 0;
    // The 2048 user bytes of a data sector whatever the stored layout (mode 1 raw: offset 16).
    bool read_user(std::uint32_t lba, std::uint8_t* out2048);

    // Katana TOC as the GETTOC2 syscall returns it: 102 words; entries 0..98 per track
    // ((ctrl << 4 | adr) << 24 | FAD), 99 first track, 100 last track, 101 lead-out; 0xFFFFFFFF
    // where absent. Area 0 is the single-density session (tracks 1-2 on a GD-ROM), area 1 the
    // high-density one (tracks 3+).
    void toc(unsigned area, std::uint32_t out[102]) const;

protected:
    std::vector<Track> tracks_;
    DiscType type_ = DiscType::CdRom;
    std::string path_;
    void finish_tracks();  // sorts, sets the disc type from the layout
};

// Opens by extension: .gdi or .chd. Returns nullptr and sets `error` on failure.
std::unique_ptr<Disc> open_disc(const std::filesystem::path& path, std::string& error);

}  // namespace dream::gdrom
