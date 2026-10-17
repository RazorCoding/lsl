#!/usr/bin/env python3
"""Embed SPIR-V files as C++ arrays so lsl binaries need no runtime data files.

usage: embed_spv.py <spv_dir> <out_cpp> <out_hpp>
"""
import re
import sys
from pathlib import Path


def ident(stem: str) -> str:
    return re.sub(r"[^0-9A-Za-z_]", "_", stem)


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2

    spv_dir, out_cpp, out_hpp = (Path(a) for a in sys.argv[1:])

    blobs = []
    for spv in sorted(spv_dir.glob("*.spv")):
        words = list(spv.read_bytes())
        while len(words) % 4:
            words.append(0)
        data = [
            int.from_bytes(words[i : i + 4], "little") for i in range(0, len(words), 4)
        ]
        blobs.append((ident(spv.stem), data))

    hpp = ["#pragma once", "#include <cstddef>", "#include <cstdint>", ""]
    cpp = [f'#include "spv_blob.hpp"', ""]

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

    out_hpp.write_text("\n".join(hpp) + "\n")
    out_cpp.write_text("\n".join(cpp) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
