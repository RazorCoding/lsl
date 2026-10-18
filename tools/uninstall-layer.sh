#!/usr/bin/env bash
#
# Remove a layer installed by tools/install-layer.sh.
#
#   tools/uninstall-layer.sh [--prefix DIR]
#
# Only the lsl layer directory is touched.  Steam launch options still carry the
# VK_ADD_LAYER_PATH/VK_INSTANCE_LAYERS line afterwards -- they are per game and
# this script has no business rewriting Steam's files -- so it prints the line
# to look for.  lsl-gui clears it (its "launch options" toggle) or you can.
set -eu

PREFIX="${PREFIX:-$HOME/.local}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix) PREFIX="${2:?--prefix needs a directory}"; shift 2 ;;
    -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "uninstall-layer: unknown argument: $1" >&2; exit 2 ;;
  esac
done

MANIFEST="$(find "$PREFIX" -name VkLayer_lsl_frame.json -print -quit 2>/dev/null || true)"
if [[ -z "$MANIFEST" ]]; then
  echo "uninstall-layer: nothing installed under $PREFIX"
  exit 0
fi

DIR="$(cd "$(dirname "$MANIFEST")" && pwd)"
# Only ever remove a path that looks exactly like ours: <prefix>/<libdir>/lsl/layer.
if [[ "$(basename "$DIR")" != "layer" || "$(basename "$(dirname "$DIR")")" != "lsl" ]]; then
  echo "uninstall-layer: refusing to remove an unexpected directory: $DIR" >&2
  exit 1
fi

rm -rf "$DIR"
# Tidy up the now-empty parents, but leave anything else in them alone.
rmdir "$(dirname "$DIR")" 2>/dev/null || true

echo "removed $DIR"
echo
echo "Reminder: any game that had lsl enabled still has this in its Steam launch"
echo "options, and will log a loader warning until it is cleared:"
echo "  VK_ADD_LAYER_PATH=\"$PREFIX/lib/lsl/layer\" VK_INSTANCE_LAYERS=VK_LAYER_LSL_frame"
