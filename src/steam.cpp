// Steam's on-disk configuration: libraries, installed games, and the per-game
// launch options the layer is injected through.
//
// The launch-options half is the part that matters and the part that can do
// damage, so it is written defensively: the value is spliced into the file in
// place (never reserialised), the result has to still be well-formed VDF, and a
// backup of the file as lsl first found it is kept beside it.

#include "steam.hpp"

#include "method.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace lsl {
namespace {

// ---------------------------------------------------------------- file access

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string homeDir() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    return {};
}

std::string escapeVdf(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// Raw VDF text with its escapes resolved.  Used wherever a span is handed back
// to a caller that wants the value rather than the bytes.
std::string unescapeVdf(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) ++i;
        out.push_back(s[i]);
    }
    return out;
}

// --------------------------------------------------------------------- scanner
//
// VDF is `"key" "value"` and `"key" { ... }`, whitespace between everything,
// `//` to end of line, and backslash escapes inside quoted text.  The scanner
// only ever walks forwards and never copies the file: the caller gets byte
// offsets into the original text, which is what makes in-place editing exact.

struct Scanner {
    const std::string& t;
    size_t i = 0;
    size_t n;
    explicit Scanner(const std::string& text) : t(text), n(text.size()) {}

    bool atEnd() const { return i >= n; }

    void skipSpace() {
        while (i < n) {
            const char c = t[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++i;
            } else if (c == '/' && i + 1 < n && t[i + 1] == '/') {
                while (i < n && t[i] != '\n') ++i;
            } else {
                break;
            }
        }
    }

    // The next non-space character, or '\0' at the end of the text.
    char peek() {
        skipSpace();
        return i < n ? t[i] : '\0';
    }

    // Reads a quoted string at the cursor.  `contents` has its escapes resolved;
    // [begin,end) is the raw text between the quotes, so a replacement can be
    // spliced back without rescanning.  Running out of text means the string
    // was never closed, and that is a failure, not an empty string.
    bool readString(std::string* contents, size_t* begin, size_t* end) {
        skipSpace();
        if (i >= n || t[i] != '"') return false;
        ++i;
        const size_t start = i;
        contents->clear();
        while (i < n && t[i] != '"') {
            if (t[i] == '\\' && i + 1 < n) {
                ++i;
                contents->push_back(t[i] == 'n' ? '\n' : t[i]);
            } else {
                contents->push_back(t[i]);
            }
            ++i;
        }
        if (i >= n) return false;
        *begin = start;
        *end = i;
        ++i;
        return true;
    }
};

// Moves the cursor from the `{` at `brace` to just past the matching `}`.  Raw
// character scanning, but quoted strings are stepped over, so a brace inside a
// string cannot balance anything.
bool skipBlockFrom(Scanner& s, size_t brace) {
    if (brace >= s.n || s.t[brace] != '{') return false;
    size_t i = brace;
    int depth = 0;
    while (i < s.n) {
        const char c = s.t[i];
        if (c == '"') {
            ++i;
            while (i < s.n && s.t[i] != '"') {
                if (s.t[i] == '\\' && i + 1 < s.n) ++i;
                ++i;
            }
            if (i >= s.n) return false;
            ++i;
            continue;
        }
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (--depth == 0) {
                s.i = i + 1;
                return true;
            }
        }
        ++i;
    }
    return false;
}

// Whitespace at the start of the line containing `pos`.
std::string lineIndent(const std::string& t, size_t pos) {
    size_t start = pos;
    while (start > 0 && t[start - 1] != '\n') --start;
    size_t end = start;
    while (end < t.size() && (t[end] == ' ' || t[end] == '\t')) ++end;
    return t.substr(start, end - start);
}

// A block of the file.  `body_end` is the index of its closing brace, which is
// where a new child key belongs.
struct Block {
    size_t body_end = 0;
    std::string indent;  // indentation of the block's own key line
};

