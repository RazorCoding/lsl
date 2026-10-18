// lsl's GUI: pick a game, pick a scale, write the launch options, launch.
//
// The window is deliberately thin, because the interface to the layer already
// exists: the layer's directory and LSL_LAYER_SCALE, in the game's launch
// options string.  All this does is remember a profile and splice that string
// into Steam's own configuration file.  Everything that decides *what* to write
// lives in src/steam.cpp and is tested without a display.
//
// Two things are worth knowing while reading this:
//
//   * Steam owns localconfig.vdf and rewrites it when it exits, so a write made
//     while Steam is running is likely to be lost.  The window says so rather
//     than letting the user discover it later.
//   * The launch options string is shown in full before it is written, and the
//     box under "In Steam right now" is re-read from the file after every
//     write.  The point of a layer is that the game is untouched; the same
//     should hold for the user's configuration, so nothing is hidden.

#include "profile.hpp"
#include "method.hpp"
#include "steam.hpp"

#include <gtkmm.h>
#include <gtkmm/init.h>
// For gtk_init_check(): gtkmm 4 has no Gtk::init(), and gtk_init() exits the
// process when it cannot open a display, which is no good for a test that
// should report "skipped" instead.
#include <gtk/gtk.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

const char* kAppId = "org.lsl.gui";

// A row in the game list.  It carries its index into the game vector so that
// selection keeps working while the list is filtered.
class GameRow : public Gtk::ListBoxRow {
public:
    GameRow(size_t index, const std::string& name, const std::string& appid)
        : index_(index), needle_(Glib::ustring(name).lowercase()) {
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        box->set_margin(6);
        box->set_margin_start(8);
        box->set_margin_end(8);

        auto* title = Gtk::make_managed<Gtk::Label>();
        title->set_xalign(0.0f);
        title->set_ellipsize(Pango::EllipsizeMode::END);
        title->set_text(name);

        auto* sub = Gtk::make_managed<Gtk::Label>(appid);
        sub->set_xalign(0.0f);
        sub->add_css_class("dim-label");

        box->append(*title);
        box->append(*sub);
        set_child(*box);
    }

    size_t index() const { return index_; }

    bool matches(const Glib::ustring& needle) const {
        return needle.empty() || needle_.find(needle) != Glib::ustring::npos;
    }

private:
    size_t index_;
    Glib::ustring needle_;
};

// Is a Steam client running?  Steam rewrites localconfig.vdf when it exits, so
// anything written now is likely to be undone by that.
bool steamRunning() {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/proc", ec)) {
        const std::string pid = entry.path().filename().string();
        if (pid.empty() ||
            pid.find_first_not_of("0123456789") != std::string::npos)
            continue;
        std::ifstream comm(entry.path() / "comm");
        std::string line;
        if (comm && std::getline(comm, line) && line == "steam") return true;
    }
    return false;
}

std::string executableDir() {
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    return self.parent_path().string();
}

// Where VkLayer_lsl_frame.json might be: $LSL_LAYER_DIR first, then the build
// tree beside this executable, then the unpackaged tree it was built from, then
// the place tools/install-layer.sh installs to.  The installed locations come
// last so a development tree keeps winning, but they mean the window still
// finds a real layer after the build tree is gone.
std::string guessLayerDir() {
    if (const char* env = std::getenv("LSL_LAYER_DIR"); env && *env) return env;
    const std::string here = executableDir();
    std::vector<std::string> candidates;
    if (!here.empty()) {
        candidates.push_back(here + "/layer");
        candidates.push_back(here + "/../build/layer");
        candidates.push_back(here + "/../layer");
    }
    if (const char* home = std::getenv("HOME"); home && *home)
        candidates.push_back(std::string(home) + "/.local/lib/lsl/layer");
    candidates.push_back("/usr/local/lib/lsl/layer");
    candidates.push_back("/usr/lib/lsl/layer");

    for (const std::string& dir : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(dir + "/VkLayer_lsl_frame.json", ec)) {
            const fs::path canonical = fs::weakly_canonical(dir, ec);
            return ec ? dir : canonical.string();
        }
    }
    return here.empty() ? std::string() : here + "/layer";
}

// Where the loader looks on its own, in the order it looks, for the layer's
// implicit manifest.  A file there means the loader loads the layer into every
// Vulkan process -- no launch options, no Steam restart -- and the layer decides
// from SteamAppId whether this process is one the profile store enabled.  Empty
// when there is no such manifest, which is the explicit, launch-options world
// the window was built for.
std::string implicitManifestPath() {
    std::vector<std::string> dirs;
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        dirs.push_back(std::string(xdg) + "/vulkan/implicit_layer.d");
    if (const char* home = std::getenv("HOME"); home && *home)
        dirs.push_back(std::string(home) +
                        "/.local/share/vulkan/implicit_layer.d");
    dirs.push_back("/usr/local/share/vulkan/implicit_layer.d");
    dirs.push_back("/usr/share/vulkan/implicit_layer.d");
    dirs.push_back("/etc/vulkan/implicit_layer.d");

    for (const std::string& dir : dirs) {
        const std::string file = dir + "/VkLayer_lsl_frame.json";
        std::error_code ec;
        if (fs::is_regular_file(file, ec)) return file;
    }
    return {};
}

