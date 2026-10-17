#!/usr/bin/env bash
# Verify that the layer's present-time upscale really is the Phase 1 upscaler.
#
# The layer rewrites the swapchain and upscales the frame on the queue at
# present time, so there is nothing here to call and assert on.  Instead the
# layer dumps the two images it works with on the first upscaled frame
# (LSL_LAYER_DUMP), and this script feeds the dumped source through the Phase 1
# command-line upscaler (lsl-upscale, byte-for-byte the same shaders) and
# compares the two results.  That turns "the picture looks sharper" into a
# number.
#
#   8-bit swapchains must match byte for byte.  A BGRA swapchain reaches the
#   shader with R and B exchanged relative to the dump's human-readable byte
#   order, so the comparison is done in the shader's channel order (see below).
#   16F swapchains cannot be compared byte for byte: the dumped source is 8-bit,
#   so the reference works from quantised input and a PSNR gate is used instead.
#
# When the Khronos validation layer is installed it is loaded *underneath* the
# injected layer for the same run, so the commands the layer records are
# validated too, and the pass requires both a matching image and zero validation
# messages.  Layer order matters: the injected layer has to stay closest to the
# application, otherwise the validation layer never sees the layer's own
# commands.  That works because the layer stamps the dispatch word into the
# command buffers it allocates itself -- the same word the loader would have
# stamped (see lsl_stamp_command_buffers).  Set LSL_CHECK_VVL=0 to run without
# the validation layer.
#
# A clean validation run is only worth something if validation is really looking
# at the layer, so when the validation layer is used this script also runs one
# short capture pass with LSL_LAYER_BREAK_LAYOUT=1 -- the image is left in the
# wrong layout at present -- and requires that the validation layer reports it.
# If that pass is silent the clean pass proves nothing.
#
#   LSL_CHECK_VVL=0        skip the validation layer even when installed
#   LSL_CHECK_SELFTEST=0   skip the deliberate-error pass
#   LSL_VVL_DIR=...        where the layer manifest lives
#                          (default ~/.local/share/vulkan-validation)
#   LSL_CHECK_WSI=xcb|wayland
#   LSL_CHECK_SCALE=N      (default 2)
#
# Exits 77 (CTest SKIP) when a prerequisite such as vkcube is missing.
#
set -u

