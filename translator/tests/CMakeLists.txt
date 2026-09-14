add_executable(dream_translator_tests
  test_main.cpp
  test_decoder.cpp
  test_config.cpp
  test_symbols.cpp
)
target_include_directories(dream_translator_tests SYSTEM PRIVATE ${DREAM_ROOT}/third_party/doctest)
target_link_libraries(dream_translator_tests PRIVATE dream::translator)
target_compile_definitions(dream_translator_tests PRIVATE
  DREAM_SH4_ORACLE="${DREAM_ROOT}/tests/sh4/oracle/sh4_objdump.txt")
dream_target_defaults(dream_translator_tests)
add_test(NAME translator.unit COMMAND dream_translator_tests)
