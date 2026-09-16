// See state.h.
#include "dream/runtime/state/state.h"

#include <cstdio>

namespace dream::state {

namespace {

// Fixed-width fields in the header, so the header's size never depends on what a title is called.
constexpr std::size_t kGameIdBytes = 32, kSha1Bytes = 48, kNameBytes = 16;
// The header is written at a fixed size and its length is recorded in it, so a later version can
// grow it and this reader can still find the section table.
constexpr std::uint32_t kHeaderBytes = 8 + 4 + 4 + 8 + 8 + kGameIdBytes + kSha1Bytes + 4 + 8 + 8 +
                                       8 + 8 + 4 + 4;

void put(std::vector<std::uint8_t>& v, const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    v.insert(v.end(), b, b + n);
}
template <typename T>
void put_pod(std::vector<std::uint8_t>& v, const T& x) {
    put(v, &x, sizeof x);
}
void put_fixed(std::vector<std::uint8_t>& v, const std::string& s, std::size_t n) {
    std::vector<char> buf(n, '\0');
    std::memcpy(buf.data(), s.data(), s.size() < n ? s.size() : n - 1);
    put(v, buf.data(), n);
}
std::string get_fixed(const std::uint8_t* p, std::size_t n) {
    const auto* z = static_cast<const std::uint8_t*>(std::memchr(p, 0, n));
    return std::string(reinterpret_cast<const char*>(p), z ? static_cast<std::size_t>(z - p) : n);
}

}  // namespace

void Writer::begin(const char* name, std::uint32_t version, std::uint32_t flags) {
    // DREAM_STATE_TRACE=1 names each section as it starts. A save that dies inside a device's
    // save_state leaves a stack of inlined frames that says nothing about which device it was;
    // the last name printed does.
    static const bool trace = std::getenv("DREAM_STATE_TRACE") != nullptr;
    if (trace) {
        std::fprintf(stderr, "state: writing %s (payload %zu bytes so far)\n", name,
                     payload_.size());
        std::fflush(stderr);
    }
    sections_.push_back(SectionInfo{name, version, flags, payload_.size(), 0});
}

void Writer::bytes(const void* p, std::size_t n) {
    put(payload_, p, n);
    if (!sections_.empty())
        sections_.back().size = payload_.size() - sections_.back().offset;
}

void Writer::str(const std::string& s) {
    u32(static_cast<std::uint32_t>(s.size()));
    bytes(s.data(), s.size());
}

bool Writer::write(const std::string& path, const Provenance& prov, std::uint64_t cycles,
                   std::uint64_t frames, std::uint32_t flags, std::string& error) {
    const std::uint64_t table_bytes =
        static_cast<std::uint64_t>(sections_.size()) * (kNameBytes + 4 + 4 + 8 + 8);
    const std::uint64_t payload_base = kHeaderBytes + table_bytes;

    std::vector<std::uint8_t> head;
    head.reserve(payload_base);
    put(head, kMagic, sizeof kMagic);
    put_pod(head, kFormatVersion);
    put_pod(head, kHeaderBytes);
    put_pod(head, static_cast<std::uint64_t>(sections_.size()));
    put_pod(head, static_cast<std::uint64_t>(kHeaderBytes));  // section table offset
    put_fixed(head, prov.game_id, kGameIdBytes);
    put_fixed(head, prov.binary_sha1, kSha1Bytes);
    put_pod(head, prov.entry);
    put_pod(head, prov.functions);
    put_pod(head, prov.table_fingerprint);
    put_pod(head, cycles);
    put_pod(head, frames);
    put_pod(head, flags);
    put_pod(head, std::uint32_t{0});  // reserved
    for (const auto& s : sections_) {
        put_fixed(head, s.name, kNameBytes);
        put_pod(head, s.version);
        put_pod(head, s.flags);
        put_pod(head, payload_base + s.offset);
        put_pod(head, s.size);
    }

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        error = "cannot open " + path + " for writing";
        return false;
    }
    const bool wrote = std::fwrite(head.data(), 1, head.size(), f) == head.size() &&
                       std::fwrite(payload_.data(), 1, payload_.size(), f) == payload_.size();
    std::fclose(f);
    if (!wrote) {
        error = "short write to " + path;
        return false;
    }
    return true;
}

