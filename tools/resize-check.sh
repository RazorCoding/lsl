#!/usr/bin/env bash
#
# Verify the layer's swapchain path when a swapchain is recreated.
#
# tools/upscale-check.sh proves the upscale itself -- the pixels the layer
# produces at present time match the Phase 1 reference -- but it presents one
# swapchain that never changes.  What an engine actually does on an alt-tab, a
# resolution change or a window drag -- vkCreateSwapchainKHR with oldSwapchain
# set, a different extent every time, the old swapchain destroyed right after --
# is the layer's other job, and the one where a missing wait turns into a
# use-after-free instead of a wrong picture.
#
# tools/resize-check.c drives that: an XCB window, a swapchain created from the
# *previous* one on every resize, two frames in flight, and a fixed pattern so
# the upscaler has something to upscale.  This script then reads the four things
# that can be checked from outside:
#
#   1. the layer logged "- upscaling" for every swapchain the program created,
#      so the extent it lies about and the rebuild both survived every resize;
#   2. at least one resize rebuilt the buffers without rebuilding the pipelines
#      ("pipelines kept"), so the extent-only path is what ran;
#   3. the frame the layer dumped (LSL_LAYER_DUMP dumps the first frame it
#      upscales) came from a recreated swapchain, and matches the Phase 1
#      upscaler byte for byte -- the end-to-end proof, since upscale-check.sh
#      can only ever dump a swapchain that was never recreated;
#   4. with the Khronos validation layer loaded underneath the injected layer,
#      nothing is reported.  The same deliberate-error pass upscale-check.sh
#      uses runs first, so silence means something.
#
#   LSL_CHECK_VVL=0        skip the validation layer even when installed
#   LSL_CHECK_SELFTEST=0   skip the deliberate-error pass
#   LSL_VVL_DIR=...        where the validation layer manifest lives
#   LSL_CHECK_SCALE=N      the LSL_LAYER_SCALE to run with (default 2)
#   LSL_CHECK_CC=...       the compiler for the harness (default cc)
#
# Needs a display: the window is where the resize comes from.  Skips (77)
# without one.
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
UPSCALE="${BUILD}/lsl-upscale"
PPM_TOOL="${ROOT}/tools/ppm_tool.py"
SCALE="${LSL_CHECK_SCALE:-2}"
CC_BIN="${LSL_CHECK_CC:-cc}"
VVL_DIR="${LSL_VVL_DIR:-${HOME}/.local/share/vulkan-validation}"
VVL_MANIFEST="${VVL_DIR}/VkLayer_khronos_validation.json"

for f in "${LAYER_DIR}/VkLayer_lsl_frame.json" "${UPSCALE}" "${PPM_TOOL}"; do
  if [[ ! -f "${f}" ]]; then
    echo "SKIP: ${f} not found -- build first:" >&2
    echo "      cmake -S . -B ${BUILD_IN} -G Ninja && cmake --build ${BUILD_IN}" >&2
    exit 77
  fi
done

if [[ -z "${DISPLAY:-}" ]]; then
  echo "SKIP: the resize harness needs DISPLAY (X11; XWayland is fine)" >&2
  exit 77
fi

PY="$(command -v python3 || true)"
if [[ -z "${PY}" ]]; then
  echo "SKIP: python3 not found" >&2
  exit 77
fi

# How the two runs are configured.  Default: validate when installed.
check_vvl="${LSL_CHECK_VVL:-auto}"
use_vvl=0
if [[ -f "${VVL_MANIFEST}" ]]; then
  case "${check_vvl}" in
    0 | no | off) use_vvl=0 ;;
    *) use_vvl=1 ;;
  esac
else
  echo "note: validation layers not installed; the resize is still checked"
  echo "      for pixels, but not for validation errors. Install with:"
  echo "      ${ROOT}/tools/setup-validation-layers.sh"
fi

TMP="$(mktemp -d -t lsl-resize-check.XXXXXX)"
trap 'rm -rf "${TMP}"' EXIT
BIN="${TMP}/resize-check"
LOG="${TMP}/layer.log"
DUMP="${TMP}/dump"
OUT="${TMP}/output.log"

echo "building the harness: ${CC_BIN} tools/resize-check.c"
# The harness is a tool, not part of the library: it is built here, beside the
# scripts that use it, the way validation-check.sh builds its self-test.
if ! "${CC_BIN}" -O2 -o "${BIN}" "${ROOT}/tools/resize-check.c" \
     $(pkg-config --cflags --libs xcb 2>/dev/null) -lvulkan 2>"${TMP}/cc.log"; then
  echo "SKIP: could not compile tools/resize-check.c (needs xcb and vulkan headers)" >&2
  sed 's/^/      /' "${TMP}/cc.log" | head -10 >&2
  exit 77
