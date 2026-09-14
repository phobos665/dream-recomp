cmake_minimum_required(VERSION 3.24)
project(dream_recomp VERSION 0.1.0 LANGUAGES C CXX)

# Static recompilation of Sega Dreamcast titles. See docs/implementation-plan.md and
# docs/decisions/README.md; the compiler flags below implement ADR 16 (cross-ISA determinism).

# Where this project's own sources are, which is not the same thing as CMAKE_SOURCE_DIR once a game
# repository consumes this as a submodule and becomes the top-level project itself. Cached rather
# than a plain variable because a sibling add_subdirectory() -- which is exactly how such a
# repository adds its game directory -- does not see normal variables set in here.
set(DREAM_ROOT ${CMAKE_CURRENT_SOURCE_DIR} CACHE INTERNAL "dream-recomp source root")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(DREAM_DEV_INTERPRETER
  "Build the development-only SH-4 interpreter fallback (ADR 2). Never enabled in release builds." OFF)
option(DREAM_BUILD_TESTS "Build unit tests and register them with CTest" ON)
option(DREAM_WERROR "Treat warnings as errors in dream targets" ON)
option(DREAM_RENDERER
  "Build the Vulkan renderer and the SDL3 window (WP2.3). Needs the Vulkan SDK and SDL3; without
   them the rest of the project still builds." ON)
set(DREAM_FLYCAST_CORE "" CACHE FILEPATH
    "Flycast libretro core built with tools/flycast/oracle; enables the differential test")

include(cmake/DreamTargetDefaults.cmake)
include(cmake/DreamAddGame.cmake)

if(DREAM_BUILD_TESTS)
  enable_testing()  # before any add_subdirectory so per-library add_test() calls register
endif()

# libchdr (BSD-3) for CHD disc images, built shared so tools/dcdisc can load it via ctypes.
function(dream_add_libchdr)
  # libchdr declares cmake_minimum_required(3.10), under which option() ignores a normal variable
  # of the same name (CMP0077 OLD). Force NEW so the settings below actually apply.
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
  set(BUILD_SHARED_LIBS ON)
  set(CHDR_WANT_TESTS OFF)
  set(INSTALL_STATIC_LIBS OFF)
  add_subdirectory(third_party/libchdr)
endfunction()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libchdr/CMakeLists.txt")
  dream_add_libchdr()
else()
  message(FATAL_ERROR "third_party/libchdr is empty: run `git submodule update --init --recursive`")
endif()

add_subdirectory(runtime)
# toml++ (MIT, third_party/tomlplusplus): per-game configuration files (ADR 14). Header-only.
add_library(dream_toml INTERFACE)
target_include_directories(dream_toml SYSTEM INTERFACE ${DREAM_ROOT}/third_party/tomlplusplus)
target_compile_definitions(dream_toml INTERFACE TOML_EXCEPTIONS=0)
add_library(dream::toml ALIAS dream_toml)

add_subdirectory(render)
add_subdirectory(audio)
add_subdirectory(translator)
# Every games/<id>/ with a CMakeLists.txt, so adding a title is adding a directory rather than
# editing this file too. Each is a no-op without that title's extracted executable, which is every
# checkout but the one whose owner dumped the disc.
# Only when this is the project being built. A game repository consumes this as a submodule and
# brings its own title, and it wants the tool rather than the reference titles shipped with it --
# which would otherwise collide on target names the moment it names its game the same thing.
if(PROJECT_IS_TOP_LEVEL)
  file(GLOB _dream_games RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/games/*)
  list(SORT _dream_games)  # a stable configure order, so build output compares between machines
  foreach(_g IN LISTS _dream_games)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_g}/CMakeLists.txt)
      add_subdirectory(${_g})
    endif()
  endforeach()
endif()

if(DREAM_BUILD_TESTS)
  add_subdirectory(tests)
endif()

message(STATUS "dream-recomp ${PROJECT_VERSION}: dev interpreter ${DREAM_DEV_INTERPRETER}, "
               "tests ${DREAM_BUILD_TESTS}, renderer ${DREAM_RENDERER}, "
               "host ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}")
