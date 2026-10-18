/*
 * lsl_config.h -- what this process should upscale, and where that came from.
 *
 * Two ways in, in this order:
 *
 *   1. LSL_LAYER_SCALE in the environment.  This is the historical, explicit
 *      path: the launch-options string names the layer and the factor together,
 *      and a value <= 1 means "loaded, but do nothing".
 *   2. The GUI's profile store, if this process carries a SteamAppId that the
 *      store has enabled.  This is what makes "turn the game on in the window,
 *      press Play" work with no launch options at all.
 *
 * LSL_LAYER_DISABLE=1 wins over both, and is the layer's own off switch -- the
 * one to reach for when an implicit install misbehaves.
 *
 * Resolution happens once, on the first question, because the answer cannot
 * change within a process: a build log reads the values it will use for every
 * frame that follows.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int enabled;         /* upscaling is on for this process */
    float scale;         /* the factor; 0 when off */
    float sharpness;     /* RCAS stops, read only by that method */
    uint32_t mode;       /* LSL_MODE_*; LSL_MODE_AUTO defers to the engine */
    const char *source;  /* "disabled"/"environment"/"profile"/"off"; for the log */
    const char *appid;   /* set when the answer came from the profile store */
} LslConfig;

/* The resolved settings.  Never NULL; a process with nothing to do gets
 * enabled=0. */
const LslConfig *lsl_config(void);

/* Whether the layer should write its log at all.  This is the difference
 * between an explicit install and an implicit one: an implicit layer is loaded
 * into every Vulkan application on the machine, so a process with nothing to do
 * -- no LSL_* knobs, no enabled profile -- has to stay silent, while a process
 * that is upscaling, or that asked for a log with LSL_LAYER_LOG, still gets one.
 * The environment is consulted on every call, so a test can turn logging on
 * for a run regardless of what the config decided. */
int lsl_log_enabled(void);

#ifdef __cplusplus
}
#endif