// Walks `path` as a chain of blocks, leaving `out` describing the last one.
// The cursor starts inside the enclosing block, or at the top of the file,
// because a file is itself a list of keys.
bool findBlockIn(Scanner& s, const std::vector<std::string>& path, size_t depth,
                 Block* out) {
    while (true) {
        s.skipSpace();
        if (s.atEnd() || s.peek() == '}') return false;

        const size_t key_start = s.i;
        std::string key;
        size_t kb = 0, ke = 0;
        if (!s.readString(&key, &kb, &ke)) return false;
        s.skipSpace();
        if (s.atEnd()) return false;

        if (s.t[s.i] != '{') {  // a plain value: not the path we are after
            std::string v;
            size_t vb = 0, ve = 0;
            if (!s.readString(&v, &vb, &ve)) return false;
            continue;
        }

        const size_t brace = s.i;
        ++s.i;
        if (key == path[depth]) {
            if (depth + 1 == path.size()) {
                Scanner probe = s;
                if (!skipBlockFrom(probe, brace)) return false;
                out->body_end = probe.i - 1;  // the closing brace itself
                out->indent = lineIndent(s.t, key_start);
                return true;
            }
            if (findBlockIn(s, path, depth + 1, out)) return true;
            s.i = brace;  // that descent failed; skip this block instead
        }
        if (!skipBlockFrom(s, brace)) return false;
    }
}

bool findBlock(const std::string& text, const std::vector<std::string>& path,
               Block* out) {
    if (path.empty()) return false;
    Scanner s(text);
    return findBlockIn(s, path, 0, out);
}

// The children of a block, in file order: `visit(key, value)` for a plain value
// and `visit(key, nullptr)` for a nested block.
template <typename Fn>
bool forEachChild(const std::string& text, const std::vector<std::string>& path,
                  Fn visit) {
    Block b;
    Scanner s(text);
    if (!findBlockIn(s, path, 0, &b)) return false;
    while (true) {
        s.skipSpace();
        if (s.atEnd() || s.peek() == '}' || s.i >= b.body_end) return true;
        std::string key;
        size_t kb = 0, ke = 0;
        if (!s.readString(&key, &kb, &ke)) return false;
        s.skipSpace();
        if (s.i < s.n && s.t[s.i] == '{') {
            if (!skipBlockFrom(s, s.i)) return false;
            visit(key, nullptr);
        } else {
            std::string v;
            size_t vb = 0, ve = 0;
            if (!s.readString(&v, &vb, &ve)) return false;
            visit(key, &v);
        }
    }
}

// Whitespace at the start of the line containing `pos`.
// ------------------------------------------------------------------ options

std::vector<std::string> splitTokens(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool have = false;
    bool quoted = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quoted) {
            cur.push_back(c);
            if (c == '\\' && i + 1 < s.size()) {
                cur.push_back(s[++i]);
            } else if (c == '"') {
                quoted = false;
            }
        } else if (c == '"') {
            quoted = true;
            have = true;
            cur.push_back(c);
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (have) {
                out.push_back(cur);
                cur.clear();
                have = false;
            }
        } else {
            cur.push_back(c);
            have = true;
        }
    }
    if (have) out.push_back(cur);
    return out;
}

std::string joinTokens(const std::vector<std::string>& t) {
    std::string out;
    for (const std::string& s : t) {
        if (!out.empty()) out.push_back(' ');
        out += s;
    }
    return out;
}

bool envNameChar(char c, bool first) {
    if (c == '_') return true;
    if (std::isalpha(static_cast<unsigned char>(c))) return true;
    return !first && std::isdigit(static_cast<unsigned char>(c));
}

// Splits `KEY=value` for an environment assignment, returning the value
// unquoted.  `was_quoted` records whether the quotes were there, so a rebuild
// can put them back only where they are needed.
bool splitAssign(const std::string& tok, std::string* key, std::string* val,
                 bool* was_quoted) {
    const size_t eq = tok.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    for (size_t i = 0; i < eq; ++i)
        if (!envNameChar(tok[i], i == 0)) return false;

    *key = tok.substr(0, eq);
    std::string v = tok.substr(eq + 1);
    *was_quoted = false;
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        *was_quoted = true;
        v = unescapeVdf(v.substr(1, v.size() - 2));
    }
    *val = v;
    return true;
}

std::vector<std::string> splitList(const std::string& v) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= v.size()) {
        const size_t colon = v.find(':', start);
        const size_t end = colon == std::string::npos ? v.size() : colon;
        if (end > start) out.push_back(v.substr(start, end - start));
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return out;
}

std::string joinList(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) {
        if (!out.empty()) out.push_back(':');
        out += s;
    }
    return out;
}

void removeListEntry(std::vector<std::string>* v, const std::string& entry) {
    v->erase(std::remove(v->begin(), v->end(), entry), v->end());
}