// The method dropdown's contents: "auto" first, then every name in
// src/method.hpp's table.  Built from the table rather than written out, so a
// method added there appears in the window too -- and the strings are the same
// ones LSL_LAYER_MODE accepts, because they come from the same place.
std::vector<std::string> methodNames() {
    std::vector<std::string> v{"auto"};
    for (unsigned m = 0; m < lsl::MODE_COUNT; ++m) v.push_back(lsl::modeName(m));
    return v;
}

std::vector<Glib::ustring> methodLabels() {
    std::vector<Glib::ustring> v{"Auto"};
    for (unsigned m = 0; m < lsl::MODE_COUNT; ++m) v.push_back(lsl::modeLabel(m));
    return v;
}

class Window : public Gtk::ApplicationWindow {
public:
    Window();

    // For --selftest-window, after the launch-options checks have run: the
    // window has been built on that data, so check what it built.
    int selfCheck();

private:
    // construction and loading
    void buildWidgets();
    void loadSteam();
    void fillGameList();

    // reactions
    void onRowSelected(Gtk::ListBoxRow* row);
    void onSettingsChanged();
    void onLayerDirChanged();
    void onApplyClicked();
    void onLaunchClicked();
    void onCheckSetupClicked();
    void onViewLogClicked();

    // display
    void selectIndex(size_t index);
    void refreshDetails();
    void refreshPreview();
    void refreshBanner();
    void refreshLayerNote();
    void updateEnableTooltip();
    void setStatus(const std::string& message, bool error);
    void addRow(Gtk::Box& parent, const char* text, Gtk::Widget& widget);

    // state
    const lsl::SteamGame* current() const;
    lsl::Profile widgetProfile() const;
    std::string selectedMethod() const;
    void setSelectedMethod(const std::string& name);
    void saveProfile();
    bool layerDirLooksRight() const;
    bool layerIsImplicit() const;

    Gtk::SearchEntry* search_ = nullptr;
    Gtk::Switch* hideTools_ = nullptr;
    Gtk::ListBox* list_ = nullptr;
    Gtk::Label* banner_ = nullptr;
    Gtk::Label* name_ = nullptr;
    Gtk::Label* meta_ = nullptr;
    Gtk::Switch* enable_ = nullptr;
    Gtk::SpinButton* scale_ = nullptr;
    Gtk::SpinButton* sharp_ = nullptr;
    Gtk::DropDown* method_ = nullptr;
    Gtk::Entry* currentEntry_ = nullptr;
    Gtk::Entry* previewEntry_ = nullptr;
    Gtk::Entry* layerEntry_ = nullptr;
    Gtk::Label* layerNote_ = nullptr;
    Gtk::Label* configNote_ = nullptr;
    Gtk::Button* apply_ = nullptr;
    Gtk::Button* launch_ = nullptr;
    Gtk::Button* checkSetup_ = nullptr;
    Gtk::Button* viewLog_ = nullptr;
    Gtk::Label* status_ = nullptr;

    std::vector<lsl::SteamGame> games_;
    lsl::ProfileStore store_;
    // The dropdown's rows, in the order methodLabels() produced them, so the
    // widget and the strings the launch options take cannot disagree.
    std::vector<std::string> methodNames_;
    std::string steamRoot_;
    std::string configPath_;
    std::string profilePath_;
    // The loader's own manifest for the layer, if one is installed, so the
    // window can tell the user that turning a game on here is all it takes.
    std::string implicitManifest_;
    size_t selected_ = 0;
    bool hasSelection_ = false;
    // Set while widgets are being filled programmatically, so that the change
    // signals they emit do not look like the user editing them.
    bool loading_ = false;
};

Window::Window() {
    buildWidgets();
    loadSteam();
}

// Each detail row is a label on the left and its control on the right.
void Window::addRow(Gtk::Box& parent, const char* text, Gtk::Widget& widget) {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    auto* label = Gtk::make_managed<Gtk::Label>(text);
    label->set_xalign(0.0f);
    label->set_hexpand(true);
    label->set_valign(Gtk::Align::CENTER);
    widget.set_valign(Gtk::Align::CENTER);
    row->append(*label);
    row->append(widget);
    parent.append(*row);
}

