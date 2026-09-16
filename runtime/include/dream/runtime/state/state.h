// Save states: the container format, and the reader and writer every subsystem serialises through.
//
// Why a recompiler needs its own answer here. An emulator's machine state is a struct: save the
// struct, reload it, carry on. A statically recompiled title has no such struct, because the
// guest's call chain *is* the host call chain -- translated functions call translated functions,
// and at an arbitrary instant that chain lives in host stack frames nothing can serialise.
//
// What makes it possible anyway is a mechanism this runtime already had to build for cooperative
// task switching: sh4::nonlocal_return and sh4::resume_at reconstruct execution from (Ctx, memory)
// alone, one guest frame at a time, because Katana's kernel abandons host frames on every context
// switch. A save state is the same trick applied deliberately: capture at an instant where Ctx.pc
// is exact, and on load re-enter through the same door. See docs/design/save-states.md.
//
// The format is sectioned and versioned from its first write. A state is a 26 MB file that takes a
// play session to produce; the cost of discovering later that it is an undocumented memory dump is
// the session, so the header pays for itself immediately. A reader may skip a section it does not
// know (flag kOptional), and must refuse one it knows but whose version it does not.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dream::state {

inline constexpr char kMagic[8] = {'D', 'R', 'M', 'S', 'T', 'A', 'T', 'E'};
// Bumped only for a change to the container itself (header or section-table layout). A change to
// what a section contains bumps that section's own version instead, which is what lets one build
// read a state another wrote with a different set of sections.
inline constexpr std::uint32_t kFormatVersion = 1;

// Header flags.
// The state was captured by a build with the development interpreter linked in. A resume pc that
// is not a translated block start falls back to the interpreter, so a release build cannot
// necessarily continue from a state a development build wrote: refuse it rather than fault.
inline constexpr std::uint32_t kFlagDevInterpreter = 1u << 0;
// The capture instant was verified resumable: a translated function contains Ctx.pc and has a
// resume entry. Without it the load depends on the interpreter fallback.
inline constexpr std::uint32_t kFlagResumable = 1u << 1;

// Section flags.
inline constexpr std::uint32_t kOptional = 1u << 0;  // a reader that does not know it may skip it

// What produced a state. A state carries it so that loading one into a build whose translation
// differs fails with a sentence rather than executing garbage: the guest addresses in Ctx and in
// 16 MB of guest RAM only mean anything against the function table they were captured with.
struct Provenance {
    std::string game_id;        // [game] id from the title's TOML
    std::string binary_sha1;    // [binary] sha1_1st_read: which disc build this is
    std::uint32_t entry = 0;    // [binary] entry
    std::uint64_t functions = 0;        // how many translations the build registered
    std::uint64_t table_fingerprint = 0;  // a hash over their (address, end) pairs
};

struct SectionInfo {
    std::string name;
    std::uint32_t version = 1;
    std::uint32_t flags = 0;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

// Builds a state in memory, then writes it. Sections are appended in any order; the table is
// written first so a reader can find them without scanning.
class Writer {
public:
    // Opens a section. Everything written until the next begin() or to file() belongs to it.
    void begin(const char* name, std::uint32_t version = 1, std::uint32_t flags = 0);
    void bytes(const void* p, std::size_t n);
    template <typename T>
    void pod(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>, "pod() is for trivially copyable types");
        bytes(&v, sizeof v);
    }
    void u32(std::uint32_t v) { pod(v); }
    void u64(std::uint64_t v) { pod(v); }
    void str(const std::string& s);  // u32 length then the bytes

    // Writes the file. Returns false and sets `error` on any I/O failure.
    bool write(const std::string& path, const Provenance& prov, std::uint64_t cycles,
               std::uint64_t frames, std::uint32_t flags, std::string& error);

    std::uint64_t section_bytes() const noexcept { return payload_.size(); }

private:
    std::vector<SectionInfo> sections_;
    std::vector<std::uint8_t> payload_;
};

// Reads a state. Sections are looked up by name; one that is absent simply does not run, which is
// how a state written by a build with fewer sections still loads.
class Reader {
public:
    // Reads and validates the header. Returns false and sets `error` when the file is not a state,
    // is a container version this build does not know, or does not match `expect`.
    bool open(const std::string& path, const Provenance& expect, std::string& error);

    // Positions at a section. Returns false when it is not present.
    bool seek(const char* name, std::uint32_t max_version = ~0u);
    // The version of the section seek() last found.
    std::uint32_t section_version() const noexcept { return section_version_; }

    // All of these return false once anything has run past the end of the current section, and
    // leave the destination untouched; check ok() after a group of reads rather than each one.
    bool bytes(void* p, std::size_t n);
    template <typename T>
    bool pod(T& v) {
        static_assert(std::is_trivially_copyable_v<T>, "pod() is for trivially copyable types");
        return bytes(&v, sizeof v);
    }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        pod(v);
        return v;
    }
    std::uint64_t u64() {
        std::uint64_t v = 0;
        pod(v);
        return v;
    }
    std::string str();
    bool ok() const noexcept { return ok_; }
    std::uint64_t remaining() const noexcept { return end_ - pos_; }

    const Provenance& provenance() const noexcept { return prov_; }
    std::uint64_t cycles() const noexcept { return cycles_; }
    std::uint64_t frames() const noexcept { return frames_; }
    std::uint32_t flags() const noexcept { return flags_; }
    const std::vector<SectionInfo>& sections() const noexcept { return sections_; }

private:
    std::vector<std::uint8_t> file_;
    std::vector<SectionInfo> sections_;
    Provenance prov_;
    std::uint64_t cycles_ = 0, frames_ = 0;
    std::uint32_t flags_ = 0, section_version_ = 0;
    std::uint64_t pos_ = 0, end_ = 0;
    bool ok_ = true;
};

// A one-line summary of a state's header, for the launcher to print on load and for `--load-state`
// to name in a refusal.
std::string describe(const Reader& r);

}  // namespace dream::state