// `KEY=value`, quoted when the value needs it.  A layer path containing a space
// is not hypothetical: a checkout under "lsl build" is one, and unquoted it
// would reach the process as two arguments.
std::string formatVar(const char* key, const std::string& value) {
    std::string out = std::string(key) + "=";
    if (value.find_first_of(" \t\"'") == std::string::npos) return out + value;
    out.push_back('"');
    for (char c : value) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// Every environment variable the layer reads is LSL_*.  Matching the prefix
// rather than a fixed list means a variable added to the layer later is still
// cleaned up by an older GUI.
constexpr const char* kLayerName = "VK_LAYER_LSL_frame";

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

// The shared environment variables taken apart, plus everything in the string
// that is not ours and therefore has to survive.
struct OptionScan {
    std::vector<std::string> kept;
    std::vector<std::string> add_layers;  // user's VK_ADD_LAYER_PATH entries
    std::vector<std::string> layers;      // user's VK_INSTANCE_LAYERS entries
    bool have_add = false;
    bool have_layers = false;
    bool have_command = false;
};

OptionScan scanOptions(const std::string& existing) {
    OptionScan sc;
    for (const std::string& tok : splitTokens(existing)) {
        if (tok == "%command%") {
            sc.have_command = true;
            sc.kept.push_back(tok);
            continue;
        }
        std::string key, val;
        bool quoted = false;
        if (splitAssign(tok, &key, &val, &quoted)) {
            if (key.rfind("LSL_", 0) == 0) continue;
            if (key == "VK_ADD_LAYER_PATH") {
                sc.have_add = true;
                sc.add_layers = splitList(val);
                continue;
            }
            if (key == "VK_INSTANCE_LAYERS") {
                sc.have_layers = true;
                sc.layers = splitList(val);
                continue;
            }
        }
        sc.kept.push_back(tok);
    }
    return sc;
}

}  // namespace

// ------------------------------------------------------------------ Steam dirs

std::string steamRoot() {
    if (const char* e = std::getenv("LSL_STEAM_ROOT"); e && *e) return e;
    const std::string home = homeDir();
    if (home.empty()) return {};
    const std::string candidates[] = {home + "/.local/share/Steam",
                                      home + "/.steam/steam"};
    std::error_code ec;
    for (const std::string& c : candidates)
        if (fs::is_directory(c + "/steamapps", ec)) return c;
    return {};
}

std::vector<std::string> steamLibraries(const std::string& root) {
    std::vector<std::string> out;
    if (root.empty()) return out;
    out.push_back(root);

    const std::string text = readFile(root + "/steamapps/libraryfolders.vdf");
    if (text.empty()) return out;

    // Two formats exist.  The old one puts the path straight after a numeric
    // key under LibraryFolders; the current one nests a block per library with
    // a `path` key inside.  Both are handled, and the capitalization has
    // varied, so both spellings of the root are tried.
    const char* roots[] = {"libraryfolders", "LibraryFolders"};
    for (const char* name : roots) {
        const bool walked = forEachChild(
            text, {name}, [&](const std::string& key, const std::string* value) {
                std::string path;
                if (value) {
                    if (key.empty() ||
                        key.find_first_not_of("0123456789") != std::string::npos)
                        return;
                    path = unescapeVdf(*value);
                } else {
                    VdfSpan span;
                    if (!vdfFind(text, {name, key, "path"}, &span)) return;
                    path = unescapeVdf(
                        text.substr(span.begin, span.end - span.begin));
                }
                if (path.empty()) return;
                std::error_code ec;
                if (!fs::is_directory(path + "/steamapps", ec)) return;
                if (std::find(out.begin(), out.end(), path) == out.end())
                    out.push_back(path);
            });
        if (walked) break;
    }
    return out;
}

bool steamLooksLikeTool(const std::string& name) {
    if (name == "Steam Linux Runtime") return true;
    if (name.rfind("Steam Linux Runtime ", 0) == 0) return true;
    if (name == "Steamworks Common Redistributables") return true;
    if (name == "Proton") return true;

    // "Proton 11.0", "Proton - Experimental", "Proton Experimental",
    // "Proton Hotfix" -- but not a game whose title merely begins with the
    // word, such as "Proton Pulse", which is why the rest has to look like a
    // version or one of Valve's own suffixes.
    if (name.rfind("Proton ", 0) == 0) {
        const std::string rest = name.substr(7);
        if (!rest.empty() &&
            (rest[0] == '-' || (rest[0] >= '0' && rest[0] <= '9') ||
             rest == "Experimental" || rest == "Hotfix"))
            return true;
    }
    return false;
}