void Window::buildWidgets() {
    set_title("lsl");
    set_default_size(1000, 660);

    auto css = Gtk::CssProvider::create();
    css->load_from_string(".lsl-error { color: #c01c28; }\n");
    if (auto display = Gdk::Display::get_default()) {
        Gtk::StyleProvider::add_provider_for_display(
            display, css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    auto* header = Gtk::make_managed<Gtk::HeaderBar>();
    auto* title = Gtk::make_managed<Gtk::Label>("lsl");
    title->add_css_class("title");
    header->set_title_widget(*title);
    auto* refresh = Gtk::make_managed<Gtk::Button>("Refresh");
    refresh->set_tooltip_text(
        "Re-read Steam's game list and launch options, in case they changed "
        "outside this window");
    refresh->signal_clicked().connect([this] { loadSteam(); });
    header->pack_start(*refresh);
    set_titlebar(*header);

    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    set_child(*root);

    banner_ = Gtk::make_managed<Gtk::Label>();
    banner_->set_wrap(true);
    banner_->set_xalign(0.0f);
    banner_->set_margin(8);
    banner_->set_visible(false);
    root->append(*banner_);

    auto* paned = Gtk::make_managed<Gtk::Paned>(Gtk::Orientation::HORIZONTAL);
    paned->set_position(300);
    paned->set_vexpand(true);
    root->append(*paned);

    // ---------------------------------------------------------- the game list
    auto* left = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    search_ = Gtk::make_managed<Gtk::SearchEntry>();
    search_->set_placeholder_text("Search games");
    search_->set_margin(6);

    list_ = Gtk::make_managed<Gtk::ListBox>();
    list_->set_selection_mode(Gtk::SelectionMode::SINGLE);
    list_->set_filter_func([this](Gtk::ListBoxRow* row) {
        auto* game = dynamic_cast<GameRow*>(row);
        if (!game) return true;
        if (hideTools_->get_active() &&
            lsl::steamLooksLikeTool(games_[game->index()].name))
            return false;
        return game->matches(search_->get_text().lowercase());
    });
    search_->signal_search_changed().connect([this] { list_->invalidate_filter(); });

    auto* filterRow =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    filterRow->set_margin_start(8);
    filterRow->set_margin_end(8);
    filterRow->set_margin_bottom(6);
    hideTools_ = Gtk::make_managed<Gtk::Switch>();
    hideTools_->set_active(true);
    hideTools_->set_valign(Gtk::Align::CENTER);
    hideTools_->set_tooltip_text(
        "Hide Proton, the Steam Linux Runtimes and the redistributables, which "
        "are installed like games but cannot be played.  Nothing in Steam's "
        "files records an app's type, so this goes by Valve's names; switch it "
        "off if it hides something it should not.");
    hideTools_->property_active().signal_changed().connect(
        [this] { list_->invalidate_filter(); });
    auto* filterLabel = Gtk::make_managed<Gtk::Label>("Hide Valve's tools");
    filterLabel->set_xalign(0.0f);
    filterLabel->set_hexpand(true);
    filterRow->append(*filterLabel);
    filterRow->append(*hideTools_);
    list_->signal_row_selected().connect(
        [this](Gtk::ListBoxRow* row) { onRowSelected(row); });

    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroller->set_child(*list_);
    scroller->set_vexpand(true);

    left->append(*search_);
    left->append(*filterRow);
    left->append(*scroller);

    auto* panedLeft = Gtk::make_managed<Gtk::ScrolledWindow>();
    panedLeft->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::NEVER);
    panedLeft->set_child(*left);
    paned->set_start_child(*panedLeft);

    // The details, in a scroller so a small window does not clip them.
    auto* details = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    details->set_margin(16);

    name_ = Gtk::make_managed<Gtk::Label>();
    name_->set_xalign(0.0f);
    name_->set_wrap(true);
    name_->add_css_class("title-2");
    meta_ = Gtk::make_managed<Gtk::Label>();
    meta_->set_xalign(0.0f);
    meta_->set_wrap(true);
    meta_->add_css_class("dim-label");
    details->append(*name_);
    details->append(*meta_);
    details->append(*Gtk::make_managed<Gtk::Separator>(
        Gtk::Orientation::HORIZONTAL));

    enable_ = Gtk::make_managed<Gtk::Switch>();
    enable_->set_tooltip_text(
        "Whether the profile is enabled for this game.  Nothing reaches the "
        "game until this is written to the launch options with Apply.");
    enable_->property_active().signal_changed().connect(
        [this] { onSettingsChanged(); });
    addRow(*details, "Enable lsl for this game", *enable_);

    scale_ = Gtk::make_managed<Gtk::SpinButton>(
        Gtk::Adjustment::create(2.0, 1.0, 4.0, 0.25, 1.0), 0.25, 2);
    scale_->set_tooltip_text(
        "The game renders at 1/scale of the window, and the layer upscales it. "
        "2 means half resolution in each axis.");
    scale_->signal_value_changed().connect([this] { onSettingsChanged(); });
    addRow(*details, "Scale factor", *scale_);

    sharp_ = Gtk::make_managed<Gtk::SpinButton>(
        Gtk::Adjustment::create(0.0, -2.0, 4.0, 0.1, 0.5), 0.1, 2);
    sharp_->set_tooltip_text(
        "FSR1's RCAS strength, in stops: 0 is the strongest lobe AMD's RCAS "
        "applies, and each whole stop halves it, so larger values sharpen less "
        "and 4 is close to leaving the image alone.  Negative values sharpen "
        "harder than AMD's default.  Only the FSR1 + RCAS method reads it; the "
        "others have fixed tuning.");
    sharp_->signal_value_changed().connect([this] { onSettingsChanged(); });
    addRow(*details, "Sharpness", *sharp_);

    methodNames_ = methodNames();
    auto methodModel = Gtk::StringList::create(methodLabels());
    method_ = Gtk::make_managed<Gtk::DropDown>(methodModel);
    method_->set_hexpand(true);
    method_->set_tooltip_text(
        "Which filter the layer runs.  Auto picks one from the render size: AI "
        "when the window is small enough for a 4-layer reconstruction to fit a "
        "frame, NIS when the game renders well below the window, and "
        "FSR1 + RCAS otherwise.  LSL_LAYER_MODE in the launch options.");
    method_->property_selected().signal_changed().connect(
        [this] { onSettingsChanged(); });
    addRow(*details, "Upscaling method", *method_);

    details->append(*Gtk::make_managed<Gtk::Separator>(
        Gtk::Orientation::HORIZONTAL));

    auto* nowLabel = Gtk::make_managed<Gtk::Label>(
        "In Steam right now (read back from localconfig.vdf)");
    nowLabel->set_xalign(0.0f);
    nowLabel->add_css_class("dim-label");
    currentEntry_ = Gtk::make_managed<Gtk::Entry>();
    currentEntry_->set_editable(false);
    currentEntry_->set_can_focus(true);
    currentEntry_->set_hexpand(true);
    details->append(*nowLabel);
    details->append(*currentEntry_);

    auto* nextLabel =
        Gtk::make_managed<Gtk::Label>("What Apply will write");
    nextLabel->set_xalign(0.0f);
    nextLabel->add_css_class("dim-label");
    previewEntry_ = Gtk::make_managed<Gtk::Entry>();
    previewEntry_->set_editable(false);
    previewEntry_->set_can_focus(true);
    previewEntry_->set_hexpand(true);
    details->append(*nextLabel);
    details->append(*previewEntry_);

    auto* buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    apply_ = Gtk::make_managed<Gtk::Button>("Apply to Steam");
    apply_->set_hexpand(true);
    apply_->signal_clicked().connect([this] { onApplyClicked(); });
    launch_ = Gtk::make_managed<Gtk::Button>("Launch game");
    launch_->set_tooltip_text("Ask Steam to start this game (steam -applaunch).");
    launch_->signal_clicked().connect([this] { onLaunchClicked(); });
    checkSetup_ = Gtk::make_managed<Gtk::Button>("Check setup");
    checkSetup_->set_tooltip_text(
        "Diagnose the install: is the layer where the loader can find it, "
        "is the profile saved, is Steam running?");
    checkSetup_->signal_clicked().connect([this] { onCheckSetupClicked(); });
    viewLog_ = Gtk::make_managed<Gtk::Button>("View log");
    viewLog_->set_tooltip_text(
        "Open the layer's log file (set LSL_LAYER_LOG to enable logging).");
    viewLog_->signal_clicked().connect([this] { onViewLogClicked(); });
    buttons->append(*apply_);
    buttons->append(*launch_);
    buttons->append(*checkSetup_);
    buttons->append(*viewLog_);
    details->append(*buttons);

    details->append(*Gtk::make_managed<Gtk::Separator>(
        Gtk::Orientation::HORIZONTAL));

    auto* layerLabel = Gtk::make_managed<Gtk::Label>(
        "Layer directory (the one holding VkLayer_lsl_frame.json)");
    layerLabel->set_xalign(0.0f);
    layerLabel->add_css_class("dim-label");
    layerEntry_ = Gtk::make_managed<Gtk::Entry>();
    layerEntry_->set_hexpand(true);
    layerEntry_->signal_changed().connect([this] { onLayerDirChanged(); });
    layerNote_ = Gtk::make_managed<Gtk::Label>();
    layerNote_->set_xalign(0.0f);
    layerNote_->set_wrap(true);
    configNote_ = Gtk::make_managed<Gtk::Label>();
    configNote_->set_xalign(0.0f);
    configNote_->set_wrap(true);
    configNote_->add_css_class("dim-label");
    details->append(*layerLabel);
    details->append(*layerEntry_);
    details->append(*layerNote_);
    details->append(*configNote_);

    status_ = Gtk::make_managed<Gtk::Label>();
    status_->set_xalign(0.0f);
    status_->set_wrap(true);
    status_->set_margin_top(6);
    details->append(*status_);

    auto* detailsScroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    detailsScroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    detailsScroll->set_child(*details);
    paned->set_end_child(*detailsScroll);
}

void Window::loadSteam() {
    steamRoot_ = lsl::steamRoot();
    configPath_ = lsl::steamLocalConfig(steamRoot_);
    profilePath_ = lsl::defaultProfilePath();
    store_ = lsl::ProfileStore::load(profilePath_);
    if (store_.layer_dir.empty()) store_.layer_dir = guessLayerDir();
    implicitManifest_ = implicitManifestPath();
    games_ = lsl::steamGames(steamRoot_);

    loading_ = true;
    layerEntry_->set_text(store_.layer_dir);
    loading_ = false;

    fillGameList();
    refreshLayerNote();
    refreshBanner();

    if (games_.empty()) {
        hasSelection_ = false;
        refreshDetails();
        return;
    }
    if (selected_ >= games_.size()) selected_ = 0;
    // Skip over anything the filter is hiding, so the window opens on a game
    // rather than on a blank detail pane.
    while (selected_ < games_.size() && hideTools_->get_active() &&
           lsl::steamLooksLikeTool(games_[selected_].name))
        ++selected_;
    if (selected_ >= games_.size()) selected_ = 0;
    selectIndex(selected_);
}

void Window::fillGameList() {
    while (Gtk::ListBoxRow* row = list_->get_row_at_index(0)) list_->remove(*row);
    for (size_t i = 0; i < games_.size(); ++i) {
        list_->append(*Gtk::make_managed<GameRow>(i, games_[i].name,
                                                  games_[i].appid));
    }
}

void Window::onRowSelected(Gtk::ListBoxRow* row) {
    if (auto* game = dynamic_cast<GameRow*>(row)) selectIndex(game->index());
}

void Window::selectIndex(size_t index) {
    if (index >= games_.size()) return;
    selected_ = index;
    hasSelection_ = true;
    refreshDetails();
}

const lsl::SteamGame* Window::current() const {
    if (!hasSelection_ || selected_ >= games_.size()) return nullptr;
    return &games_[selected_];
}

lsl::Profile Window::widgetProfile() const {
    lsl::Profile profile;
    profile.enabled = enable_->get_active();
    profile.scale = scale_->get_value();
    profile.sharpness = sharp_->get_value();
    profile.method = selectedMethod();
    return profile;
}

// GTK_INVALID_LIST_POSITION (and an empty model) mean "nothing selected",
// which the launch options read as a method the layer gets to pick.
std::string Window::selectedMethod() const {
    const guint i = method_->get_selected();
    if (i == GTK_INVALID_LIST_POSITION || i >= methodNames_.size()) return "auto";
    return methodNames_[i];
}

void Window::setSelectedMethod(const std::string& name) {
    for (size_t i = 0; i < methodNames_.size(); ++i) {
        if (methodNames_[i] == name) {
            method_->set_selected(static_cast<guint>(i));
            return;
        }
    }
    method_->set_selected(0);  // "auto"
}

void Window::refreshDetails() {
    const lsl::SteamGame* game = current();
    const bool have = game != nullptr;

    enable_->set_sensitive(have);
    scale_->set_sensitive(have);
    method_->set_sensitive(have);
    apply_->set_sensitive(have);
    launch_->set_sensitive(have);
    currentEntry_->set_sensitive(have);
    previewEntry_->set_sensitive(have);

    if (!have) {
        name_->set_text("No game selected");
        meta_->set_text(games_.empty()
                            ? "No game manifests were found.  Install a game, or "
                              "point lsl at another Steam library."
                            : "Choose a game on the left.");
        loading_ = true;
        setSelectedMethod("auto");
        loading_ = false;
        sharp_->set_sensitive(false);
        currentEntry_->set_text("");
        previewEntry_->set_text("");
        apply_->set_label("Apply to Steam");
        return;
    }

    name_->set_text(game->name);

    std::string meta = "App " + game->appid;
    if (!game->library.empty()) meta += " in " + game->library;
    double currentScale = 0.0, currentSharpness = 0.0;
    std::string currentMethod;
    if (lsl::steamOptionsParse(lsl::steamReadLaunchOptions(configPath_, game->appid),
                               &currentScale, &currentSharpness, &currentMethod)) {
        meta += "  ·  lsl is already in this game's launch options";
    }
    meta_->set_text(meta);

    const lsl::Profile profile = store_.get(game->appid);
    loading_ = true;
    enable_->set_active(profile.enabled);
    scale_->set_value(profile.scale);
    sharp_->set_value(profile.sharpness);
    setSelectedMethod(profile.method);
    loading_ = false;

    // Sharpness is RCAS's alone; the other methods ignore it, so an editable
    // control would say otherwise.
    sharp_->set_sensitive(have && selectedMethod() == "fsr1rcas");

    // Read the file rather than remembering what was written: what matters is
    // what Steam will do, not what lsl intended.
    currentEntry_->set_text(lsl::steamReadLaunchOptions(configPath_, game->appid));
    refreshPreview();
}

void Window::refreshPreview() {
    const lsl::SteamGame* game = current();
    if (!game) {
        previewEntry_->set_text("");
        apply_->set_label("Apply to Steam");
        return;
    }

    const std::string existing =
        lsl::steamReadLaunchOptions(configPath_, game->appid);
    const lsl::Profile profile = widgetProfile();
    const std::string result =
        profile.enabled
            ? lsl::steamOptionsApply(existing, store_.layer_dir, profile.scale,
                                     profile.sharpness, profile.method)
            : lsl::steamOptionsClear(existing, store_.layer_dir);

    previewEntry_->set_text(result);
    apply_->set_label(profile.enabled ? "Apply to Steam"
                                      : "Remove lsl from Steam");
}

void Window::refreshBanner() {
    std::string text;
    bool error = false;

    if (steamRoot_.empty()) {
        text = "Steam was not found.  Set LSL_STEAM_ROOT to its directory if it "
               "lives somewhere unusual.";
        error = true;
    } else if (games_.empty()) {
        text = "No installed games were found under " + steamRoot_ + ".";
    }

    if (steamRunning()) {
        if (!text.empty()) text += "  ";
        // The two ways to enable a game write different things: the implicit
        // layer needs nothing from Steam at all, while Apply rewrites Steam's
        // own file, which a running Steam will overwrite on exit.
        text += layerIsImplicit()
                    ? "Steam is running.  Turning a game on here takes effect "
                      "the next time Steam starts it, with no restart.  Only "
                      "Apply touches Steam's files, and Steam may undo that "
                      "when it exits."
                    : "Steam is running.  It rewrites its configuration files "
                      "when it exits, so anything written here may be lost — "
                      "restart Steam after applying.";
    }

    banner_->set_text(text);
    banner_->set_visible(!text.empty());
    if (error) {
        banner_->add_css_class("lsl-error");
    } else {
        banner_->remove_css_class("lsl-error");
    }
}

void Window::refreshLayerNote() {
    std::string note;
    bool error = false;

    if (layerIsImplicit()) {
        // The layer loads into every Vulkan process; what decides whether a
        // given one is upscaled is the profile store, not a launch line.
        note = "The layer is installed for every Vulkan program (" +
               implicitManifest_ +
               ").  Turn a game on here and press Play: the layer reads "
               "SteamAppId and upscales it.  No launch options needed.";
    } else if (layerDirLooksRight()) {
        note = "The layer is installed at " + store_.layer_dir +
               ", but nothing loads it automatically.  Apply writes it into "
               "the game's Steam launch options; re-run tools/install-layer.sh "
               "with --implicit to skip that.";
    } else {
        note = "The layer directory has no VkLayer_lsl_frame.json, so nothing "
               "will be upscaled.";
        error = true;
    }

    layerNote_->set_text(note);
    if (error) {
        layerNote_->remove_css_class("dim-label");
        layerNote_->add_css_class("lsl-error");
    } else {
        layerNote_->remove_css_class("lsl-error");
        layerNote_->add_css_class("dim-label");
    }

    configNote_->set_text(profilePath_.empty()
                              ? std::string("No home directory, so profiles "
                                            "cannot be saved.")
                              : "Profiles are saved in " + profilePath_ + ".");

    updateEnableTooltip();
}

// What the enable switch does depends on how the layer got loaded, and the two
// ways are different enough that one tooltip cannot be honest about both.
void Window::updateEnableTooltip() {
    if (layerIsImplicit()) {
        enable_->set_tooltip_text(
            "Whether the layer upscales this game.  The layer is installed for "
            "every Vulkan program, so this switch is all it takes: Steam starts "
            "the game with its appid in the environment and the layer reads it "
            "from the saved profile.  No launch options.");
    } else {
        enable_->set_tooltip_text(
            "Whether the profile is enabled for this game.  Nothing reaches "
            "the game until this is written to the launch options with Apply.");
    }
}

bool Window::layerIsImplicit() const { return !implicitManifest_.empty(); }

bool Window::layerDirLooksRight() const {
    return !store_.layer_dir.empty() &&
           fs::exists(fs::path(store_.layer_dir) / "VkLayer_lsl_frame.json");
}

void Window::setStatus(const std::string& message, bool error) {
    status_->set_text(message);
    if (error) {
        status_->add_css_class("lsl-error");
    } else {
        status_->remove_css_class("lsl-error");
    }
}

void Window::onSettingsChanged() {
    if (loading_) return;
    // Changing the method can make the sharpness box meaningless (it is RCAS's
    // alone) or meaningful again.
    sharp_->set_sensitive(current() != nullptr && selectedMethod() == "fsr1rcas");
    saveProfile();
    refreshPreview();
}

void Window::onLayerDirChanged() {
    if (loading_) return;
    store_.layer_dir = layerEntry_->get_text();
    saveProfile();
    refreshLayerNote();
    refreshPreview();
}

void Window::saveProfile() {
    const lsl::SteamGame* game = current();
    if (game) store_.set(game->appid, widgetProfile());
    std::string error;
    if (!store_.save(profilePath_, &error)) setStatus(error, true);
}

void Window::onApplyClicked() {
    const lsl::SteamGame* game = current();
    if (!game) return;

    saveProfile();
    const std::string existing =
        lsl::steamReadLaunchOptions(configPath_, game->appid);
    const lsl::Profile profile = widgetProfile();
    const std::string result =
        profile.enabled
            ? lsl::steamOptionsApply(existing, store_.layer_dir, profile.scale,
                                     profile.sharpness, profile.method)
            : lsl::steamOptionsClear(existing, store_.layer_dir);

    std::string error;
    if (!lsl::steamWriteLaunchOptions(configPath_, game->appid, result, &error)) {
        setStatus(error, true);
        return;
    }

    const std::string name = game->name;
    refreshDetails();  // re-reads from the file, so the box above is the truth

    std::string message = profile.enabled
                              ? "Applied to " + name + "."
                              : "Removed lsl from " + name + ".";
    if (layerIsImplicit() && profile.enabled) {
        // With an implicit install the launch options are a second way in, not
        // the way in; say so rather than implying they were required.
        message += "  This is optional: the layer is already loaded for every "
                   "program, so the switch above is enough.";
    } else if (profile.enabled && !layerDirLooksRight()) {
        message += "  The layer directory has no VkLayer_lsl_frame.json, so the "
                   "game will start without upscaling.";
    }
    if (steamRunning() && !layerIsImplicit()) {
        message += "  Restart Steam for the change to take effect.";
    }
    setStatus(message, profile.enabled && !layerIsImplicit() && !layerDirLooksRight());
}

void Window::onLaunchClicked() {
    const lsl::SteamGame* game = current();
    if (!game) return;

    // Anything that means "this will start without upscaling" is collected here
    // and reported *after* Steam is asked to start the game: it used to be set
    // at this point and then overwritten by the "Asked Steam to start" line
    // below, which made the warning invisible.
    std::string warning;
    bool warning_error = false;
    if (layerIsImplicit()) {
        // The layer is loaded for every process and reads the profile store, so
        // there is nothing to apply: just make sure the store says "on".
        saveProfile();
        if (!enable_->get_active()) {
            warning = "it is switched off in its profile, so nothing will be "
                      "upscaled";
        }
    } else if (!layerDirLooksRight()) {
        warning = "the layer directory has no VkLayer_lsl_frame.json, so "
                  "nothing will be upscaled";
        warning_error = true;
    } else if (enable_->get_active()) {
        const std::string existing =
            lsl::steamReadLaunchOptions(configPath_, game->appid);
        double scale = 0.0, sharpness = 0.0;
        std::string method;
        if (!lsl::steamOptionsParse(existing, &scale, &sharpness, &method)) {
            warning = "this profile has not been applied to Steam's launch "
                      "options yet, so nothing will be upscaled — press Apply "
                      "first";
            warning_error = true;
        }
    }

    try {
        Glib::spawn_command_line_async("steam -applaunch " + game->appid);
        if (warning.empty()) {
            setStatus("Asked Steam to start " + game->name + ".", false);
        } else {
            setStatus("Asked Steam to start " + game->name + ", but " + warning +
                          ".",
                      warning_error);
        }
    } catch (const Glib::Error& error) {
        setStatus(std::string("Could not run Steam: ") + error.what(), true);
    }
}

void Window::onCheckSetupClicked() {
    std::ostringstream r;
    bool ok = true;

    if (layerIsImplicit()) {
        r << "Layer: implicit (loader loads it into every Vulkan process)\n";
        r << "  manifest: " << implicitManifest_ << "\n";
    } else if (layerDirLooksRight()) {
        r << "Layer: explicit (must be in Steam launch options)\n";
        r << "  manifest: " << layerEntry_->get_text() << "/VkLayer_lsl_frame.json\n";
    } else {
        r << "Layer: NOT FOUND - no VkLayer_lsl_frame.json anywhere the GUI looked.\n";
        r << "  fix: run tools/install-layer.sh (add --implicit for the no-launch-options mode).\n";
        ok = false;
    }

    std::error_code ec;
    if (fs::is_regular_file(profilePath_, ec)) {
        r << "Profile store: " << profilePath_ << "\n";
    } else {
        r << "Profile store: MISSING (" << profilePath_ << ") - cannot save.\n";
        ok = false;
    }

    if (steamRunning()) {
        r << "Steam: running.\n";
    } else {
        r << "Steam: not running (launch options and steam -applaunch need it).\n";
    }

    const lsl::SteamGame* g = current();
    if (g) {
        r << "Selected game: " << g->name << " (appid " << g->appid << ").\n";
        r << "  enabled in profile: " << (enable_->get_active() ? "yes" : "no")
          << ", scale " << scale_->get_value() << ", method "
          << selectedMethod() << ".\n";
    } else {
        r << "No game selected.\n";
    }

    r << "\n";
    if (ok) {
        r << "Setup looks OK. To prove it works: set LSL_LAYER_LOG=/tmp/lsl.log, "
             "launch the game, then \"View log\" - look for "
             "\"scale: appid ... is enabled\" and \"upscale 1\".";
        setStatus(r.str(), false);
    } else {
        setStatus(r.str(), true);
    }
}

void Window::onViewLogClicked() {
    const char* p = std::getenv("LSL_LAYER_LOG");
    std::string path = (p && *p) ? p : "";
    if (path.empty()) {
        setStatus("Set LSL_LAYER_LOG to a file path (e.g. /tmp/lsl.log) before "
                  "launching the game, then click this again. Logging is off "
                  "otherwise so the layer stays silent in every other program.",
                  true);
        return;
    }
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        setStatus("Log file not found yet: " + path +
                      ". Launch the game once with LSL_LAYER_LOG set, then "
                      "retry.",
                  true);
        return;
    }
    // Prefer a real text editor over xdg-open: .log files often resolve to a
    // browser (or nothing) under xdg-open, which is not what the user wants.
    const char* editors[] = {"gedit", "mousepad", "kate", "kwrite", "pluma",
                             "xed", "leafpad", "gvim", "vim", "nano", nullptr};
    std::string cmd;
    for (int i = 0; editors[i]; ++i) {
        std::error_code ec;
        if (fs::exists("/usr/bin/" + std::string(editors[i]), ec) ||
            fs::exists("/bin/" + std::string(editors[i]), ec)) {
            cmd = std::string(editors[i]) + " \"" + path + "\"";
            break;
        }
    }
    if (cmd.empty()) cmd = "xdg-open \"" + path + "\"";
    try {
        Glib::spawn_command_line_async(cmd);
        setStatus("Opened " + path + " with " + cmd.substr(0, cmd.find(' ')) + ".", false);
    } catch (const Glib::Error& e) {
        setStatus("Could not open the log: " + std::string(e.what()) +
                      "  (file is at " + path + ")", true);
    }
}

