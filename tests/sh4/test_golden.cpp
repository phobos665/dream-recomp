// Golden replay: every file in tests/sh4/golden records a case's inputs and the Flycast
// interpreter's final state (tools/oracle/write_golden.py). Replaying them here, on every CI host,
// is the cross-ISA check of ADR 16 without needing the oracle core: a mismatch on x86-64 of a state
// captured on ARM64 is a build failure.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "dream/runtime/fenv.h"
#include "dream/runtime/memory.h"
#include "dream/runtime/sh4/abi.h"
#include "dream/runtime/sh4/ctx.h"
#include "dream/runtime/sh4/ops.h"
#ifdef DREAM_DEV_INTERPRETER
#include "dream/runtime/devinterp/interpreter.h"
#endif

#include "doctest.h"
#include "emitted_programs.h"

namespace {

struct Golden {
    std::string name, program, image;
    std::uint32_t base = 0, entry = 0, fpscr = 0x00040001;
    std::vector<std::pair<unsigned, std::uint32_t>> regs, fregs;
    std::uint32_t fill_addr = 0, fill_len = 0, fill_seed = 0, hash_addr = 0, hash_len = 0;
    std::uint32_t r[16] = {}, fr[16] = {}, xf[16] = {};
    std::uint32_t t = 0, gbr = 0, mach = 0, macl = 0, fpul = 0, fpscr_out = 0;
    std::vector<std::uint32_t> mem;
};

std::uint32_t hexv(const std::string& s) {
    return static_cast<std::uint32_t>(std::strtoul(s.c_str(), nullptr, 0));
}

Golden parse(const std::filesystem::path& path) {
    Golden g;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        auto read16 = [&](std::uint32_t* dst) {
            for (int i = 0; i < 16; ++i) {
                std::string v;
                ls >> v;
                dst[i] = hexv(v);
            }
        };
        std::string a, b, c;
        if (key == "name")
            ls >> g.name;
        else if (key == "program")
            ls >> g.program;
        else if (key == "image")
            ls >> g.image;
        else if (key == "base")
            ls >> a, g.base = hexv(a);
        else if (key == "entry")
            ls >> a, g.entry = hexv(a);
        else if (key == "fpscr")
            ls >> a, g.fpscr = hexv(a);
        else if (key == "reg")
            ls >> a >> b, g.regs.push_back({static_cast<unsigned>(std::stoul(a)), hexv(b)});
        else if (key == "freg")
            ls >> a >> b, g.fregs.push_back({static_cast<unsigned>(std::stoul(a)), hexv(b)});
        else if (key == "fill")
            ls >> a >> b >> c, g.fill_addr = hexv(a), g.fill_len = hexv(b), g.fill_seed = hexv(c);
        else if (key == "hash")
            ls >> a >> b, g.hash_addr = hexv(a), g.hash_len = hexv(b);
        else if (key == "r")
            read16(g.r);
        else if (key == "fr")
            read16(g.fr);
        else if (key == "xf")
            read16(g.xf);
        else if (key == "t")
            ls >> a, g.t = hexv(a);
        else if (key == "gbr")
            ls >> a, g.gbr = hexv(a);
        else if (key == "mach")
            ls >> a, g.mach = hexv(a);
        else if (key == "macl")
            ls >> a, g.macl = hexv(a);
        else if (key == "fpul")
            ls >> a, g.fpul = hexv(a);
        else if (key == "fpscr_out")
            ls >> a, g.fpscr_out = hexv(a);
        else if (key == "mem")
            while (ls >> a) g.mem.push_back(hexv(a));
    }
    return g;
}

// How a golden is executed. Translated: the emitted function. With the dev interpreter, the same
// program is also run purely interpreted from the loaded image, and in hybrid mode where calls into
// translated functions are made natively; all three must reproduce the oracle's state, which is the
// interpreter's correctness argument (docs/runtime-devinterp.md).
enum class Mode { Translated, Interpreted, Hybrid };

const char* mode_name(Mode md) {
    switch (md) {
        case Mode::Translated:
            return "translated";
        case Mode::Interpreted:
            return "interpreted";
        default:
            return "hybrid";
    }
}