std::vector<SteamGame> steamGames(const std::string& root) {
    std::vector<SteamGame> games;
    std::error_code ec;
    for (const std::string& lib : steamLibraries(root)) {
        const std::string dir = lib + "/steamapps";
        if (!fs::is_directory(dir, ec)) continue;
        for (const fs::directory_entry& ent : fs::directory_iterator(dir, ec)) {
            const std::string fname = ent.path().filename().string();
            if (fname.rfind("appmanifest_", 0) != 0) continue;
            if (fname.size() < 12 || fname.substr(fname.size() - 4) != ".acf")
                continue;
            const std::string text = readFile(ent.path().string());
            if (text.empty()) continue;

            SteamGame g;
            VdfSpan span;
            if (vdfFind(text, {"AppState", "appid"}, &span))
                g.appid = text.substr(span.begin, span.end - span.begin);
            else
                g.appid = fname.substr(12, fname.size() - 16);
            if (!vdfFind(text, {"AppState", "name"}, &span)) continue;
            g.name = unescapeVdf(text.substr(span.begin, span.end - span.begin));
            if (g.name.empty()) continue;
            if (vdfFind(text, {"AppState", "installdir"}, &span))
                g.installdir =
                    unescapeVdf(text.substr(span.begin, span.end - span.begin));
            g.library = lib;
            games.push_back(g);
        }
    }
    std::sort(games.begin(), games.end(),
              [](const SteamGame& a, const SteamGame& b) {
                  if (a.name != b.name) return a.name < b.name;
                  return a.appid < b.appid;
              });
    return games;
}

std::string steamLocalConfig(const std::string& root) {
    if (root.empty()) return {};
    std::error_code ec;

    // The user Steam would resume: config/loginusers.vdf marks one MostRecent.
    const std::string users = readFile(root + "/config/loginusers.vdf");
    if (!users.empty()) {
        std::string wanted;
        forEachChild(users, {"users"}, [&](const std::string& key,
                                           const std::string* value) {
            if (value || !wanted.empty()) return;
            VdfSpan span;
            if (vdfFind(users, {"users", unescapeVdf(key), "MostRecent"},
                        &span) &&
                users.substr(span.begin, span.end - span.begin) == "1")
                wanted = unescapeVdf(key);
        });
        if (!wanted.empty()) {
            const std::string cand =
                root + "/userdata/" + wanted + "/config/localconfig.vdf";
            if (fs::is_regular_file(cand, ec)) return cand;
        }
    }

    // Otherwise the lowest-numbered userdata directory that has one, so the
    // answer does not depend on readdir order.
    std::string best, best_id;
    if (fs::is_directory(root + "/userdata", ec)) {
        for (const fs::directory_entry& ent :
             fs::directory_iterator(root + "/userdata", ec)) {
            if (!ent.is_directory()) continue;
            const std::string id = ent.path().filename().string();
            if (id.empty() || id.find_first_not_of("0123456789") !=
                                  std::string::npos)
                continue;
            const std::string cand =
                ent.path().string() + "/config/localconfig.vdf";
            if (!fs::is_regular_file(cand, ec)) continue;
            if (best.empty() || id.size() < best_id.size() ||
                (id.size() == best_id.size() && id < best_id)) {
                best = cand;
                best_id = id;
            }
        }
    }
    return best;
}

// ---------------------------------------------------------------------- VDF

bool vdfFind(const std::string& text, const std::vector<std::string>& path,
             VdfSpan* out) {
    if (path.empty()) return false;
    const std::string& last = path.back();

    // Everything but the last element is a block chain; the last is a value.
    // With one element that chain is empty and the search is the file's own
    // top level.
    Scanner s(text);
    std::vector<std::string> parent(path.begin(), path.end() - 1);
    size_t limit = text.size();
    if (!parent.empty()) {
        Block b;
        if (!findBlockIn(s, parent, 0, &b)) return false;
        limit = b.body_end;
    }

    while (true) {
        s.skipSpace();
        if (s.atEnd() || s.peek() == '}' || s.i >= limit) return false;
        std::string key;
        size_t kb = 0, ke = 0;
        if (!s.readString(&key, &kb, &ke)) return false;
        s.skipSpace();
        if (s.i < s.n && s.t[s.i] == '{') {
            if (!skipBlockFrom(s, s.i)) return false;
            continue;
        }
        std::string val;
        size_t vb = 0, ve = 0;
        if (!s.readString(&val, &vb, &ve)) return false;
        if (key == last) {
            out->begin = vb;
            out->end = ve;
            return true;
        }
    }
}

