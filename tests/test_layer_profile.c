// Tests for the layer's per-game configuration: reading the GUI's profile store
// (layer/lsl_profile.c) and choosing what this process should upscale
// (layer/lsl_config.c).
//
// This is the half of the implicit-layer feature that can be checked without a
// GPU, and it is the half most worth checking: the layer is loaded into every
// Vulkan process on the machine once it is installed implicitly, so a parse that
// says "enabled" for the wrong thing -- or trips over a truncated line -- is not
// a wrong picture, it is somebody else's program breaking.  Every malformed
// record below is required to mean "off".
//
// The layer is C, so this is too; it links the two files directly rather than
// the shared object, so a failure here names the function at fault.  The two
// functions lsl_config.c borrows from the layer -- lsl_log and lsl_env_flag --
// stand in below.

#include "lsl_config.h"
#include "lsl_method.h"
#include "lsl_profile.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_checks;
static int g_failures;

static void check(int ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        printf("FAIL %s\n", what);
    }
}

/* ---- the two functions lsl_config.c expects from the layer --------------- */

void lsl_log(const char* fmt, ...) {
    (void)fmt; /* logging is what the layer does; resolving must not depend on it */
}

int lsl_env_flag(const char* name) {
    const char* e = getenv(name);
    return (e && *e && *e != '0') ? 1 : 0;
}

/* -------------------------------------------------------------------------- */

static void write_file(const char* path, const char* text) {
    FILE* f = fopen(path, "w");
    if (!f) {
        perror(path);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
}

/* lsl_config() answers once per process, so each scenario gets a child rather
 * than a reset hook the layer would otherwise have to carry. */
static void expect_config(const char* what, const char* profiles, const char* appid,
                          const char* scale, int want_enabled, float want_scale,
                          uint32_t want_mode, const char* want_source) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(2);
    }
    if (pid == 0) {
        setenv("LSL_PROFILES", profiles, 1);
        if (appid)
            setenv("SteamAppId", appid, 1);
        else
            unsetenv("SteamAppId");
        unsetenv("SteamGameId");
        unsetenv("LSL_LAYER_DISABLE");
        if (scale)
            setenv("LSL_LAYER_SCALE", scale, 1);
        else
            unsetenv("LSL_LAYER_SCALE");
        unsetenv("LSL_LAYER_SHARPNESS");
        unsetenv("LSL_LAYER_MODE");

        const LslConfig* c = lsl_config();
        const int ok = c->enabled == want_enabled && c->scale == want_scale &&
                       c->mode == want_mode && strcmp(c->source, want_source) == 0;
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        perror("waitpid");
        exit(2);
    }
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, what);
}

static void expect_mode(const char* what, const char* profiles, const char* scale,
                        const char* mode, uint32_t want_mode) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(2);
    }
    if (pid == 0) {
        setenv("LSL_PROFILES", profiles, 1);
        setenv("LSL_LAYER_SCALE", scale, 1);
        setenv("LSL_LAYER_MODE", mode, 1);
        unsetenv("SteamAppId");
        unsetenv("SteamGameId");
        unsetenv("LSL_LAYER_DISABLE");
        _exit(lsl_config()->mode == want_mode ? 0 : 1);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        perror("waitpid");
        exit(2);
    }
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, what);
}

