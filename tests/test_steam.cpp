// Tests for the Steam configuration core the GUI is built on.
//
// This is the half of the GUI that can destroy something: it edits the files
// Steam owns, and a user's localconfig.vdf holds every game's launch options,
// not just the ones lsl touches.  So the tests here are mostly about *not*
// touching what lsl was not asked to change, and about refusing to write at all
// when the result would not parse.  None of it needs a display.

#include "steam.hpp"
#include "profile.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

void checkEq(const std::string& got, const std::string& want,
             const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("FAIL %s\n  got:  %s\n  want: %s\n", what.c_str(),
                    got.c_str(), want.c_str());
    }
}

// True when `hay` contains `needle`.
bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

void writeFile(const std::string& path, const std::string& text) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A trimmed-down localconfig.vdf with the same shape as the real one: tabs,
// two app blocks, one of which has LaunchOptions.
const char* kLocalConfig =
    "\"UserLocalConfigStore\"\n"
    "{\n"
    "\t\"Software\"\n"
    "\t{\n"
    "\t\t\"Valve\"\n"
    "\t\t{\n"
    "\t\t\t\"Steam\"\n"
    "\t\t\t{\n"
    "\t\t\t\t\"apps\"\n"
    "\t\t\t\t{\n"
    "\t\t\t\t\t\"730\"\n"
    "\t\t\t\t\t{\n"
    "\t\t\t\t\t\t\"LastPlayed\"\t\t\"1790590228\"\n"
    "\t\t\t\t\t\t\"LaunchOptions\"\t\t\"ENABLE_LSFG=1 DXVK_HUD=compiler %command%\"\n"
    "\t\t\t\t\t}\n"
    "\t\t\t\t\t\"440\"\n"
    "\t\t\t\t\t{\n"
    "\t\t\t\t\t\t\"LastPlayed\"\t\t\"1\"\n"
    "\t\t\t\t\t}\n"
    "\t\t\t\t}\n"
    "\t\t\t}\n"
    "\t\t}\n"
    "\t}\n"
    "}\n";

// The path whose value the tests write, and the path for an app Steam has
// never written a block for.
const std::vector<std::string> kLaunchPath = {
    "UserLocalConfigStore", "Software", "Valve", "Steam", "apps", "730",
    "LaunchOptions"};

std::vector<std::string> launchPath(const std::string& appid) {
    return {"UserLocalConfigStore", "Software", "Valve", "Steam", "apps", appid,
            "LaunchOptions"};
}

// ---------------------------------------------------------------- VDF basics

void testVdfFind() {
    const std::string t = kLocalConfig;
    lsl::VdfSpan span;

    check(lsl::vdfFind(kLocalConfig,
                       {"UserLocalConfigStore", "Software", "Valve", "Steam",
                        "apps", "730", "LaunchOptions"},
                       &span),
          "vdfFind: nested LaunchOptions");
    checkEq(t.substr(span.begin, span.end - span.begin),
            "ENABLE_LSFG=1 DXVK_HUD=compiler %command%",
            "vdfFind: value text");

    check(lsl::vdfFind(t, {"UserLocalConfigStore", "Software", "Valve", "Steam",
                           "apps", "440", "LastPlayed"},
                       &span),
          "vdfFind: second app block");
    checkEq(t.substr(span.begin, span.end - span.begin), "1",
            "vdfFind: second app's value");

    check(!lsl::vdfFind(t, {"UserLocalConfigStore", "Software", "Valve", "Steam",
                            "apps", "1465360", "LaunchOptions"},
                        &span),
          "vdfFind: a missing app is not found");
    check(!lsl::vdfFind(t, {"NoSuchRoot", "whatever"}, &span),
          "vdfFind: a missing root is not found");
    // A path element that names a block is not a value.
    check(!lsl::vdfFind(t, {"UserLocalConfigStore", "Software"}, &span),
          "vdfFind: a block is not a value");
}

