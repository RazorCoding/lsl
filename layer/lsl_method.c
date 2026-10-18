/*
 * lsl_method.c -- the names LSL_LAYER_MODE accepts.
 *
 * The layer is C and cannot include src/method.hpp (which is C++ and belongs to
 * the benchmark, the Steam writer and the GUI).  The vocabulary is therefore
 * repeated here, and the two copies are kept honest by tools/upscale-check.sh:
 * it runs every name in src/method.hpp's table through the layer and demands
 * that the frame come out identical to `lsl-upscale --modes <name>`.  A name
 * the layer cannot parse, or parses to the wrong filter, fails that gate.
 */

#include "lsl_method.h"

typedef struct {
    const char *name;       /* what LSL_LAYER_MODE= takes */
    const char *build_text; /* the human half of `via ...` in the log line */
} LslMethod;

static const LslMethod k_methods[LSL_MODE_COUNT] = {
    {"nearest",  "Nearest"},
    {"bilinear", "Bilinear"},
    {"cubic",    "Bicubic"},
    {"lanczos2", "Lanczos-2"},
    {"fsr1",     "FSR1 EASU"},
    {"fsr1rcas", "FSR1 EASU+RCAS"},
    {"lanczos3", "Lanczos-3"},
    {"nis",      "FSR1 EASU + NIS sharpen"},
    {"cnn",      "FSR1 EASU + Anime4K CNN"},
};

/* String equality, spelled out because the layer avoids a libc dependency it
 * does not actually need here (strcmp is fine, but this keeps the file to the
 * two includes the rest of the layer uses). */
static int name_is(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == '\0' && *b == '\0';
}

int lsl_mode_parse(const char *name, uint32_t *out) {
    if (!name || !*name) return 1;
    if (name_is(name, "auto")) {
        *out = LSL_MODE_AUTO;
        return 0;
    }
    for (uint32_t i = 0; i < LSL_MODE_COUNT; ++i) {
        if (name_is(name, k_methods[i].name)) {
            *out = i;
            return 0;
        }
    }
    return 1;
}

const char *lsl_mode_name(uint32_t mode) {
    if (mode == LSL_MODE_AUTO) return "auto";
    return mode < LSL_MODE_COUNT ? k_methods[mode].name : "unknown";
}

const char *lsl_mode_build_text(uint32_t mode) {
    if (mode == LSL_MODE_AUTO) return "auto";
    return mode < LSL_MODE_COUNT ? k_methods[mode].build_text : "unknown";
}