BUILD_DIR="${1:-build}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
case "${BUILD_DIR}" in
  /*) BUILD_ROOT="${BUILD_DIR}" ;;
  *) BUILD_ROOT="${ROOT}/${BUILD_DIR}" ;;
esac
LAYER_DIR="${BUILD_ROOT}/layer"
UPSCALE="${BUILD_ROOT}/lsl-upscale"
PPM_TOOL="${ROOT}/tools/ppm_tool.py"
WSI="${LSL_CHECK_WSI:-xcb}"
SCALE="${LSL_CHECK_SCALE:-2}"
FRAMES="${LSL_CHECK_FRAMES:-5}"
PY="${PYTHON:-python3}"
VVL_DIR="${LSL_VVL_DIR:-$HOME/.local/share/vulkan-validation}"
VVL_MANIFEST="${VVL_DIR}/VkLayer_khronos_validation.json"

for f in "${LAYER_DIR}/VkLayer_lsl_frame.json" "${UPSCALE}" "${ROOT}/tools/ppm_tool.py"; do
  if [[ ! -f "$f" ]]; then
    echo "SKIP: $f not found -- build first:" >&2
    echo "      cmake -S . -B ${1:-build} -G Ninja && cmake --build ${1:-build}" >&2
    exit 77
  fi
done

if ! command -v vkcube >/dev/null 2>&1; then
  echo "SKIP: vkcube not installed (vulkan-tools)"
  exit 77
fi

if [[ "${WSI}" == "xcb" || "${WSI}" == "xlib" ]]; then
  if [[ -z "${DISPLAY:-}" ]]; then
    echo "SKIP: --wsi ${WSI} needs DISPLAY (XWayland is fine)"
    exit 77
  fi
else
  if [[ -z "${WAYLAND_DISPLAY:-}" ]]; then
    echo "SKIP: --wsi ${WSI} needs WAYLAND_DISPLAY"
    exit 77
  fi
fi

# Which configuration to run.  Default: validate when the layers are installed.
check_vvl="${LSL_CHECK_VVL:-auto}"
use_vvl=0
if [[ -f "${VVL_MANIFEST}" ]]; then
  case "${check_vvl}" in
    0 | no | off) use_vvl=0 ;;
    *) use_vvl=1 ;;
  esac
else
  echo "note: validation layers not installed; pixel check only"
  echo "      install with: ${ROOT}/tools/setup-validation-layers.sh"
fi

TMP="$(mktemp -d -t lsl-upscale-check.XXXXXX)"
trap 'rm -rf "${TMP}"' EXIT
LOG="${TMP}/layer.log"
DUMP="${TMP}/dump"
OUT="${TMP}/output.log"

add_path="${LAYER_DIR}"
layers="VK_LAYER_LSL_frame"
vvl_env=""
if [[ "${use_vvl}" -eq 1 ]]; then
  add_path="${LAYER_DIR}:${VVL_DIR}"
  layers="VK_LAYER_LSL_frame:VK_LAYER_KHRONOS_validation"
  # Make a validation layer that fails to load loud instead of silent.
  vvl_env="VK_LOADER_DEBUG=error"
fi

# A clean validation run proves nothing on its own: without a deliberate error
# there is no way to tell "validation looked at the layer and found nothing"
# from "validation never saw the layer".  LSL_LAYER_BREAK_LAYOUT leaves the
# captured image in the wrong layout at present, which the validation layer has
# to report.  Same layer order and same WSI as the real run below.
if [[ "${use_vvl}" -eq 1 ]]; then
  case "${LSL_CHECK_SELFTEST:-auto}" in
    0 | no | off) ;;
    *)
      echo "self-test: one capture pass with a deliberate wrong layout, which validation must report"
      : > "${TMP}/selftest.log"
      env VK_ADD_LAYER_PATH="${add_path}" \
          VK_INSTANCE_LAYERS="${layers}" \
          LSL_LAYER_LOG="${TMP}/selftest.log" \
          LSL_LAYER_CAPTURE=1 \
          LSL_LAYER_CAPTURE_FRAMES=3 \
          LSL_LAYER_BREAK_LAYOUT=1 \
          ${vvl_env} \
          timeout 60 vkcube --wsi "${WSI}" --c 3 >"${TMP}/selftest.out" 2>&1
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

echo "running: vkcube --wsi ${WSI} --c ${FRAMES}, scale ${SCALE}$([[ ${use_vvl} -eq 1 ]] && echo ', validation layers')"
: > "${LOG}"
env VK_ADD_LAYER_PATH="${add_path}" \
    VK_INSTANCE_LAYERS="${layers}" \
    LSL_LAYER_LOG="${LOG}" \
    LSL_LAYER_SCALE="${SCALE}" \
    LSL_LAYER_DUMP="${DUMP}" \
    ${vvl_env} \
    timeout 60 vkcube --wsi "${WSI}" --c "${FRAMES}" >"${OUT}" 2>&1
run_status=$?
# vkcube stops exiting cleanly above a handful of frames on some compositors
# (it blocks waiting for a frame callback).  There is nothing to wait for here:
# the layer dumps its images on the first upscaled frame, so a timeout that
# came after that is still a complete run.
if [[ "${run_status}" -eq 124 && ! -s "${DUMP}.out.ppm" ]]; then
  echo "FAIL: vkcube timed out before the layer upscaled a frame" >&2
  echo "      log: ${LOG}" >&2
  grep -i 'error' "${LOG}" | head -5 >&2
  exit 1
elif [[ "${run_status}" -ne 0 && "${run_status}" -ne 124 ]]; then
  echo "FAIL: vkcube exited ${run_status} with the layer loaded" >&2
  echo "      log: ${LOG}" >&2
  tail -20 "${OUT}" | sed 's/^/      /' >&2
  grep -i 'error' "${LOG}" | head -5 >&2
  exit 1
fi

# The layer logs one line per important event; the check is driven off those
# rather than off guessed sizes, so it cannot pass by comparing the wrong files.
build_line="$(grep -m1 'via FSR1 EASU+RCAS' "${LOG}")"
create_line="$(grep -m1 -- '- upscaling$' "${LOG}")"

if [[ -z "${build_line}" || -z "${create_line}" ]]; then
  echo "FAIL: the layer did not build or create an upscaled swapchain" >&2
  echo "      upscale line: ${build_line:-<none>}" >&2
  echo "      create line : ${create_line:-<none>}" >&2
  echo "      log: ${LOG}" >&2
  exit 1
fi

app_size="$(sed -n 's/.*scale: \([0-9]*x[0-9]*\) -> .*/\1/p' <<<"${build_line}")"
real_size="$(sed -n 's/.*scale: [0-9]*x[0-9]* -> \([0-9]*x[0-9]*\) via.*/\1/p' <<<"${build_line}")"
fmt="$(sed -n 's/.*fmt \([0-9]*\)).*/\1/p' <<<"${create_line}")"