void testVdfSetReplace() {
    std::string out;
    check(lsl::vdfSet(kLocalConfig, kLaunchPath, "LSL_LAYER_SCALE=2 %command%",
                      &out),
          "vdfSet: replace succeeds");

    // Only the value may change: build the expected file by swapping the value
    // out of the original and compare whole files.
    std::string want = kLocalConfig;
    const std::string old = "ENABLE_LSFG=1 DXVK_HUD=compiler %command%";
    want.replace(want.find(old), old.size(), "LSL_LAYER_SCALE=2 %command%");
    checkEq(out, want, "vdfSet: replacing touches nothing else");

    lsl::VdfSpan span;
    check(lsl::vdfFind(out, kLaunchPath, &span) &&
              out.substr(span.begin, span.end - span.begin) ==
                  "LSL_LAYER_SCALE=2 %command%",
          "vdfSet: the new value reads back");
    check(lsl::vdfWellFormed(out), "vdfSet: the result is still well formed");
}

void testVdfSetInsertIntoExistingBlock() {
    // App 440 exists but has no LaunchOptions: the key goes into its block and
    // its LastPlayed is left alone.
    const std::vector<std::string> path = launchPath("440");
    std::string out;
    check(lsl::vdfSet(kLocalConfig, path, "LSL_LAYER_SCALE=2 %command%", &out),
          "vdfSet: insert into an existing app block");

    lsl::VdfSpan span;
    check(lsl::vdfFind(out, path, &span), "vdfSet: the inserted value is found");
    checkEq(out.substr(span.begin, span.end - span.begin),
            "LSL_LAYER_SCALE=2 %command%", "vdfSet: the inserted value's text");
    check(lsl::vdfWellFormed(out), "vdfSet: the insert is well formed");
    check(has(out, "\"LastPlayed\"\t\t\"1\""),
          "vdfSet: the block's other keys survive");
    check(has(out, "\"LaunchOptions\"\t\t\"ENABLE_LSFG=1 DXVK_HUD=compiler "
                   "%command%\""),
          "vdfSet: app 730 is untouched");
    // 440's block has to be the one that grew: its LastPlayed must still parse
    // *and* the new key must sit before the brace that closes it.  Comparing
    // the two insert positions is enough to catch an insertion after the brace.
    const size_t ins = out.find("\"LaunchOptions\"\t\t\"LSL_LAYER_SCALE");
    const size_t block = out.find("\"440\"");
    const size_t next = out.find("\"730\"", block);
    check(block < ins, "vdfSet: insertion follows the app's own key");
    if (next != std::string::npos) check(ins < next, "vdfSet: insertion precedes the next app");
}

void testVdfSetCreatesChain() {
    // The app has never been played, so nothing but "apps" exists.
    const std::vector<std::string> path = launchPath("1465360");
    std::string out;
    check(lsl::vdfSet(kLocalConfig, path, "LSL_LAYER_SCALE=2 %command%", &out),
          "vdfSet: create an app block and the key");

    check(lsl::vdfWellFormed(out), "vdfSet: the created chain is well formed");
    lsl::VdfSpan span;
    check(lsl::vdfFind(out, path, &span) &&
              out.substr(span.begin, span.end - span.begin) ==
                  "LSL_LAYER_SCALE=2 %command%",
          "vdfSet: the created value reads back");
    check(has(out, "\"1465360\""), "vdfSet: the new app block was written");
    check(has(out, "\"730\""), "vdfSet: the existing blocks survive");
}

void testVdfSetRefusesWithoutRoot() {
    std::string out;
    check(!lsl::vdfSet("\"Other\"\n{\n}\n", kLaunchPath, "x", &out),
          "vdfSet: refuses to invent a missing root");
    check(out.empty(), "vdfSet: a refusal leaves the output untouched");
}

void testVdfWellFormed() {
    check(lsl::vdfWellFormed(kLocalConfig), "well formed: the fixture");
    check(!lsl::vdfWellFormed("\"A\"\n{\n"), "well formed: an unclosed brace");
    check(!lsl::vdfWellFormed("\"A\"\n{\n}\n}\n"), "well formed: an extra brace");
    check(!lsl::vdfWellFormed("\"A\" \"unterminated\n"),
          "well formed: an unterminated string");
    // A brace inside a string is not a brace.
    check(lsl::vdfWellFormed("\"A\"\t\"{\"\n"), "vdfWellFormed: brace in a value");
}

