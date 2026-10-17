#!/usr/bin/env bash
#
# Install the Khronos Vulkan validation layers for the current user only.
#
# Why not just `pacman -S vulkan-validation-layers`?  It needs root.  This
# instead downloads the same official Arch package and unpacks it under
# ~/.local/opt, then writes a loader manifest under ~/.local/share.  Nothing
# outside $HOME is touched and removal is `rm -rf` of those two directories.
#
# Two things learnt the hard way, both encoded below:
#
#   1. The manifest must use an ABSOLUTE library_path.  The loader does *not*
#      resolve a relative library_path against the directory containing the
#      manifest -- it searches LD_LIBRARY_PATH and the system library paths
#      instead.  With the packaged relative path ("libVkLayer_khronos_
#      validation.so") the loader reports
#          "Requested layer VK_LAYER_KHRONOS_validation failed to load."
#      and every subsequent run looks clean while validating nothing at all.
#      (The same mistake is why the system-installed lsfg-vk layer never loads
#      on this host.)
#
#   2. Presence of the layer is not evidence it is running.  Use
#      tools/validation-check.sh, which proves activity by requiring a
#      deliberate violation to be caught.
#
# Usage:
#   tools/setup-validation-layers.sh [package-url]
#   LSL_VVL_URL=... tools/setup-validation-layers.sh
#
set -euo pipefail

VERSION="${LSL_VVL_VERSION:-1.4.363.0-1}"
URL="${1:-${LSL_VVL_URL:-https://geo.mirror.pkgbuild.com/extra/os/x86_64/vulkan-validation-layers-${VERSION}-x86_64.pkg.tar.zst}}"

OPT_DIR="${HOME}/.local/opt/vulkan-validation-layers"
MAN_DIR="${LSL_VVL_DIR:-${HOME}/.local/share/vulkan-validation}"

echo "==> package: $URL"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
curl -fsSL --max-time 300 -o "$tmp/pkg.tar.zst" "$URL"

echo "==> unpacking into $OPT_DIR"
rm -rf "$OPT_DIR"
mkdir -p "$OPT_DIR"
bsdtar -xf "$tmp/pkg.tar.zst" -C "$OPT_DIR"

lib="$(find "$OPT_DIR" -name 'libVkLayer_khronos_validation.so' | head -1)"
json="$(find "$OPT_DIR" -name 'VkLayer_khronos_validation.json' | head -1)"
[[ -n "$lib" && -n "$json" ]] || { echo "!! expected files not found in package" >&2; exit 1; }

echo "==> shared library dependencies"
if ldd "$lib" | grep -q 'not found'; then
    echo "!! missing dependencies:" >&2
    ldd "$lib" | grep 'not found' >&2
    exit 1
fi
echo "    all satisfied"

echo "==> writing manifest into $MAN_DIR"
rm -rf "$MAN_DIR"
mkdir -p "$MAN_DIR"
ln -sf "$lib" "$MAN_DIR/$(basename "$lib")"
python3 - "$json" "$MAN_DIR/VkLayer_khronos_validation.json" "$lib" <<'PY'
import json, sys
src, dst, lib = sys.argv[1], sys.argv[2], sys.argv[3]
d = json.load(open(src))
assert d["layer"]["name"] == "VK_LAYER_KHRONOS_validation", d["layer"]["name"]
d["layer"]["library_path"] = lib          # absolute: see the header comment
json.dump(d, open(dst, "w"), indent=4)
print("    library_path ->", lib)
PY

# Prove it actually loads rather than just claiming success.
echo "==> verifying the layer really activates"
check="$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/validation-check.sh"
if "$check" true >/dev/null 2>&1; then
    echo "    OK"
else
    echo "    layer installed but could not be verified" >&2
    exit 1
fi

cat <<EOF

Done.  Use it with:

    tools/with-validation.sh ./build/lsl-upscale --make-test 640x400 --dst 1600x1000
    tools/validation-check.sh ./build/lsl-upscale --make-test 640x400 --dst 1600x1000

To remove:

    rm -rf "$OPT_DIR" "$MAN_DIR"
EOF