if grep -q 'half-float pixels' <<<"${build_line}"; then
  pixels=16f
  pixels_text="16-bit float"
else
  pixels=8
  pixels_text="8-bit"
fi

echo "upscale: ${app_size} -> ${real_size}, format ${fmt}, ${pixels_text} pixels"

for f in "${DUMP}.src.ppm" "${DUMP}.out.ppm"; do
  if [[ ! -s "${f}" ]]; then
    echo "FAIL: the layer did not write ${f}" >&2
    echo "      log: ${LOG}" >&2
    exit 1
  fi
done

# BGRA swapchains reach the shader with R and B exchanged relative to the dump's
# human-readable byte order.  Compare in the shader's order so the 8-bit match
# can be demanded exactly.
src="${DUMP}.src.ppm"
out="${DUMP}.out.ppm"
case "${fmt}" in
  44 | 50)  # B8G8R8A8_UNORM / _SRGB
    echo "note: BGRA swapchain; comparing in the shader's channel order"
    "${PY}" "${ROOT}/tools/ppm_tool.py" swap "${src}" "${TMP}/src.ppm"
    "${PY}" "${ROOT}/tools/ppm_tool.py" swap "${out}" "${TMP}/out.ppm"
    ;;
  *)
    cp "${src}" "${TMP}/src.ppm"
    cp "${out}" "${TMP}/out.ppm"
    ;;
esac

if ! "${UPSCALE}" --in "${TMP}/src.ppm" --dst "${real_size}" \
                  --modes fsr1rcas --out "${TMP}/ref.ppm" --quiet >/dev/null; then
  echo "FAIL: lsl-upscale could not process the dumped source" >&2
  exit 1
fi

echo "comparing the layer's present-time result against lsl-upscale:"
if [[ "${pixels}" == 16f ]]; then
  # The dumped source is 8-bit, so the reference is working from quantised
  # input.  EASU alone stays within a couple of dB of the original; RCAS then
  # amplifies the quantisation at edges, hence the thresholds.
  "${PY}" "${ROOT}/tools/ppm_tool.py" cmp "${TMP}/ref.ppm" "${TMP}/out.ppm" \
      --max-delta 8 --min-psnr 45
else
  "${PY}" "${ROOT}/tools/ppm_tool.py" cmp "${TMP}/ref.ppm" "${TMP}/out.ppm" \
      --max-bytes 0
fi
status=$?

if grep -qiE 'validation|VUID-' "${LOG}"; then
  echo "FAIL: validation messages in the layer log:" >&2
  grep -iE 'validation|VUID-' "${LOG}" | head -10 >&2
  status=1
fi

if [[ "${use_vvl}" -eq 1 ]]; then
  # The layer sat above the validation layer, so every command it recorded
  # went through validation.  Anything it found lands in vkcube's output.
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
  echo "PASS: present-time upscale matches the Phase 1 reference"
else
  echo "FAIL: ${LOG}"
fi
exit "${status}"
