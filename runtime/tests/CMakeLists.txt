add_executable(dream_runtime_tests
  test_main.cpp
  test_fenv.cpp
  test_memory.cpp
  test_system.cpp
  test_devinterp.cpp
  test_gdrom.cpp
  test_bios.cpp
  test_maple.cpp
  test_memcard.cpp
  test_pvr.cpp
  test_arm7.cpp
  test_mixer.cpp
  test_g2dma.cpp
  test_sysblock.cpp
  test_rtc.cpp
  test_overlay.cpp
)
target_include_directories(dream_runtime_tests SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party/doctest)
target_link_libraries(dream_runtime_tests PRIVATE dream::runtime)
target_compile_definitions(dream_runtime_tests PRIVATE
  DREAM_DCDISC_FIXTURES="${CMAKE_SOURCE_DIR}/tools/dcdisc/tests/fixtures")
dream_target_defaults(dream_runtime_tests)
add_test(NAME runtime.unit COMMAND dream_runtime_tests)