bool vdfSet(const std::string& text, const std::vector<std::string>& path,
            const std::string& value, std::string* out) {
    if (path.empty()) return false;

    VdfSpan span;
    if (vdfFind(text, path, &span)) {  // replace in place, bytes around kept
        *out = text.substr(0, span.begin) + escapeVdf(value) + text.substr(span.end);
        return true;
    }

    // The key is new.  Build the missing chain inside the deepest block of the
    // path that does exist.  If not even the first element exists there is no
    // root to hang it from, and inventing one for a file Steam owns is worse
    // than failing.
    for (size_t k = path.size() - 1; k > 0; --k) {
        std::vector<std::string> parent(path.begin(), path.begin() + k);
        Block b;
        if (!findBlock(text, parent, &b)) continue;

        // The new chain goes where the block's closing brace is, at the start
        // of that brace's own line, so the brace keeps its indentation and the
        // file keeps its shape.
        size_t at = b.body_end;
        while (at > 0 && text[at - 1] != '\n') --at;

        std::string body;
        std::string pad = b.indent;
        for (size_t d = k; d < path.size(); ++d) {
            pad += "\t";
            body += pad + "\"" + escapeVdf(path[d]) + "\"";
            if (d + 1 == path.size()) {
                body += "\t\t\"" + escapeVdf(value) + "\"\n";
            } else {
                body += "\n" + pad + "{\n";
            }
        }
        // Close the blocks opened above, innermost first, each aligned with the
        // key that opened it.
        std::vector<std::string> pads;
        std::string cpad = b.indent;
        for (size_t d = k; d + 1 < path.size(); ++d) {
            cpad += "\t";
            pads.push_back(cpad);
        }
        for (size_t i = pads.size(); i-- > 0;) body += pads[i] + "}\n";
        *out = text.substr(0, at) + body + text.substr(at);
        return true;
    }
    return false;
}

bool vdfWellFormed(const std::string& text) {
    int depth = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"') {
            ++i;
            while (i < text.size() && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < text.size()) ++i;
                ++i;
            }
            if (i >= text.size()) return false;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (--depth < 0) return false;
        }
    }
    return depth == 0;
}

// ------------------------------------------------------------ launch options

std::string steamOptionsClear(const std::string& existing,
                              const std::string& layer_dir) {
    const OptionScan sc = scanOptions(existing);
    std::vector<std::string> add = sc.add_layers;
    std::vector<std::string> layers = sc.layers;
    removeListEntry(&add, layer_dir);   // only our entry, not theirs
    removeListEntry(&layers, kLayerName);

    std::vector<std::string> out;
    if (sc.have_add && !add.empty())
        out.push_back(formatVar("VK_ADD_LAYER_PATH", joinList(add)));
    if (sc.have_layers && !layers.empty())
        out.push_back(formatVar("VK_INSTANCE_LAYERS", joinList(layers)));
    out.insert(out.end(), sc.kept.begin(), sc.kept.end());

    const std::string result = joinTokens(out);
    // What lsl leaves behind when there was nothing else: Steam reads a bare
    // %command% as no options at all, so hand back the empty string.
    if (result == "%command%") return {};
    return result;
}

std::string steamOptionsApply(const std::string& existing,
                              const std::string& layer_dir, double scale,
                              double sharpness, const std::string& method) {
    OptionScan sc = scanOptions(existing);
    removeListEntry(&sc.add_layers, layer_dir);
    sc.add_layers.insert(sc.add_layers.begin(), layer_dir);  // ours first
    removeListEntry(&sc.layers, kLayerName);
    sc.layers.insert(sc.layers.begin(), kLayerName);

    std::vector<std::string> out;
    out.push_back("LSL_LAYER_SCALE=" + formatNumber(scale));
    out.push_back("LSL_LAYER_SHARPNESS=" + formatNumber(sharpness));
    // An unrecognised method writes no variable at all rather than a name the
    // layer would only warn about: an unset LSL_LAYER_MODE is precisely the
    // default the layer already keeps.
    if (isModeName(method)) out.push_back("LSL_LAYER_MODE=" + method);
    out.push_back(formatVar("VK_ADD_LAYER_PATH", joinList(sc.add_layers)));
    out.push_back(formatVar("VK_INSTANCE_LAYERS", joinList(sc.layers)));
    if (!sc.have_command) {
        // Without a %command% Steam appends this whole string to the game's
        // command line, so the assignments above would arrive as arguments.
        // Say where the game itself goes.
        out.push_back("%command%");
    }
    out.insert(out.end(), sc.kept.begin(), sc.kept.end());
    return joinTokens(out);
}

