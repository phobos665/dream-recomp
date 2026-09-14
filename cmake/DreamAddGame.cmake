# dream_add_game(<id> [TITLE "Name"] [BOOT_TEST])
#
# Everything a recompiled title needs: translate its binary at build time, compile the generated
# units, and link them with the runtime into a `<id>_boot` launcher.
#
# This was seventy lines copied into each game's directory with the title's name substituted in a
# dozen places. Three copies is where that stops being duplication and starts being drift: a fix
# made in one game's file is a fix the others silently do not have, and the copy is the first thing
# a newcomer has to do. A game is now one line.
#
#   dream_add_game(crazytaxi TITLE "Crazy Taxi" BOOT_TEST)
#
# Does nothing at all when the title's extracted binary is absent, which is every checkout but the
# owner's. Nothing from a disc is ever committed, so the configs can live here and the games cannot.
#
# TITLE      what the build prints while translating; defaults to <id>.
# BOOT_TEST  register `<id>.boot`, a CTest that boots the title as far as its first display list.
#            Only with a disc image next to the game directory and DREAM_DEV_INTERPRETER on: until
#            discovery is closed the boot reaches code the translator has not emitted, and only the
#            development configuration can carry on past that (ADR 2). The release configuration
#            aborts with the fault log by design.

set(DREAM_TRANSLATE_EXTRA_FLAGS "" CACHE STRING
    "Extra dream-translate flags for the game units (--trace, --no-irq); development trees only")

function(dream_add_game id)
  cmake_parse_arguments(ARG "BOOT_TEST" "TITLE" "" ${ARGN})
  if(NOT ARG_TITLE)
    set(ARG_TITLE ${id})
  endif()

  set(_dir ${CMAKE_CURRENT_SOURCE_DIR})
  set(_toml_path ${_dir}/${id}.toml)
  if(NOT EXISTS ${_toml_path})
    message(FATAL_ERROR "dream_add_game(${id}): no ${id}.toml in ${_dir}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_toml_path})
  file(READ ${_toml_path} _toml)

  # The executable's location is the TOML's to decide, not this file's. Assuming
  # extracted/fs/1ST_READ.BIN here would quietly ignore a config that says otherwise.
  string(REGEX MATCH "path[ \t]*=[ \t]*\"([^\"]+)\"" _m "${_toml}")
  if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "dream_add_game(${id}): ${id}.toml has no [binary] path")
  endif()
  set(_bin ${_dir}/${CMAKE_MATCH_1})
  if(NOT EXISTS ${_bin})
    return()  # nobody's disc here: not an error, just nothing to build
  endif()

  set(_gen ${CMAKE_CURRENT_BINARY_DIR}/gen)
  file(MAKE_DIRECTORY ${_gen})

  # One unit for the main binary, plus one per [[relocations]] entry: a region the program copies
  # somewhere else and runs from there, which has to be translated as it looks at its destination.
  set(_units ${_gen}/${id}.cpp)
  set(_headers ${_gen}/${id}.h)
  string(REGEX MATCHALL "dest = 0x[0-9A-Fa-f]+" _dests "${_toml}")
  set(_seen "")
  foreach(d IN LISTS _dests)
    string(REGEX REPLACE "dest = 0x" "" d "${d}")
    string(TOLOWER "${d}" d)
    # Several [[relocations]] may share a destination (overlays: the same address holding different
    # code at different times). Their later units are named _2, _3, ... exactly as
    # `dream-translate game` names them, or the build would expect files it never produces.
    list(APPEND _seen ${d})
    set(_nth 0)
    foreach(s IN LISTS _seen)
      if(s STREQUAL d)
        math(EXPR _nth "${_nth} + 1")
      endif()
    endforeach()
    if(_nth GREATER 1)
      set(d "${d}_${_nth}")
    endif()
    list(APPEND _units ${_gen}/${id}_reloc_${d}.cpp)
    list(APPEND _headers ${_gen}/${id}_reloc_${d}.h)
  endforeach()

  # A symbol table only names the emitted functions, so it is optional: a title with no Ghidra
  # database still translates, with its functions named by address. Depending on a file that need
  # not exist is how the second title failed to build at all.
  set(_symbols "")
  if(EXISTS ${_dir}/symbols.tsv)
    set(_symbols ${_dir}/symbols.tsv)
  endif()

  separate_arguments(DREAM_TRANSLATE_EXTRA_FLAGS)
  add_custom_command(
    OUTPUT ${_units} ${_headers}
    COMMAND dream-translate game --config ${_toml_path} --out-dir ${_gen} ${DREAM_TRANSLATE_EXTRA_FLAGS}
    DEPENDS dream-translate ${_toml_path} ${_symbols} ${_bin}
    COMMENT "Translating ${ARG_TITLE}"
    VERBATIM)

  add_executable(${id}_boot ${DREAM_ROOT}/runtime/boot/boot_main.cpp ${_units})
  target_link_libraries(${id}_boot PRIVATE dream::runtime dream::translator)
  # --window needs the Vulkan backend, which is optional (render/CMakeLists.txt returns early when
  # Vulkan, SDL3 or glslc are missing). Without it the launcher still builds and runs headless, and
  # --window says this build has no renderer rather than doing nothing.
  if(TARGET dream_render_vk)
    target_link_libraries(${id}_boot PRIVATE dream::render_vk)
    target_compile_definitions(${id}_boot PRIVATE DREAM_WITH_RENDERER)
  endif()
  # Sound is optional in the same way and independent of the renderer: a build with SDL3 but no
  # Vulkan still makes sound, and a build with neither still records through --wav.
  if(TARGET dream_audio)
    target_link_libraries(${id}_boot PRIVATE dream::audio)
    target_compile_definitions(${id}_boot PRIVATE DREAM_WITH_AUDIO)
  endif()
  dream_target_defaults(${id}_boot)
  if(NOT MSVC)
    # Tens of megabytes of generated C++: keep the optimiser modest so the edit-build-run loop
    # stays short.
    set_source_files_properties(${_units} PROPERTIES COMPILE_OPTIONS "-O1")
  endif()

  if(ARG_BOOT_TEST AND DREAM_DEV_INTERPRETER AND EXISTS ${_dir}/../${id}.chd)
    add_test(NAME ${id}.boot
      COMMAND ${id}_boot --config ${_toml_path} --stop-on-ta --max-frames 900
              --report ${CMAKE_CURRENT_BINARY_DIR}/boot-report.txt)
  endif()
endfunction()
