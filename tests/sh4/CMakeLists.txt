# Translate the test programs at build time and run them (docs/emitter-design.md).
include(${CMAKE_CURRENT_SOURCE_DIR}/programs/programs.cmake)
# Optional: adds Crazy Taxi functions to the harness when the owner-supplied image is present.
# Absent in a checkout without that title, so the include is guarded rather than required.
set(_ct_slice ${DREAM_ROOT}/games/crazytaxi/slice.cmake)
if(EXISTS ${_ct_slice})
  include(${_ct_slice})
endif()

set(_gen_dir ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${_gen_dir})
set(_gen_sources)
set(_gen_headers)
set(_program_table)
foreach(entry IN LISTS DREAM_SH4_PROGRAMS)
  string(REPLACE "|" ";" entry "${entry}")
  list(GET entry 0 name)
  list(GET entry 1 bin)
  list(GET entry 2 base)
  list(GET entry 3 specs)
  separate_arguments(spec_list UNIX_COMMAND "${specs}")
  set(fn_args)
  foreach(s IN LISTS spec_list)
    list(APPEND fn_args --function ${s})
  endforeach()
  set(src ${_gen_dir}/${name}.cpp)
  set(hdr ${_gen_dir}/${name}.h)
  if(NOT IS_ABSOLUTE "${bin}")
    set(bin ${CMAKE_CURRENT_SOURCE_DIR}/programs/${bin})
  endif()
  add_custom_command(
    OUTPUT ${src} ${hdr}
    COMMAND dream-translate emit --image ${bin} --base ${base}
            ${fn_args} --out ${src} --header ${hdr}
    DEPENDS dream-translate ${bin} ${CMAKE_CURRENT_SOURCE_DIR}/programs/programs.cmake
    COMMENT "Translating ${name}"
    VERBATIM)
  list(APPEND _gen_sources ${src})
  list(APPEND _gen_headers ${hdr})
  # The program name selects the first spec's function; every function is also selectable by its
  # own name (ENTRY:END:name), so one C unit can host many harness entries.
  list(GET spec_list 0 first_spec)
  string(REGEX REPLACE "^[^:]*:[^:]*:" "" entry_fn "${first_spec}")
  string(APPEND _program_table "    {\"${name}\", &dream::gen::${entry_fn}},\n")
  foreach(s IN LISTS spec_list)
    string(REGEX REPLACE "^[^:]*:[^:]*:" "" fn "${s}")
    if(NOT fn STREQUAL s AND NOT fn STREQUAL name)
      string(APPEND _program_table "    {\"${fn}\", &dream::gen::${fn}},\n")
    endif()
  endforeach()
endforeach()

# One umbrella header including every generated header.
set(_umbrella ${_gen_dir}/emitted_programs.h)
set(_umbrella_content "// Generated. Do not edit.\n#pragma once\n")
foreach(h IN LISTS _gen_headers)
  get_filename_component(hn ${h} NAME)
  string(APPEND _umbrella_content "#include \"${hn}\"\n")
endforeach()
# Program table for the runner: every program is linked at the same base, so the address-keyed
# function table cannot tell them apart; the runner selects the entry function by program name.
string(APPEND _umbrella_content "\nnamespace dream::gen {\nstruct Program {\n    const char* name;\n    void (*entry)(dream::sh4::Ctx&, dream::Memory&);\n};\ninline constexpr Program kPrograms[] = {\n${_program_table}};\n}  // namespace dream::gen\n")
file(WRITE ${_umbrella} "${_umbrella_content}")

add_executable(dream_emit_tests test_programs.cpp test_golden.cpp ${DREAM_ROOT}/runtime/tests/test_main.cpp ${_gen_sources})
target_include_directories(dream_emit_tests PRIVATE ${_gen_dir})
target_compile_definitions(dream_emit_tests PRIVATE
  DREAM_SH4_PROGRAMS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/programs"
  DREAM_SH4_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
  DREAM_REPO_DIR="${DREAM_ROOT}")
target_include_directories(dream_emit_tests SYSTEM PRIVATE ${DREAM_ROOT}/third_party/doctest)
target_link_libraries(dream_emit_tests PRIVATE dream::runtime)
dream_target_defaults(dream_emit_tests)
# Generated code is checked with the project's warning set; -Wconversion noise on the casts the
# emitter writes is intentional feedback, not something to silence.
add_test(NAME translator.programs COMMAND dream_emit_tests)

# Runner used by the differential harness (tools/oracle): same generated programs, JSON context dump.
add_executable(dream_sh4_runner runner.cpp ${_gen_sources})
target_include_directories(dream_sh4_runner PRIVATE ${_gen_dir})
target_link_libraries(dream_sh4_runner PRIVATE dream::runtime)
dream_target_defaults(dream_sh4_runner)

# Differential test against Flycast's interpreter (docs/differential-harness.md); opt-in because it
# needs the locally built oracle core.
if(DREAM_FLYCAST_CORE)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME translator.differential
    COMMAND ${Python3_EXECUTABLE} ${DREAM_ROOT}/tools/oracle/differential.py
            --runner $<TARGET_FILE:dream_sh4_runner> --lib ${DREAM_FLYCAST_CORE}
            --programs ${CMAKE_CURRENT_SOURCE_DIR}/programs)
endif()
