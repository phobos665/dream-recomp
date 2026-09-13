// dream-recomp oracle entry point (local patch to the Flycast tree, GPL-2.0 like the rest).
//
// Exposes Flycast's SH-4 interpreter as a headless oracle: load a raw image into guest RAM, set the
// registers (r0-r15 and, when fr_in is non-null, fr0-fr15 as bit patterns), single-step until the PC reaches a stop address, and dump the context as JSON. The
// differential harness (dream-recomp WP1.5) runs the same program through the recompiled code and
// compares the two dumps. Setup mirrors tests/src/Sh4InterpreterTest.cpp.
#include <cfenv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "emulator.h"
#include "hw/mem/addrspace.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_mem.h"

extern "C" __attribute__((visibility("default"))) int dream_oracle_run(
    const char* image_path, std::uint32_t image_base, std::uint32_t entry, std::uint32_t stop_pc,
    const std::uint32_t* regs_in, const std::uint32_t* fr_in, std::uint32_t fpscr_in, std::uint64_t max_steps,
    const char* dump_path, std::uint32_t hash_addr, std::uint32_t hash_len,
    std::uint32_t fill_addr, std::uint32_t fill_len, std::uint32_t fill_seed)
{
    // Flycast programs the host rounding/denormal mode from FPSCR and leaves it there; keep that
    // from leaking into the caller (Python parsed the next case's floats in round-toward-zero).
    std::fenv_t host_env;
    std::fegetenv(&host_env);
    static bool initialised = false;
    if (!initialised) {
        if (!addrspace::reserve()) return 2;
        emu.init();
        initialised = true;
    }
    mem_map_default();
    emu.dc_reset(true);
    Sh4Context* ctx = &p_sh4rcb->cntx;
    Sh4Executor* sh4 = Get_Sh4Interpreter();
    sh4->Init();

    FILE* f = std::fopen(image_path, "rb");
    if (!f) return 3;
    std::vector<std::uint8_t> bytes;
    unsigned char buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
    for (size_t i = 0; i < bytes.size(); ++i) addrspace::write8(image_base + static_cast<u32>(i), bytes[i]);
    // Deterministic data pattern (same generator in tests/sh4/runner.cpp) so functions that read
    // and write through pointer arguments see identical memory on both sides. Every aligned word
    // is itself an aligned pointer into the region, so pointer-chasing code stays inside it.
    for (std::uint32_t i = 0, x = fill_seed; i + 4 <= fill_len; i += 4) {
        x = x * 1664525u + 1013904223u;
        const std::uint32_t w = fill_addr + ((x >> 8) % fill_len & ~3u);
        for (int b = 0; b < 4; ++b) addrspace::write8(fill_addr + i + b, static_cast<u8>(w >> (8 * b)));
    }

    for (int i = 0; i < 16; ++i) ctx->r[i] = regs_in[i];
    ctx->pc = entry;
    ctx->pr = stop_pc;
    ctx->fpscr.full = fpscr_in;
    ctx->restoreHostRoundingMode();  // apply RM/DN now; Flycast's cache would otherwise keep the previous run's mode
    ctx->sr.setFull(0x700000F0);
    ctx->gbr = ctx->vbr = ctx->mac.l = ctx->mac.h = ctx->fpul = 0;
    for (int i = 0; i < 16; ++i) { ctx->fr[i] = 0.0f; ctx->xf[i] = 0.0f; }
    if (fr_in)
        for (int i = 0; i < 16; ++i) std::memcpy(&ctx->fr[i], &fr_in[i], 4);

    std::uint64_t steps = 0;
    while (ctx->pc != stop_pc && steps < max_steps) {
        sh4->Step();
        ++steps;
    }

    std::fesetenv(&host_env);
    FILE* o = std::fopen(dump_path, "w");
    if (!o) return 4;
    std::fprintf(o, "{\n  \"steps\": %llu,\n  \"pc\": \"0x%08x\",\n  \"stopped\": %s,\n",
                 static_cast<unsigned long long>(steps), ctx->pc, ctx->pc == stop_pc ? "true" : "false");
    std::fprintf(o, "  \"r\": [");
    for (int i = 0; i < 16; ++i) std::fprintf(o, "%s\"0x%08x\"", i ? ", " : "", ctx->r[i]);
    std::fprintf(o, "],\n  \"sr\": \"0x%08x\", \"t\": %u, \"gbr\": \"0x%08x\", \"vbr\": \"0x%08x\", \"pr\": \"0x%08x\",\n",
                 ctx->sr.getFull(), ctx->sr.T, ctx->gbr, ctx->vbr, ctx->pr);
    std::fprintf(o, "  \"mach\": \"0x%08x\", \"macl\": \"0x%08x\", \"fpul\": \"0x%08x\", \"fpscr\": \"0x%08x\",\n",
                 ctx->mac.h, ctx->mac.l, ctx->fpul, ctx->fpscr.full);
    std::fprintf(o, "  \"fr\": [");
    for (int i = 0; i < 16; ++i) { u32 b; std::memcpy(&b, &ctx->fr[i], 4); std::fprintf(o, "%s\"0x%08x\"", i ? ", " : "", b); }
    std::fprintf(o, "],\n  \"xf\": [");
    for (int i = 0; i < 16; ++i) { u32 b; std::memcpy(&b, &ctx->xf[i], 4); std::fprintf(o, "%s\"0x%08x\"", i ? ", " : "", b); }
    std::fprintf(o, "]");
    if (hash_len) {
        std::uint64_t h = 1469598103934665603ull;
        for (std::uint32_t i = 0; i < hash_len; ++i) { h ^= addrspace::read8(hash_addr + i); h *= 1099511628211ull; }
        std::fprintf(o, ",\n  \"mem_hash\": \"0x%016llx\",\n  \"mem\": [", static_cast<unsigned long long>(h));
        for (std::uint32_t i = 0; i + 4 <= hash_len; i += 4)
            std::fprintf(o, "%s\"0x%08x\"", i ? ", " : "", addrspace::read32(hash_addr + i));
        std::fprintf(o, "]");
    }
    std::fprintf(o, "\n}\n");
    std::fclose(o);
    return ctx->pc == stop_pc ? 0 : 1;
}
