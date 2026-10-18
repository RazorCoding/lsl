// The seam between the GUI and the layer: the profile store.
//
// The GUI writes src/profile.cpp's text and the layer re-reads it with its own
// C parser (layer/lsl_profile.c) -- the layer cannot link the C++ store, so the
// format exists twice, and the failure mode if they disagree is not a wrong
// picture.  It is an implicit layer, loaded into every Vulkan process on the
// machine, reading a record it does not understand and either upscaling
// something it should not or ignoring a game the user switched on.
//
// The other tests each cover one side: `layer-config` drives the C parser over
// stores written by hand, `steam` covers the C++ side without the layer, and
// `appid-check` runs the C parser end to end through the loader.  None of them
// puts the GUI's *writer* and the layer's *reader* in the same process.  This
// does: it saves a store with ProfileStore::save() and reads it back with
// lsl_profile_lookup() and lsl_config(), so a change to the record format or to
// the method vocabulary that only one side knows about fails here.
//
// No display and no GPU.

#include "lsl_config.h"
#include "lsl_method.h"
#include "lsl_profile.h"

#include "method.hpp"
#include "profile.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

static int g_checks;
static int g_failures;

static void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

/* ---- the two functions lsl_config.c borrows from the layer --------------- */

extern "C" void lsl_log(const char* fmt, ...) {
    (void)fmt; /* resolving must not depend on logging */
}

extern "C" int lsl_env_flag(const char* name) {
    const char* e = std::getenv(name);
    return (e && *e && *e != '0') ? 1 : 0;
}

/* -------------------------------------------------------------------------- */

// lsl_config() answers once per process, so its scenario gets a child.
static void expect_config(const std::string& profiles, const char* appid,
                          int want_enabled, float want_scale, uint32_t want_mode,
                          const std::string& what) {
    pid_t pid = fork();
    if (pid < 0) {
        std::perror("fork");
        std::exit(2);
    }
    if (pid == 0) {
        setenv("LSL_PROFILES", profiles.c_str(), 1);
        setenv("SteamAppId", appid, 1);
        unsetenv("SteamGameId");
        unsetenv("LSL_LAYER_DISABLE");
        unsetenv("LSL_LAYER_SCALE");
        unsetenv("LSL_LAYER_SHARPNESS");
        unsetenv("LSL_LAYER_MODE");
        const LslConfig* c = lsl_config();
        _exit(c->enabled == want_enabled && c->scale == want_scale &&
                      c->mode == want_mode
                  ? 0
                  : 1);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        std::perror("waitpid");
        std::exit(2);
    }
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, what);
}

int main() {
    char tmpl[] = "/tmp/lsl-layer-store.XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) {
        std::perror("mkdtemp");
        return 2;
    }
    const std::string path = std::string(dir) + "/profiles.conf";

    // A store of the shape the window leaves behind: one game on at a
    // fractional scale, one switched off, one left at the default method.
    lsl::ProfileStore store;
    store.layer_dir = "/some/where/lsl/layer";
    lsl::Profile on;
    on.enabled = true;
    on.scale = 2.5;
    on.sharpness = -0.5;
    on.method = "nis";
    lsl::Profile off;
    off.enabled = false;
    off.scale = 3.0;
    off.method = "fsr1rcas";
    lsl::Profile untouched;  // enabled = false, scale = 2, method = "auto"
    untouched.enabled = true;
    store.set("933110", on);
    store.set("730", off);
    store.set("404", untouched);

    std::string error;
    check(store.save(path, &error), "the GUI's writer saves a store: " + error);

    // The layer's reader has to agree about every one of them.
    float s = 0.0f, sh = 0.0f;
    uint32_t m = 123;
    check(lsl_profile_lookup(path.c_str(), "933110", &s, &sh, &m) == 1,
          "an enabled game is found");
    check(s == 2.5f && sh == -0.5f && m == LSL_MODE_NIS,
          "  and its scale, sharpness and method read back");
    check(lsl_profile_lookup(path.c_str(), "730", nullptr, nullptr, nullptr) == 0,
          "a game switched off is off");
    check(lsl_profile_lookup(path.c_str(), "404", nullptr, nullptr, &m) == 1,
          "a game left at the default method is found");
    check(m == LSL_MODE_AUTO, "  and its method is auto");
    check(lsl_profile_lookup(path.c_str(), "111", nullptr, nullptr, nullptr) == 0,
          "a game the window never wrote is off");

    // Every name the GUI can put in the method field has to be a name the
    // layer parses back to the same id.  This is the vocabulary half of the
    // same seam: modeName() is what the writer emits, lsl_mode_parse() is what
    // the reader runs.
    for (unsigned i = 0; i < lsl::MODE_COUNT; ++i) {
        lsl::ProfileStore one;
        lsl::Profile p;
        p.enabled = true;
        p.method = lsl::modeName(i);
        one.set("1", p);
        const std::string name = lsl::modeName(i);
        check(one.save(path, &error), "the writer saves method " + name);
        uint32_t got = 123;
        const bool found =
            lsl_profile_lookup(path.c_str(), "1", nullptr, nullptr, &got) == 1;
        check(found && got == i, "the layer reads method " + name + " back");
    }

    // And the whole decision, not just the lookup: the GUI's file, SteamAppId
    // from the environment, a resolved scale out.
    store.save(path, &error);
    setenv("LSL_PROFILES", path.c_str(), 1);
    expect_config(path, "933110", 1, 2.5f, LSL_MODE_NIS,
                  "the layer upscales a game the GUI enabled");
    expect_config(path, "730", 0, 0.0f, LSL_MODE_FSR1RCAS,
                  "the layer ignores a game the GUI left off");
    expect_config(path, "404", 1, 2.0f, LSL_MODE_AUTO,
                  "the layer upscales a default-profile game with auto");

    std::printf("layer-store: %d checks, %d failures\n", g_checks, g_failures);
    unlink(path.c_str());
    rmdir(dir);
    return g_failures == 0 ? 0 : 1;
}
