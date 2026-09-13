# Integration-level tests. Unit tests live next to their library (runtime/tests). This file wires the
# CLI smoke test and the Python disc-tooling suite into CTest so one `ctest` run covers everything.

add_test(NAME translator.version COMMAND dream-translate --version)

add_subdirectory(sh4)
set_tests_properties(translator.version PROPERTIES PASS_REGULAR_EXPRESSION "^dream-translate [0-9]+\\.[0-9]+\\.[0-9]+")

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  # The dcdisc tests build a synthetic GD-ROM; pycdlib and pytest come from `pip install -e tools/dcdisc[test]`.
  # DCDISC_LIBCHDR points at the libchdr we just built so the CHD tests run without a system copy.
  add_test(NAME dcdisc.pytest
    COMMAND ${Python3_EXECUTABLE} -m pytest -q ${CMAKE_SOURCE_DIR}/tools/dcdisc/tests
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tools/dcdisc)
  set_tests_properties(dcdisc.pytest PROPERTIES
    ENVIRONMENT "DCDISC_LIBCHDR=$<TARGET_FILE:chdr>;PYTHONPATH=${CMAKE_SOURCE_DIR}/tools/dcdisc"
    SKIP_REGULAR_EXPRESSION "No module named pytest")
else()
  message(STATUS "Python3 not found: dcdisc tests not registered")
endif()
