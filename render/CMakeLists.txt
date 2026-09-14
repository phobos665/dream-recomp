# dream::render — the Vulkan renderer (WP2.3). Optional: the build finds Vulkan and SDL3, and
# without them everything else still builds, so CI runners that lack a GPU stack are unaffected.
# Split in two on purpose:
#   dream_render       device-independent: display-list decoding and texture decoding. No Vulkan,
#                      no window, unit-tested headlessly on every CI runner.
#   dream_render_vk    the Vulkan backend and the SDL3 window. Built only when the dependencies
#                      are present.
add_library(dream_render STATIC
  src/display_list.cpp
  src/texture.cpp
  src/framebuffer.cpp
  src/background.cpp
  src/fog.cpp
  src/overlay.cpp
  src/input.cpp
  src/input_menu.cpp
)
target_include_directories(dream_render PUBLIC include)
target_link_libraries(dream_render PUBLIC dream::runtime)
dream_target_defaults(dream_render)
add_library(dream::render ALIAS dream_render)

# The device-independent half is tested on every runner, with or without a GPU.
if(DREAM_BUILD_TESTS)
  add_executable(dream_render_tests tests/test_display_list.cpp tests/test_texture.cpp tests/test_framebuffer.cpp tests/test_tsp.cpp tests/test_background.cpp tests/test_fog.cpp tests/test_overlay.cpp tests/test_input.cpp tests/test_input_menu.cpp ${DREAM_ROOT}/runtime/tests/test_main.cpp)
  target_link_libraries(dream_render_tests PRIVATE dream::render)
  target_include_directories(dream_render_tests SYSTEM PRIVATE ${DREAM_ROOT}/third_party/doctest)
  dream_target_defaults(dream_render_tests)
  add_test(NAME render.unit COMMAND dream_render_tests)
endif()

if(NOT DREAM_RENDERER)
  return()
endif()

find_package(Vulkan QUIET)
find_package(SDL3 QUIET CONFIG)
if(NOT Vulkan_FOUND OR NOT SDL3_FOUND)
  message(STATUS "dream::render: Vulkan ${Vulkan_FOUND} SDL3 ${SDL3_FOUND}; the Vulkan backend is skipped")
  return()
endif()

# Shaders are compiled to SPIR-V at build time and embedded, so a release build carries no shader
# source and needs no compiler at run time. glslc ships with the Vulkan SDK.
find_program(DREAM_GLSLC NAMES glslc HINTS ${Vulkan_GLSLC_EXECUTABLE} "$ENV{VULKAN_SDK}/bin")
if(NOT DREAM_GLSLC)
  message(STATUS "dream::render: no glslc; the Vulkan backend is skipped")
  return()
endif()

set(_spv_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${_spv_dir})
set(_spv_headers)
foreach(_shader triangle.vert triangle.frag geometry.vert geometry.frag present.vert present.frag)
  string(REPLACE "." "_" _sym ${_shader})
  set(_out ${_spv_dir}/${_sym}.h)
  # glslc -mfmt=c emits a bare initialiser list; wrap it in a declaration so the header can just
  # be included.
  add_custom_command(
    OUTPUT ${_out}
    COMMAND ${DREAM_GLSLC} --target-env=vulkan1.1 -O
            -mfmt=c -o ${_out}.inc ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_shader}
    COMMAND ${CMAKE_COMMAND} -DIN=${_out}.inc -DOUT=${_out} -DSYM=${_sym}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/wrap_spirv.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_shader}
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/wrap_spirv.cmake
    COMMENT "glslc ${_shader}"
    VERBATIM)
  list(APPEND _spv_headers ${_out})
endforeach()
add_custom_target(dream_render_shaders DEPENDS ${_spv_headers})

add_library(dream_render_vk STATIC
  src/vk/context.cpp
  src/vk/window.cpp
  src/vk/resources.cpp
  src/vk/renderer.cpp
  src/vk/texture_cache.cpp
  src/vk/offscreen.cpp
  src/vk/present.cpp
)
add_dependencies(dream_render_vk dream_render_shaders)
target_include_directories(dream_render_vk PUBLIC include PRIVATE ${_spv_dir})
target_link_libraries(dream_render_vk PUBLIC dream::render Vulkan::Vulkan SDL3::SDL3)
dream_target_defaults(dream_render_vk)
add_library(dream::render_vk ALIAS dream_render_vk)

# A standalone check that the whole graphics stack works on this machine: opens a window and draws
# a triangle. Not a test (it needs a display); run it by hand.
add_executable(dream_render_probe src/probe_main.cpp)
add_dependencies(dream_render_probe dream_render_shaders)
target_include_directories(dream_render_probe PRIVATE ${_spv_dir})
target_link_libraries(dream_render_probe PRIVATE dream::render_vk)
dream_target_defaults(dream_render_probe)
# Draws a captured display list in a window: the fastest way to see whether the decoder and the
# renderer agree with the hardware. Capture one with `crazytaxi_boot --dump-ta FILE`.
add_executable(dream_render_view src/view_main.cpp)
add_dependencies(dream_render_view dream_render_shaders)
target_include_directories(dream_render_view PRIVATE ${_spv_dir})
target_link_libraries(dream_render_view PRIVATE dream::render_vk)
dream_target_defaults(dream_render_view)

message(STATUS "dream::render: Vulkan backend enabled (glslc ${DREAM_GLSLC})")
