#pragma once

#include <string_view>

namespace dream::translator {

// Semantic version of the translator. Emitted into every generated file so a build can be traced
// back to the translator that produced it.
std::string_view version() noexcept;

}  // namespace dream::translator