bool steamOptionsParse(const std::string& existing, double* scale,
                       double* sharpness, std::string* method) {
    bool found = false;
    for (const std::string& tok : splitTokens(existing)) {
        std::string key, val;
        bool quoted = false;
        if (!splitAssign(tok, &key, &val, &quoted)) continue;
        if (key == "LSL_LAYER_SCALE") {
            *scale = std::strtod(val.c_str(), nullptr);
            found = true;
        } else if (key == "LSL_LAYER_SHARPNESS") {
            *sharpness = std::strtod(val.c_str(), nullptr);
        } else if (key == "LSL_LAYER_MODE") {
            // A typo in a hand-written string leaves the caller's value alone
            // rather than showing up in the GUI as a method that does not
            // exist.
            if (method && isModeName(val)) *method = val;
        }
    }
    return found;
}

// ---------------------------------------------------------------------- files

std::string steamReadLaunchOptions(const std::string& localconfig,
                                   const std::string& appid) {
    if (localconfig.empty() || appid.empty()) return {};
    const std::string text = readFile(localconfig);
    if (text.empty()) return {};
    VdfSpan span;
    if (!vdfFind(text,
                 {"UserLocalConfigStore", "Software", "Valve", "Steam", "apps",
                  appid, "LaunchOptions"},
                 &span))
        return {};
    return unescapeVdf(text.substr(span.begin, span.end - span.begin));
}

bool steamWriteLaunchOptions(const std::string& localconfig,
                             const std::string& appid, const std::string& options,
                             std::string* error) {
    if (localconfig.empty()) {
        if (error) *error = "no Steam user configuration found";
        return false;
    }
    if (appid.empty()) {
        if (error) *error = "no appid";
        return false;
    }
    const std::string text = readFile(localconfig);
    if (text.empty()) {
        if (error)
            *error = "cannot read " + localconfig +
                     " (has Steam ever run for this user?)";
        return false;
    }

    const std::vector<std::string> path = {"UserLocalConfigStore", "Software",
                                           "Valve", "Steam", "apps", appid,
                                           "LaunchOptions"};
    std::string next;
    if (!vdfSet(text, path, options, &next)) {
        if (error)
            *error = "could not find where to write LaunchOptions in " +
                     localconfig;
        return false;
    }
    if (!vdfWellFormed(next)) {
        if (error) *error = "refusing to write: the result would not be VDF";
        return false;
    }
    // Read the value back through the same parser the GUI uses, so a splice
    // that lost a brace or a quote cannot get as far as the disk.
    VdfSpan check;
    if (!vdfFind(next, path, &check) ||
        unescapeVdf(next.substr(check.begin, check.end - check.begin)) !=
            options) {
        if (error) *error = "refusing to write: the new value does not read back";
        return false;
    }

    // One backup, of the file as lsl first saw it.  Overwriting it on every
    // write would only ever hold the previous lsl state, which is worth less.
    std::error_code ec;
    const std::string backup = localconfig + ".lsl-backup";
    if (!fs::exists(backup, ec)) {
        std::ofstream out(backup);
        if (!out) {
            if (error) *error = "cannot write the backup " + backup;
            return false;
        }
        out << text;
    }

    const std::string tmp = localconfig + ".lsl-tmp";
    {
        std::ofstream out(tmp);
        if (!out) {
            if (error) *error = "cannot write " + tmp;
            return false;
        }
        out << next;
        if (!out) {
            if (error) *error = "short write to " + tmp;
            return false;
        }
    }
    fs::rename(tmp, localconfig, ec);
    if (ec) {
        if (error)
            *error = "cannot replace " + localconfig + ": " + ec.message();
        return false;
    }
    return true;
}

}  // namespace lsl
