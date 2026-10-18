#!/usr/bin/env bash
#
# Install the layer somewhere stable, so a game can use it without this tree.
#
# The layer is enabled per game from its Steam launch options, not globally, so
# "installing" it means putting the shared object and its manifest where they
# will stay put and pointing the launch line at them.  This deliberately does
# *not* install under vulkan/implicit_layer.d: that would load the layer into
# every Vulkan application on the machine, which is not what a per-game upscaler
# should do.
#
#   tools/install-layer.sh [--prefix DIR] [--build DIR] [--no-build]
#
# The layer lands in <prefix>/lib/lsl/layer (or lib64, whichever GNUInstallDirs
# picks) -- the same layout the build tree's build/layer has, so the GUI's
# directory check works against either.  PREFIX defaults to $PREFIX if set, else
# ~/.local, so no root is needed.
#
# The manifest has to carry an absolute library_path -- the loader does not
# resolve it relative to the manifest -- and this script writes it at install
# time, after checking that it really names a file that exists.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
BUILD="${ROOT}/build"
DO_BUILD=1

usage() {
  sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix) PREFIX="${2:?--prefix needs a directory}"; shift 2 ;;
    --build)  BUILD="${2:?--build needs a directory}"; shift 2 ;;
    --no-build) DO_BUILD=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "install-layer: unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

if [[ ! -f "$BUILD/CMakeCache.txt" ]]; then
  echo "install-layer: $BUILD is not a configured build tree." >&2
  echo "  configure one first:" >&2
  echo "    cmake -S \"$ROOT\" -B \"$BUILD\" -G Ninja -DCMAKE_BUILD_TYPE=Release" >&2
  exit 1
fi

if [[ "$DO_BUILD" -eq 1 ]]; then
  cmake --build "$BUILD"
fi

cmake --install "$BUILD" --prefix "$PREFIX"

# GNUInstallDirs may choose lib or lib64, so find what was actually written
# rather than guessing the directory.
MANIFEST="$(find "$PREFIX" -name VkLayer_lsl_frame.json -print -quit 2>/dev/null || true)"
if [[ -z "$MANIFEST" ]]; then
  echo "install-layer: the install wrote no manifest under $PREFIX" >&2
  exit 1
fi
LAYER_DIR="$(cd "$(dirname "$MANIFEST")" && pwd)"

# The one thing that must be true for the loader to find the object.
LIB="$(sed -n 's/.*"library_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$MANIFEST")"
if [[ -z "$LIB" || "$LIB" != /* ]]; then
  echo "install-layer: $MANIFEST has a non-absolute library_path: '${LIB}'" >&2
  exit 1
fi
if [[ ! -f "$LIB" ]]; then
  echo "install-layer: the manifest names a library that is not there:" >&2
  echo "  $LIB" >&2
  exit 1
fi

cat <<EOF
installed:
  manifest      $MANIFEST
  library       $LIB

For one game, this is the launch-options line (lsl-gui writes it for you):

  VK_ADD_LAYER_PATH="$LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_LSL_frame LSL_LAYER_SCALE=2 %command%

Unset LSL_LAYER_SCALE (or set it to 1) and the layer loads but does nothing, so
the line can stay in place while scaling is off.  Remove the install with
tools/uninstall-layer.sh --prefix "$PREFIX".
EOF
