#include "render_profiles_page.h"

#include "render_profiles.h"
#include "settings.h"
#include "ui_hints.h"

#include "core/model/profile_match.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ustudio::app {

namespace {

using Quality = core::RenderProfile::Quality;

constexpr Quality kQualities[] = {Quality::Draft, Quality::Good, Quality::High, Quality::Max, Quality::Bitrate};

struct RenderPage
{
    RenderProfileStore *store = nullptr;
    Settings *settings = nullptr;
    std::function<void(int)> onThreadsChanged;
    AdwSpinRow *threadsRow = nullptr;
    GtkWidget *threadsWarning = nullptr;
    AdwComboRow *profileRow = nullptr;
    std::vector<std::string> names; // parallel to profileRow's model
    GtkWidget *removeButton = nullptr;
    GtkWidget *defaultButton = nullptr;
    GtkWidget *saveButton = nullptr;
    AdwPreferencesGroup *editGroup = nullptr;
    AdwEntryRow *nameRow = nullptr;
    AdwComboRow *resolutionRow = nullptr;
    AdwComboRow *rateRow = nullptr;
    AdwComboRow *qualityRow = nullptr;
    AdwSpinRow *videoRow = nullptr;
    AdwSpinRow *audioRow = nullptr;
    std::string shown; // the profile the fields show
    bool updating = false;
};

void profileSelectedTrampoline(AdwComboRow *row, GParamSpec *pspec, gpointer userData);
void threadsChangedTrampoline(AdwSpinRow *row, GParamSpec *pspec, gpointer userData);
void qualityChangedTrampoline(AdwComboRow *row, GParamSpec *pspec, gpointer userData);
void newClickedTrampoline(GtkButton *button, gpointer userData);
void duplicateClickedTrampoline(GtkButton *button, gpointer userData);
void removeClickedTrampoline(GtkButton *button, gpointer userData);
void defaultClickedTrampoline(GtkButton *button, gpointer userData);
void saveClickedTrampoline(GtkButton *button, gpointer userData);

constexpr int kThreadsWarnAbove = 80;

// "12 of 16 threads", and above kThreadsWarnAbove a warning (it doesn't stop
// anything).
void updateThreadsRow(RenderPage &page)
{
    const int percent = static_cast<int>(adw_spin_row_get_value(page.threadsRow));
    const int hardware = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    std::string subtitle =
        std::to_string(core::renderThreadBudget(percent, hardware)) + " of " + std::to_string(hardware) + " threads";
    const bool warn = percent > kThreadsWarnAbove;
    if (warn)
        subtitle += ". Above 80% the desktop may stutter while rendering";
    adw_action_row_set_subtitle(ADW_ACTION_ROW(page.threadsRow), subtitle.c_str());
    gtk_widget_set_visible(page.threadsWarning, warn);
    if (warn)
        gtk_widget_add_css_class(GTK_WIDGET(page.threadsRow), "warning");
    else
        gtk_widget_remove_css_class(GTK_WIDGET(page.threadsRow), "warning");
}

core::RenderProfile shownProfile(const RenderPage &page)
{
    return page.store->find(page.shown).value_or(core::builtInRenderProfiles()[0]);
}

std::string defaultName(const RenderPage &page)
{
    std::string name = page.settings->defaultRenderProfile();
    return page.store->find(name) ? name : core::kDefaultRenderProfileName;
}

void updateBitrateRows(RenderPage &page)
{
    const bool bitrate = kQualities[adw_combo_row_get_selected(page.qualityRow)] == Quality::Bitrate;
    gtk_widget_set_visible(GTK_WIDGET(page.videoRow), bitrate);
    gtk_widget_set_visible(GTK_WIDGET(page.audioRow), bitrate);
}

void showProfile(RenderPage &page, const std::string &name)
{
    page.shown = name;
    const core::RenderProfile profile = shownProfile(page);
    page.updating = true;
    gtk_editable_set_text(GTK_EDITABLE(page.nameRow), profile.name.c_str());
    const auto &heights = core::renderHeights();
    auto height = std::find(heights.begin(), heights.end(), profile.height);
    adw_combo_row_set_selected(page.resolutionRow,
                               height == heights.end() ? 0 : static_cast<guint>(height - heights.begin()));
    const auto &rates = core::renderFrameRates();
    auto rate = std::find(rates.begin(), rates.end(), profile.frameRate);
    adw_combo_row_set_selected(page.rateRow, rate == rates.end() ? 0 : static_cast<guint>(rate - rates.begin()));
    adw_combo_row_set_selected(
        page.qualityRow, static_cast<guint>(std::find(std::begin(kQualities), std::end(kQualities), profile.quality) -
                                            std::begin(kQualities)));
    adw_spin_row_set_value(page.videoRow, std::round(static_cast<double>(profile.videoBitrate) / 1000.0));
    adw_spin_row_set_value(page.audioRow, std::round(static_cast<double>(profile.audioBitrate) / 1000.0));
    page.updating = false;
    updateBitrateRows(page);

    const bool editable = !profile.builtIn;
    for (GtkWidget *row :
         {GTK_WIDGET(page.nameRow), GTK_WIDGET(page.resolutionRow), GTK_WIDGET(page.rateRow),
          GTK_WIDGET(page.qualityRow), GTK_WIDGET(page.videoRow), GTK_WIDGET(page.audioRow), page.saveButton})
        gtk_widget_set_sensitive(row, editable);
    gtk_widget_set_sensitive(page.removeButton, editable);
    gtk_widget_set_visible(page.defaultButton, defaultName(page) != profile.name); // room for the name
    adw_preferences_group_set_description(
        page.editGroup, editable ? nullptr : "Built-in profiles can't be changed: Duplicate one to make your own.");
}

// Rebuilds the profile list (the default marked) and shows `select`.
void refreshList(RenderPage &page, const std::string &select)
{
    page.updating = true;
    page.names.clear();
    GtkStringList *labels = gtk_string_list_new(nullptr);
    const std::string current = defaultName(page);
    guint selected = 0;
    for (const core::RenderProfile &profile : page.store->all()) {
        if (profile.name == select)
            selected = static_cast<guint>(page.names.size());
        page.names.push_back(profile.name);
        std::string label = profile.name + (profile.name == current ? " (default)" : "");
        gtk_string_list_append(labels, label.c_str());
    }
    adw_combo_row_set_model(page.profileRow, G_LIST_MODEL(labels));
    g_object_unref(labels);
    adw_combo_row_set_selected(page.profileRow, selected);
    page.updating = false;
    showProfile(page, page.names[selected]);
}

void setStatus(RenderPage &page, const std::string &message)
{
    adw_preferences_group_set_description(page.editGroup, message.empty() ? nullptr : message.c_str());
}

// "base", else "base 2", "base 3", ...
std::string unusedName(const RenderPage &page, const std::string &base)
{
    std::string name = base;
    for (int n = 2; page.store->find(name); ++n)
        name = base + " " + std::to_string(n);
    return name;
}

void addCopy(RenderPage &page, core::RenderProfile profile, const std::string &name)
{
    profile.name = unusedName(page, name);
    profile.builtIn = false;
    if (std::string error = page.store->save(profile, ""); !error.empty()) {
        setStatus(page, error);
        return;
    }
    refreshList(page, profile.name);
}

GtkWidget *headerButton(const char *label, const char *hintId, GCallback onClicked, RenderPage *page)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(button, "flat");
    setTooltip(button, hintId);
    g_signal_connect(button, "clicked", onClicked, page);
    return button;
}

} // namespace