void replay(const Golden& g, const std::filesystem::path& image, dream::sh4::GuestFn fn, Mode md) {
    dream::BareMemory m;
    {
        std::ifstream in(image, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        REQUIRE(!bytes.empty());
        for (std::size_t i = 0; i < bytes.size(); ++i)
            m.write8(g.base + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(bytes[i]));
    }
    // Same generator as tests/sh4/runner.cpp and the oracle.
    for (std::uint32_t i = 0, x = g.fill_seed; i + 4 <= g.fill_len; i += 4) {
        x = x * 1664525u + 1013904223u;
        m.write32(g.fill_addr + i, g.fill_addr + ((x >> 8) % g.fill_len & ~3u));
    }
    dream::sh4::Ctx c{};
    c.sr = 0x700000F0;
    c.r[15] = 0x8C00F400;
    for (const auto& [n, v] : g.regs) c.r[n & 15] = v;
    for (const auto& [n, v] : g.fregs) c.fr[n & 15] = dream::sh4::u2f(v);
    c.pc = g.entry;
    c.pr = 0x8C00FFF0;
    dream::sh4::write_fpscr(c, g.fpscr);
    if (md == Mode::Translated) {
        dream::sh4::run_guest(c, m, fn);
    } else {
#ifdef DREAM_DEV_INTERPRETER
        dream::devinterp::Options opt;
        opt.call_translated = md == Mode::Hybrid;
        opt.catch_nonlocal = true;
        opt.max_instructions = 50'000'000;
        const auto stop = dream::devinterp::run(c, m, c.pr, opt);
        CHECK(stop == dream::devinterp::Stop::Returned);
#else
        (void)fn;
        return;
#endif
    }
    dream::fenv::reset_host();
    // The virtual clock must not depend on how the code ran: the interpreter mirrors the emitter's
    // cycle accounting, so every mode ends at the translated run's cycle count.
    static std::map<std::string, std::uint64_t> translated_cycles;
    if (md == Mode::Translated)
        translated_cycles[g.name] = c.cycles;
    else if (auto it = translated_cycles.find(g.name); it != translated_cycles.end())
        CHECK_MESSAGE(c.cycles == it->second, "cycle count differs from the translated run");

    for (int i = 0; i < 16; ++i) {
        CAPTURE(i);
        CHECK(c.r[i] == g.r[i]);
        CHECK(dream::sh4::f2u(c.fr[i]) == g.fr[i]);
        CHECK(dream::sh4::f2u(c.xf[i]) == g.xf[i]);
    }
    CHECK(c.t == g.t);
    CHECK(c.gbr == g.gbr);
    CHECK(c.mach == g.mach);
    CHECK(c.macl == g.macl);
    CHECK(c.fpul == g.fpul);
    CHECK(c.fpscr == g.fpscr_out);
    for (std::size_t i = 0; i < g.mem.size(); ++i) {
        CAPTURE(i);
        CHECK(m.read32(g.hash_addr + 4 * static_cast<std::uint32_t>(i)) == g.mem[i]);
    }
}

}  // namespace

TEST_CASE("golden replay: every captured oracle state is reproduced") {
    const std::filesystem::path dir = DREAM_SH4_GOLDEN_DIR;
    const std::filesystem::path repo = DREAM_REPO_DIR;
    int replayed = 0, skipped = 0;
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir))
        if (e.path().extension() == ".golden")
            files.push_back(e.path());
    std::sort(files.begin(), files.end());
    REQUIRE(!files.empty());
    std::vector<Mode> modes{Mode::Translated};
#ifdef DREAM_DEV_INTERPRETER
    modes.push_back(Mode::Interpreted);
    modes.push_back(Mode::Hybrid);
#endif
    for (const auto& path : files) {
        const Golden g = parse(path);
        CAPTURE(g.name);
        const std::filesystem::path image = repo / g.image;
        if (!std::filesystem::exists(image)) {
            ++skipped;  // owner-supplied game image: not available on CI
            continue;
        }
        dream::sh4::GuestFn fn = nullptr;
        for (const auto& p : dream::gen::kPrograms)
            if (g.program == p.name)
                fn = p.entry;
        REQUIRE_MESSAGE(fn != nullptr, "program not linked into the test: " << g.program);
        for (Mode md : modes) {
            CAPTURE(mode_name(md));
            replay(g, image, fn, md);
        }
        ++replayed;
    }
    MESSAGE("replayed " << replayed << " goldens in " << modes.size() << " mode(s), skipped "
                        << skipped << " (image not present)");
    CHECK(replayed > 0);
}
