/*
 * lsl_method.h -- the names LSL_LAYER_MODE accepts.
 *
 * The layer is C and cannot include src/method.hpp (which is C++ and belongs to
 * the benchmark, the Steam writer and the GUI).  The vocabulary is therefore
 * repeated here, and the two copies are kept honest by tools/upscale-check.sh:
 * it runs every name in src/method.hpp's table through the layer and demands
 * that the frame come out identical to `lsl-upscale --modes <name>`.  A name
 * the layer did not know, or mapped to the wrong filter, fails that gate.
 *
 * The numeric values matter beyond this file: the layer sends the id to
 * shaders/scale.comp in its push constant, and a swapchain already upscaled
 * with mode 4 has to keep meaning EASU.
 */
#pragma once

#include <stdint.h>

enum {
    LSL_MODE_NEAREST  = 0,
    LSL_MODE_BILINEAR = 1,
    LSL_MODE_CUBIC    = 2,
    LSL_MODE_LANCZOS2 = 3,
    LSL_MODE_FSR1     = 4,  /* EASU alone */
    LSL_MODE_FSR1RCAS = 5,  /* EASU, then FSR1's RCAS sharpening */
    LSL_MODE_LANCZOS3 = 6,
    LSL_MODE_NIS      = 7,  /* EASU, then NVIDIA Image Scaling's sharpening */
    LSL_MODE_CNN      = 8,  /* EASU, then Anime4K's Restore CNN */
    LSL_MODE_COUNT    = 9,

    /* Not a filter.  The layer picks one of the above once per swapchain from
     * the upscale ratio; see lsl_auto_pick() in lsl_upscale.c. */
    LSL_MODE_AUTO = 0xffffffffu
};

/* 0 on success, non-zero when `name` is not one the layer knows.  `*out` is
 * LSL_MODE_AUTO for "auto".  `name` may be NULL or empty. */
int lsl_mode_parse(const char *name, uint32_t *out);

/* For the log line; "unknown" outside the enum. */
const char *lsl_mode_name(uint32_t mode);

/* The human half of `via ...` in the build log line, e.g. "FSR1 EASU+RCAS".
 * Kept separate from `name` because a log meant for a person should not say
 * "fsr1rcas"; tools/upscale-check.sh keys off the rule that the line ends in
 * ", mode <name>" instead, so this text is free to change. */
const char *lsl_mode_build_text(uint32_t mode);
