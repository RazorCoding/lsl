#!/usr/bin/env bash
# Smoke test for the Phase 2 injection layer.
#
# The layer is a Vulkan layer, so "does it work" is not a unit test -- it is
# "does the loader insert it into a real application and does it see that
# application's presentation".  vkcube is used as a stand-in for a game: it owns
# a swapchain and presents, which is all Phase 2 needs to exercise.
#
# Two checks, both non-visual (results are read from the log, not by looking at
# a window):
#
#   1. observer   -- the layer loads and logs vkQueuePresentKHR
#   2. capture    -- the layer rewrites the swapchain and reads the presented
#                    image back; the frame hash must move, because the cube is
#                    spinning, which is what proves real pixels came out
#
# Usage: tools/layer-smoke.sh [build-dir]   (default: build)
#
# It opens a short-lived window on the current session.  Run it from a
# graphical session, not a headless one.  It does not install or modify
# anything outside the build directory.

set -u

BUILD_DIR="${1:-build}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
case "${BUILD_DIR}" in
  /*) BUILD_ROOT="${BUILD_DIR}" ;;
  *) BUILD_ROOT="${ROOT}/${BUILD_DIR}" ;;
esac
LAYER_DIR="${BUILD_ROOT}/layer"
LOG="$(mktemp -t lsl-layer-smoke.XXXXXX.log)"

if [[ ! -f "${LAYER_DIR}/VkLayer_lsl_frame.json" ]]; then
  echo "SKIP: ${LAYER_DIR}/VkLayer_lsl_frame.json not found -- build first:" >&2
  echo "      cmake -S . -B ${BUILD_DIR} -G Ninja && cmake --build ${BUILD_DIR}" >&2
  exit 77
fi

if ! command -v vkcube >/dev/null 2>&1; then
  echo "SKIP: vkcube not installed (vulkan-tools)"
  exit 77
fi

if [[ -z "${WAYLAND_DISPLAY:-}" && -z "${DISPLAY:-}" ]]; then
  echo "SKIP: no WAYLAND_DISPLAY or DISPLAY; vkcube needs a session"
  exit 77
fi

run_vkcube() {
  # $1 = extra env for the layer, $2 = frame count
  env VK_ADD_LAYER_PATH="${LAYER_DIR}" \
      VK_INSTANCE_LAYERS=VK_LAYER_LSL_frame \
      LSL_LAYER_LOG="${LOG}" \
      $1 \
      timeout 60 vkcube --c "$2" >/dev/null 2>&1
}

status=0

: > "${LOG}"
if ! run_vkcube "" 8; then
  echo "FAIL: vkcube did not exit cleanly with the layer loaded" >&2
  status=1
elif ! grep -q 'present #' "${LOG}"; then
  echo "FAIL: the layer did not observe any present" >&2
  echo "      log: ${LOG}" >&2
  status=1
else
  echo "ok  observer: $(grep -c 'present #' "${LOG}") present(s) logged"
fi

: > "${LOG}"
run_vkcube "LSL_LAYER_CAPTURE=1" 30
run_status=$?
captured=$(grep -c 'captured frame' "${LOG}")
distinct=$(grep -o 'hash [0-9a-f]*' "${LOG}" | sort -u | wc -l)
# Past a handful of frames vkcube can block on a frame callback on some
# compositors rather than exiting; the captures are written by then, so a
# timeout is accepted as long as enough frames came out.
if [[ "${run_status}" -ne 0 && "${run_status}" -ne 124 ]]; then
  echo "FAIL: vkcube exited ${run_status} with capture enabled" >&2
  status=1
elif [[ "${captured}" -lt 5 ]]; then
  echo "FAIL: capture read ${captured} frame(s); expected at least 5" >&2
  echo "      log: ${LOG}" >&2
  status=1
elif [[ "${distinct}" -lt 2 ]]; then
  echo "FAIL: ${captured} frames captured but all ${distinct} hashes identical" >&2
  echo "      the image was copied but never changed; that is not a live frame" >&2
  status=1
else
  note=""
  [[ "${run_status}" -eq 124 ]] && note=" (run timed out after the captures)"
  echo "ok  capture:  ${captured} frames read, ${distinct} distinct hashes${note}"
fi

rm -f "${LOG}"
exit "${status}"
