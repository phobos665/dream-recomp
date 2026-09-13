// Function discovery (WP1.4): which addresses in an image are code, and where functions start
// and end. See ADR 8 and the Crazy Taxi evidence in games/crazytaxi/checklist-report.md: the
// image model must be right first (link address, relocated copies), recursive descent from
// the entry alone finds almost nothing, and literal-pool pointers are candidates to be tested,
// not facts.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "dream/translator/emit.h"

namespace dream::translator {

struct DiscoverOptions {
    std::vector<std::uint32_t> seeds;  // known function entries (image entry point etc.)
    bool follow_pointers = true;       // seed from aligned words that point into the image
    bool aggressive_sweep = true;      // after descent, sweep unreached gaps for code
    std::uint32_t max_function_bytes = 0x40000;
};

struct DiscoveredFunction {
    std::uint32_t entry;
    std::uint32_t end;   // exclusive; covers every reachable block of the function
    std::string origin;  // "seed", "call", "pointer", "sweep"
};

struct DiscoverResult {
    std::vector<DiscoveredFunction> functions;  // sorted by entry
    std::size_t code_bytes = 0;
    std::size_t pointer_candidates = 0, pointer_accepted = 0, switches = 0;
    std::vector<std::uint32_t> switch_sites;  // jump addresses of recovered tables
    std::vector<std::string> notes;
};

DiscoverResult discover(const Image& image, const DiscoverOptions& options);

// JSON in the shape the emitter, Ghidra scripts and games/<id>/functions.json share.
std::string to_json(const DiscoverResult& r, const Image& image);
std::vector<FunctionSpec> to_specs(const DiscoverResult& r);
bool load_functions_json(const std::string& path, std::vector<FunctionSpec>& out,
                         std::string& error);

}  // namespace dream::translator
