#!/usr/bin/env bash
#
# Prove that *synchronization* validation is live, then run a command under it.
#
#   tools/sync-check.sh ./build/lsl-upscale --make-test 1600x1000 --dst 2560x1600
#
# tools/validation-check.sh covers core validation (API misuse).  It cannot see
# a missing barrier at all -- that is synchronisation validation's job -- and
# syncval prints nothing when the code is clean.  Silence is worthless unless a
# known hazard *would* have been reported, so step 1 runs a deliberate hazard
# (tools/sync-selftest.c): two vkCmdFillBuffer into the same buffer with no
# barrier.  Step 2 then trusts the real command.
#
# Measured limitation, do not oversell the result
# -----------------------------------------------
# On this host (VVL 1.4.363, both GPUs) syncval reports transfer hazards but
# NOT compute/shader storage-buffer hazards.  A two-dispatch shader hazard --
# the same shape as the EASU -> RCAS scratch dependency -- is reported as
# clean, and `lsl-upscale --hazard` (barrier deliberately omitted) is likewise
# silent.  This is upstream, not an lsl bug:
#
#   https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/6061
#       "synchronization validation of compute writes currently is not
#        supported" (still open)
#   https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/6096
#       NonReadable / `writeonly` bindings are not honoured yet (still open)
#
# So a PASS from this script means "no API errors, and no *transfer* hazards",
# and nothing about the compute barriers in src/upscale.cpp.  Those stay
# covered by construction (every read is preceded by a matching barrier), by
# the CPU/GPU equivalence gates, and by eye.
#
# Exits 0 with a SKIP notice when the layers are not installed.
set -uo pipefail

here="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
layer_dir="${LSL_VVL_DIR:-$HOME/.local/share/vulkan-validation}"
manifest="$layer_dir/VkLayer_khronos_validation.json"

if [[ ! -f "$manifest" ]]; then
    echo "SKIP: validation layers not installed"
    echo "      install with: $here/setup-validation-layers.sh"
    exit 0
fi

export VK_ADD_LAYER_PATH="$layer_dir"
export VK_INSTANCE_LAYERS="VK_LAYER_KHRONOS_validation"
export VK_LOADER_DEBUG=error
# The non-deprecated spelling of VK_VALIDATION_FEATURE_ENABLE_
# SYNCHRONIZATION_VALIDATION_EXT.  Mixing the two makes the layer complain.
export VK_LAYER_VALIDATE_SYNC=1

out="$(mktemp)"
cleanup() { rm -f "$out"; }
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Step 1: the hazard self-test.  It must be reported, or step 2 proves nothing.
# ---------------------------------------------------------------------------
selftest_bin="$(mktemp -u)"
if cc -O0 -o "$selftest_bin" "$here/sync-selftest.c" -lvulkan 2>/dev/null; then
    "$selftest_bin" >"$out" 2>&1
    rm -f "$selftest_bin"

    if grep -q 'SELFTEST_RAN' "$out"; then
        if ! grep -q 'SYNC-HAZARD\|synchronization hazard' "$out"; then
            echo "FAIL: synchronization validation is on but silent about a"
            echo "      deliberate write-after-write. Every 'clean' result from"
            echo "      this machine would be meaningless."
            echo "--- self-test output ---"
            sed 's/^/      /' "$out" | head -20
            exit 1
        fi
    else
        echo "NOTE: the sync self-test could not create a Vulkan device;"
        echo "      continuing without positive confirmation."
    fi
else
    echo "NOTE: could not compile the sync self-test (no cc/vulkan headers)."
fi

# ---------------------------------------------------------------------------
# Step 2: the real command.
# ---------------------------------------------------------------------------
"$@" >"$out" 2>&1
status=$?

if grep -qE 'Validation Error|SYNC-HAZARD' "$out"; then
    echo "FAIL: synchronization validation reported errors:"
    grep -B1 -A6 -E 'Validation Error|SYNC-HAZARD' "$out" | head -60
    exit 1
fi

if grep -qE 'ERROR.*(khronos_validation|libVkLayer_khronos)' "$out"; then
    echo "FAIL: the validation layer failed to load:"
    grep -E 'ERROR.*(khronos_validation|libVkLayer_khronos)' "$out" | head -5
    exit 1
fi

if [[ $status -ne 0 ]]; then
    echo "FAIL: command exited with status $status"
    tail -20 "$out" | sed 's/^/      /'
    exit 1
fi

echo "OK: no validation errors ($*)"
echo "    note: shader/buffer hazards are not covered by this VVL build"
echo "          (issues 6061, 6096) -- see the header of this script."
exit 0
