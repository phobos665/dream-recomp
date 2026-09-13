// dream-translate: command-line front end.
//
//   dream-translate --version
//   dream-translate disasm --image FILE --base ADDR [--start ADDR] [--count N]
//   dream-translate emit   --image FILE --base ADDR --function ENTRY:END[:name]... --out FILE.cpp
//                          [--header FILE.h] [--no-fold] [--trace] [--no-irq]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "dream/translator/analysis/discover.h"
#include "dream/translator/config/game_config.h"
#include "dream/translator/config/symbols.h"
#include "dream/translator/emit.h"
#include "dream/translator/sh4/decoder.h"
#include "dream/translator/version.h"

namespace {

int usage(int code) {
    std::fprintf(stderr,
                 "dream-translate %s\n"
                 "usage:\n"
                 "  dream-translate --version\n"
                 "  dream-translate disasm --image FILE --base ADDR [--start ADDR] [--count N]\n"
                 "  dream-translate discover --image FILE --base ADDR [--entry ADDR]... [--out "
                 "functions.json] [--no-pointers] [--no-sweep]\n"
                 "  dream-translate emit --image FILE --base ADDR --function ENTRY:END[:name]... "
                 "--out FILE.cpp\n"
                 "                       [--header FILE.h] [--no-fold] [--trace] [--no-irq] "
                 "[--symbols FILE.tsv]\n"
                 "  dream-translate symbols --ghidra functions.json --out symbols.tsv "
                 "[--keep-conflicts]\n"
                 "  dream-translate game --config games/ID/ID.toml --out-dir DIR [--no-fold] "
                 "[--trace] [--no-irq]\n",
                 dream::translator::version().data());
    return code;
}

std::uint32_t parse_addr(const char* s) {
    return static_cast<std::uint32_t>(std::strtoul(s, nullptr, 0));
}

bool load_image(const std::string& path, std::uint32_t base, dream::translator::Image& img) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    img.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    img.base = base;
    return true;
}

int cmd_disasm(int argc, char** argv) {
    std::string image;
    std::uint32_t base = 0, start = 0, count = 64;
    bool have_start = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--image") && i + 1 < argc)
            image = argv[++i];
        else if (!std::strcmp(argv[i], "--base") && i + 1 < argc)
            base = parse_addr(argv[++i]);
        else if (!std::strcmp(argv[i], "--start") && i + 1 < argc) {
            start = parse_addr(argv[++i]);
            have_start = true;
        } else if (!std::strcmp(argv[i], "--count") && i + 1 < argc)
            count = parse_addr(argv[++i]);
        else
            return usage(2);
    }
    dream::translator::Image img;
    if (image.empty() || !load_image(image, base, img)) {
        std::fprintf(stderr, "cannot read %s\n", image.c_str());
        return 2;
    }
    if (!have_start)
        start = base;
    for (std::uint32_t pc = start; count-- && img.contains(pc, 2); pc += 2) {
        const auto ins = dream::sh4::decode(img.read16(pc));
        std::printf("%08x  %04x  %s\n", pc, ins.raw, dream::sh4::format(ins, pc).c_str());
    }
    return 0;
}

int cmd_discover(int argc, char** argv) {
    std::string image, out;
    std::uint32_t base = 0;
    dream::translator::DiscoverOptions opt;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--image") && i + 1 < argc)
            image = argv[++i];
        else if (!std::strcmp(argv[i], "--base") && i + 1 < argc)
            base = parse_addr(argv[++i]);
        else if (!std::strcmp(argv[i], "--entry") && i + 1 < argc)
            opt.seeds.push_back(parse_addr(argv[++i]));
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc)
            out = argv[++i];
        else if (!std::strcmp(argv[i], "--no-pointers"))
            opt.follow_pointers = false;
        else if (!std::strcmp(argv[i], "--no-sweep"))
            opt.aggressive_sweep = false;
        else
            return usage(2);
    }
    if (image.empty())
        return usage(2);
    dream::translator::Image img;
    if (!load_image(image, base, img)) {
        std::fprintf(stderr, "cannot read %s\n", image.c_str());
        return 2;
    }
    if (opt.seeds.empty())
        opt.seeds.push_back(base);
    const auto r = dream::translator::discover(img, opt);
    const std::string json = dream::translator::to_json(r, img);
    if (out.empty())
        std::fputs(json.c_str(), stdout);
    else {
        std::ofstream o(out);
        o << json;
    }
    std::size_t by_origin[5] = {0, 0, 0, 0, 0};
    for (const auto& f : r.functions) {
        by_origin[f.origin == "seed"      ? 0
                  : f.origin == "call"    ? 1
                  : f.origin == "pointer" ? 2
                  : f.origin == "branch"  ? 3
                                          : 4]++;
    }
    std::fprintf(stderr,
                 "discovered %zu functions (%zu seed, %zu call, %zu pointer, %zu branch, %zu "
                 "sweep), %zu code bytes of %zu (%.1f%%), pointers %zu/%zu accepted, %zu switch "
                 "tables\n",
                 r.functions.size(), by_origin[0], by_origin[1], by_origin[2], by_origin[3],
                 by_origin[4], r.code_bytes, img.bytes.size(),
                 100.0 * static_cast<double>(r.code_bytes) / static_cast<double>(img.bytes.size()),
                 r.pointer_accepted, r.pointer_candidates, r.switches);
    return 0;
}

