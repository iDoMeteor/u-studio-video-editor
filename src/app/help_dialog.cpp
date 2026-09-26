// The Help dialog: Controls, Keyboard Shortcuts and About (moved from
// app_window.cpp unchanged, ahead of M4 G's changes).

#include "app_window.h"

#include "action_registry.h"
#include "ui_hints.h"

#include <string>
#include <utility>
#include <vector>

namespace ustudio::app {

GtkWidget *AppWindow::buildShortcutsPage() const
{
    GtkWidget *page = adw_preferences_page_new();

    // One AdwPreferencesGroup per ActionSpec::category, created the first
    // time that category is seen -- action_registry.h's table is already
    // grouped by category (Playback, Editing, Project), so this walks it
    // once, in that same order, rather than sorting or pre-declaring the
    // category list separately.
    std::vector<std::pair<std::string, AdwPreferencesGroup *>> groups;

    // The shell's actions, then the drop-ins' under their own categories.
    for (const ActionSpec &spec : allActionSpecs()) {
        AdwPreferencesGroup *group = nullptr;
        for (auto &entry : groups) {
            if (entry.first == spec.category) {
                group = entry.second;
                break;
            }
        }
        if (group == nullptr) {
            group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
            adw_preferences_group_set_title(group, spec.category);
            adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), group);
            groups.emplace_back(spec.category, group);
        }

        std::string accelLabel = shortcutLabel(spec.name);

        // Row titles and subtitles are Pango markup: a shortcut's label can
        // be "<" itself ("Shift+,, <" failed to parse and showed nothing).
        GtkWidget *row = adw_action_row_new();
        char *title = g_markup_escape_text(spec.label, -1);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
        g_free(title);
        if (!accelLabel.empty()) {
            char *subtitle = g_markup_escape_text(accelLabel.c_str(), -1);
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
            g_free(subtitle);
        }
        adw_preferences_group_add(group, row);
    }

    return page;
}

GtkWidget *AppWindow::buildControlsPage() const
{
    GtkWidget *page = adw_preferences_page_new();
    std::vector<std::pair<std::string, AdwPreferencesGroup *>> groups;

    for (const HintSpec &hint : hintSpecs()) {
        AdwPreferencesGroup *group = nullptr;
        for (auto &entry : groups) {
            if (entry.first == hint.category) {
                group = entry.second;
                break;
            }
        }
        if (group == nullptr) {
            group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
            adw_preferences_group_set_title(group, hint.category);
            adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), group);
            groups.emplace_back(hint.category, group);
        }

        // "Click the clip, then Delete": the gesture, then the key.
        std::string how = hint.gesture != nullptr ? hint.gesture : "";
        if (std::string shortcut = shortcutLabel(hint.action); !shortcut.empty())
            how += how.empty() ? shortcut : " " + shortcut;
        std::string subtitle = how;
        if (hint.detail != nullptr)
            subtitle += subtitle.empty() ? hint.detail : std::string("\n") + hint.detail;

        // Row titles and subtitles are Pango markup.
        GtkWidget *row = adw_action_row_new();
        char *title = g_markup_escape_text(hint.title, -1);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
        g_free(title);
        if (!subtitle.empty()) {
            char *escaped = g_markup_escape_text(subtitle.c_str(), -1);
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), escaped);
            g_free(escaped);
        }
        adw_preferences_group_add(group, row);
    }

    return page;
}

GtkWidget *AppWindow::buildAboutPage() const
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(box, TRUE);
    gtk_widget_set_margin_top(box, 24);
    gtk_widget_set_margin_bottom(box, 24);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);

    GtkWidget *title = gtk_label_new("u Studio Video Editor");
    gtk_widget_add_css_class(title, "title-1");
    gtk_box_append(GTK_BOX(box), title);

    GtkWidget *version = gtk_label_new("Version " USTUDIO_VERSION);
    gtk_widget_add_css_class(version, "dim-label");
    gtk_box_append(GTK_BOX(box), version);

    // Verbatim from data/com.ustudio.VideoEditor.metainfo.xml's <summary>
    // -- one source of truth for the app's own self-description.
    GtkWidget *summary = gtk_label_new("Non-linear video editor built on GTK4, libadwaita and MLT");
    gtk_label_set_wrap(GTK_LABEL(summary), TRUE);
    gtk_label_set_justify(GTK_LABEL(summary), GTK_JUSTIFY_CENTER);
    gtk_widget_set_margin_top(summary, 12);
    gtk_box_append(GTK_BOX(box), summary);

    GtkWidget *license = gtk_label_new("GPL-3.0-or-later");
    gtk_widget_add_css_class(license, "dim-label");
    gtk_widget_set_margin_top(license, 12);
    gtk_box_append(GTK_BOX(box), license);

    return box;
}

void AppWindow::showHelpDialog()
{
    AdwDialog *dialog = ADW_DIALOG(adw_dialog_new());
    adw_dialog_set_title(dialog, "Help");
    adw_dialog_set_content_width(dialog, 640);
    adw_dialog_set_content_height(dialog, 560);

    AdwViewStack *stack = ADW_VIEW_STACK(adw_view_stack_new());
    adw_view_stack_add_titled_with_icon(stack, buildControlsPage(), "controls", "Controls", "input-mouse-symbolic");
    adw_view_stack_add_titled_with_icon(stack, buildShortcutsPage(), "shortcuts", "Keyboard Shortcuts",
                                        "preferences-desktop-keyboard-shortcuts-symbolic");
    adw_view_stack_add_titled_with_icon(stack, buildAboutPage(), "about", "About", "help-about-symbolic");

    GtkWidget *switcher = adw_view_switcher_new();
    adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), stack);
    adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);

    GtkWidget *headerBar = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(headerBar), switcher);

    GtkWidget *toolbarView = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbarView), headerBar);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbarView), GTK_WIDGET(stack));

    adw_dialog_set_child(dialog, toolbarView);
    adw_dialog_present(dialog, GTK_WIDGET(m_window));
}

} // namespace ustudio::app