fi

add_path="${LAYER_DIR}"
layers="VK_LAYER_LSL_frame"
vvl_env=""
if [[ "${use_vvl}" -eq 1 ]]; then
  add_path="${LAYER_DIR}:${VVL_DIR}"
  # The injected layer must stay closest to the application, or the validation
  # layer below it never sees the commands the layer records.
  layers="VK_LAYER_LSL_frame:VK_LAYER_KHRONOS_validation"
  vvl_env="VK_LOADER_DEBUG=error"
fi

# A clean validation run proves nothing on its own, so the deliberate-error
# pass comes first: the capture path is told to leave the image in the wrong
# layout, which the validation layer has to report.
if [[ "${use_vvl}" -eq 1 ]]; then
  case "${LSL_CHECK_SELFTEST:-auto}" in
    0 | no | off) ;;
    *)
      echo "self-test: one capture pass with a deliberate wrong layout, which validation must report"
      env VK_ADD_LAYER_PATH="${add_path}" \
          VK_INSTANCE_LAYERS="${layers}" \
          LSL_LAYER_LOG="${TMP}/selftest.log" \
          LSL_LAYER_CAPTURE=1 \
          LSL_LAYER_CAPTURE_FRAMES=6 \
          LSL_LAYER_BREAK_LAYOUT=1 \
          ${vvl_env} \
          timeout 120 "${BIN}" >"${TMP}/selftest.out" 2>&1
      if grep -q 'VUID-VkPresentInfoKHR-pImageIndices-01430' "${TMP}/selftest.out"; then
        echo "self-test: ok -- the validation layer sees the layer's commands"
      else
        echo "FAIL: LSL_LAYER_BREAK_LAYOUT produced no validation error," >&2
        echo "      so a clean run below would prove nothing" >&2
        echo "      output: ${TMP}/selftest.out" >&2
        tail -20 "${TMP}/selftest.out" | sed 's/^/      /' >&2
        exit 1
      fi
      ;;
  esac
fi

echo "running: the resize storm, scale ${SCALE}$([[ ${use_vvl} -eq 1 ]] && echo ', validation layers')"
: > "${LOG}"
env VK_ADD_LAYER_PATH="${add_path}" \
    VK_INSTANCE_LAYERS="${layers}" \
    LSL_LAYER_LOG="${LOG}" \
    LSL_LAYER_SCALE="${SCALE}" \
    LSL_LAYER_DUMP="${DUMP}" \
    ${vvl_env} \
    timeout 120 "${BIN}" >"${OUT}" 2>&1
run_status=$?

if [[ "${run_status}" -eq 124 ]]; then
  echo "FAIL: the resize harness timed out" >&2
  echo "      log: ${LOG}" >&2
  tail -10 "${OUT}" | sed 's/^/      /' >&2
  exit 1
fi
if [[ "${run_status}" -ne 0 ]] || ! grep -q '^RESIZE_CHECK_OK$' "${OUT}"; then
  echo "FAIL: the resize harness exited ${run_status} without RESIZE_CHECK_OK" >&2
  echo "      log: ${LOG}" >&2
  tail -15 "${OUT}" | sed 's/^/      /' >&2
  grep -i 'error' "${LOG}" | head -5 | sed 's/^/      /' >&2
  exit 1
fi

# 1. Every swapchain the program created was one the layer upscaled.
creates="$(grep -c 'vkCreateSwapchainKHR:' "${LOG}" || true)"
upscaled="$(grep -c -- ' - upscaling$' "${LOG}" || true)"
if [[ "${creates}" -eq 0 ]]; then
  echo "FAIL: the layer created no swapchain (log: ${LOG})" >&2
  exit 1
fi
if [[ "${creates}" -ne "${upscaled}" ]]; then
  echo "FAIL: ${creates} swapchains created, but only ${upscaled} upscaling" >&2
  grep 'vkCreateSwapchainKHR:' "${LOG}" | sed 's/^/      /' >&2
  exit 1
fi

# 2. The extent-only rebuild ran, rather than a full rebuild each time.
kept="$(grep -c 'pipelines kept' "${LOG}" || true)"
if [[ "${kept}" -eq 0 ]]; then
  echo "FAIL: no resize kept the pipelines, so the extent-only path is untested" >&2
  grep -E 'vkCreateSwapchainKHR|resized to' "${LOG}" | head -6 | sed 's/^/      /' >&2
  exit 1
