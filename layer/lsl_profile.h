/*
 * lsl_profile.h -- read the GUI's per-game profile store from inside the layer.
 *
 * The store is the plain file the GUI writes (src/profile.cpp): one
 * `game <appid> <enabled> <scale> <sharpness> <method>` record per line.  The
 * layer reads exactly one thing out of it -- whether *this* process is a game
 * the user enabled -- and it knows which app it is because Steam puts
 * SteamAppId/SteamGameId in the environment of every game it launches.  That is
 * what lets a game be upscaled from the GUI alone: no launch options, nothing
 * written into Steam's configuration, the game files untouched.
 *
 * The format is duplicated rather than shared for the same reason the method
 * vocabulary is (see lsl_method.h): the layer is C and is loaded into arbitrary
 * processes, so it cannot link the C++ store.  tests/test_layer_store.cpp keeps
 * the two honest -- it writes a store with the GUI's own writer and reads it
 * back here -- and tools/appid-check.sh exercises the same reader end to end
 * through the loader.
 *
 * Reads are deliberately forgiving.  A missing file, a line with too few
 * fields, a number that will not parse: each of them means "off".  When the
 * layer is installed as an implicit layer it is loaded into every Vulkan
 * process on the machine, so no profile may be able to break one.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where the store lives: $LSL_PROFILES when set, else
 * $XDG_CONFIG_HOME/lsl/profiles.conf, else $HOME/.config/lsl/profiles.conf.
 * NULL when none of those can be named.  Points at a static buffer. */
const char *lsl_profile_path(void);

/* This process's appid, from $SteamAppId or $SteamGameId, or NULL.  Steam sets
 * both for every game it launches; another program's process has neither, which
 * is what makes the implicit layer a no-op everywhere else. */
const char *lsl_profile_appid(void);

/* 1 when `path` holds a `game <appid>` record with enabled=1, in which case
 * *scale, *sharpness and *mode are filled from it.  Any out pointer may be
 * NULL.  0 -- and nothing written -- for a missing file, a missing or disabled
 * record, a record that asks for no upscaling (a scale of 1, or a number that
 * will not parse), or one too malformed to trust. */
int lsl_profile_lookup(const char *path, const char *appid,
                       float *scale, float *sharpness, uint32_t *mode);

#ifdef __cplusplus
}
#endif
