#!/usr/bin/env bash
# Prove the layer can be installed so that the GUI alone turns a game on.
#
# The other layer tests hand the layer to a process through its launch options:
# VK_ADD_LAYER_PATH points the loader at the manifest, VK_INSTANCE_LAYERS names
# the layer, and LSL_LAYER_SCALE says what to do.  This one removes all of that.
# The manifest is put where the loader looks on its own, and the only thing the
# process is told is what Steam tells every game it launches: SteamAppId.  The
# layer has to read the GUI's profile store, find that appid, and upscale -- and,
# just as important, do nothing at all for anything else.
#
# That second half is the reason this test exists.  An implicit layer is loaded
# into every Vulkan application on the machine, so "it upscaled the game" is
# only half the property; "it stayed out of the way of everything else" is the
# other half, and a bug in either direction is a broken program, not a wrong
# picture.
#
# What is checked:
#
#   1. an enabled appid upscales from the profile store, with no launch options,
#      and the frame matches `lsl-upscale --modes fsr1rcas` byte for byte;
#   2. a disabled record, an appid the store never heard of, and no appid at all
#      each load the layer and upscale nothing, with the log proving the layer
#      really was loaded -- so silence means the layer chose to do nothing, not
#      that the loader never found it;
#   3. LSL_LAYER_DISABLE=1 keeps the layer out of the process entirely, and
#      LSL_LAYER_SCALE still wins over the store, even for a disabled game.
#
#   LSL_CHECK_WSI=xcb|wayland
#   LSL_CHECK_FRAMES=N        (default 5)
#   LSL_LAYER_DIR=...         the layer to load, instead of <build>/layer
#
# Exits 77 (CTest SKIP) when a prerequisite such as vkcube is missing.
set -u