AdwPreferencesPage *buildRenderProfilesPage(RenderProfileStore &store, Settings &settings, bool qualityMode,
                                            std::function<void(int percent)> onThreadsChanged)
{
    auto *page = new RenderPage;
    page->store = &store;
    page->settings = &settings;
    page->onThreadsChanged = std::move(onThreadsChanged);
    AdwPreferencesPage *prefs = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(prefs, "Render");
    adw_preferences_page_set_icon_name(prefs, "video-x-generic-symbolic");
    g_object_set_data_full(G_OBJECT(prefs), "ustudio-render-page", page,
                           [](gpointer data) { delete static_cast<RenderPage *>(data); });

    // Global, above the profiles: applies to every render from the next one.
    AdwPreferencesGroup *renderingGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(renderingGroup, "Rendering");
    adw_preferences_page_add(prefs, renderingGroup);
    page->threadsRow = ADW_SPIN_ROW(adw_spin_row_new_with_range(10.0, 100.0, 5.0));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(page->threadsRow), "Render threads (%)");
    setTooltip(GTK_WIDGET(page->threadsRow), "render-profiles.threads");
    adw_spin_row_set_digits(page->threadsRow, 0);
    adw_spin_row_set_value(page->threadsRow, static_cast<double>(settings.renderThreadsPercent()));
    page->threadsWarning = gtk_image_new_from_icon_name("dialog-warning-symbolic");
    adw_action_row_add_prefix(ADW_ACTION_ROW(page->threadsRow), page->threadsWarning);
    g_signal_connect(page->threadsRow, "notify::value", G_CALLBACK(threadsChangedTrampoline), page);
    adw_preferences_group_add(renderingGroup, GTK_WIDGET(page->threadsRow));
    updateThreadsRow(*page);

    AdwPreferencesGroup *profilesGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(profilesGroup, "Profiles");
    adw_preferences_group_set_description(
        profilesGroup, qualityMode ? "MP4 files: H.264 video and AAC audio"
                                   : "MP4 files: H.264 video and AAC audio. This system's H.264 encoder (OpenH264) "
                                     "has no quality setting, so Draft to Max pick a bitrate for the picture size.");
    GtkWidget *profileButtons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(profileButtons),
                   headerButton("New", "render-profiles.new", G_CALLBACK(newClickedTrampoline), page));
    gtk_box_append(GTK_BOX(profileButtons), headerButton("Duplicate", "render-profiles.duplicate",
                                                         G_CALLBACK(duplicateClickedTrampoline), page));
    page->removeButton = headerButton("Remove", "render-profiles.remove", G_CALLBACK(removeClickedTrampoline), page);
    gtk_box_append(GTK_BOX(profileButtons), page->removeButton);
    adw_preferences_group_set_header_suffix(profilesGroup, profileButtons);
    adw_preferences_page_add(prefs, profilesGroup);

    page->profileRow = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(page->profileRow), "Profile");
    setTooltip(GTK_WIDGET(page->profileRow), "render-profiles.profile");
    page->defaultButton =
        headerButton("Set as Default", "render-profiles.set-default", G_CALLBACK(defaultClickedTrampoline), page);
    gtk_widget_set_valign(page->defaultButton, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(ADW_ACTION_ROW(page->profileRow), page->defaultButton);
    adw_preferences_group_add(profilesGroup, GTK_WIDGET(page->profileRow));

    page->editGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(page->editGroup, "Settings");
    page->saveButton = headerButton("Save", "render-profiles.save", G_CALLBACK(saveClickedTrampoline), page);
    gtk_widget_add_css_class(page->saveButton, "suggested-action");
    gtk_widget_remove_css_class(page->saveButton, "flat");
    adw_preferences_group_set_header_suffix(page->editGroup, page->saveButton);
    adw_preferences_page_add(prefs, page->editGroup);

    page->nameRow = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(page->nameRow), "Name");
    adw_preferences_group_add(page->editGroup, GTK_WIDGET(page->nameRow));

    const char *resolutions[] = {"Project", "2160p", "1440p", "1080p", "720p", nullptr};
    page->resolutionRow = ADW_COMBO_ROW(adw_combo_row_new());
    GtkStringList *resolutionModel = gtk_string_list_new(resolutions);
    adw_combo_row_set_model(page->resolutionRow, G_LIST_MODEL(resolutionModel));
    g_object_unref(resolutionModel);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(page->resolutionRow), "Resolution");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(page->resolutionRow), "The height; the width follows the project");
    setTooltip(GTK_WIDGET(page->resolutionRow), "render-profiles.resolution");
    adw_preferences_group_add(page->editGroup, GTK_WIDGET(page->resolutionRow));

    // Another rate renders a retimed copy of the project (renderProject()).
    GtkStringList *rateModel = gtk_string_list_new(nullptr);
    for (const core::Rational &rate : core::renderFrameRates())
        gtk_string_list_append(rateModel, rate.num > 0 ? (core::formatFps(rate) + " fps").c_str() : "Project");
    page->rateRow = ADW_COMBO_ROW(adw_combo_row_new());
    adw_combo_row_set_model(page->rateRow, G_LIST_MODEL(rateModel));
    g_object_unref(rateModel);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(page->rateRow), "Frame rate");
    setTooltip(GTK_WIDGET(page->rateRow), "render-profiles.frame-rate");
    adw_preferences_group_add(page->editGroup, GTK_WIDGET(page->rateRow));

    const char *qualities[] = {"Draft", "Good", "High", "Max", "Exact bitrates", nullptr};
    page->qualityRow = ADW_COMBO_ROW(adw_combo_row_new());
    GtkStringList *qualityModel = gtk_string_list_new(qualities);
    adw_combo_row_set_model(page->qualityRow, G_LIST_MODEL(qualityModel));
    g_object_unref(qualityModel);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(page->qualityRow), "Quality");
    setTooltip(GTK_WIDGET(page->qualityRow), "render-profiles.quality");
    g_signal_connect(page->qualityRow, "notify::selected", G_CALLBACK(qualityChangedTrampoline), page);
    adw_preferences_group_add(page->editGroup, GTK_WIDGET(page->qualityRow));

    page->videoRow = ADW_SPIN_ROW(adw_spin_row_new_with_range(100.0, 200000.0, 100.0));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(page->videoRow), "Video bitrate (kbit/s)");
    adw_preferences_group_add(page->editGroup, GTK_WIDGET(page->videoRow));
    page->audioRow = ADW_SPIN_ROW(adw_spin_row_new_with_range(32.0, 512.0, 32.0));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(page->audioRow), "Audio bitrate (kbit/s)");
    adw_preferences_group_add(page->editGroup, GTK_WIDGET(page->audioRow));

    g_signal_connect(page->profileRow, "notify::selected", G_CALLBACK(profileSelectedTrampoline), page);
    refreshList(*page, defaultName(*page));
    return prefs;
}

