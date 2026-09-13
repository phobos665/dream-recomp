# dream_translator: SH-4 machine code to C++. The decoder, analysis passes and emitter arrive in
# Phase 1 (WP1.1 to WP1.6). Kept free of any dependency on dream_runtime so it can be relicensed or
# reused independently (ADR 1).
# The decoder on its own: the dev interpreter (runtime, behind DREAM_DEV_INTERPRETER) reuses it
# without pulling the emitter, discovery or TOML into the runtime.
add_library(dream_sh4_decoder STATIC src/sh4/decoder.cpp)
target_include_directories(dream_sh4_decoder PUBLIC include)
dream_target_defaults(dream_sh4_decoder)
add_library(dream::sh4dec ALIAS dream_sh4_decoder)

add_library(dream_translator STATIC
  src/version.cpp
  src/emit/emit.cpp
  src/analysis/discover.cpp
  src/analysis/switch.cpp
  src/analysis/constprop.cpp
  src/config/game_config.cpp
  src/config/symbols.cpp
)
target_include_directories(dream_translator PUBLIC include)
target_link_libraries(dream_translator PUBLIC dream::sh4dec PRIVATE dream::toml)
dream_target_defaults(dream_translator)
add_library(dream::translator ALIAS dream_translator)

add_executable(dream-translate src/main.cpp)
target_link_libraries(dream-translate PRIVATE dream::translator)
dream_target_defaults(dream-translate)

if(DREAM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
