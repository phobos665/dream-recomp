#include "dream/runtime/gdrom/disc.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include <libchdr/chd.h>

namespace dream::gdrom {

// ---- Disc --------------------------------------------------------------------------------------

const Track* Disc::track_for(std::uint32_t lba) const noexcept {
    for (const auto& t : tracks_)
        if (lba >= t.lba && lba < t.end_lba())
            return &t;
    return nullptr;
}

const Track* Disc::hd_track() const noexcept {
    for (const auto& t : tracks_)
        if (t.data && t.lba >= kHdAreaLba)
            return &t;
    for (const auto& t : tracks_)
        if (t.data)
            return &t;
    return nullptr;
}

std::uint32_t Disc::leadout_lba() const noexcept {
    return tracks_.empty() ? 0 : tracks_.back().end_lba();
}

void Disc::finish_tracks() {
    std::sort(tracks_.begin(), tracks_.end(),
              [](const Track& a, const Track& b) { return a.number < b.number; });
    bool hd = false;
    for (const auto& t : tracks_) hd |= t.lba >= kHdAreaLba;
    type_ = hd ? DiscType::GdRom : DiscType::CdRomXa;
}

bool Disc::read_user(std::uint32_t lba, std::uint8_t* out) {
    const Track* t = track_for(lba);
    if (!t)
        return false;
    std::uint8_t raw[2352];
    if (!read_raw(lba, raw))
        return false;
    switch (t->sector_size) {
        case 2048:
            std::memcpy(out, raw, kUserBytes);
            return true;
        case 2352:
            std::memcpy(out, raw + 16, kUserBytes);
            return true;  // mode 1: sync 12, header 4
        case 2336:
            std::memcpy(out, raw + 8, kUserBytes);
            return true;  // mode 2 form 1 without sync
        default:
            return false;
    }
}

void Disc::toc(unsigned area, std::uint32_t out[102]) const {
    for (int i = 0; i < 102; ++i) out[i] = 0xFFFFFFFFu;
    if (tracks_.empty())
        return;
    const bool gd = type_ == DiscType::GdRom;
    if (area == 1 && !gd)
        return;
    std::size_t first = 1, last = tracks_.size();
    if (area == 1)
        first = 3;
    else if (gd)
        last = 2;
    if (first > tracks_.size() || last > tracks_.size())
        return;
    auto entry = [](const Track& t, std::uint32_t low24) {
        const std::uint32_t ctrl_adr = (t.data ? 0x4u : 0x0u) << 4 | 1u;
        return (ctrl_adr << 24) | (low24 & 0xFFFFFFu);
    };
    for (std::size_t i = first; i <= last; ++i)
        out[i - 1] = entry(tracks_[i - 1], tracks_[i - 1].lba + kFadOffset);
    out[99] = entry(tracks_[first - 1], static_cast<std::uint32_t>(first) << 16);
    out[100] = entry(tracks_[last - 1], static_cast<std::uint32_t>(last) << 16);
    const std::uint32_t leadout =
        (gd && area == 0) ? tracks_[1].end_lba() + kFadOffset : leadout_lba() + kFadOffset;
    Track lo;
    lo.data = true;
    out[101] = entry(lo, leadout);
}

// ---- GDI ---------------------------------------------------------------------------------------

namespace {

class GdiDisc final : public Disc {
public:
    bool open(const std::filesystem::path& path, std::string& error) {
        path_ = path.string();
        std::ifstream in(path);
        if (!in) {
            error = "cannot open " + path_;
            return false;
        }
        std::string line;
        if (!std::getline(in, line)) {
            error = "empty GDI";
            return false;
        }
        const auto dir = path.parent_path();
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            unsigned number = 0, lba = 0, type = 0, ssize = 0;
            std::string file;
            std::uint64_t offset = 0;
            if (!(ls >> number >> lba >> type >> ssize))
                continue;
            // The file name may be quoted and contain spaces.
            ls >> std::ws;
            if (ls.peek() == '"') {
                ls.get();
                std::getline(ls, file, '"');
            } else {
                ls >> file;
            }
            ls >> offset;
            const auto fpath = dir / file;
            std::error_code ec;
            const auto fsize = std::filesystem::file_size(fpath, ec);
            if (ec) {
                error = "GDI track file missing: " + fpath.string();
                return false;
            }
            Track t;
            t.number = number;
            t.lba = lba;
            t.sector_size = ssize;
            t.sectors = static_cast<std::uint32_t>((fsize - offset) / ssize);
            t.data = type == 4;
            tracks_.push_back(t);
            files_[number] = {fpath.string(), offset};
        }
        if (tracks_.empty()) {
            error = "GDI lists no tracks";
            return false;
        }
        finish_tracks();
        return true;
    }