// ----------------------------------------------------------- launch options

void testOptionsApplyBasic() {
    checkEq(lsl::steamOptionsApply("", "/lsl/layer", 2.0, 0.0),
            "LSL_LAYER_SCALE=2 LSL_LAYER_SHARPNESS=0 "
            "VK_ADD_LAYER_PATH=/lsl/layer "
            "VK_INSTANCE_LAYERS=VK_LAYER_LSL_frame %command%",
            "options: written from nothing");

    const std::string once = lsl::steamOptionsApply("", "/lsl/layer", 2.0, 0.0);
    checkEq(lsl::steamOptionsApply(once, "/lsl/layer", 2.0, 0.0), once,
            "options: applying twice changes nothing");

    const std::string twice = lsl::steamOptionsApply(once, "/lsl/layer", 1.5, 0.25);
    check(has(twice, "LSL_LAYER_SCALE=1.5 "),
          "options: a new scale is written");
    check(!has(twice, "LSL_LAYER_SCALE=2 "),
          "options: the old scale is gone");
    check(has(twice, "%command%"), "options: %command% survives a re-apply");
    checkEq(lsl::steamOptionsClear(once, "/lsl/layer"), "", "options: clearing from nothing");
}

void testOptionsScaleFormatting() {
    const std::string a = lsl::steamOptionsApply("", "/d", 1.5, 0.25);
    check(has(a, "LSL_LAYER_SCALE=1.5 "), "options: 1.5 is written as 1.5");
    check(has(a, "LSL_LAYER_SHARPNESS=0.25 "), "options: 0.25 as 0.25");
    const std::string b = lsl::steamOptionsApply("", "/d", 3.0, 0.0);
    check(has(b, "LSL_LAYER_SCALE=3 "), "options: 3.0 is written as 3");
}

void testOptionsKeepUserTokens() {
    // Exactly the shape the real localconfig on this machine had.
    const std::string before = "ENABLE_LSFG=1 DXVK_HUD=compiler %command%";
    const std::string got = lsl::steamOptionsApply(before, "/lsl/layer", 2.0, 0.0);
    check(has(got, "ENABLE_LSFG=1 "), "options: the user's own environment stays");
    check(has(got, "DXVK_HUD=compiler "), "options: the user's own arguments stay");
    check(has(got, "VK_ADD_LAYER_PATH=/lsl/layer"),
          "options: the layer's path is added");
    check(got.find("VK_INSTANCE_LAYERS=") < got.find("%command%"),
          "options: assignments come before %command%");
    check(got.find("VK_INSTANCE_LAYERS=") < got.find("ENABLE_LSFG=1"),
          "options: and before the user's own environment");
    checkEq(lsl::steamOptionsClear(got, "/lsl/layer"), before,
            "options: clearing gives the user's string back");
}

void testOptionsMissingCommand() {
    // No %command%: Steam would append the whole string to the game's command
    // line, so the assignments would arrive as arguments instead of environment.
    const std::string got = lsl::steamOptionsApply("-dx11 -novid", "/d", 2.0, 0.0);
    check(has(got, "%command%"), "options: %command% is added when missing");
    check(got.find("VK_INSTANCE_LAYERS=") < got.find("%command%"),
          "options: the assignments precede it");
    check(got.find("%command%") < got.find("-dx11"),
          "options: the user's arguments stay after it");
    checkEq(lsl::steamOptionsClear(got, "/d"), "%command% -dx11 -novid",
            "options: clearing leaves the %command% lsl inserted in place");
}

void testOptionsMergeInstanceLayers() {
    const std::string before =
        "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation %command%";
    const std::string got = lsl::steamOptionsApply(before, "/d", 2.0, 0.0);
    check(has(got, "VK_INSTANCE_LAYERS=VK_LAYER_LSL_frame:"
                   "VK_LAYER_KHRONOS_validation"),
          "options: the user's own instance layers are kept, ours first");
    checkEq(lsl::steamOptionsClear(got, "/d"), before,
            "options: clearing removes only our entry");
}

