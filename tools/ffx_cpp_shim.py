#!/usr/bin/env python3
"""Make a C++-compilable copy of AMD's FSR1 header.

third_party/ffx/ffx_fsr1.h is written in the subset of GLSL that
third_party/ffx/ffx_a.h maps onto whichever language is being targeted. It
spells in/out parameters the GLSL way:

    void FsrEasuTapF(inout AF3 aC, inout AF1 aW, ...)

There is no C++ macro that can express that: a macro can only sit *before* the
type, and C++ requires the '&' to follow it. So the FSR1 CPU reference gets a
build-tree copy in which exactly that one qualifier is rewritten into a C++
reference. Nothing else is touched, the vendored original is never modified,
and the original is the copy shaders/scale.comp and shaders/rcas.comp include -
so the CPU and GPU still compile the same algorithm. The rewrite is verified to
be total, and an empty result is treated as an error so a future header update
cannot silently turn the reference into a no-op.
"""

import re
import sys

if len(sys.argv) != 3:
    sys.exit(f"usage: {sys.argv[0]} <ffx_fsr1.h> <out.h>")

src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8").read()

# "inout AF3 aC" / "out AF3 pix" -> "AF3& aC" / "AF3& pix", also when several
# share a line. "inout" is tried first so its trailing "out" is not re-matched.
qualifier = re.compile(r"\b(inout|out)[ \t]+(A[FHLUD][0-9])\b")
out, count = qualifier.subn(r"\2&", text)

if count == 0:
    sys.exit(f"{src}: no 'inout' parameters found - did the header change?")
leftover = qualifier.search(out)
if leftover:
    sys.exit(f"{src}: qualifier {leftover.group(0)!r} was not rewritten")

header = (
    f"// Generated from {src} by tools/ffx_cpp_shim.py - do not edit, do not commit.\n"
    f"// Only the {count} GLSL 'inout' parameter qualifiers were rewritten; AMD's\n"
    "// MIT notice and the algorithm itself are preserved verbatim below.\n"
)
with open(dst, "w", encoding="utf-8") as f:
    f.write(header + out)
