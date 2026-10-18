#!/usr/bin/env bash
#
# Remove a layer installed by tools/install-layer.sh.
#
#   tools/uninstall-layer.sh [--prefix DIR]
#
# Both installs are removed: the layer directory, and -- if it is there -- the
# implicit-layer manifest under <prefix>/share/vulkan/implicit_layer.d, which is
# what the loader reads on its own.  Nothing else is touched.  Steam launch
# options still carry the VK_ADD_LAYER_PATH/VK_INSTANCE_LAYERS line afterwards --
# they are per game and this script has no business rewriting Steam's files -- so
# it prints the line to look for.  lsl-gui clears it, or you can.
set -eu

PREFIX="${PREFIX:-$HOME/.local}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix) PREFIX="${2:?--prefix needs a directory}"; shift 2 ;;
    -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "uninstall-layer: unknown argument: $1" >&2; exit 2 ;;
  esac
done

# The implicit copy first: if it is still there when the search below runs, the
# search could find it and then "remove" a directory that is not a layer dir.
IMPLICIT="$PREFIX/share/vulkan/implicit_layer.d/VkLayer_lsl_frame.json"
if [[ -f "$IMPLICIT" ]]; then
  rm -f "$IMPLICIT"
  rmdir "$(dirname "$IMPLICIT")" 2>/dev/null || true
  rmdir "$(dirname "$(dirname "$IMPLICIT")")" 2>/dev/null || true
  echo "removed:    $IMPLICIT"
fi

MANIFEST="$(find "$PREFIX" -path '*/lsl/layer/VkLayer_lsl_frame.json' -print -quit 2>/dev/null || true)"
if [[ -z "$MANIFEST" ]]; then
  echo "uninstall-layer: no layer directory under $PREFIX"
  exit 0
fi

DIR="$(cd "$(dirname "$MANIFEST")" && pwd)"
# Only ever remove a path that looks exactly like ours: <prefix>/<libdir>/lsl/layer.
if [[ "$(basename "$DIR")" != "layer" || "$(basename "$(dirname "$DIR")")" != "lsl" ]]; then
  echo "uninstall-layer: refusing to remove a path that is not ours:" >&2
  echo "  $DIR" >&2
  exit 1
fi

rm -rf "$DIR"
rmdir "$(dirname "$DIR")" 2>/dev/null || true   # <prefix>/<libdir>/lsl, if empty
echo "removed:    $DIR"

cat <<EOF

Steam launch options still carry the per-game line (lsl-gui clears it):

  VK_ADD_LAYER_PATH="$DIR" VK_INSTANCE_LAYERS=VK_LAYER_LSL_frame LSL_LAYER_SCALE=2 %command%
EOF