// The window is built, so its list can be checked against the games found.
// Everything else about the selftest is in the free functions below: this part
// needs a display, and those do not.
int Window::selfCheck() {
    int rows = 0;
    while (list_->get_row_at_index(rows)) ++rows;

    int failures = 0;
    if (static_cast<size_t>(rows) != games_.size()) {
        std::printf("FAIL the list shows %d rows for %zu games\n", rows,
                    games_.size());
        ++failures;
    }
    std::printf("gui selftest: window built, %d rows for %zu games\n", rows,
                games_.size());

    // Valve's tools are installed exactly like games; the window hides them by
    // name, so say how many that is.
    int tools = 0;
    for (const lsl::SteamGame& game : games_) {
        if (lsl::steamLooksLikeTool(game.name)) ++tools;
    }
    std::printf("gui selftest: %d of those names are Valve's own tools\n", tools);

    return failures == 0 ? 0 : 1;
}

}  // namespace

// ------------------------------------------------------------------ selftest
//
// Two things can be checked without a human.  The first is that the launch
// options logic does not damage a real Steam installation: every game's options
// are put through apply-then-clear and compared with clearing the original,
// which is where a bug would show up as a lost DXVK_HUD or a dropped %command%.
// That needs no display, so it runs everywhere, including CI.
//
// The second is that the window can be built on this data at all, which needs a
// display and is skipped without one.

