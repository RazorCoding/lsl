#!/usr/bin/env bash
#
# Smoke test for the layer's device lifetime when a process holds more than one
# instance and device.
#
# tools/upscale-check.sh and tools/resize-check.sh both drive a single window
# with a single device, which is the simplest case a layer can get.  A Proton
# game is not: SnowRunner's log shows several instances and devices in one
# process and a device destroyed while the next is made, and that is the shape
# that crashed the layer before any swapchain appeared (the next chain's entry
# points were cached in one global, so the second device overwrote the first's).
#
# tools/multidev-check.c is that shape with the window removed.  This script
# reads the three things that can be checked from outside:
#
#   1. the harness itself: every call returned VK_SUCCESS and it reached the end
#      (MULTIDEV_CHECK_OK);
#   2. the layer loaded and saw the whole lifecycle -- two vkCreateInstance and
#      four vkCreateDevice lines in its log, so the bookkeeping this checks was
#      really running and not silently skipped;
#   3. with the Khronos validation layer loaded underneath, nothing was
#      reported.  A wrong-chain dispatch is a validation error, not a quiet
#      wrong picture.
#
#   LSL_CHECK_VVL=0        skip the validation layer even when installed
#   LSL_VVL_DIR=...        where the validation layer manifest lives
#   LSL_CHECK_CC=...       the compiler for the harness (default cc)
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${ROOT}/.." && pwd)"
BUILD_IN="${1:-build}"
if [[ "${BUILD_IN}" = /* ]]; then
  BUILD="${BUILD_IN}"
else
  BUILD="${ROOT}/${BUILD_IN}"
fi
LAYER_DIR="${BUILD}/layer"

CC_BIN="${LSL_CHECK_CC:-cc}"
VVL_DIR="${LSL_VVL_DIR:-$HOME/.local/share/vulkan-validation}"
VVL_MANIFEST="${VVL_DIR}/VkLayer_khronos_validation.json"

if [[ ! -f "${LAYER_DIR}/VkLayer_lsl_frame.json" ]]; then
  echo "SKIP: ${LAYER_DIR}/VkLayer_lsl_frame.json not found -- build first" >&2
  exit 77
fi

check_vvl="${LSL_CHECK_VVL:-auto}"
use_vvl=0
if [[ -f "${VVL_MANIFEST}" ]]; then
  case "${check_vvl}" in
    0|no|off) use_vvl=0 ;;
    *) use_vvl=1 ;;
  esac
fi

TMP="$(mktemp -d -t lsl-multidev-check.XXXXXX)"
trap 'rm -rf "${TMP}"' EXIT
BIN="${TMP}/multidev-check"
LOG="${TMP}/layer.log"
OUT="${TMP}/output.log"

if ! "${CC_BIN}" -O2 -o "${BIN}" "${ROOT}/tools/multidev-check.c" -lvulkan 2>"${TMP}/cc.log"; then
  echo "SKIP: could not compile tools/multidev-check.c (needs vulkan headers)" >&2
  sed 's/^/      /' "${TMP}/cc.log" | head -10 >&2
  exit 77
fi

add_path="${LAYER_DIR}"
layers="VK_LAYER_LSL_frame"
env_extra=()
if [[ "${use_vvl}" -eq 1 ]]; then
  add_path="${LAYER_DIR}:${VVL_DIR}"
  # The injected layer must stay closest to the application, or the validation
  # layer below it never sees the layer's commands.
  layers="VK_LAYER_LSL_frame:VK_LAYER_KHRONOS_validation"
  # Make a validation layer that fails to load loud instead of silent.
  env_extra=(VK_LOADER_DEBUG=error)
fi

echo "running: tools/multidev-check.c under the layer$([[ ${use_vvl} -eq 1 ]] && echo ", with validation")"
: >"${LOG}"
env VK_ADD_LAYER_PATH="${add_path}" \
    VK_INSTANCE_LAYERS="${layers}" \
    "${env_extra[@]}" \
    LSL_LAYER_LOG="${LOG}" \
    timeout 120 "${BIN}" >"${OUT}" 2>&1
status=$?
cat "${OUT}"

fail=0
if [[ ${status} -ne 0 ]]; then
  echo "FAIL: the harness exited ${status}" >&2
  fail=1
fi
if ! grep -q 'MULTIDEV_CHECK_OK' "${OUT}"; then
  echo "FAIL: the harness did not report success" >&2
  fail=1
fi

insts="$(grep -c 'vkCreateInstance -> 0' "${LOG}" 2>/dev/null || true)"
devs="$(grep -c 'vkCreateDevice -> 0' "${LOG}" 2>/dev/null || true)"
if [[ "${insts:-0}" -lt 2 ]]; then
  echo "FAIL: the layer logged ${insts:-0} instance(s), expected 2" >&2
  fail=1
fi
if [[ "${devs:-0}" -lt 4 ]]; then
  echo "FAIL: the layer logged ${devs:-0} device(s), expected 4" >&2
  fail=1
fi

if grep -qiE 'VUID-|Validation Error' "${OUT}" "${LOG}" 2>/dev/null; then
  echo "FAIL: validation messages:" >&2
  grep -iE 'VUID-|Validation Error' "${OUT}" "${LOG}" | head -10 >&2
  fail=1
fi

if [[ ${fail} -ne 0 ]]; then
  exit 1
fi
echo "PASS: ${insts} instances, ${devs} devices, zero validation errors"
exit 0