    bool read_raw(std::uint32_t lba, std::uint8_t* out) override {
        const Track* t = track_for(lba);
        if (!t)
            return false;
        auto& f = files_[t->number];
        if (!f.handle) {
            f.handle = std::fopen(f.path.c_str(), "rb");
            if (!f.handle)
                return false;
        }
        const std::uint64_t pos =
            f.offset + static_cast<std::uint64_t>(lba - t->lba) * t->sector_size;
        if (std::fseek(f.handle, static_cast<long>(pos), SEEK_SET) != 0)
            return false;
        return std::fread(out, 1, t->sector_size, f.handle) == t->sector_size;
    }

    ~GdiDisc() override {
        for (auto& [n, f] : files_)
            if (f.handle)
                std::fclose(f.handle);
    }

private:
    struct File {
        std::string path;
        std::uint64_t offset = 0;
        FILE* handle = nullptr;
    };
    std::map<unsigned, File> files_;
};

// ---- CHD ---------------------------------------------------------------------------------------

constexpr std::uint32_t kFrameBytes = 2448;  // 2352 sector + 96 subchannel
constexpr std::uint32_t kTrackPadding = 4;   // chdman pads each track to a multiple of 4 frames

class ChdDisc final : public Disc {
public:
    struct ChdTrack {
        Track track;
        std::uint32_t chd_frame = 0;  // frame index of the track's first sector in the CHD
    };

    bool open(const std::filesystem::path& path, std::string& error) {
        path_ = path.string();
        const chd_error err = chd_open(path_.c_str(), CHD_OPEN_READ, nullptr, &chd_);
        if (err != CHDERR_NONE) {
            error = std::string("chd_open: ") + chd_error_string(err);
            return false;
        }
        const chd_header* h = chd_get_header(chd_);
        hunk_bytes_ = h->hunkbytes;
        frames_per_hunk_ = hunk_bytes_ / kFrameBytes;
        if (frames_per_hunk_ == 0) {
            error = "CHD hunk smaller than one CD frame";
            return false;
        }
        hunk_buf_.resize(hunk_bytes_);
        return parse_tracks(error);
    }

    bool read_raw(std::uint32_t lba, std::uint8_t* out) override {
        for (const auto& ct : layout_) {
            const Track& t = ct.track;
            if (lba < t.lba || lba >= t.end_lba())
                continue;
            const std::uint32_t frame = ct.chd_frame + (lba - t.lba);
            const std::uint8_t* f = frame_ptr(frame);
            if (!f)
                return false;
            std::memcpy(out, f, t.sector_size);
            if (!t.data)  // chdman stores audio big-endian; the disc is little-endian
                for (std::uint32_t i = 0; i + 1 < t.sector_size; i += 2)
                    std::swap(out[i], out[i + 1]);
            return true;
        }
        return false;
    }