void report_coverage_gaps(const dream::translator::EmitResult& res);

int cmd_emit(int argc, char** argv) {
    std::string image, out, header, symbols;
    std::uint32_t base = 0;
    std::vector<dream::translator::FunctionSpec> fns;
    dream::translator::EmitOptions opt;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--image") && i + 1 < argc)
            image = argv[++i];
        else if (!std::strcmp(argv[i], "--base") && i + 1 < argc)
            base = parse_addr(argv[++i]);
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc)
            out = argv[++i];
        else if (!std::strcmp(argv[i], "--header") && i + 1 < argc)
            header = argv[++i];
        else if (!std::strcmp(argv[i], "--symbols") && i + 1 < argc)
            symbols = argv[++i];
        else if (!std::strcmp(argv[i], "--no-fold"))
            opt.fold_literals = false;
        else if (!std::strcmp(argv[i], "--replay-hooks"))
            opt.replay_hooks = true;
        else if (!std::strcmp(argv[i], "--trace"))
            opt.trace = true;
        else if (!std::strcmp(argv[i], "--no-irq"))
            opt.irq_checks = false;
        else if (!std::strcmp(argv[i], "--functions") && i + 1 < argc) {
            std::string err;
            if (!dream::translator::load_functions_json(argv[++i], fns, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return 2;
            }
        } else if (!std::strcmp(argv[i], "--function") && i + 1 < argc) {
            std::string spec = argv[++i];
            dream::translator::FunctionSpec f;
            const auto c1 = spec.find(':');
            if (c1 == std::string::npos)
                return usage(2);
            f.entry = parse_addr(spec.substr(0, c1).c_str());
            const auto c2 = spec.find(':', c1 + 1);
            f.end = parse_addr(
                spec.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1)
                    .c_str());
            if (c2 != std::string::npos)
                f.name = spec.substr(c2 + 1);
            fns.push_back(f);
        } else
            return usage(2);
    }
    if (image.empty() || out.empty() || fns.empty())
        return usage(2);
    dream::translator::Image img;
    if (!load_image(image, base, img)) {
        std::fprintf(stderr, "cannot read %s\n", image.c_str());
        return 2;
    }
    if (!symbols.empty()) {
        dream::translator::SymbolTable syms;
        std::string err;
        if (!dream::translator::load_symbols(symbols, syms, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 2;
        }
        std::fprintf(stderr, "named %zu of %zu functions from %s\n",
                     dream::translator::apply_symbols(syms, fns), fns.size(), symbols.c_str());
    }
    const auto res = dream::translator::emit_unit(img, fns, opt);
    {
        std::ofstream o(out);
        o << res.source;
    }
    if (!header.empty()) {
        std::ofstream h(header);
        h << res.header;
    }
    for (const auto& w : res.warnings) std::fprintf(stderr, "note: %s\n", w.c_str());
    report_coverage_gaps(res);
    std::fprintf(
        stderr,
        "emitted %zu functions, %zu instructions, %zu not lowered, %zu FP runtime branches, "
        "%zu functions with unknown FPSCR effect, %zu switch tables, %zu direct jsr/bsrf, "
        "%zu entry modes inferred (%zu ambiguous) -> %s\n",
        fns.size(), res.instructions, res.unlowered, res.fp_runtime_branches, res.unknown_summaries,
        res.switches_recovered, res.direct_calls, res.entries_inferred, res.entries_unknown,
        out.c_str());
    return 0;
}

