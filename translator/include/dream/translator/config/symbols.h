// Function names for emitted code. A symbols file is tab-separated `address<TAB>name<TAB>source`
// (one per line, `#` comments), produced by `dream-translate symbols` from a Ghidra export and kept
// under games/<id>/. Names are made into unique C++ identifiers when applied.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "dream/translator/emit.h"

namespace dream::translator {

struct Symbol {
    std::uint32_t address = 0;
    std::string name;    // as exported
    std::string source;  // e.g. "fid", "user", "analysis"
};

using SymbolTable = std::map<std::uint32_t, Symbol>;

bool load_symbols(const std::filesystem::path& path, SymbolTable& out, std::string& error);
bool save_symbols(const std::filesystem::path& path, const SymbolTable& syms, std::string& error);

// Reads the JSON written by tools/ghidra/ExportFunctions.java ({"functions": [{"address", "name",
// "source"}]}). Skips Ghidra's default FUN_/thunk_ names; FID conflicts (several candidate
// names) are skipped unless keep_conflicts is set, in which case the first candidate is kept with
// source "fid-conflict".
bool import_ghidra_symbols(const std::filesystem::path& json_path, SymbolTable& out,
                           bool keep_conflicts, std::string& error);

// Turns a symbol into a C++ identifier: leading underscores dropped, other characters mapped to
// '_', keywords and digits-first names prefixed.
std::string identifier_for(const std::string& name);

// Names every spec whose entry has a symbol (aliases of the same RAM match); identifiers are made
// unique by appending the address when two functions would share one. Returns how many were named.
std::size_t apply_symbols(const SymbolTable& syms, std::vector<FunctionSpec>& specs);

}  // namespace dream::translator