void testOptionsMergeAddLayerPath() {
    const std::string before = "VK_ADD_LAYER_PATH=/other/layers %command%";
    const std::string got = lsl::steamOptionsApply(before, "/lsl/layer", 2.0, 0.0);
    check(has(got, "VK_ADD_LAYER_PATH=/lsl/layer:/other/layers"),
          "options: our path goes in front of the user's");
    checkEq(lsl::steamOptionsClear(got, "/lsl/layer"), before,
            "options: clearing restores the user's path");
}

void testOptionsQuotedValues() {
    // A value with a space has to come back quoted, in every component, or it
    // would split into two arguments.
    const std::string before = "VK_ADD_LAYER_PATH=\"/my layers\" %command%";
    const std::string got =
        lsl::steamOptionsApply(before, "/lsl layer/dir", 2.0, 0.0);
    check(has(got, "VK_ADD_LAYER_PATH=\"/lsl layer/dir:/my layers\""),
          "options: a value with spaces is quoted");
    checkEq(lsl::steamOptionsClear(got, "/lsl layer/dir"), before,
            "options: clearing a quoted value gives it back quoted");
}

void testOptionsParse() {
    const std::string s = lsl::steamOptionsApply("", "/d", 1.25, 0.5);
    double scale = 0, sharp = 0;
    check(lsl::steamOptionsParse(s, &scale, &sharp),
          "options: the settings are found again");
    check(scale > 1.24 && scale < 1.26, "options: the scale reads back");
    check(sharp > 0.49 && sharp < 0.51, "options: the sharpness reads back");
    check(!lsl::steamOptionsParse("DXVK_HUD=compiler %command%", &scale, &sharp),
          "options: absence is reported when lsl is not set");
}

// -------------------------------------------------------------- Steam files

// A whole simulated Steam installation: two libraries, two named games, one
// nameless manifest, two users with one marked MostRecent.
std::string makeSteamRoot() {
    const fs::path base = fs::temp_directory_path() / "lsl-steam-test";
    fs::remove_all(base);
    const std::string root = (base / "Steam").string();
    const std::string lib2 = (base / "Library2").string();

    writeFile(root + "/steamapps/appmanifest_730.acf",
              "\"AppState\"\n{\n\t\"appid\"\t\t\"730\"\n\t\"name\"\t\t\""
              "Counter-Strike 2\"\n\t\"installdir\"\t\t\"Counter-Strike Global "
              "Offensive\"\n}\n");
    writeFile(root + "/steamapps/appmanifest_999.acf",
              "\"AppState\"\n{\n\t\"appid\"\t\t\"999\"\n}\n");
    writeFile(lib2 + "/steamapps/appmanifest_570.acf",
              "\"AppState\"\n{\n\t\"appid\"\t\t\"570\"\n\t\"name\"\t\t\"Dota 2\"\n"
              "\t\"installdir\"\t\t\"dota 2 beta\"\n}\n");

    writeFile(root + "/steamapps/libraryfolders.vdf",
              "\"libraryfolders\"\n{\n"
              "\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + root +
                  "\"\n\t\t\"label\"\t\t\"\"\n\t}\n"
                  "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" +
                  lib2 + "\"\n\t}\n}\n");

    writeFile(root + "/config/loginusers.vdf",
              "\"users\"\n{\n"
              "\t\"76561198000000001\"\n\t{\n\t\t\"AccountName\"\t\t\"other\"\n"
              "\t\t\"MostRecent\"\t\t\"0\"\n\t}\n"
              "\t\"76561198389339717\"\n\t{\n\t\t\"AccountName\"\t\t\"gnana\"\n"
              "\t\t\"MostRecent\"\t\t\"1\"\n\t}\n}\n");
    writeFile(root + "/userdata/76561198000000001/config/localconfig.vdf",
              "\"UserLocalConfigStore\"\n{\n}\n");
    writeFile(root + "/userdata/76561198389339717/config/localconfig.vdf",
              kLocalConfig);

    return base.string();
}

