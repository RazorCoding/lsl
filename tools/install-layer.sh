#!/usr/bin/env bash
#
# Install the layer somewhere stable, so a game can use it without this tree.
#
# There are two ways to enable it, and --implicit chooses between them:
#
#   (default)     Put the layer where a launch-options line can point at it.
#                 The line names the layer and the scale together, and only the
#                 game it is written for loads it.  lsl-gui writes that line.
#
#   --implicit    Additionally drop the manifest into the loader's own
#                 implicit-layer directory, so the loader loads the layer into
#                 every Vulkan process without being asked.  The layer then
#                 decides, from the SteamAppId Steam puts in a game's
#                 environment, whether this process is one the GUI enabled; a
#                 process that is not stays silent and does nothing.  This is
#                 what lets a game upscale from the GUI alone -- no launch
#                 options, nothing in Steam's configuration, no restart.
#
#   tools/install-layer.sh [--prefix DIR] [--build DIR] [--no-build] [--implicit]
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
DO_IMPLICIT=0

usage() {
  sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix) PREFIX="${2:?--prefix needs a directory}"; shift 2 ;;
    --build)  BUILD="${2:?--build needs a directory}"; shift 2 ;;
    --no-build) DO_BUILD=0; shift ;;
    --implicit) DO_IMPLICIT=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "install-layer: unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

DISK_PREFIX="${DESTDIR:-}${PREFIX}"
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
# rather than guessing the directory.  Restricted to lsl/layer: a --implicit
# install leaves a second copy of the manifest in the loader's directory, and
# this has to keep finding the one the launch-options line points at.
MANIFEST="$(find "$DISK_PREFIX" -path '*/lsl/layer/VkLayer_lsl_frame.json' -print -quit 2>/dev/null || true)"
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
if [[ ! -f "${DESTDIR:-}$LIB" ]]; then
  echo "install-layer: the manifest names a library that is not there:" >&2
  echo "  $LIB" >&2
  exit 1
fi

# The implicit copy is byte-identical -- same layer name, same absolute
# library_path -- so the only thing that changes is where the loader finds it:
# a file it scans on its own instead of one a launch line adds.
IMPLICIT_DIR=
if [[ "$DO_IMPLICIT" -eq 1 ]]; then
  IMPLICIT_DIR="$DISK_PREFIX/share/vulkan/implicit_layer.d"
  mkdir -p "$IMPLICIT_DIR"
  cp "$MANIFEST" "$IMPLICIT_DIR/VkLayer_lsl_frame.json"
fi

echo "installed:"
echo "  manifest      $MANIFEST"
echo "  library       $LIB"
if [[ -n "$IMPLICIT_DIR" ]]; then
  echo "  implicit      $IMPLICIT_DIR/VkLayer_lsl_frame.json"
  cat <<EOF

The loader now finds the layer on its own, so no launch options are needed.  A
game is upscaled when it is enabled in lsl-gui and Steam starts it -- the layer
reads SteamAppId and looks it up in the profile store -- and every other Vulkan
program loads it and does nothing.  Set LSL_LAYER_DISABLE=1 to keep it out
everywhere.  The launch-options line still works too, and an LSL_LAYER_SCALE in
it takes precedence over the store:
EOF
else
  cat <<EOF

For one game, this is the launch-options line (lsl-gui writes it for you):
EOF
fi
cat <<EOF

  VK_ADD_LAYER_PATH="$LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_LSL_frame LSL_LAYER_SCALE=2 %command%

Unset LSL_LAYER_SCALE (or set it to 1) and the layer loads but does nothing, so
the line can stay in place while scaling is off.  Remove the install with

  tools/uninstall-layer.sh --prefix "$PREFIX"
EOF