namespace {

struct SteamData {
    std::string root;
    std::string config;
    std::string profiles;
    std::string layerDir;
    std::vector<lsl::SteamGame> games;
};

SteamData gatherSteamData() {
    SteamData data;
    data.root = lsl::steamRoot();
    data.config = lsl::steamLocalConfig(data.root);
    data.profiles = lsl::defaultProfilePath();
    data.layerDir = lsl::ProfileStore::load(data.profiles).layer_dir;
    if (data.layerDir.empty()) data.layerDir = guessLayerDir();
    data.games = lsl::steamGames(data.root);
    return data;
}

int checkLaunchOptions(const SteamData& data) {
    int failures = 0;
    // Every name the layer accepts has to survive apply-then-clear, and the
    // method has to come back out of steamOptionsParse, which is what the
    // window shows.  "auto" is the default; a method name exercises the rest.
    const std::vector<std::string> methods = {"auto", "fsr1rcas", "nis", "cnn"};
    for (const lsl::SteamGame& game : data.games) {
        const std::string existing =
            lsl::steamReadLaunchOptions(data.config, game.appid);
        for (const std::string& method : methods) {
            const std::string applied = lsl::steamOptionsApply(
                existing, data.layerDir, 2.0, 0.0, method);

            const std::string roundTrip =
                lsl::steamOptionsClear(applied, data.layerDir);
            const std::string baseline =
                lsl::steamOptionsClear(existing, data.layerDir);
            if (roundTrip != baseline) {
                ++failures;
                std::printf("FAIL %s: apply then clear changed the options "
                            "(method %s)\n"
                            "  before   [%s]\n"
                            "  applied  [%s]\n"
                            "  cleared  [%s]\n"
                            "  baseline [%s]\n",
                            game.appid.c_str(), method.c_str(), existing.c_str(),
                            applied.c_str(), roundTrip.c_str(), baseline.c_str());
            }

            double scale = 0.0, sharpness = 0.0;
            std::string readBack;
            if (lsl::steamOptionsParse(applied, &scale, &sharpness, &readBack) &&
                readBack != method) {
                ++failures;
                std::printf("FAIL %s: wrote method %s, read back %s\n",
                            game.appid.c_str(), method.c_str(),
                            readBack.c_str());
            }

            if (lsl::steamOptionsApply(applied, data.layerDir, 2.0, 0.0,
                                       method) != applied) {
                ++failures;
                std::printf("FAIL %s: applying twice changed the options\n",
                            game.appid.c_str());
            }
        }
    }
    return failures;
}

// A text picture of the window.  The selftest is meant to be usable without a
// screen, so it prints what it built rather than expecting anyone to look at it.
void dumpWidgetTree(GtkWidget* widget, int depth, int& lines, int limit) {
    if (!widget) return;
    if (lines >= limit) {
        return;
    }

    std::string text = G_OBJECT_TYPE_NAME(widget);
    if (GTK_IS_LABEL(widget)) {
        text += "  \"" + std::string(gtk_label_get_text(GTK_LABEL(widget))) +
                "\"";
    } else if (GTK_IS_BUTTON(widget)) {
        if (const char* label = gtk_button_get_label(GTK_BUTTON(widget))) {
            text += "  [" + std::string(label) + "]";
        }
    } else if (GTK_IS_EDITABLE(widget)) {
        text += "  \"" + std::string(gtk_editable_get_text(GTK_EDITABLE(widget))) +
                "\"";
    } else if (GTK_IS_SWITCH(widget)) {
        text += gtk_switch_get_active(GTK_SWITCH(widget)) ? "  on" : "  off";
    }

    for (int i = 0; i < depth; ++i) std::fputs("  ", stdout);
    std::printf("%s\n", text.c_str());
    ++lines;

    for (GtkWidget* child = gtk_widget_get_first_child(widget); child;
         child = gtk_widget_get_next_sibling(child)) {
        dumpWidgetTree(child, depth + 1, lines, limit);
    }
}

int runSelfTest(bool withWindow) {
    if (withWindow) {
        // What Gtk::Application::create() would otherwise do for us: register
        // gtkmm's GObject wrappers, then open the display without exiting when
        // there is none.
        Gtk::init_gtkmm_internals();
        if (!gtk_init_check()) {
            std::printf("gui selftest: no display, skipping the window check\n");
            return 77;
        }
    }

    const SteamData data = gatherSteamData();
    std::printf("gui selftest: steam root %s\n", data.root.c_str());
    std::printf("gui selftest: config %s\n", data.config.c_str());
    std::printf("gui selftest: profiles %s\n", data.profiles.c_str());
    std::printf("gui selftest: layer dir %s%s\n", data.layerDir.c_str(),
                fs::exists(fs::path(data.layerDir) / "VkLayer_lsl_frame.json")
                    ? ""
                    : "  (no VkLayer_lsl_frame.json)");
    std::printf("gui selftest: %zu games found\n", data.games.size());

    int failures = checkLaunchOptions(data);
    if (withWindow) {
        Window window;
        failures += window.selfCheck();

        int lines = 0;
        std::printf("gui selftest: widget tree\n");
        dumpWidgetTree(GTK_WIDGET(window.gobj()), 0, lines, 120);
        if (lines >= 120) std::printf("  ... (truncated)\n");
    }

    std::printf("lsl gui selftest: %zu games, %d failures\n", data.games.size(),
                failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--selftest") {
        return runSelfTest(false);
    }
    if (argc > 1 && std::string(argv[1]) == "--selftest-window") {
        return runSelfTest(true);
    }
    auto app = Gtk::Application::create(kAppId);
    return app->make_window_and_run<Window>(argc, argv);
}
