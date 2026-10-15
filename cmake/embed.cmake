# Converts a binary file into a C++ byte array: -DIN=<file> -DOUT=<cpp>
file(READ ${IN} HEXDATA HEX)
string(LENGTH "${HEXDATA}" HEXLEN)
math(EXPR NBYTES "${HEXLEN} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BYTES "${HEXDATA}")
file(WRITE ${OUT} "// generated from ${IN}\n#include <cstddef>\nextern const unsigned char g_embedded_font[] = {${BYTES}};\nextern const size_t g_embedded_font_size = ${NBYTES};\n")