bool Reader::open(const std::string& path, const Provenance& expect, std::string& error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    file_.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
    const bool read = !file_.empty() && std::fread(file_.data(), 1, file_.size(), f) == file_.size();
    std::fclose(f);
    if (!read) {
        error = path + ": cannot read";
        return false;
    }
    if (file_.size() < kHeaderBytes || std::memcmp(file_.data(), kMagic, sizeof kMagic) != 0) {
        error = path + ": not a save state (bad magic)";
        return false;
    }
    const std::uint8_t* p = file_.data() + sizeof kMagic;
    std::uint32_t version = 0, header_bytes = 0;
    std::memcpy(&version, p, 4);
    p += 4;
    std::memcpy(&header_bytes, p, 4);
    p += 4;
    if (version != kFormatVersion) {
        error = path + ": save-state format version " + std::to_string(version) +
                ", this build writes and reads " + std::to_string(kFormatVersion);
        return false;
    }
    std::uint64_t count = 0, table_off = 0;
    std::memcpy(&count, p, 8);
    p += 8;
    std::memcpy(&table_off, p, 8);
    p += 8;
    prov_.game_id = get_fixed(p, kGameIdBytes);
    p += kGameIdBytes;
    prov_.binary_sha1 = get_fixed(p, kSha1Bytes);
    p += kSha1Bytes;
    std::memcpy(&prov_.entry, p, 4);
    p += 4;
    std::memcpy(&prov_.functions, p, 8);
    p += 8;
    std::memcpy(&prov_.table_fingerprint, p, 8);
    p += 8;
    std::memcpy(&cycles_, p, 8);
    p += 8;
    std::memcpy(&frames_, p, 8);
    p += 8;
    std::memcpy(&flags_, p, 4);

    // Provenance. Guest addresses in Ctx and in 16 MB of RAM only mean anything against the
    // function table they were captured with, so a mismatch is refused here rather than found
    // later as a fault with no visible cause.
    if (!expect.game_id.empty() && prov_.game_id != expect.game_id) {
        error = path + ": captured from '" + prov_.game_id + "', this build is '" + expect.game_id +
                "'";
        return false;
    }
    if (!expect.binary_sha1.empty() && !prov_.binary_sha1.empty() &&
        prov_.binary_sha1 != expect.binary_sha1) {
        error = path + ": captured from a different disc binary (" + prov_.binary_sha1 + ")";
        return false;
    }
    if (expect.table_fingerprint && prov_.table_fingerprint != expect.table_fingerprint) {
        char buf[224];
        std::snprintf(buf, sizeof buf,
                      "%s: captured from a different translation (%llu functions, fingerprint "
                      "%016llx); this build has %llu and %016llx. Rebuild changed the emitted "
                      "code, so the state's guest addresses no longer line up.",
                      path.c_str(), static_cast<unsigned long long>(prov_.functions),
                      static_cast<unsigned long long>(prov_.table_fingerprint),
                      static_cast<unsigned long long>(expect.functions),
                      static_cast<unsigned long long>(expect.table_fingerprint));
        error = buf;
        return false;
    }

    if (table_off + count * (kNameBytes + 24) > file_.size()) {
        error = path + ": section table runs past the end of the file";
        return false;
    }
    const std::uint8_t* t = file_.data() + table_off;
    for (std::uint64_t i = 0; i < count; ++i) {
        SectionInfo s;
        s.name = get_fixed(t, kNameBytes);
        t += kNameBytes;
        std::memcpy(&s.version, t, 4);
        t += 4;
        std::memcpy(&s.flags, t, 4);
        t += 4;
        std::memcpy(&s.offset, t, 8);
        t += 8;
        std::memcpy(&s.size, t, 8);
        t += 8;
        if (s.offset + s.size > file_.size()) {
            error = path + ": section '" + s.name + "' runs past the end of the file";
            return false;
        }
        sections_.push_back(std::move(s));
    }
    (void)header_bytes;
    return true;
}

bool Reader::seek(const char* name, std::uint32_t max_version) {
    for (const auto& s : sections_) {
        if (s.name != name)
            continue;
        if (s.version > max_version)
            return false;
        pos_ = s.offset;
        end_ = s.offset + s.size;
        section_version_ = s.version;
        ok_ = true;
        return true;
    }
    return false;
}

bool Reader::bytes(void* p, std::size_t n) {
    if (!ok_ || pos_ + n > end_) {
        ok_ = false;
        return false;
    }
    std::memcpy(p, file_.data() + pos_, n);
    pos_ += n;
    return true;
}

std::string Reader::str() {
    const std::uint32_t n = u32();
    if (!ok_ || pos_ + n > end_) {
        ok_ = false;
        return {};
    }
    std::string s(reinterpret_cast<const char*>(file_.data() + pos_), n);
    pos_ += n;
    return s;
}

std::string describe(const Reader& r) {
    char buf[256];
    std::uint64_t total = 0;
    for (const auto& s : r.sections()) total += s.size;
    std::snprintf(buf, sizeof buf,
                  "%s at frame %llu (%.3f guest s), %zu sections, %.1f MB%s",
                  r.provenance().game_id.c_str(), static_cast<unsigned long long>(r.frames()),
                  static_cast<double>(r.cycles()) / 200e6, r.sections().size(),
                  static_cast<double>(total) / (1024.0 * 1024.0),
                  (r.flags() & kFlagResumable) ? "" : ", capture pc not known resumable");
    return buf;
}

}  // namespace dream::state