// ---- GTK trampolines ----

namespace {

void profileSelectedTrampoline(AdwComboRow *row, GParamSpec *, gpointer userData)
{
    auto *page = static_cast<RenderPage *>(userData);
    guint selected = adw_combo_row_get_selected(row);
    if (page->updating || selected >= page->names.size())
        return;
    setStatus(*page, "");
    showProfile(*page, page->names[selected]);
}

void threadsChangedTrampoline(AdwSpinRow *row, GParamSpec *, gpointer userData)
{
    auto *page = static_cast<RenderPage *>(userData);
    const int percent = static_cast<int>(adw_spin_row_get_value(row));
    page->settings->setRenderThreadsPercent(percent);
    updateThreadsRow(*page);
    if (page->onThreadsChanged)
        page->onThreadsChanged(percent);
}

void qualityChangedTrampoline(AdwComboRow *, GParamSpec *, gpointer userData)
{
    auto *page = static_cast<RenderPage *>(userData);
    if (!page->updating)
        updateBitrateRows(*page);
}

void newClickedTrampoline(GtkButton *, gpointer userData)
{
    auto *page = static_cast<RenderPage *>(userData);
    core::RenderProfile profile;
    profile.quality = Quality::High;
    addCopy(*page, profile, "New profile");
}

void duplicateClickedTrampoline(GtkButton *, gpointer userData)
{
    auto *page = static_cast<RenderPage *>(userData);
    addCopy(*page, shownProfile(*page), "Copy of " + page->shown);
}

void removeClickedTrampoline(GtkButton *, gpointer userData)
{
    auto *page = static_cast<RenderPage *>(userData);
    const std::string name = page->shown;
    if (std::string error = page->store->remove(name); !error.empty()) {
        setStatus(*page, error);
        return;
    }
    if (page->settings->defaultRenderProfile() == name)
        page->settings->setDefaultRenderProfile(core::kDefaultRenderProfileName);
    refreshList(*page, defaultName(*page));
}

void defaultClickedTrampoline(GtkButton *, gpointer userData)
{
    auto *page = static_cast<RenderPage *>(userData);
    page->settings->setDefaultRenderProfile(page->shown);
    refreshList(*page, page->shown);
}

void saveClickedTrampoline(GtkButton *, gpointer userData)
{
    auto *page = static_cast<RenderPage *>(userData);
    core::RenderProfile profile;
    profile.name = gtk_editable_get_text(GTK_EDITABLE(page->nameRow));
    profile.height = core::renderHeights()[adw_combo_row_get_selected(page->resolutionRow)];
    profile.frameRate = core::renderFrameRates()[adw_combo_row_get_selected(page->rateRow)];
    profile.quality = kQualities[adw_combo_row_get_selected(page->qualityRow)];
    if (profile.quality == Quality::Bitrate) {
        profile.videoBitrate = std::llround(adw_spin_row_get_value(page->videoRow) * 1000.0);
        profile.audioBitrate = std::llround(adw_spin_row_get_value(page->audioRow) * 1000.0);
    }
    const std::string previous = page->shown;
    if (std::string error = page->store->save(profile, previous); !error.empty()) {
        setStatus(*page, error);
        return;
    }
    if (previous != profile.name && page->settings->defaultRenderProfile() == previous)
        page->settings->setDefaultRenderProfile(profile.name);
    refreshList(*page, profile.name);
    setStatus(*page, "Saved.");
}

} // namespace

} // namespace ustudio::app