int main(void) {
    char tmpl[] = "/tmp/lsl-layer-config.XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) {
        perror("mkdtemp");
        return 2;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/profiles.conf", dir);

    write_file(path,
               "# lsl profiles\n"
               "layer_dir /some/where/lsl/layer\n"
               "game 933110 1 2 -0.5 nis\n"        /* the ordinary case */
               "game 730 0 3 0 fsr1rcas\n"         /* present, disabled */
               "game 555 1\n"                      /* before the fields existed */
               "game 777 1 2.5 0 bogus\n"          /* a method nobody knows */
               "game 888 yes 2 0 nis\n"            /* enabled is not "1" */
               "game 999 1 1 0 nis\n"              /* a scale of 1 means off */
               "game 111 1 notanumber 0 nis\n"     /* a number that is not one */
               "\n"
               "game 222 1 2 0\n");                /* no method at all */

    float s = -1.0f, sh = -1.0f;
    uint32_t m = 123;

    check(lsl_profile_lookup(path, "933110", &s, &sh, &m) == 1,
          "an enabled record is found");
    check(s == 2.0f && sh == -0.5f && m == LSL_MODE_NIS,
          "an enabled record's fields come back");
    check(lsl_profile_lookup(path, "730", NULL, NULL, NULL) == 0,
          "a record with enabled=0 is off");
    check(lsl_profile_lookup(path, "404", NULL, NULL, NULL) == 0,
          "an appid the store never heard of is off");

    check(lsl_profile_lookup(path, "555", &s, &sh, &m) == 1,
          "a record with no scale field still counts");
    check(s == 2.0f && sh == 0.0f && m == LSL_MODE_AUTO,
          "  and takes the profile defaults");
    check(lsl_profile_lookup(path, "222", &s, &sh, &m) == 1,
          "a record with no method field still counts");
    check(m == LSL_MODE_AUTO, "  and defaults the method to auto");

    check(lsl_profile_lookup(path, "777", &s, &sh, &m) == 1,
          "an unknown method name still enables the game");
    check(m == LSL_MODE_AUTO, "  ...falling back to auto");
    check(lsl_profile_lookup(path, "888", NULL, NULL, NULL) == 0,
          "enabled has to be exactly 1");
    check(lsl_profile_lookup(path, "999", NULL, NULL, NULL) == 0,
          "a profile scale of 1 means off");
    check(lsl_profile_lookup(path, "111", NULL, NULL, NULL) == 0,
          "an unparsable scale means off");

    check(lsl_profile_lookup(NULL, "1", NULL, NULL, NULL) == 0, "no path is off");
    check(lsl_profile_lookup(path, NULL, NULL, NULL, NULL) == 0, "no appid is off");
    check(lsl_profile_lookup(path, "", NULL, NULL, NULL) == 0, "an empty appid is off");
    check(lsl_profile_lookup("/nonexistent/nope.conf", "1", NULL, NULL, NULL) == 0,
          "a store that is not there is off");

    /* Where the store is looked for. */
    setenv("LSL_PROFILES", "/explicit/profiles.conf", 1);
    check(strcmp(lsl_profile_path(), "/explicit/profiles.conf") == 0,
          "$LSL_PROFILES is used when set");
    unsetenv("LSL_PROFILES");
    setenv("XDG_CONFIG_HOME", "/xdg", 1);
    check(strcmp(lsl_profile_path(), "/xdg/lsl/profiles.conf") == 0,
          "$XDG_CONFIG_HOME is the next choice");
    unsetenv("XDG_CONFIG_HOME");
    setenv("HOME", "/home/someone", 1);
    check(strcmp(lsl_profile_path(), "/home/someone/.config/lsl/profiles.conf") == 0,
          "$HOME is the fallback");

    /* Which app this process is. */
    unsetenv("SteamAppId");
    unsetenv("SteamGameId");
    check(lsl_profile_appid() == NULL, "no Steam variable means no appid");
    setenv("SteamGameId", "42", 1);
    check(lsl_profile_appid() != NULL && strcmp(lsl_profile_appid(), "42") == 0,
          "SteamGameId is the fallback");
    setenv("SteamAppId", "7", 1);
    check(strcmp(lsl_profile_appid(), "7") == 0, "SteamAppId wins over SteamGameId");
    unsetenv("SteamAppId");
    unsetenv("SteamGameId");

    /* What the layer does with all of that. */
    expect_config("an enabled appid upscales from the profile", path, "933110", NULL, 1,
                  2.0f, LSL_MODE_NIS, "profile");
    expect_config("a disabled appid does nothing", path, "730", NULL, 0, 0.0f,
                  LSL_MODE_FSR1RCAS, "off");
    expect_config("an appid nobody enabled does nothing", path, "404", NULL, 0, 0.0f,
                  LSL_MODE_FSR1RCAS, "off");
    expect_config("an explicit LSL_LAYER_SCALE still wins", path, "730", "3", 1, 3.0f,
                  LSL_MODE_FSR1RCAS, "environment");
    expect_config("LSL_LAYER_SCALE=1 is loaded but inert", path, "933110", "1", 0, 0.0f,
                  LSL_MODE_FSR1RCAS, "environment");
    expect_config("a scale with no appid still upscales", path, NULL, "2", 1, 2.0f,
                  LSL_MODE_FSR1RCAS, "environment");
    expect_config("nothing at all does nothing", path, NULL, NULL, 0, 0.0f,
                  LSL_MODE_FSR1RCAS, "off");

    expect_mode("LSL_LAYER_MODE names the filter", path, "2", "cnn", LSL_MODE_CNN);
    expect_mode("an unknown LSL_LAYER_MODE falls back", path, "2", "nope",
                LSL_MODE_FSR1RCAS);

    printf("layer-config: %d checks, %d failures\n", g_checks, g_failures);
    unlink(path);
    rmdir(dir);
    return g_failures == 0 ? 0 : 1;
}