// Bytes inside a function that were never decoded. A literal pool looks the same from here, so
// this reports and never fails; a long gap is the signal worth looking at, because the emitted
// program is then missing code the guest can still branch to.
void report_coverage_gaps(const dream::translator::EmitResult& res) {
    if (res.bytes_in_range == 0)
        return;
    // Function ranges overlap: discovery gives a caller a range that covers callees it falls into,
    // and those callees are separately emitted functions. Adding per-function figures therefore
    // double-counts, and counts as "missing" bytes that were compiled under another name. Merging
    // both sets of intervals and comparing the unions is the only figure that answers the question
    // that matters, which is whether a byte was compiled at all.
    auto merged = [](std::vector<std::pair<std::uint32_t, std::uint32_t>> v) -> std::uint64_t {
        std::sort(v.begin(), v.end());
        std::uint64_t total = 0;
        bool open = false;
        std::uint32_t lo = 0, hi = 0;
        for (const auto& [a, b] : v) {
            if (b <= a)
                continue;
            if (!open) {
                lo = a;
                hi = b;
                open = true;
            } else if (a > hi) {
                total += hi - lo;
                lo = a;
                hi = b;
            } else if (b > hi) {
                hi = b;
            }
        }
        if (open)
            total += hi - lo;
        return total;
    };
    // What is left when the decoded instructions and the literal pools are taken out of the union
    // of function ranges. That remainder is the honest measure of code the emitted program does not
    // contain: a byte compiled under another function's name is not missing, and a pool word some
    // instruction reads as a constant was never code.
    auto subtract = [](std::vector<std::pair<std::uint32_t, std::uint32_t>> from,
                       std::vector<std::pair<std::uint32_t, std::uint32_t>> take) {
        std::sort(from.begin(), from.end());
        std::sort(take.begin(), take.end());
        std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
        for (auto [a, b] : from) {
            std::uint32_t at = a;
            for (const auto& [ta, tb] : take) {
                if (tb <= at)
                    continue;
                if (ta >= b)
                    break;
                if (ta > at)
                    out.push_back({at, std::min(ta, b)});
                at = std::max(at, tb);
                if (at >= b)
                    break;
            }
            if (at < b)
                out.push_back({at, b});
        }
        return out;
    };
    const std::uint64_t range_union = merged(res.function_ranges);
    const std::uint64_t decoded_union = merged(res.decoded_runs);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> known = res.decoded_runs;
    known.insert(known.end(), res.literal_words.begin(), res.literal_words.end());
    const auto missing = subtract(res.function_ranges, known);
    std::uint64_t missing_bytes = 0;
    std::size_t long_runs = 0;
    for (auto [a, b] : missing) {
        missing_bytes += b - a;
        if (b - a >= 16)
            ++long_runs;
    }
    const double pct =
        range_union ? 100.0 * static_cast<double>(decoded_union) / static_cast<double>(range_union)
                    : 0.0;
    std::fprintf(stderr,
                 "coverage: %llu of %llu bytes decoded (%.1f%%); %llu bytes are neither "
                 "instruction nor literal pool, in %zu runs of 16 bytes or more\n",
                 static_cast<unsigned long long>(decoded_union),
                 static_cast<unsigned long long>(range_union), pct,
                 static_cast<unsigned long long>(missing_bytes), long_runs);
    std::size_t shown = 0;
    for (auto [a, b] : missing) {
        if (b - a < 16)
            continue;
        if (shown++ >= 20) {
            std::fprintf(stderr, "  (more not listed)\n");
            break;
        }
        std::fprintf(stderr, "  %u bytes at 0x%08x..0x%08x\n", b - a, a, b);
    }
}

