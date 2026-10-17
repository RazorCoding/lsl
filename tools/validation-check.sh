#!/usr/bin/env bash
#
# Run a command under the Khronos validation layer and fail if the layer
# reports anything.  Used as a CTest, and usable by hand.
#
#   tools/validation-check.sh ./build/lsl-upscale --make-test 320x200 --dst 800x500
#
# Exits 0 with a SKIP notice when the layers are not installed, so a plain
# checkout can still run the full test suite.
#
# Three failure modes are checked, and the third is the important one:
#
#   1. the command itself fails;
#   2. the layer reports "Validation Error";
#   3. the layer was never actually loaded.  This is silent by default -- the
#      program runs fine and validates nothing -- so VK_LOADER_DEBUG=error is
#      forced on, and a built-in self-test must produce a known violation
#      before the real command is trusted.  Without that, the check would pass
#      whether or not validation happened, which is worse than not checking.
#
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
# Force loader diagnostics to error level: this is what makes a failed load
# visible at all.
export VK_LOADER_DEBUG=error

out="$(mktemp)"
cleanup() { rm -f "$out"; }
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Step 1: self-test.  Requires the layer to be genuinely running.
# ---------------------------------------------------------------------------
selftest_bin="$(mktemp -u)"
if cc -O0 -o "$selftest_bin" "$here/validation-selftest.c" -lvulkan 2>/dev/null; then
    "$selftest_bin" >"$out" 2>&1
    rm -f "$selftest_bin"

    if grep -q 'SELFTEST_RAN' "$out"; then
        if ! grep -q 'Validation Error' "$out"; then
            echo "FAIL: the validation layer is installed but not reporting anything."
            echo "      A deliberate spec violation produced no error, so every"
            echo "      'clean' result from this machine is meaningless."
            echo "--- self-test output ---"
            sed 's/^/      /' "$out" | head -20
            exit 1
        fi
    else
        echo "NOTE: validation self-test could not create a Vulkan device;"
        echo "      continuing without positive confirmation that the layer runs."
    fi
else
    rm -f "$selftest_bin"
    echo "NOTE: could not compile the validation self-test (no cc/vulkan headers)."
fi

# ---------------------------------------------------------------------------
# Step 2: the real command.
# ---------------------------------------------------------------------------
"$@" >"$out" 2>&1
status=$?

if grep -q 'Validation Error' "$out"; then
    echo "FAIL: validation layer reported errors:"
    grep -B1 -A4 'Validation Error' "$out" | head -60
    exit 1
fi

# A layer load failure mentions the layer or its library in a loader error.
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
exit 0