BUILD_DIR="${1:-build}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
case "${BUILD_DIR}" in
  /*) BUILD_ROOT="${BUILD_DIR}" ;;
  *) BUILD_ROOT="${ROOT}/${BUILD_DIR}" ;;
esac
LAYER_DIR="${LSL_LAYER_DIR:-${BUILD_ROOT}/layer}"
UPSCALE="${BUILD_ROOT}/lsl-upscale"
PPM_TOOL="${ROOT}/tools/ppm_tool.py"
WSI="${LSL_CHECK_WSI:-xcb}"
FRAMES="${LSL_CHECK_FRAMES:-5}"
PY="${PYTHON:-python3}"

for f in "${LAYER_DIR}/VkLayer_lsl_frame.json" "${UPSCALE}" "${PPM_TOOL}"; do
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
  [[ -n "${DISPLAY:-}" ]] || { echo "SKIP: --wsi ${WSI} needs DISPLAY"; exit 77; }
else
  [[ -n "${WAYLAND_DISPLAY:-}" ]] || { echo "SKIP: --wsi ${WSI} needs WAYLAND_DISPLAY"; exit 77; }
fi

TMP="$(mktemp -d -t lsl-appid-check.XXXXXX)"
trap 'rm -rf "${TMP}"' EXIT

# Put the layer where the loader looks without being asked.  $XDG_DATA_HOME is
# redirected rather than $HOME: it moves the loader's per-user manifest
# directory to a throwaway one -- so the real installation cannot make the
# negative controls pass by accident -- while leaving $HOME alone for every
# other layer, several of which find their own data through it.
DATA_HOME="${TMP}/data"
IMPLICIT_DIR="${DATA_HOME}/vulkan/implicit_layer.d"
mkdir -p "${IMPLICIT_DIR}"
cp "${LAYER_DIR}/VkLayer_lsl_frame.json" "${IMPLICIT_DIR}/VkLayer_lsl_frame.json"

# The GUI's store, in miniature: one game on, one game off, and one appid that
# appears nowhere at all.
STORE="${TMP}/config/lsl/profiles.conf"
mkdir -p "$(dirname "${STORE}")"
cat >"${STORE}" <<'EOF'
# Written by tools/appid-check.sh.
game 933110 1 2 0 fsr1rcas
game 730 0 3 0 nis
EOF

WSI_FLAGS=(--wsi "${WSI}" --c "${FRAMES}")

# Everything the implicit path is allowed to see.  Deliberately absent:
# VK_ADD_LAYER_PATH, VK_INSTANCE_LAYERS, LSL_LAYER_SCALE.
run() { # run <log> <dump> <out> [VAR=VALUE ...]
  local log="$1" dump="$2" out="$3"
  shift 3
  : >"${log}"
  env XDG_DATA_HOME="${DATA_HOME}" \
      LSL_PROFILES="${STORE}" \
      LSL_LAYER_LOG="${log}" \
      LSL_LAYER_DUMP="${dump}" \
      "$@" \
      timeout 60 vkcube "${WSI_FLAGS[@]}" >"${out}" 2>&1
  return $?
}

build_line() { grep -m1 -E 'scale: [0-9]+x[0-9]+ -> ' "$1"; }

status=0

# 1. The enabled appid.  This is the whole feature: nothing but SteamAppId.
LOG="${TMP}/enabled.log"; DUMP="${TMP}/enabled"; OUT="${TMP}/enabled.out"
echo "running: SteamAppId=933110 (enabled in the store), no launch options"
run "${LOG}" "${DUMP}" "${OUT}" SteamAppId=933110
line="$(build_line "${LOG}")"
if [[ -z "${line}" ]]; then
  echo "FAIL: the layer did not upscale an appid the store has enabled" >&2
  echo "      log: ${LOG}" >&2
  sed -n '1,20p' "${LOG}" | sed 's/^/      /' >&2
  tail -20 "${OUT}" | sed 's/^/      /' >&2
  status=1
else
  echo "  ${line}"
  resolved="$(sed -n 's/.*, mode \([a-z0-9]*\).*/\1/p' <<<"${line}")"
  if [[ "${resolved}" != "fsr1rcas" ]]; then
    echo "FAIL: the profile named fsr1rcas but the layer built ${resolved}" >&2
    status=1
  fi
  if [[ ! -s "${DUMP}.out.ppm" ]]; then
    echo "FAIL: the layer built an upscaler but dumped no frame" >&2
    status=1
  else
    # Same byte-for-byte comparison the explicit path is held to: this is the
    # Phase 1 upscaler, reached through the profile store rather than a launch
    # line.
    create_line="$(grep -m1 -- '- upscaling$' "${LOG}")"
    fmt="$(sed -n 's/.*fmt \([0-9]*\)).*/\1/p' <<<"${create_line}")"
    real_size="$(sed -n 's/.*scale: [0-9]*x[0-9]* -> \([0-9]*x[0-9]*\) via.*/\1/p' <<<"${line}")"
    SRC_PPM="${TMP}/src.ppm"; OUT_PPM="${TMP}/out.ppm"; REF_PPM="${TMP}/ref.ppm"
    case "${fmt}" in
      44 | 50) # B8G8R8A8_UNORM / _SRGB: the shader sees R and B exchanged
        "${PY}" "${PPM_TOOL}" swap "${DUMP}.src.ppm" "${SRC_PPM}"
        "${PY}" "${PPM_TOOL}" swap "${DUMP}.out.ppm" "${OUT_PPM}"
        ;;
      *)
        cp "${DUMP}.src.ppm" "${SRC_PPM}"
        cp "${DUMP}.out.ppm" "${OUT_PPM}"
        ;;
    esac
    if "${UPSCALE}" --in "${SRC_PPM}" --dst "${real_size}" --modes fsr1rcas \
                   --out "${REF_PPM}" --quiet >/dev/null 2>&1; then
      if [[ "${line}" == *"half-float pixels"* ]]; then
        # The dumped source is 8-bit, so a 16F swapchain cannot be held to it
        # byte for byte; the PSNR gate is the same one the explicit path uses.
        "${PY}" "${PPM_TOOL}" cmp "${REF_PPM}" "${OUT_PPM}" --max-delta 8 --min-psnr 45 ||
          status=1
      else
        "${PY}" "${PPM_TOOL}" cmp "${REF_PPM}" "${OUT_PPM}" --max-bytes 0 || status=1
      fi
    else
      echo "FAIL: lsl-upscale could not process the dumped source" >&2
      status=1
    fi
  fi