void testSteamDiscovery() {
    const std::string base = makeSteamRoot();
    const std::string root = base + "/Steam";

    const std::vector<std::string> libs = lsl::steamLibraries(root);
    check(libs.size() == 2, "libraries: the root and the one in libraryfolders");
    check(!libs.empty() && libs[0] == root, "libraries: the root comes first");
    check(libs.size() == 2 && libs[1] == base + "/Library2",
          "libraries: the second library is found");

    const std::vector<lsl::SteamGame> games = lsl::steamGames(root);
    check(games.size() == 2, "games: the named ones, the nameless one skipped");
    if (games.size() == 2) {
        check(games[0].name == "Counter-Strike 2", "games: sorted by name");
        check(games[0].appid == "730", "games: the appid comes from the manifest");
        check(games[0].installdir == "Counter-Strike Global Offensive",
              "games: the install directory comes through");
        check(games[1].name == "Dota 2", "games: a game from the other library");
        check(games[1].appid == "570", "games: its appid");
        check(games[1].library == base + "/Library2",
              "games: and which library it came from");
    }

    checkEq(lsl::steamLocalConfig(root),
            root + "/userdata/76561198389339717/config/localconfig.vdf",
            "localconfig: the user marked MostRecent wins");

    fs::remove_all(base);
}

void testSteamWrites() {
    const std::string base = makeSteamRoot();
    const std::string root = base + "/Steam";
    const std::string config = lsl::steamLocalConfig(root);
    check(!config.empty(), "write: there is a localconfig to write to");

    checkEq(lsl::steamReadLaunchOptions(config, "730"),
            "ENABLE_LSFG=1 DXVK_HUD=compiler %command%",
            "read: an existing launch options string");

    const std::string before = readFile(config);
    const std::string options = lsl::steamOptionsApply(
        lsl::steamReadLaunchOptions(config, "730"), "/lsl/layer", 2.0, 0.0);
    std::string error;
    check(lsl::steamWriteLaunchOptions(config, "730", options, &error),
          "write: succeeds (" + error + ")");
    checkEq(lsl::steamReadLaunchOptions(config, "730"), options,
            "write: the value reads back through the parser");

    // Only one line of the file may differ.  Anything else means the splice
    // disturbed Steam's own data.
    const std::string after = readFile(config);
    size_t diff_lines = 0;
    size_t bi = 0, ai = 0;
    while (bi < before.size() || ai < after.size()) {
        const size_t be = before.find('\n', bi);
        const size_t ae = after.find('\n', ai);
        if (before.substr(bi, be - bi) != after.substr(ai, ae - ai))
            ++diff_lines;
        if (be == std::string::npos && ae == std::string::npos) break;
        bi = be + 1;
        ai = ae + 1;
    }
    check(diff_lines == 1, "write: exactly one line of the file changed");

    check(fs::exists(config + ".lsl-backup"), "write: a backup was kept");
    checkEq(readFile(config + ".lsl-backup"), kLocalConfig,
            "write: the backup holds the file as it was");

    // A game Steam has never written an app block for.
    std::string error2;
    check(lsl::steamWriteLaunchOptions(
              config, "1465360",
              lsl::steamOptionsApply("", "/lsl/layer", 2.0, 0.0), &error2),
          "write: creates a block for a game Steam has not seen (" + error2 + ")");
    check(has(lsl::steamReadLaunchOptions(config, "1465360"),
              "VK_LAYER_LSL_frame"),
          "write: the new app's value reads back");
    lsl::VdfSpan created;
    check(lsl::vdfFind(readFile(config), launchPath("1465360"), &created),
          "write: the new app's key is reachable by path");

    // And clearing gives the user's options back.
    check(lsl::steamWriteLaunchOptions(
              config, "730",
              lsl::steamOptionsClear(
                  lsl::steamReadLaunchOptions(config, "730"), "/lsl/layer"),
              &error2),
          "write: clearing succeeds (" + error2 + ")");
    checkEq(lsl::steamReadLaunchOptions(config, "730"),
            "ENABLE_LSFG=1 DXVK_HUD=compiler %command%",
            "write: clearing restores the original options");
    (void)error2;

    fs::remove_all(base);
}

