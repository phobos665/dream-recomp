# Wraps glslc's -mfmt=c output (a bare brace-enclosed list of words) in a C++ declaration, so the
# generated header can simply be included. Invoked with -DIN= -DOUT= -DSYM=.
file(READ ${IN} _words)
file(WRITE ${OUT}
     "// Generated from a GLSL shader by glslc. Do not edit.\n"
     "#pragma once\n#include <cstdint>\n"
     "static const std::uint32_t ${SYM}[] = ${_words};\n")
