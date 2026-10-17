#!/usr/bin/env python3
"""Small helpers for the layer's PPM dumps.

The upscale check needs two things the shell is bad at: flipping the R/B
channel order of a P6 image, and comparing two P6 images.  Channel order is not
a detail here.  EASU's luma is ``0.5*B + (0.5*R + G)``, which is symmetric in R
and B mathematically but not bit-for-bit: swapping the two changes the order of
two floating-point additions, so a strong gradient can land one ULP apart.  The
layer's swapchain is BGRA while its dump is written in human RGB order, so the
check compares in the shader's own channel order and is then exact.

Usage:
    ppm_tool.py swap IN OUT
    ppm_tool.py cmp A B [--max-bytes N] [--max-delta N] [--min-psnr DB]
"""

import math
import sys


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError(f"{path}: not a P6 image")
    fields = []
    i = 2
    while len(fields) < 3:
        while i < len(data) and data[i : i + 1].isspace():
            i += 1
        if data[i : i + 1] == b"#":
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        start = i
        while i < len(data) and not data[i : i + 1].isspace():
            i += 1
        fields.append(int(data[start:i]))
    i += 1  # the single whitespace byte after maxval
    w, h, maxval = fields
    if maxval != 255:
        raise ValueError(f"{path}: only 8-bit P6 images are supported")
    body = data[i:]
    if len(body) != w * h * 3:
        raise ValueError(f"{path}: expected {w * h * 3} bytes, found {len(body)}")
    return w, h, body


def write_ppm(path, w, h, body):
    with open(path, "wb") as f:
        f.write(f"P6\n{w} {h}\n255\n".encode())
        f.write(body)


def cmd_swap(argv):
    if len(argv) != 2:
        print("usage: ppm_tool.py swap IN OUT")
        return 2
    src, dst = argv
    w, h, body = read_ppm(src)
    out = bytearray(body)
    for i in range(0, len(out), 3):
        out[i], out[i + 2] = body[i + 2], body[i]
    write_ppm(dst, w, h, out)
    return 0


def cmd_cmp(argv):
    max_bytes = None
    max_delta = None
    min_psnr = None
    files = []
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--max-bytes":
            i += 1
            max_bytes = int(argv[i])
        elif arg == "--max-delta":
            i += 1
            max_delta = int(argv[i])
        elif arg == "--min-psnr":
            i += 1
            min_psnr = float(argv[i])
        else:
            files.append(arg)
        i += 1

    if len(files) != 2:
        print("usage: ppm_tool.py cmp A B [--max-bytes N] [--max-delta N] [--min-psnr DB]")
        return 2

    wa, ha, a = read_ppm(files[0])
    wb, hb, b = read_ppm(files[1])
    if (wa, ha) != (wb, hb):
        print(f"FAIL: size mismatch: {wa}x{ha} vs {wb}x{hb}")
        return 1

    diff_bytes = sum(1 for x, y in zip(a, b) if x != y)
    max_seen = max((abs(x - y) for x, y in zip(a, b)), default=0)
    mse = sum((x - y) ** 2 for x, y in zip(a, b)) / max(1, len(a))
    psnr = float("inf") if mse == 0 else 10.0 * math.log10(255 * 255 / mse)
    psnr_text = "inf" if mse == 0 else f"{psnr:.2f} dB"

    print(f"  differing bytes  : {diff_bytes}/{len(a)}")
    print(f"  max channel delta: {max_seen}")
    print(f"  PSNR             : {psnr_text}")

    ok = True
    if max_bytes is not None and diff_bytes > max_bytes:
        print(f"FAIL: {diff_bytes} differing bytes, limit {max_bytes}")
        ok = False
    if max_delta is not None and max_seen > max_delta:
        print(f"FAIL: max channel delta {max_seen}, limit {max_delta}")
        ok = False
    if min_psnr is not None and mse != 0 and psnr < min_psnr:
        print(f"FAIL: PSNR {psnr:.2f} dB is below {min_psnr:.2f} dB")
        ok = False
    return 0 if ok else 1


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    if argv[0] == "swap":
        return cmd_swap(argv[1:])
    if argv[0] == "cmp":
        return cmd_cmp(argv[1:])
    print(f"unknown subcommand: {argv[0]}")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
