// dream_sh4_runner: execute one translated test program with a given initial state and dump the
// final context in the oracle's JSON shape (tools/oracle), for the differential harness.
//
//   dream_sh4_runner --program p1_sum --image p1_sum.bin --base 0x8c010000 --reg r4=5 --out x.json
//
// All test programs are linked at the same base, so the entry is chosen by program name from the
// generated kPrograms table rather than through the address-keyed function table (which only sees
// the first registration for a colliding address).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "dream/runtime/fenv.h"
#include "dream/runtime/memory.h"
#include "dream/runtime/sh4/abi.h"
#include "dream/runtime/sh4/ctx.h"
#include "dream/runtime/sh4/ops.h"

#include "emitted_programs.h"

namespace {
constexpr std::uint32_t kStopPc = 0x8C00FFF0;

std::uint32_t parse(const char* s) {
    return static_cast<std::uint32_t>(std::strtoul(s, nullptr, 0));
}

void dump(FILE* o, const dream::sh4::Ctx& c, dream::BareMemory& m, std::uint32_t hash_addr,
          std::uint32_t hash_len) {
    std::fprintf(o, "{\n  \"steps\": 0,\n  \"pc\": \"0x%08x\",\n  \"stopped\": true,\n", kStopPc);
    std::fprintf(o, "  \"r\": [");
    for (int i = 0; i < 16; ++i) std::fprintf(o, "%s\"0x%08x\"", i ? ", " : "", c.r[i]);
    std::fprintf(o,
                 "],\n  \"sr\": \"0x%08x\", \"t\": %u, \"gbr\": \"0x%08x\", \"vbr\": \"0x%08x\", "
                 "\"pr\": \"0x%08x\",\n",
                 dream::sh4::read_sr(c), c.t, c.gbr, c.vbr, c.pr);
    std::fprintf(o,
                 "  \"mach\": \"0x%08x\", \"macl\": \"0x%08x\", \"fpul\": \"0x%08x\", \"fpscr\": "
                 "\"0x%08x\",\n",
                 c.mach, c.macl, c.fpul, c.fpscr);
    std::fprintf(o, "  \"fr\": [");
    for (int i = 0; i < 16; ++i)
        std::fprintf(o, "%s\"0x%08x\"", i ? ", " : "", dream::sh4::f2u(c.fr[i]));
    std::fprintf(o, "],\n  \"xf\": [");
    for (int i = 0; i < 16; ++i)
        std::fprintf(o, "%s\"0x%08x\"", i ? ", " : "", dream::sh4::f2u(c.xf[i]));
    std::fprintf(o, "]");
    if (hash_len) {
        std::uint64_t h = 1469598103934665603ull;
        for (std::uint32_t i = 0; i < hash_len; ++i) {
            h ^= m.read8(hash_addr + i);
            h *= 1099511628211ull;
        }
        std::fprintf(o, ",\n  \"mem_hash\": \"0x%016llx\",\n  \"mem\": [",
                     static_cast<unsigned long long>(h));
        for (std::uint32_t i = 0; i + 4 <= hash_len; i += 4)
            std::fprintf(o, "%s\"0x%08x\"", i ? ", " : "", m.read32(hash_addr + i));
        std::fprintf(o, "]");
    }
    std::fprintf(o, "\n}\n");
}
}  // namespace

int main(int argc, char** argv) {
    std::string image, out, program;
    std::uint32_t base = 0, entry = 0, fpscr = 0x00040001, hash_addr = 0, hash_len = 0;
    std::uint32_t fill_addr = 0, fill_len = 0, fill_seed = 0;
    dream::sh4::Ctx c{};
    c.r[15] = 0x8C00F400;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--image") && i + 1 < argc)
            image = argv[++i];
        else if (!std::strcmp(argv[i], "--program") && i + 1 < argc)
            program = argv[++i];
        else if (!std::strcmp(argv[i], "--base") && i + 1 < argc)
            base = parse(argv[++i]);
        else if (!std::strcmp(argv[i], "--entry") && i + 1 < argc)
            entry = parse(argv[++i]);
        else if (!std::strcmp(argv[i], "--fpscr") && i + 1 < argc)
            fpscr = parse(argv[++i]);
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc)
            out = argv[++i];
        else if (!std::strcmp(argv[i], "--hash") && i + 1 < argc) {
            std::string h = argv[++i];
            auto colon = h.find(':');
            hash_addr = parse(h.substr(0, colon).c_str());
            hash_len = parse(h.substr(colon + 1).c_str());
        } else if (!std::strcmp(argv[i], "--fill") && i + 1 < argc) {
            std::string f = argv[++i];
            auto c1 = f.find(':');
            auto c2 = f.find(':', c1 + 1);
            fill_addr = parse(f.substr(0, c1).c_str());
            fill_len = parse(f.substr(c1 + 1, c2 - c1 - 1).c_str());
            fill_seed = parse(f.substr(c2 + 1).c_str());
        } else if (!std::strcmp(argv[i], "--freg") && i + 1 < argc) {
            std::string r = argv[++i];
            auto eq = r.find('=');
            const unsigned n = static_cast<unsigned>(std::strtoul(r.c_str() + 2, nullptr, 10));
            std::string v = r.substr(eq + 1);
            if (!v.empty() && v.back() == 'f')
                c.fr[n & 15] = std::strtof(v.c_str(), nullptr);
            else
                c.fr[n & 15] = dream::sh4::u2f(parse(v.c_str()));
        } else if (!std::strcmp(argv[i], "--reg") && i + 1 < argc) {
            std::string r = argv[++i];
            auto eq = r.find('=');
            const unsigned n = static_cast<unsigned>(std::strtoul(r.c_str() + 1, nullptr, 10));
            c.r[n & 15] = parse(r.substr(eq + 1).c_str());
        } else {
            std::fprintf(stderr,
                         "usage: dream_sh4_runner --program NAME --image BIN --base ADDR [--entry "
                         "ADDR] [--reg rN=V]... [--fpscr V] [--hash ADDR:LEN] --out FILE\n");
            return 2;
        }
    }
    if (image.empty() || out.empty() || program.empty())
        return 2;
    if (!entry)
        entry = base;
    dream::BareMemory m;
    {
        std::ifstream in(image, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        for (std::size_t i = 0; i < bytes.size(); ++i)
            m.write8(base + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(bytes[i]));
    }
    // Same generator as tools/flycast/oracle/dream_oracle.cpp: every aligned word is an aligned
    // pointer into the region.
    for (std::uint32_t i = 0, x = fill_seed; i + 4 <= fill_len; i += 4) {
        x = x * 1664525u + 1013904223u;
        m.write32(fill_addr + i, fill_addr + ((x >> 8) % fill_len & ~3u));
    }
    c.sr = 0x700000F0;
    c.pc = entry;
    c.pr = kStopPc;
    dream::sh4::write_fpscr(c, fpscr);
    dream::sh4::GuestFn fn = nullptr;
    for (const auto& p : dream::gen::kPrograms)
        if (program == p.name)
            fn = p.entry;
    if (!fn) {
        std::fprintf(stderr, "unknown program %s\n", program.c_str());
        return 3;
    }
    try {
        dream::sh4::run_guest(c, m, fn);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "runtime fault: %s\n", e.what());
        return 4;
    }
    dream::fenv::reset_host();
    FILE* o = std::fopen(out.c_str(), "w");
    if (!o)
        return 5;
    dump(o, c, m, hash_addr, hash_len);
    std::fclose(o);
    return 0;
}
