#pragma once
// Reading and writing what Steam keeps on disk: which libraries exist, which
// games are installed, and -- the one the project actually needs -- the per-game
// launch options, because that string is where the layer gets injected.
//
// There is no Steamworks API here and no dependency on a running Steam.  The
// GUI edits the same files Steam edits, which is the whole point of a layer:
// a game can be wrapped without touching the game.  Steam also *owns* those
// files, so nothing here reserialises them -- a nested key's value is located
// and edited in place, and everything else comes back byte for byte.

#include <cstddef>
#include <string>
#include <vector>

namespace lsl {

struct SteamGame {
    std::string appid;
    std::string name;
    std::string installdir;
    std::string library;  // library root the manifest was found in
};

// Whether a name is one of Valve's own non-games -- Proton, a Steam Linux
// Runtime, the redistributables -- which sit in the same library and the same
// manifest format as a game.
//
// This is a guess, and deliberately a conservative one: nothing lsl can read
// records an app's type.  appmanifest_<id>.acf and libraryfolders.vdf are
// identical in shape for Proton and for a game, appinfo.vdf stores its records
// as opaque hashed blobs in the current Steam, and the store metadata in
// Steam's cache is missing for a game that has never been launched -- so using
// it would hide real games.  Valve's names, on the other hand, are stable, and
// a misfire here is both visible and reversible: the GUI has a switch for it.
bool steamLooksLikeTool(const std::string& name);

// Root of the Steam installation: $LSL_STEAM_ROOT if set, else the first of
// ~/.local/share/Steam and ~/.steam/steam that has a steamapps directory,
// else "".  The environment override exists because a second Steam (Flatpak,
// a prefix, a test fixture) is a normal thing to have.
std::string steamRoot();

// Every library root from steamapps/libraryfolders.vdf, with `root` itself
// first.  An unreadable or missing file yields just `root`.
std::vector<std::string> steamLibraries(const std::string& root);

// Installed games: every appmanifest_*.acf across all libraries that names a
// game, sorted by name.  Manifests without a name (a partly-written download)
// are skipped.
std::vector<SteamGame> steamGames(const std::string& root);

// The active user's localconfig.vdf, which is where per-game launch options
// live, or "" when no user has ever logged in.  "Active" is whatever
// config/loginusers.vdf marks MostRecent; with several userdata directories and
// no such marking the lowest-numbered one is used, deterministically.
std::string steamLocalConfig(const std::string& root);

// Current launch options for one game, or "" when unset.
std::string steamReadLaunchOptions(const std::string& localconfig,
                                   const std::string& appid);

// Write launch options, creating the game's block if Steam has never written
// one.  Refuses to write anything that is not well-formed VDF, and keeps a
// single .lsl-backup of the previous contents.  Returns false and sets *error
// rather than leaving a damaged file behind.
bool steamWriteLaunchOptions(const std::string& localconfig,
                             const std::string& appid,
                             const std::string& options, std::string* error);

// ---- VDF ------------------------------------------------------------------

// Valve's text key-value format.  Two operations are needed: find the byte
// range of a nested value, and replace it in place.
struct VdfSpan {
    size_t begin = 0;  // first byte of the value, inside the quotes
    size_t end = 0;    // one past the last byte
};

// Find `path` as a *value*.  A path that names a block, or that is absent,
// returns false.  `path` is walked in order from the top of the file, so
// {"UserLocalConfigStore","Software",...,"apps","730","LaunchOptions"} is the
// launch-options string for app 730.
bool vdfFind(const std::string& text, const std::vector<std::string>& path,
             VdfSpan* out);

// Text with `path` set to `value`, creating missing blocks and escaping the
// value.  Returns false (and leaves *out untouched) when there is nowhere safe
// to insert -- an unparseable file, or a missing top-level block -- because
// inventing a second root block for a file Steam owns is worse than failing.
bool vdfSet(const std::string& text, const std::vector<std::string>& path,
            const std::string& value, std::string* out);

// Every brace balanced and every quoted string closed, examined outside quoted
// text.  A cheap dry run: it cannot prove a file is valid VDF, but it does catch
// the failure mode that matters, a splice that lost a brace.
bool vdfWellFormed(const std::string& text);

// ---- launch options -------------------------------------------------------

// `${LSL_LAYER_SCALE}` and friends are ordinary environment assignments, so
// enabling the layer means editing a token list.  Steam substitutes %command%
// with the game; without that substitution the options string is *appended* to
// the command line, which is why adding env assignments to a string that has no
// %command% has to insert one.
//
// Everything else in the string belongs to the user and survives: their own
// arguments, their own environment, their own VK_INSTANCE_LAYERS.  Where the
// layer needs a variable the user also uses, the two are merged rather than
// overwritten -- the layer's entry is added to the front of the list, and
// clearing removes only that entry.

// Options with lsl's contribution removed and nothing added.  `layer_dir` is
// needed because VK_ADD_LAYER_PATH is a shared list: removing "the lsl entry"
// means removing this one path from it, not the whole variable, which may hold
// another layer's path too.
//
// One asymmetry is deliberate: a %command% is not removed.  clear cannot tell a
// %command% lsl inserted from one the user wrote, and removing the user's would
// change what the game is launched with.  A lone %command% left over from
// lsl -- the case where there was nothing else in the string -- is dropped,
// because that and an empty string mean the same thing to Steam.
std::string steamOptionsClear(const std::string& existing,
                              const std::string& layer_dir);

// Options with lsl enabled at this scale/sharpness, replacing any previous lsl
// tokens.  `layer_dir` is the directory holding VkLayer_lsl_frame.json.
std::string steamOptionsApply(const std::string& existing,
                              const std::string& layer_dir, double scale,
                              double sharpness);

// Read back the settings lsl is currently present with, for the GUI to show.
// Returns false when these options do not enable lsl at all.
bool steamOptionsParse(const std::string& existing, double* scale,
                       double* sharpness);

}  // namespace lsl
