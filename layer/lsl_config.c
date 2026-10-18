#include "lsl_config.h"

#include "lsl_layer.h" /* lsl_log, lsl_env_flag */
#include "lsl_method.h"
#include "lsl_profile.h"

#include <stdlib.h>

static LslConfig g_cfg;
static int g_done;

static void resolve(LslConfig *c) {
    c->enabled = 0;
    c->scale = 0.0f;
    c->sharpness = 0.0f;
    c->mode = LSL_MODE_FSR1RCAS;
    c->source = "off";
    c->appid = NULL;

    /* The layer's own kill switch: wins over everything, including a profile
     * that says the game is on.  This is the knob to reach for when an implicit
     * install does something unexpected. */
    if (lsl_env_flag("LSL_LAYER_DISABLE")) {
        c->source = "disabled";
        return;
    }

    /* 1. The environment, when it names a scale.  This is the launch-options
     * path, and it keeps meaning exactly what it always did: a value <= 1 is
     * "loaded but inert", so a launch line can stay in place while scaling is
     * off. */
    const char *scale_env = getenv("LSL_LAYER_SCALE");
    if (scale_env && *scale_env) {
        c->source = "environment";
        const float s = strtof(scale_env, NULL);
        if (!(s > 1.0f)) return;

        const char *sharp_env = getenv("LSL_LAYER_SHARPNESS");
        const char *mode_env = getenv("LSL_LAYER_MODE");
        c->enabled = 1;
        c->scale = s;
        c->sharpness = (sharp_env && *sharp_env) ? strtof(sharp_env, NULL) : 0.0f;
        if (mode_env && *mode_env && lsl_mode_parse(mode_env, &c->mode) != 0) {
            /* A typo costs a log line, not the feature. */
            c->mode = LSL_MODE_FSR1RCAS;
            lsl_log("scale: unknown LSL_LAYER_MODE=\"%s\"; using the default",
                    mode_env);
        }
        return;
    }

    /* 2. The GUI's profile, for the game Steam says this process is.  No appid
     * means no game -- the loader is free to put the layer into a browser or a
     * benchmark -- and that has to stay a no-op. */
    const char *appid = lsl_profile_appid();
    if (appid) {
        float s = 0.0f, sh = 0.0f;
        uint32_t m = LSL_MODE_AUTO;
        if (lsl_profile_lookup(lsl_profile_path(), appid, &s, &sh, &m)) {
            c->enabled = 1;
            c->scale = s;
            c->sharpness = sh;
            c->mode = m;
            c->appid = appid;
            c->source = "profile";
            lsl_log("scale: appid %s is enabled in %s", appid,
                    lsl_profile_path());
        }
    }
}

const LslConfig *lsl_config(void) {
    if (!g_done) {
        /* Set before resolving: lsl_log() during resolve calls lsl_config()
         * back through the logging gate, and it must not resolve twice. */
        g_done = 1;
        resolve(&g_cfg);
    }
    return &g_cfg;
}

int lsl_log_enabled(void) {
    /* Anything explicitly asked for gets its log, whatever the config says. */
    if (getenv("LSL_LAYER_LOG")) return 1;
    if (getenv("LSL_LAYER_SCALE")) return 1;
    if (getenv("LSL_LAYER_DUMP")) return 1;
    if (getenv("LSL_LAYER_CAPTURE_FRAMES")) return 1;
    if (lsl_env_flag("LSL_LAYER_CAPTURE")) return 1;
    if (lsl_env_flag("LSL_LAYER_BREAK_LAYOUT")) return 1;
    /* Otherwise: only a process that is actually upscaling says anything.  An
     * implicit layer in every other process stays silent. */
    return lsl_config()->enabled;
}
