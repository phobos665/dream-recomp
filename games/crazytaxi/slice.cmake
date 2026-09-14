# Crazy Taxi slice for the differential harness (docs/differential-harness.md): pure leaf FPU
# functions picked from the emitted code (no calls, no memory writes). Included by
# tests/sh4/CMakeLists.txt only when the owner-supplied image exists; nothing here is
# committed from the disc.
set(_ct_image ${DREAM_ROOT}/games/crazytaxi/extracted/fs/1ST_READ.BIN)
if(EXISTS ${_ct_image})
  list(APPEND DREAM_SH4_PROGRAMS
    "ct_077320|${_ct_image}|0x0c010000|0x0c077320:0x0c07735c:ct_077320"
    "ct_077990|${_ct_image}|0x0c010000|0x0c077990:0x0c0779bc:ct_077990"
    "ct_0749c0|${_ct_image}|0x0c010000|0x0c0749c0:0x0c0749d8:ct_0749c0"
    "ct_077460|${_ct_image}|0x0c010000|0x0c077460:0x0c077488:ct_077460"
    "ct_075010|${_ct_image}|0x0c010000|0x0c075010:0x0c075056:ct_075010"
    "ct_074cd0|${_ct_image}|0x0c010000|0x0c074cd0:0x0c074d16:ct_074cd0"
    "ct_074d20|${_ct_image}|0x0c010000|0x0c074d20:0x0c074d56:ct_074d20"
    "ct_075060|${_ct_image}|0x0c010000|0x0c075060:0x0c0750a6:ct_075060"
    "ct_066d04|${_ct_image}|0x0c010000|0x0c066d04:0x0c066d62:ct_066d04"
    "ct_066d62|${_ct_image}|0x0c010000|0x0c066d62:0x0c066dcc:ct_066d62"
    "ct_0750f0|${_ct_image}|0x0c010000|0x0c0750f0:0x0c075168:ct_0750f0"
    "ct_074e50|${_ct_image}|0x0c010000|0x0c074e50:0x0c074ec6:ct_074e50"
    "ct_085e40|${_ct_image}|0x0c010000|0x0c085e40:0x0c085ee8:ct_085e40"
    "ct_07adb0|${_ct_image}|0x0c010000|0x0c07adb0:0x0c07ae3a:ct_07adb0"
    "ct_07ae40|${_ct_image}|0x0c010000|0x0c07ae40:0x0c07aecc:ct_07ae40"
    "ct_02238e|${_ct_image}|0x0c010000|0x0c02238e:0x0c0223f4:ct_02238e"
    "ct_074b50|${_ct_image}|0x0c010000|0x0c074b50:0x0c075168:ct_074b50"
    "ct_081960|${_ct_image}|0x0c010000|0x0c081960:0x0c0819ec:ct_081960"
    "ct_082ac0|${_ct_image}|0x0c010000|0x0c082ac0:0x0c082b4c:ct_082ac0"
    "ct_033d68|${_ct_image}|0x0c010000|0x0c033d68:0x0c033dda:ct_033d68"
    "ct_0386c2|${_ct_image}|0x0c010000|0x0c0386c2:0x0c038770:ct_0386c2"
    "ct_07ba60|${_ct_image}|0x0c010000|0x0c07ba60:0x0c07bac2:ct_07ba60"
    "ct_075220|${_ct_image}|0x0c010000|0x0c075220:0x0c07527c:ct_075220"
    "ct_0752d0|${_ct_image}|0x0c010000|0x0c0752d0:0x0c07532c:ct_0752d0"
    "ct_086940|${_ct_image}|0x0c010000|0x0c086940:0x0c0869ba:ct_086940"
    "ct_077b10|${_ct_image}|0x0c010000|0x0c077b10:0x0c077b44:ct_077b10"
    "ct_154306|${_ct_image}|0x0c010000|0x0c154306:0x0c15438e:ct_154306"
    "ct_0293ae|${_ct_image}|0x0c010000|0x0c0293ae:0x0c029440:ct_0293ae"
    "ct_0525b4|${_ct_image}|0x0c010000|0x0c0525b4:0x0c052626:ct_0525b4"
    "ct_14a404|${_ct_image}|0x0c010000|0x0c14a404:0x0c14a4f8:ct_14a404"
    "ct_14b860|${_ct_image}|0x0c010000|0x0c14b860:0x0c14b8d0:ct_14b860"
  )
endif()