    ~ChdDisc() override {
        if (chd_)
            chd_close(chd_);
    }

private:
    bool parse_tracks(std::string& error) {
        struct Meta {
            unsigned track = 0;
            std::string type, subtype, pgtype;
            std::uint32_t frames = 0, pad = 0, pregap = 0, postgap = 0;
            bool gd = false;
        };
        std::vector<Meta> metas;
        const std::uint32_t tags[] = {CDROM_TRACK_METADATA_TAG, CDROM_TRACK_METADATA2_TAG,
                                      GDROM_OLD_METADATA_TAG, GDROM_TRACK_METADATA_TAG};
        char buf[512];
        for (std::uint32_t tag : tags) {
            for (std::uint32_t idx = 0;; ++idx) {
                std::uint32_t len = 0, rtag = 0;
                std::uint8_t flags = 0;
                if (chd_get_metadata(chd_, tag, idx, buf, sizeof buf - 1, &len, &rtag, &flags) !=
                    CHDERR_NONE)
                    break;
                buf[std::min<std::uint32_t>(len, sizeof buf - 1)] = 0;
                Meta m;
                m.gd = tag == GDROM_OLD_METADATA_TAG || tag == GDROM_TRACK_METADATA_TAG;
                std::istringstream ts(buf);
                std::string tok;
                while (ts >> tok) {
                    const auto colon = tok.find(':');
                    if (colon == std::string::npos)
                        continue;
                    const std::string k = tok.substr(0, colon), v = tok.substr(colon + 1);
                    if (k == "TRACK")
                        m.track = static_cast<unsigned>(std::stoul(v));
                    else if (k == "TYPE")
                        m.type = v;
                    else if (k == "SUBTYPE")
                        m.subtype = v;
                    else if (k == "FRAMES")
                        m.frames = static_cast<std::uint32_t>(std::stoul(v));
                    else if (k == "PAD")
                        m.pad = static_cast<std::uint32_t>(std::stoul(v));
                    else if (k == "PREGAP")
                        m.pregap = static_cast<std::uint32_t>(std::stoul(v));
                    else if (k == "PGTYPE")
                        m.pgtype = v;
                    else if (k == "POSTGAP")
                        m.postgap = static_cast<std::uint32_t>(std::stoul(v));
                }
                if (m.track)
                    metas.push_back(m);
            }
        }
        if (metas.empty()) {
            error = "CHD has no CD track metadata";
            return false;
        }
        std::sort(metas.begin(), metas.end(),
                  [](const Meta& a, const Meta& b) { return a.track < b.track; });
        std::uint32_t chd_frame = 0, lba = 0;
        for (const auto& m : metas) {
            // A pregap whose type starts with 'V' has its frames stored in the CHD; otherwise it is
            // implied and only shifts the LBA. GDI-derived images have neither.
            const bool stored_pregap = !m.pgtype.empty() && m.pgtype[0] == 'V';
            if (!stored_pregap)
                lba += m.pregap;
            ChdTrack ct;
            ct.track.number = m.track;
            ct.track.lba = lba;
            ct.track.sectors = m.frames > m.pad ? m.frames - m.pad : 0;  // PAD frames are the gap
            ct.track.data = m.type != "AUDIO";
            ct.track.sector_size = m.type == "AUDIO" || m.type.find("RAW") != std::string::npos ||
                                           m.type.find("2352") != std::string::npos
                                       ? 2352
                                       : (m.type.find("2336") != std::string::npos ? 2336 : 2048);
            ct.chd_frame = chd_frame + (stored_pregap ? m.pregap : 0);
            layout_.push_back(ct);
            tracks_.push_back(ct.track);
            const std::uint32_t padded =
                (m.frames + kTrackPadding - 1) / kTrackPadding * kTrackPadding;
            chd_frame += padded;
            lba += m.frames + m.postgap;
        }
        finish_tracks();
        return true;
    }

    const std::uint8_t* frame_ptr(std::uint32_t frame) {
        const std::uint32_t hunk = frame / frames_per_hunk_;
        if (hunk != cached_hunk_) {
            if (chd_read(chd_, hunk, hunk_buf_.data()) != CHDERR_NONE)
                return nullptr;
            cached_hunk_ = hunk;
        }
        return hunk_buf_.data() + static_cast<std::size_t>(frame % frames_per_hunk_) * kFrameBytes;
    }

    chd_file* chd_ = nullptr;
    std::uint32_t hunk_bytes_ = 0, frames_per_hunk_ = 0;
    std::uint32_t cached_hunk_ = 0xFFFFFFFFu;
    std::vector<std::uint8_t> hunk_buf_;
    std::vector<ChdTrack> layout_;
};

}  // namespace

std::unique_ptr<Disc> open_disc(const std::filesystem::path& path, std::string& error) {
    std::string ext = path.extension().string();
    for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ext == ".gdi") {
        auto d = std::make_unique<GdiDisc>();
        return d->open(path, error) ? std::unique_ptr<Disc>(std::move(d)) : nullptr;
    }
    if (ext == ".chd") {
        auto d = std::make_unique<ChdDisc>();
        return d->open(path, error) ? std::unique_ptr<Disc>(std::move(d)) : nullptr;
    }
    error = "unsupported disc image type: " + ext;
    return nullptr;
}

}  // namespace dream::gdrom