int cmd_symbols(int argc, char** argv) {
    std::string ghidra, out;
    bool keep_conflicts = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--ghidra") && i + 1 < argc)
            ghidra = argv[++i];
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc)
            out = argv[++i];
        else if (!std::strcmp(argv[i], "--keep-conflicts"))
            keep_conflicts = true;
        else
            return usage(2);
    }
    if (ghidra.empty() || out.empty())
        return usage(2);
    dream::translator::SymbolTable syms;
    std::string err;
    if (std::filesystem::exists(out) && !dream::translator::load_symbols(out, syms, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    const std::size_t before = syms.size();
    if (!dream::translator::import_ghidra_symbols(ghidra, syms, keep_conflicts, err) ||
        !dream::translator::save_symbols(out, syms, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    std::fprintf(stderr, "%zu symbols (%zu new) -> %s\n", syms.size(), syms.size() - before,
                 out.c_str());
    return 0;
}

// Maps an address given in the config (load-address spelling) into the image's link space.
std::uint32_t to_link_space(const dream::translator::GameConfig& cfg, std::uint32_t a) {
    return (a & 0x1FFFFFFFu) - (cfg.load_address & 0x1FFFFFFFu) + cfg.link_address;
}

void write_functions_json(const std::string& path,
                          const std::vector<dream::translator::FunctionSpec>& fns,
                          const dream::translator::DiscoverResult& r) {
    std::ofstream o(path);
    o << "{\n  \"functions\": [\n";
    for (std::size_t i = 0; i < fns.size(); ++i) {
        const auto& f = fns[i];
        const std::string origin = i < r.functions.size() ? r.functions[i].origin : "";
        char e[16], en[16];
        std::snprintf(e, sizeof e, "0x%08x", f.entry);
        std::snprintf(en, sizeof en, "0x%08x", f.end);
        o << "    {\"entry\": \"" << e << "\", \"end\": \"" << en
          << "\", \"size\": " << (f.end - f.entry) << ", \"origin\": \"" << origin
          << "\", \"name\": \"" << f.name << "\"}" << (i + 1 < fns.size() ? "," : "") << "\n";
    }
    o << "  ]\n}\n";
}

// Translate one image view (the main binary or a relocated copy) into DIR/<stem>.{cpp,h,json}.
int translate_view(const dream::translator::GameConfig& cfg, const dream::translator::Image& img,
                   const std::vector<std::uint32_t>& seeds, const std::string& stem,
                   const std::filesystem::path& out_dir, const dream::translator::EmitOptions& opt,
                   const dream::translator::SymbolTable* syms) {
    dream::translator::DiscoverOptions dopt;
    dopt.seeds = seeds;
    dopt.follow_pointers = cfg.pointers;
    dopt.aggressive_sweep = cfg.sweep;
    auto r = dream::translator::discover(img, dopt);
    if (!cfg.exclude.empty()) {
        std::vector<dream::translator::DiscoveredFunction> kept;
        for (const auto& f : r.functions) {
            bool ex = false;
            for (std::uint32_t x : cfg.exclude) ex |= to_link_space(cfg, x) == f.entry;
            if (!ex)
                kept.push_back(f);
        }
        r.functions = kept;
    }
    auto fns = dream::translator::to_specs(r);
    std::size_t named = 0;
    if (syms)
        named = dream::translator::apply_symbols(*syms, fns);
    if (opt.overlay) {
        // Several overlay units can translate the same address: keep their symbols distinct.
        const auto pos = stem.find("_reloc_");
        const std::string suffix = "_" + (pos == std::string::npos ? stem : stem.substr(pos + 7));
        for (auto& f : fns) f.name += suffix;
    }
    const auto res = dream::translator::emit_unit(img, fns, opt);
    const auto base = out_dir / stem;
    {
        std::ofstream o(base.string() + ".cpp");
        o << res.source;
        std::ofstream h(base.string() + ".h");
        h << res.header;
    }
    write_functions_json(base.string() + ".functions.json", fns, r);
    {
        std::ofstream rep(base.string() + ".report.txt");
        rep << "unit " << stem << "\nimage base 0x" << std::hex << img.base << " size " << std::dec
            << img.bytes.size() << " bytes\nfunctions " << fns.size() << " (named " << named
            << ")\ninstructions " << res.instructions << "\nnot lowered " << res.unlowered
            << "\nfp runtime branches " << res.fp_runtime_branches << "\nunknown fpscr summaries "
            << res.unknown_summaries << "\nswitch tables " << res.switches_recovered << " of "
            << r.switches << " discovered\ndirect jsr/bsrf calls " << res.direct_calls
            << "\npatched pool words (not folded) " << res.patched_words
            << "\nentry modes inferred " << res.entries_inferred << " (ambiguous "
            << res.entries_unknown << ")\n";
        for (const auto& w : res.warnings) rep << "note: " << w << "\n";
    }
    report_coverage_gaps(res);
    std::fprintf(stderr,
                 "%s: %zu functions (%zu named), %zu instructions, %zu not lowered, %zu FP runtime "
                 "branches, %zu switch tables -> %s.cpp\n",
                 stem.c_str(), fns.size(), named, res.instructions, res.unlowered,
                 res.fp_runtime_branches, res.switches_recovered, base.string().c_str());
    return 0;
}

int cmd_game(int argc, char** argv) {
    std::string config, out_dir;
    dream::translator::EmitOptions opt;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--config") && i + 1 < argc)
            config = argv[++i];
        else if (!std::strcmp(argv[i], "--out-dir") && i + 1 < argc)
            out_dir = argv[++i];
        else if (!std::strcmp(argv[i], "--no-fold"))
            opt.fold_literals = false;
        else if (!std::strcmp(argv[i], "--replay-hooks"))
            opt.replay_hooks = true;
        else if (!std::strcmp(argv[i], "--trace"))
            opt.trace = true;
        else if (!std::strcmp(argv[i], "--no-irq"))
            opt.irq_checks = false;
        else
            return usage(2);
    }
    if (config.empty() || out_dir.empty())
        return usage(2);
    dream::translator::GameConfig cfg;
    std::string err;
    if (!dream::translator::load_game_config(config, cfg, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    dream::translator::Image img;
    if (!load_image(cfg.binary_path.string(), cfg.link_address, img)) {
        std::fprintf(stderr, "cannot read %s\n", cfg.binary_path.string().c_str());
        return 2;
    }
    dream::translator::SymbolTable syms;
    if (cfg.symbols && !dream::translator::load_symbols(*cfg.symbols, syms, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    std::filesystem::create_directories(out_dir);
    std::vector<std::uint32_t> seeds{to_link_space(cfg, cfg.entry)};
    for (std::uint32_t a : cfg.extra_functions) seeds.push_back(to_link_space(cfg, a));
    int rc = translate_view(cfg, img, seeds, cfg.id, out_dir, opt, cfg.symbols ? &syms : nullptr);
    // Relocated copies: the same bytes viewed at the address they execute from. A destination
    // that appears more than once (overlays) numbers its later units _2, _3, ...; the game's
    // CMakeLists derives the same names from the TOML.
    std::map<std::uint32_t, int> dest_count;
    for (const auto& rel : cfg.relocations) {
        const std::uint32_t off = rel.source - img.base;
        if (rel.source < img.base || off + rel.size > img.bytes.size()) {
            std::fprintf(stderr, "relocation source 0x%08x+0x%x is outside the image\n", rel.source,
                         rel.size);
            return 2;
        }
        dream::translator::Image view;
        view.base = (rel.dest & 0x1FFFFFFFu) | (cfg.link_address & 0xE0000000u);
        view.bytes.assign(img.bytes.begin() + off, img.bytes.begin() + off + rel.size);
        char stem[80];
        const int nth = ++dest_count[rel.dest];
        if (nth == 1)
            std::snprintf(stem, sizeof stem, "%s_reloc_%08x", cfg.id.c_str(), rel.dest);
        else
            std::snprintf(stem, sizeof stem, "%s_reloc_%08x_%d", cfg.id.c_str(), rel.dest, nth);
        std::vector<std::uint32_t> seeds_r;
        for (std::uint32_t e : rel.entries)
            seeds_r.push_back((e & 0x1FFFFFFFu) | (cfg.link_address & 0xE0000000u));
        if (seeds_r.empty())
            seeds_r.push_back(view.base);
        dream::translator::EmitOptions view_opt = opt;
        if (rel.no_fold)
            view_opt.fold_literals = false;
        view_opt.overlay = rel.overlay;
        rc |= translate_view(cfg, view, seeds_r, stem, out_dir, view_opt,
                             cfg.symbols ? &syms : nullptr);
    }
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && (!std::strcmp(argv[1], "--version") || !std::strcmp(argv[1], "-V"))) {
        std::printf("dream-translate %s\n", dream::translator::version().data());
        return 0;
    }
    if (argc >= 2 && !std::strcmp(argv[1], "disasm"))
        return cmd_disasm(argc, argv);
    if (argc >= 2 && !std::strcmp(argv[1], "emit"))
        return cmd_emit(argc, argv);
    if (argc >= 2 && !std::strcmp(argv[1], "discover"))
        return cmd_discover(argc, argv);
    if (argc >= 2 && !std::strcmp(argv[1], "symbols"))
        return cmd_symbols(argc, argv);
    if (argc >= 2 && !std::strcmp(argv[1], "game"))
        return cmd_game(argc, argv);
    return usage(argc >= 2 ? 2 : 0);
}
