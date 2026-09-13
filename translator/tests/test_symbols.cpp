#include <filesystem>
#include <fstream>
#include <string>

#include "dream/translator/config/symbols.h"

#include "doctest.h"

using namespace dream::translator;

TEST_CASE("symbols: identifiers are sanitised") {
    CHECK(identifier_for("_kmSetFogTable") == "kmSetFogTable");
    CHECK(identifier_for("__divls") == "divls");
    CHECK(identifier_for("FID_conflict:_x") == "FID_conflict__x");
    CHECK(identifier_for("9lives") == "f_9lives");
    CHECK(identifier_for("switch") == "f_switch");
    CHECK(identifier_for("c") == "f_c");  // the emitted functions' parameter names
    CHECK(identifier_for("___") == "fn");
}

TEST_CASE("symbols: applying names matches RAM aliases and keeps identifiers unique") {
    SymbolTable syms;
    syms[0x0C073B70] = {0x0C073B70, "_kmSetFogTable", "fid"};
    syms[0x0C075B50] = {0x0C075B50, "_kmSetFogTable", "fid"};
    syms[0x8C010000] = {0x8C010000, "_loader", "user"};
    std::vector<FunctionSpec> fns = {{0x0C010000, 0x0C010018, "fn_0c010000"},
                                     {0x0C073B70, 0x0C073B90, "fn_0c073b70"},
                                     {0x0C075B50, 0x0C075B70, "fn_0c075b50"},
                                     {0x0C080000, 0x0C080010, "fn_0c080000"}};
    CHECK(apply_symbols(syms, fns) == 3);
    CHECK(fns[0].name == "loader");  // 0x8C010000 names the 0x0C010000 function
    CHECK(fns[1].name == "kmSetFogTable");
    CHECK(fns[2].name == "kmSetFogTable_0c075b50");
    CHECK(fns[3].name == "fn_0c080000");
}

TEST_CASE("symbols: TSV round trip and Ghidra import filtering") {
    const auto dir = std::filesystem::temp_directory_path() / "dream-symbols-test";
    std::filesystem::create_directories(dir);
    const auto json = dir / "ghidra.json";
    {
        std::ofstream o(json);
        o << R"({"functions": [
  {"address": "0x0c010000", "size": 24, "name": "_loader", "source": "USER_DEFINED"},
  {"address": "0x0c010630", "size": 82, "name": "FUN_0c010630", "source": "DEFAULT"},
  {"address": "0x0c02a808", "size": 8, "name": "thunk_FUN_0c02a698", "source": "DEFAULT"},
  {"address": "0x0c073b70", "size": 40, "name": "_kmSetFogTable", "source": "ANALYSIS"},
  {"address": "0x0c0a0000", "size": 40, "name": "FID_conflict:_kdSetAllStatusRequest", "source": "ANALYSIS"}
]})";
    }
    SymbolTable syms;
    std::string err;
    REQUIRE_MESSAGE(import_ghidra_symbols(json, syms, false, err), err);
    CHECK(syms.size() == 2);
    CHECK(syms.at(0x0C010000).name == "_loader");
    CHECK(syms.at(0x0C010000).source == "user");
    CHECK(syms.at(0x0C073B70).source == "fid");
    SymbolTable with_conflicts;
    REQUIRE(import_ghidra_symbols(json, with_conflicts, true, err));
    CHECK(with_conflicts.size() == 3);
    CHECK(with_conflicts.at(0x0C0A0000).name == "_kdSetAllStatusRequest");
    CHECK(with_conflicts.at(0x0C0A0000).source == "fid-conflict");

    const auto tsv = dir / "symbols.tsv";
    REQUIRE(save_symbols(tsv, syms, err));
    SymbolTable back;
    REQUIRE_MESSAGE(load_symbols(tsv, back, err), err);
    CHECK(back.size() == 2);
    CHECK(back.at(0x0C073B70).name == "_kmSetFogTable");
    std::filesystem::remove_all(dir);
}
