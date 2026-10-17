#!/usr/bin/env python3
"""Embed SPIR-V files as C/C++ arrays so lsl binaries need no runtime data files.

usage: embed_spv.py [--lang c|cpp] <spv_dir> <out_src> <out_hdr>

--lang cpp (the default) emits a C++ translation unit; --lang c emits the same
arrays behind a C header, because the Vulkan layer is C and must not link a C++
runtime. """
import re
import sys
from pathlib import Path


def ident(stem: str) -> str:
    return re.sub(r"[^0-9A-Za-z_]", "_", stem)


def main() -> int:
    args = sys.argv[1:]
    lang = "cpp"
    if args and args[0].startswith("--lang"):
        if "=" in args[0]:
            lang = args[0].split("=", 1)[1]
        else:
            lang = args[1]
            args = args[1:]
        args = args[1:]
    if len(args) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    if lang not in ("c", "cpp"):
        print(f"unknown --lang {lang!r}", file=sys.stderr)
        return 2

    spv_dir, out_src, out_hdr = (Path(a) for a in args)

    blobs = []
    for spv in sorted(spv_dir.glob("*.spv")):
        words = list(spv.read_bytes())
        while len(words) % 4:
            words.append(0)
        data = [
            int.from_bytes(words[i : i + 4], "little") for i in range(0, len(words), 4)
        ]
        blobs.append((ident(spv.stem), data))

    if lang == "c":
        hpp = ["#pragma once", "#include <stddef.h>", "#include <stdint.h>", ""]
    else:
        hpp = ["#pragma once", "#include <cstddef>", "#include <cstdint>", ""]
    cpp = [f'#include "{out_hdr.name}"', ""]

    for name, data in blobs:
        hpp.append(f"extern const uint32_t lsl_spv_{name}[];")
        hpp.append(f"extern const size_t  lsl_spv_{name}_words;")
        cpp.append(f"const uint32_t lsl_spv_{name}[] = {{")
        for i in range(0, len(data), 6):
            chunk = ", ".join(f"0x{w:08x}u" for w in data[i : i + 6])
            cpp.append(f"    {chunk},")
        cpp.append("};")
        cpp.append(f"const size_t lsl_spv_{name}_words = {len(data)};")
        cpp.append("")

    out_hdr.write_text("\n".join(hpp) + "\n")
    out_src.write_text("\n".join(cpp) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
