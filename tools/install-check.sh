#!/usr/bin/env bash
#
# Verify that what `cmake --install` writes is a layer that actually loads.
#
# Every other layer test loads the build tree's manifest (build/layer), so a
# manifest whose library_path is relative, stale, or missing from the installed
# tree passes all of them and then fails on the machine it was shipped to -- the
# exact failure that stops the system lsfg-vk layer from loading on this host.
# This installs into a throwaway prefix and checks the installed copy instead.
#
# Two things are checked:
#
#   1. the installed manifest names an absolute library_path that exists, and
#      the object next to it is the one the manifest means;
#   2. the installed copy really runs: tools/upscale-check.sh is pointed at it
#      with LSL_LAYER_DIR (its byte-for-byte comparison machinery, unchanged)
#      for a few representative methods, and a negative control repeats the run
#      with the library_path broken and requires that the layer does *not* load
#      -- otherwise "it loaded" would only mean the loader was not looking.
#
# The manifest half needs no display and is what packaging is for; the load half
# needs vkcube and a display, and is reported as a note without one.  Exits 77
# only if the build tree itself is missing.
#
#   LSL_CHECK_MODES=...   methods for the load half (default fsr1rcas,lanczos2,nis)
set -u

BUILD_DIR="${1:-build}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
case "${BUILD_DIR}" in
  /*) BUILD_ROOT="${BUILD_DIR}" ;;
  *) BUILD_ROOT="${ROOT}/${BUILD_DIR}" ;;
esac

if [[ ! -f "${BUILD_ROOT}/CMakeCache.txt" ]]; then
  echo "SKIP: ${BUILD_ROOT} is not a configured build tree" >&2
  exit 77
fi
if [[ ! -x "${BUILD_ROOT}/lsl-upscale" ]]; then
  echo "SKIP: ${BUILD_ROOT}/lsl-upscale not built" >&2
  exit 77
fi

TMP="$(mktemp -d -t lsl-install-check.XXXXXX)"
trap 'rm -rf "${TMP}"' EXIT
PREFIX="${TMP}/prefix"

echo "installing into a throwaway prefix: ${PREFIX}"
if ! cmake --install "${BUILD_ROOT}" --prefix "${PREFIX}" >"${TMP}/install.log" 2>&1; then
  echo "FAIL: cmake --install failed:" >&2
  cat "${TMP}/install.log" >&2
  exit 1
fi

MANIFEST="$(find "${PREFIX}" -name VkLayer_lsl_frame.json -print -quit 2>/dev/null || true)"
if [[ -z "${MANIFEST}" ]]; then
  echo "FAIL: the install wrote no VkLayer_lsl_frame.json under ${PREFIX}" >&2
  cat "${TMP}/install.log" >&2
  exit 1
fi
LAYER_DIR="$(cd "$(dirname "${MANIFEST}")" && pwd)"

LIB="$(sed -n 's/.*"library_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "${MANIFEST}")"
if [[ -z "${LIB}" ]]; then
  echo "FAIL: ${MANIFEST} has no library_path" >&2
  exit 1
fi
if [[ "${LIB}" != /* ]]; then
  # The exact failure the README records for the system lsfg-vk layer.
  echo "FAIL: installed library_path is not absolute: '${LIB}'" >&2
  exit 1
fi
if [[ ! -f "${LIB}" ]]; then
  echo "FAIL: installed library_path does not exist: ${LIB}" >&2
  exit 1
fi
# It must be the copy that was just installed, not the build tree's.
case "$(cd "$(dirname "${LIB}")" && pwd)" in
  "${PREFIX}"/*) ;;
  *) echo "FAIL: library_path points outside the install prefix: ${LIB}" >&2; exit 1 ;;
esac
echo "manifest ok: ${MANIFEST}"
echo "  library_path: ${LIB}"

# The other way to install it: --implicit drops a byte-identical copy where the
# loader scans without being asked, which is what lets the GUI turn a game on
# with no launch options at all.  The copy has to be there, and it has to be the
# same manifest -- if the two ever drift, the mode that is supposed to need no
# configuration is the one nobody tested.
echo
echo "installing with --implicit"
if ! "${ROOT}/tools/install-layer.sh" --prefix "${PREFIX}" --build "${BUILD_ROOT}" \
       --no-build --implicit >"${TMP}/install-implicit.log" 2>&1; then
  echo "FAIL: install-layer.sh --implicit failed:" >&2
  cat "${TMP}/install-implicit.log" >&2
  exit 1
fi
IMPLICIT="${PREFIX}/share/vulkan/implicit_layer.d/VkLayer_lsl_frame.json"
if [[ ! -f "${IMPLICIT}" ]]; then
  echo "FAIL: --implicit wrote no manifest at ${IMPLICIT}" >&2
  cat "${TMP}/install-implicit.log" >&2
  exit 1
fi
if ! cmp -s "${IMPLICIT}" "${MANIFEST}"; then
  echo "FAIL: the implicit manifest is not identical to the installed one" >&2
  exit 1
fi
echo "implicit manifest ok: ${IMPLICIT}"

# The load half: the same byte-for-byte comparison the rest of the suite runs,
# but pointed at the installed copy instead of the build tree's.
MODES="${LSL_CHECK_MODES:-fsr1rcas,lanczos2,nis}"
echo
echo "running the upscale checks against the installed copy (modes: ${MODES})"
LSL_LAYER_DIR="${LAYER_DIR}" LSL_CHECK_MODES="${MODES}" \
  "${ROOT}/tools/upscale-check.sh" "${BUILD_ROOT}"
status=$?

if [[ "${status}" -eq 77 ]]; then
  echo "note: no display or vkcube; the installed manifest was validated but not loaded"
  exit 0
fi
if [[ "${status}" -ne 0 ]]; then
  exit "${status}"
fi

# Negative control.  upscale-check.sh just proved the installed manifest loads;
# this proves the *path in it* is what did the loading.  Copy the manifest with
# a library_path that cannot exist and require that the layer stays out.
#
# A system-wide implicit install of lsl (the normal end-user state) would also
# load the real layer and mask this check, so temporarily move any such manifest
# aside for the duration of the control and restore it afterwards.
BROKEN_DIR="${TMP}/broken"
mkdir -p "${BROKEN_DIR}"
sed 's#"library_path"[[:space:]]*:.*#"library_path": "/nonexistent/liblsl-layer.so",#' \
    "${MANIFEST}" >"${BROKEN_DIR}/VkLayer_lsl_frame.json"

# Stash any implicit manifests we can (writable) so the negative control is
# meaningful; a system-wide install we cannot move (no root) makes the control
# inconclusive, in which case we skip its vkcube check rather than fail.
STASHED=()
SKIPPED=0
for cand in \
    /usr/share/vulkan/implicit_layer.d/VkLayer_lsl_frame.json \
    /usr/local/share/vulkan/implicit_layer.d/VkLayer_lsl_frame.json \
    /etc/vulkan/implicit_layer.d/VkLayer_lsl_frame.json \
    "${HOME}/.local/share/vulkan/implicit_layer.d/VkLayer_lsl_frame.json" \
    "${XDG_DATA_HOME:-}/vulkan/implicit_layer.d/VkLayer_lsl_frame.json"; do
  [[ -z "${cand}" || "${cand}" == "/vulkan/implicit_layer.d/VkLayer_lsl_frame.json" ]] && continue
  if [[ -f "${cand}" ]]; then
    if mv "${cand}" "${cand}.lsl-stash" 2>/dev/null; then
      STASHED+=("${cand}")
    else
      SKIPPED=1
    fi
  fi
done
restore_stash() {
  for cand in "${STASHED[@]}"; do
    [[ -f "${cand}.lsl-stash" ]] && mv "${cand}.lsl-stash" "${cand}"
  done
}
trap restore_stash EXIT

WSI="${LSL_CHECK_WSI:-xcb}"
BROKEN_LOG="${TMP}/broken.log"
BROKEN_OUT="${TMP}/broken.out"
: >"${BROKEN_LOG}"
timeout 60 env \
    VK_ADD_LAYER_PATH="${BROKEN_DIR}" \
    VK_INSTANCE_LAYERS=VK_LAYER_LSL_frame \
    LSL_LAYER_LOG="${BROKEN_LOG}" \
    LSL_LAYER_SCALE=2 \
    vkcube --wsi "${WSI}" --c 2 >"${BROKEN_OUT}" 2>&1 || true

if [[ "${SKIPPED}" -eq 1 ]]; then
  echo "negative control: skipped (a system-wide implicit manifest could not be"
  echo "  moved aside without root); the positive load check above is the gate"
else
  if grep -q "scale:.*->.*via" "${BROKEN_LOG}" 2>/dev/null; then
    echo "FAIL: the layer loaded from a manifest whose library_path does not exist," >&2
    echo "      so a passing load check above proves nothing" >&2
    exit 1
  fi
  echo "negative control: a manifest with a bad library_path stays unloaded"
fi

# And the property the explicit path cannot check: the installed copy also works
# when the loader finds it on its own and the only thing that says "upscale this"
# is SteamAppId.  appid-check.sh installs the manifest into a throwaway
# XDG_DATA_HOME and runs the same enabled/disabled/no-appid controls.
# Restore any stashed implicit manifests before the self-gating checks, which
# need the real implicit layer to be findable.
restore_stash
trap - EXIT

echo
echo "running the self-gating checks against the installed copy"
LSL_LAYER_DIR="${LAYER_DIR}" "${ROOT}/tools/appid-check.sh" "${BUILD_ROOT}"
status=$?
if [[ "${status}" -ne 0 ]]; then
  exit "${status}"
fi

echo "PASS: the installed layer loads, upscales, and self-gates by appid"
