#include "profile.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace lsl {
namespace {

// Numbers are written the same way the launch options write them, so the file
// and the command line agree: 2 rather than 2.000000.
std::string formatNumber(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s = buf;
    const size_t dot = s.find('.');
    if (dot != std::string::npos) {
        size_t last = s.find_last_not_of('0');
        if (last == dot) --last;
        s.erase(last + 1);
    }
    return s;
}

}  // namespace

Profile ProfileStore::get(const std::string& appid) const {
    const auto it = games.find(appid);
    return it == games.end() ? Profile{} : it->second;
}

void ProfileStore::set(const std::string& appid, const Profile& profile) {
    games[appid] = profile;
}

ProfileStore ProfileStore::load(const std::string& path) {
    ProfileStore store;
    std::ifstream in(path);
    if (!in) return store;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        if (key == "layer_dir") {
            std::string rest;
            std::getline(ls, rest);
            // The path is the rest of the line, so it may contain spaces.
            size_t begin = rest.find_first_not_of(" \t");
            store.layer_dir = begin == std::string::npos ? "" : rest.substr(begin);
        } else if (key == "game") {
            std::string appid;
            int enabled = 0;
            double scale = 2.0, sharpness = 0.0;
            if (!(ls >> appid >> enabled >> scale >> sharpness)) continue;
            Profile p;
            p.enabled = enabled != 0;
            p.scale = scale;
            p.sharpness = sharpness;
            store.games[appid] = p;
        }
        // Unknown keys are ignored rather than fatal, so a newer GUI's file
        // still loads here.
    }
    return store;
}

bool ProfileStore::save(const std::string& path, std::string* error) const {
    std::error_code ec;
    const fs::path p(path);
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
    }
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            *error = "cannot write " + tmp;
            return false;
        }
        out << "# lsl profiles.  One game per line:\n"
               "#   game <appid> <enabled 0|1> <scale> <sharpness>\n"
               "# Written by the lsl GUI; safe to edit by hand.\n";
        if (!layer_dir.empty()) out << "layer_dir " << layer_dir << "\n";
        for (const auto& [appid, profile] : games) {
            out << "game " << appid << ' ' << (profile.enabled ? 1 : 0) << ' '
                << formatNumber(profile.scale) << ' '
                << formatNumber(profile.sharpness) << "\n";
        }
        if (!out) {
            *error = "short write to " + tmp;
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        *error = "cannot replace " + path + ": " + ec.message();
        return false;
    }
    return true;
}

std::string defaultProfilePath() {
    std::string base;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        base = xdg;
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        base = std::string(home) + "/.config";
    }
    if (base.empty()) return {};
    return base + "/lsl/profiles.conf";
}

}  // namespace lsl