// ---------------------------------------------------------------- profiles

void testProfileStore() {
    const fs::path dir = fs::temp_directory_path() / "lsl-profile-test";
    fs::remove_all(dir);
    const std::string path = (dir / "profiles.conf").string();

    const lsl::ProfileStore fresh = lsl::ProfileStore::load(path);
    check(fresh.games.empty(), "profiles: a missing file loads empty");
    check(fresh.get("730").scale == 2.0, "profiles: the default scale is 2");
    check(!fresh.get("730").enabled, "profiles: a game starts disabled");

    lsl::ProfileStore store;
    store.layer_dir = "/home/me/My Projects/lsl/build/layer";
    store.set("730", {true, 1.5, 0.25});
    store.set("1465360", {false, 2.0, 0.0});
    std::string error;
    check(store.save(path, &error), "profiles: save (" + error + ")");

    const lsl::ProfileStore back = lsl::ProfileStore::load(path);
    checkEq(back.layer_dir, store.layer_dir,
            "profiles: a layer directory with spaces round-trips");
    check(back.get("730").enabled, "profiles: enabled round-trips");
    check(back.get("730").scale == 1.5, "profiles: scale round-trips");
    check(back.get("730").sharpness == 0.25, "profiles: sharpness round-trips");
    check(back.games.size() == 2, "profiles: both games round-trip");
    check(!back.get("999").enabled && back.get("999").scale == 2.0,
          "profiles: an unconfigured game gets the defaults");

    // Unknown keys and comments are skipped rather than fatal, so a file from a
    // newer GUI still loads.
    writeFile(path,
              "# a comment\n"
              "future_key 1 2 3\n"
              "layer_dir /x\n"
              "game 1 1 3 0.5\n");
    const lsl::ProfileStore tolerant = lsl::ProfileStore::load(path);
    checkEq(tolerant.layer_dir, "/x", "profiles: unknown keys are skipped");
    check(tolerant.get("1").scale == 3.0, "profiles: the rest still loads");

    fs::remove_all(dir);
}

void testLooksLikeTool() {
    // The names Steam actually installs.
    check(lsl::steamLooksLikeTool("Proton Experimental"), "tools: proton experimental");
    check(lsl::steamLooksLikeTool("Proton Hotfix"), "tools: proton hotfix");
    check(lsl::steamLooksLikeTool("Proton 11.0"), "tools: proton versioned");
    check(lsl::steamLooksLikeTool("Proton - Experimental"), "tools: proton dashes");
    check(lsl::steamLooksLikeTool("Steam Linux Runtime 4.0"), "tools: runtime");
    check(lsl::steamLooksLikeTool("Steam Linux Runtime 1.0 (scout)"),
          "tools: runtime scout");
    check(lsl::steamLooksLikeTool("Steamworks Common Redistributables"),
          "tools: redistributables");

    // Games.  "Proton Pulse" is the case a prefix test alone would get wrong.
    check(!lsl::steamLooksLikeTool("Proton Pulse"), "tools: a game named proton");
    check(!lsl::steamLooksLikeTool("SnowRunner"), "tools: an ordinary game");
    check(!lsl::steamLooksLikeTool("Lossless Scaling"), "tools: this project's twin");
    check(!lsl::steamLooksLikeTool("SteamVR Game"), "tools: a game near the name");
    check(!lsl::steamLooksLikeTool(""), "tools: an empty name");
}

}  // namespace

int main() {
    testVdfFind();
    testVdfSetInsertIntoExistingBlock();
    testVdfSetCreatesChain();
    testVdfSetRefusesWithoutRoot();
    testVdfWellFormed();
    testOptionsApplyBasic();
    testOptionsScaleFormatting();
    testOptionsKeepUserTokens();
    testOptionsMissingCommand();
    testOptionsMergeInstanceLayers();
    testOptionsMergeAddLayerPath();
    testOptionsQuotedValues();
    testOptionsParse();
    testSteamDiscovery();
    testSteamWrites();
    testProfileStore();
    testLooksLikeTool();

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
