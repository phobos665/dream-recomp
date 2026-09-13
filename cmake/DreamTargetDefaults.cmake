# Apply the project-wide compile settings to a target. Every dream target, and every translation unit
# of emitted game code, must go through here so that ADR 16 holds:
#   - no floating-point contraction (an FMA on ARM64 but not on x86-64 changes results in the last
#     bit), hence -ffp-contract=off and /fp:strict;
#   - no compile-time evaluation under the compiler's default rounding: the guest runs with
#     FPSCR.RM = round-toward-zero and switches modes at run time, so a folded 1.0f/3.0f (found by
#     the differential harness, docs/differential-harness.md) is wrong in the last bit. Hence
#     -frounding-math and /fp:strict, which make the compiler assume a dynamic rounding mode;
#   - warnings on, and as errors by default, so portability problems surface on the first platform
#     that sees them rather than the third.
function(dream_target_defaults tgt)
  target_compile_features(${tgt} PUBLIC cxx_std_20)
  if(MSVC)
    target_compile_options(${tgt} PRIVATE
      /W4 /permissive- /Zc:preprocessor /Zc:__cplusplus /utf-8
      /fp:strict /fp:except-
      /external:W0   # third-party headers (doctest, libchdr) are SYSTEM includes; do not warn on them
      $<$<BOOL:${DREAM_WERROR}>:/WX>)
    target_compile_definitions(${tgt} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  else()
    target_compile_options(${tgt} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
      -ffp-contract=off -frounding-math
      $<$<BOOL:${DREAM_WERROR}>:-Werror>)
  endif()
  if(DREAM_DEV_INTERPRETER)
    target_compile_definitions(${tgt} PRIVATE DREAM_DEV_INTERPRETER=1)
  endif()
  set_target_properties(${tgt} PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
endfunction()