fi

# 2. What must stay untouched.  Each of these upscales nothing.  Except for the
#    LSL_LAYER_DISABLE case -- where the manifest's disable_environment keeps the
#    layer from loading at all -- the log must show the layer running, or a
#    negative proves nothing.
expect_off() { # expect_off <label> <must-load 0|1> <log> <out> [VAR=VALUE ...]
  local label="$1" must_load="$2" log="$3" out="$4"
  shift 4
  local dump="${log%.log}.dump"
  run "${log}" "${dump}" "${out}" "$@"
  local line
  line="$(build_line "${log}")"
  if [[ -n "${line}" ]]; then
    echo "FAIL (${label}): the layer upscaled when it should not have:" >&2
    echo "      ${line}" >&2
    status=1
    return
  fi
  if [[ "${must_load}" == "1" ]] && ! grep -q 'vkCreateInstance -> 0' "${log}"; then
    echo "FAIL (${label}): the layer never loaded, so staying quiet proves" >&2
    echo "      nothing -- the implicit install is not being found" >&2
    status=1
    return
  fi
  if compgen -G "${dump}.*.ppm" >/dev/null; then
    echo "FAIL (${label}): the layer dumped a frame it should not have" >&2
    status=1
    return
  fi
  echo "  ok: ${label}"
}

echo
echo "running the negative controls"
expect_off "a record that is disabled" 1 "${TMP}/off-disabled.log" "${TMP}/off-disabled.out" \
           SteamAppId=730
expect_off "an appid the store never heard of" 1 "${TMP}/off-absent.log" "${TMP}/off-absent.out" \
           SteamAppId=404
expect_off "no SteamAppId at all" 1 "${TMP}/off-nogame.log" "${TMP}/off-nogame.out"
# LSL_LAYER_DISABLE is in the manifest's disable_environment, so here the loader
# is expected to keep the layer out of the process entirely.
expect_off "LSL_LAYER_DISABLE=1" 0 "${TMP}/off-disabled-env.log" \
           "${TMP}/off-disabled-env.out" LSL_LAYER_DISABLE=1 SteamAppId=933110

# 3. The explicit path still wins: a disabled profile must not be able to veto an
#    LSL_LAYER_SCALE somebody put in a launch options string.
echo
echo "running: LSL_LAYER_SCALE=3 on the appid the store has disabled"
LOG="${TMP}/env.log"; DUMP="${TMP}/env"; OUT="${TMP}/env.out"
run "${LOG}" "${DUMP}" "${OUT}" SteamAppId=730 LSL_LAYER_SCALE=3
line="$(build_line "${LOG}")"
if [[ -z "${line}" ]]; then
  echo "FAIL: LSL_LAYER_SCALE no longer overrides the profile store" >&2
  status=1
else
  echo "  ${line}"
  app="$(sed -n 's/.*scale: \([0-9]*\)x[0-9]* -> .*/\1/p' <<<"${line}")"
  real="$(sed -n 's/.*scale: [0-9]*x[0-9]* -> \([0-9]*\)x.*/\1/p' <<<"${line}")"
  # The layer divides the surface by the scale and floors, so the ratio is only
  # exact within one pixel of the factor.
  if [[ -z "${app}" || -z "${real}" ]] ||
     (( real > app * 3 + 3 || real < app * 3 - 3 )); then
    echo "FAIL: LSL_LAYER_SCALE=3 did not render near a third of the window" >&2
    status=1
  fi
fi

if [[ "${status}" -eq 0 ]]; then
  echo
  echo "PASS: the layer upsizes the games the GUI enabled and only those"
fi
exit "${status}"
