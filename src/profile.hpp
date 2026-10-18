#pragma once
// The GUI's own state: which games have lsl enabled, at what scale and
// sharpness, and where the layer lives.
//
// None of this belongs to Steam, and none of it needs to be there.  The launch
// options string is the entire interface to the layer; this file just remembers
// what to put in it so the user does not have to.  That is also why the format
// is plain text, one record per line: it is meant to be readable and editable
// without the GUI, in the spirit of the launch options themselves.

#include <map>
#include <string>

namespace lsl {

struct Profile {
    bool enabled = false;
    double scale = 2.0;
    double sharpness = 0.0;
    // Which filter the layer runs, as a LSL_LAYER_MODE= value: a name from
    // methodTable(), or "auto". Named rather than numeric so the profile and
    // the launch options read the same, and so this file cannot silently change
    // meaning if an id is ever renumbered. The default is to let the layer
    // choose, which is also what a game that has never been configured gets.
    std::string method = "auto";
};

struct ProfileStore {
    // Where VkLayer_lsl_frame.json lives.  Empty means "not chosen yet", and
    // the GUI falls back to looking beside its own executable.
    std::string layer_dir;

    // By appid.  A game absent from here has never been configured.
    std::map<std::string, Profile> games;

    // The stored profile, or the defaults when the game is not in here.
    Profile get(const std::string& appid) const;
    void set(const std::string& appid, const Profile& profile);

    // A missing or unreadable file is not an error: it is a first run.
    static ProfileStore load(const std::string& path);

    // Writes the whole store, creating the directory.  Reports why on failure.
    bool save(const std::string& path, std::string* error) const;
};

// $XDG_CONFIG_HOME/lsl/profiles.conf, or ~/.config/lsl/profiles.conf.
std::string defaultProfilePath();

}  // namespace lsl