fi

echo "layer: ${creates} swapchains, all upscaling; ${kept} resize(s) kept the pipelines"

# 3. The dumped frame came from a recreated swapchain and is pixel-correct.
for f in "${DUMP}.src.ppm" "${DUMP}.out.ppm"; do
  if [[ ! -s "${f}" ]]; then
    echo "FAIL: the layer did not write ${f} (log: ${LOG})" >&2
    exit 1
  fi
done

real_of() { sed -n 's/.*vkCreateSwapchainKHR: \([0-9]*x[0-9]*\) (.*/\1/p' <<<"$1"; }
believed_of() { sed -n 's/.*application believes \([0-9]*x[0-9]*\),.*/\1/p' <<<"$1"; }
ppm_size() { sed -n '2p' "$1" | tr -s ' ' 'x'; }

first="$(grep -m1 'vkCreateSwapchainKHR:' "${LOG}")"
second="$(grep -m2 'vkCreateSwapchainKHR:' "${LOG}" | tail -1)"
src_size="$(ppm_size "${DUMP}.src.ppm")"
out_size="$(ppm_size "${DUMP}.out.ppm")"

if [[ "${src_size}" == "$(believed_of "${first}")" ]]; then
  echo "FAIL: the dumped frame is ${src_size}, the swapchain that was never" >&2
  echo "      recreated -- the point is to check a frame presented after a" >&2
  echo "      recreate.  log: ${LOG}" >&2
  exit 1
fi
if [[ "${src_size}" != "$(believed_of "${second}")" || "${out_size}" != "$(real_of "${second}")" ]]; then
  echo "FAIL: the dumped frame is ${src_size} -> ${out_size}," >&2
  echo "      the second swapchain is $(believed_of "${second}") -> $(real_of "${second}")" >&2
  echo "      log: ${LOG}" >&2
  exit 1
fi

fmt="$(sed -n 's/.*fmt \([0-9]*\)).*/\1/p' <<<"${second}")"
echo "upscaled after a resize: ${src_size} -> ${out_size}, format ${fmt}"

# BGRA swapchains reach the shader with R and B exchanged relative to the
# dump's human-readable byte order, so compare in the shader's channel order.
case "${fmt}" in
  44 | 50)
    echo "note: BGRA swapchain; comparing in the shader's channel order"
    "${PY}" "${PPM_TOOL}" swap "${DUMP}.src.ppm" "${TMP}/src.ppm"
    "${PY}" "${PPM_TOOL}" swap "${DUMP}.out.ppm" "${TMP}/out.ppm"
    ;;
  *)
    cp "${DUMP}.src.ppm" "${TMP}/src.ppm"
    cp "${DUMP}.out.ppm" "${TMP}/out.ppm"
    ;;
esac

if ! "${UPSCALE}" --in "${TMP}/src.ppm" --dst "${out_size}" \
                  --modes fsr1rcas --out "${TMP}/ref.ppm" --quiet >/dev/null; then
  echo "FAIL: lsl-upscale could not process the dumped source" >&2
  exit 1
fi

status=0
echo "comparing the layer's result after the resize against lsl-upscale:"
if ! "${PY}" "${PPM_TOOL}" cmp "${TMP}/ref.ppm" "${TMP}/out.ppm" --max-bytes 0; then
  status=1
fi

if grep -qiE 'validation|VUID-' "${LOG}"; then
  echo "FAIL: validation messages in the layer log:" >&2
  grep -iE 'validation|VUID-' "${LOG}" | head -10 >&2
  status=1
fi

# 4. The validation layer saw the layer and reported nothing.
if [[ "${use_vvl}" -eq 1 ]]; then
  if grep -qE 'ERROR.*(khronos_validation|libVkLayer_khronos)' "${OUT}"; then
    echo "FAIL: the validation layer failed to load:" >&2
    grep -E 'ERROR.*(khronos_validation|libVkLayer_khronos)' "${OUT}" | head -5 >&2
    status=1
  elif grep -qiE 'VALIDATION ERROR|Validation Error|VUID-' "${OUT}" "${LOG}"; then
    echo "FAIL: validation reported errors:" >&2
    grep -iE 'VALIDATION ERROR|Validation Error|VUID-' "${OUT}" "${LOG}" | head -10 >&2
    status=1
  else
    echo "validation: the layer's commands were validated, nothing reported"
  fi
fi

if [[ "${status}" -eq 0 ]]; then
  echo "PASS: the layer upscaled correctly across recreated swapchains"
else
  echo "FAIL: ${LOG}"
fi
exit "${status}"
