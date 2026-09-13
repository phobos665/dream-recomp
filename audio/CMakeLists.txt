# dream::audio — playback through SDL3 (WP2.5). Optional in exactly the way the renderer is: the
# AICA and its mixer are part of dream::runtime and are tested headlessly, and a build without SDL3
# still produces sound into a WAV file through the launcher's --wav.
find_package(SDL3 QUIET CONFIG)
if(NOT SDL3_FOUND)
  message(STATUS "dream::audio: no SDL3; playback is skipped")
  return()
endif()

add_library(dream_audio STATIC src/sdl_sink.cpp)
target_include_directories(dream_audio PUBLIC include)
target_link_libraries(dream_audio PUBLIC SDL3::SDL3)
dream_target_defaults(dream_audio)
add_library(dream::audio ALIAS dream_audio)
message(STATUS "dream::audio: SDL3 playback enabled")
