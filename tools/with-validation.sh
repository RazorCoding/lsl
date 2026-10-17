#!/usr/bin/env bash
#
# Run a command with the Khronos validation layer forced on.
#
# Forcing is necessary because the code does not request the layer itself
# (deliberately -- shipping builds should not depend on it).  VK_INSTANCE_LAYERS
# makes the loader inject it into every instance regardless.
#
# The layer prints only when something is wrong, so "no output" is the success
# case.  That also means silence is not evidence the layer ran; use
# tools/validation-check.sh when the distinction matters.
#
#   tools/with-validation.sh ./build/lsl-probe
#   LSL_VALIDATE_SYNC=1 tools/with-validation.sh ./build/lsl-upscale ...
#
# Caution: LSL_VALIDATE_SYNC does make syncval active (its hazard self-test
# reports transfer hazards), but this VVL build does not report compute /
# shader storage-buffer hazards, so it does *not* verify the compute barriers.
# See the header of tools/sync-check.sh before reading anything into a pass.
#
set -euo pipefail

layer_dir="${LSL_VVL_DIR:-$HOME/.local/share/vulkan-validation}"
if [[ ! -f "$layer_dir/VkLayer_khronos_validation.json" ]]; then
    echo "with-validation.sh: no validation layer at $layer_dir" >&2
    echo "  install it with tools/setup-validation-layers.sh" >&2
    exit 2
fi

export VK_ADD_LAYER_PATH="$layer_dir"
export VK_INSTANCE_LAYERS="VK_LAYER_KHRONOS_validation"

# Synchronization validation is the single most useful check for a rendering
# project (it catches missing barriers), but it is slow and only reports at
# submit time, so it is opt-in.
#
# VK_LAYER_VALIDATE_SYNC is the current spelling.  The older
# VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
# still works but makes the layer warn about mixing deprecated and new
# settings, so it is not used here.
if [[ -n "${LSL_VALIDATE_SYNC:-}" ]]; then
    export VK_LAYER_VALIDATE_SYNC=1
fi

exec "$@"
