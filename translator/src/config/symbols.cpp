#include "dream/translator/config/symbols.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

namespace dream::translator {
namespace {

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

const std::set<std::string>& keywords() {
    static const std::set<std::string> k = {
        "alignas",  "alignof",  "and",      "asm",      "auto",     "bool",      "break",
        "case",     "catch",    "char",     "class",    "const",    "constexpr", "continue",
        "default",  "delete",   "do",       "double",   "else",     "enum",      "explicit",
        "export",   "extern",   "false",    "float",    "for",      "friend",    "goto",
        "if",       "inline",   "int",      "long",     "mutable",  "namespace", "new",
        "noexcept", "not",      "nullptr",  "operator", "or",       "private",   "protected",
        "public",   "register", "return",   "short",    "signed",   "sizeof",    "static",
        "struct",   "switch",   "template", "this",     "throw",    "true",      "try",
        "typedef",  "typeid",   "typename", "union",    "unsigned", "using",     "virtual",
        "void",     "volatile", "while",    "xor",      "main",     "c",         "m"};
    return k;
}

}  // namespace

std::string identifier_for(const std::string& name) {
    std::string s;
    std::size_t i = 0;
    while (i < name.size() && name[i] == '_') ++i;
    for (; i < name.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(name[i]);
        s.push_back(std::isalnum(ch) ? static_cast<char>(ch) : '_');
    }
    if (s.empty())
        return "fn";
    if (std::isdigit(static_cast<unsigned char>(s[0])) || keywords().count(s))
        s = "f_" + s;
    return s;
}

bool load_symbols(const std::filesystem::path& path, SymbolTable& out, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path.string();
        return false;
    }
    std::string line;
    std::size_t lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ls(line);
        std::string a, n, src;
        std::getline(ls, a, '\t');
        std::getline(ls, n, '\t');
        std::getline(ls, src, '\t');
        char* end = nullptr;
        const unsigned long addr = std::strtoul(a.c_str(), &end, 0);
        if (!end || *end != '\0' || n.empty()) {
            error = path.string() + ":" + std::to_string(lineno) + ": expected address<TAB>name";
            return false;
        }
        out[static_cast<std::uint32_t>(addr)] =
            Symbol{static_cast<std::uint32_t>(addr), trim(n), trim(src)};
    }
    return true;
}

bool save_symbols(const std::filesystem::path& path, const SymbolTable& syms, std::string& error) {
    std::ofstream o(path);
    if (!o) {
        error = "cannot write " + path.string();
        return false;
    }
    o << "# address\tname\tsource  (dream-translate symbols; edit freely, keep tabs)\n";
    for (const auto& [a, s] : syms) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "0x%08x", a);
        o << buf << '\t' << s.name << '\t' << s.source << '\n';
    }
    return true;
}

bool import_ghidra_symbols(const std::filesystem::path& json_path, SymbolTable& out,
                           bool keep_conflicts, std::string& error) {
    std::ifstream in(json_path);
    if (!in) {
        error = "cannot open " + json_path.string();
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // Minimal parse of ExportFunctions.java's shape: {"address": "0x..", ..., "name": "..",
    // "source": ".."} objects in order.
    std::size_t pos = 0;
    std::size_t imported = 0;
    while ((pos = text.find("\"address\"", pos)) != std::string::npos) {
        const std::size_t obj_end = text.find('}', pos);
        auto field = [&](const char* key) -> std::string {
            const std::size_t k = text.find(key, pos);
            if (k == std::string::npos || k > obj_end)
                return "";
            const std::size_t q1 = text.find('"', text.find(':', k)) + 1;
            const std::size_t q2 = text.find('"', q1);
            return text.substr(q1, q2 - q1);
        };
        const std::string addr = field("\"address\"");
        std::string name = field("\"name\"");
        std::string source = field("\"source\"");
        pos = obj_end == std::string::npos ? text.size() : obj_end;
        if (addr.empty() || name.empty())
            continue;
        if (name.rfind("FUN_", 0) == 0 || name.rfind("thunk_", 0) == 0)
            continue;
        if (name.rfind("FID_conflict:", 0) == 0) {
            if (!keep_conflicts)
                continue;
            name = name.substr(std::string("FID_conflict:").size());
            source = "fid-conflict";
        } else {
            source = source == "USER_DEFINED" ? "user" : source == "ANALYSIS" ? "fid" : "ghidra";
        }
        const std::uint32_t a = static_cast<std::uint32_t>(std::strtoul(addr.c_str(), nullptr, 0));
        out[a] = Symbol{a, name, source};
        ++imported;
    }
    if (imported == 0) {
        error = "no usable names in " + json_path.string();
        return false;
    }
    return true;
}

std::size_t apply_symbols(const SymbolTable& syms, std::vector<FunctionSpec>& specs) {
    // Index by physical address so 0x0C... and 0x8C... spellings meet.
    std::map<std::uint32_t, const Symbol*> by_phys;
    for (const auto& [a, s] : syms) by_phys[a & 0x1FFFFFFFu] = &s;
    std::map<std::string, std::size_t> used;
    for (const auto& f : specs) used[f.name]++;
    std::size_t named = 0;
    for (auto& f : specs) {
        auto it = by_phys.find(f.entry & 0x1FFFFFFFu);
        if (it == by_phys.end())
            continue;
        std::string id = identifier_for(it->second->name);
        if (used.count(id)) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "_%08x", f.entry);
            id += buf;
        }
        used[id]++;
        f.name = id;
        ++named;
    }
    return named;
}

}  // namespace dream::translator
