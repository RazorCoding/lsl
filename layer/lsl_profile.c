#include "lsl_profile.h"

#include "lsl_method.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *lsl_profile_path(void) {
    static char buf[4096];
    const char *p = getenv("LSL_PROFILES");
    if (p && *p) {
        snprintf(buf, sizeof buf, "%s", p);
        return buf;
    }
    const char *base = getenv("XDG_CONFIG_HOME");
    if (base && *base) {
        snprintf(buf, sizeof buf, "%s/lsl/profiles.conf", base);
        return buf;
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(buf, sizeof buf, "%s/.config/lsl/profiles.conf", home);
        return buf;
    }
    return NULL;
}

const char *lsl_profile_appid(void) {
    const char *a = getenv("SteamAppId");
    if (a && *a) return a;
    a = getenv("SteamGameId");
    return (a && *a) ? a : NULL;
}

/* One whitespace-delimited token from *pp, NUL-terminated into `tok` and
 * advancing *pp past it.  0 when the rest of the line holds none. */
static int next_token(const char **pp, char *tok, size_t cap) {
    const char *p = *pp;
    while (*p == ' ' || *p == '\t') ++p;
    if (!*p || *p == '\r' || *p == '\n') {
        *pp = p;
        return 0;
    }
    size_t n = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
        if (n + 1 < cap) tok[n++] = *p;
        ++p; /* a token longer than the buffer is skipped, not split */
    }
    tok[n] = '\0';
    *pp = p;
    return 1;
}

int lsl_profile_lookup(const char *path, const char *appid,
                       float *scale, float *sharpness, uint32_t *mode) {
    if (!path || !*path || !appid || !*appid) return 0;

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[512];
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;

        char key[16];
        if (!next_token(&p, key, sizeof key) || strcmp(key, "game") != 0) continue;

        /* Look before parsing the rest: most lines belong to other games. */
        char id[32];
        if (!next_token(&p, id, sizeof id) || strcmp(id, appid) != 0) continue;

        char enabled[8];
        if (!next_token(&p, enabled, sizeof enabled) || strcmp(enabled, "1") != 0)
            continue; /* present but disabled, or too malformed to trust */

        /* scale, sharpness and method are optional and default to the profile
         * defaults, so a record written before any of them existed still means
         * something rather than nothing. */
        char tok[32];
        float s = 2.0f, sh = 0.0f;
        uint32_t m = LSL_MODE_AUTO;
        if (next_token(&p, tok, sizeof tok)) s = strtof(tok, NULL);
        if (next_token(&p, tok, sizeof tok)) sh = strtof(tok, NULL);
        if (next_token(&p, tok, sizeof tok)) {
            uint32_t parsed;
            if (lsl_mode_parse(tok, &parsed) == 0) m = parsed;
        }
        if (!(s > 1.0f)) {
            /* The record asked for no upscaling (scale 1, or a number that
             * would not parse).  Off, and done -- a second record for the same
             * appid must not be able to turn it back on. */
            fclose(f);
            return 0;
        }

        if (scale) *scale = s;
        if (sharpness) *sharpness = sh;
        if (mode) *mode = m;
        found = 1;
    }
    fclose(f);
    return found;
}
