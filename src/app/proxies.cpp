// M4 C (doc 07, "Proxies"): smaller stand-ins for editing, made by
// `u-studio-render --proxy` in child processes (ProxyQueue) and played in
// place of the originals while "Proxies" is on. The toggle is view state (a
// setting, never the project); making or removing a proxy changes only the
// asset's proxyPath, which isn't an undo step (like a probe result) but is
// saved. Renders always use the originals. Defaults for a 1080p-first
// editor: sources taller than 1080 are offered proxies once per project
// (doc 13, Q8); 1080p sources are left alone unless asked.

#include "app_window.h"

#include "proxy_launcher.h"
#include "core/log.h"
#include "core/media/utf8_path.h"

#include <filesystem>

namespace ustudio::app {

namespace Log = ustudio::core::Log;

namespace {

constexpr int kProxyThreshold = 1080;            // taller than this is offered a proxy
constexpr const char *kProxySetting = "proxies"; // Project::settings: "always" / "never"; unset: ask

bool proxyable(const core::Asset &asset)
{
    // Not with alpha: a proxy is H.264, which has none, so an overlay would
    // preview on black.
    return asset.info.hasVideo && !asset.info.isStillImage && !asset.info.hasAlpha &&
           asset.status != core::Asset::Status::Missing && asset.info.height > 0;
}

} // namespace

void AppWindow::setUpProxies()
{
    m_proxyQueue = std::make_unique<ProxyQueue>(
        std::make_unique<GioLauncher>(), locateRenderTool(),
        ProxyQueue::Callbacks{
            .progress = [this](core::AssetId, double) { queueRefresh(); },
            .done = [this](core::AssetId asset, const std::string &output) { onProxyDone(asset, output); },
            .failed =
                [this](core::AssetId asset, const std::string &message) {
                    const std::string name = m_model.hasAsset(asset) ? m_model.asset(asset).displayName : "a clip";
                    showStatus("Couldn't make a proxy of " + name + ": " + message);
                    queueRefresh();
                },
        });
    m_engine->setUseProxies(m_settings->useProxies());
}

void AppWindow::buildProxyToggle(GtkWidget *transport)
{
    // Beside the preview scale: the two playback-performance controls.
    m_proxyToggle = gtk_toggle_button_new_with_label("Proxies");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(m_proxyToggle), m_settings->useProxies());
    gtk_widget_set_focusable(m_proxyToggle, FALSE); // audit A7, as the dropdown
    setTooltip(m_proxyToggle, "transport.proxies");
    g_signal_connect_swapped(
        m_proxyToggle, "toggled", G_CALLBACK(+[](gpointer self) {
            auto *window = static_cast<AppWindow *>(self);
            const bool use = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(window->m_proxyToggle));
            window->m_settings->setUseProxies(use);
            window->m_engine->setUseProxies(use);
            window->showStatus(use ? "Playing proxies where clips have them." : "Playing the original media.");
        }),
        this);
    gtk_box_append(GTK_BOX(transport), m_proxyToggle);
}

void AppWindow::addProxyMenuItems(GtkWidget *menuBox)
{
    auto item = [&](const char *label, const char *hint, void (*clicked)(gpointer)) {
        GtkWidget *button = gtk_button_new_with_label(label);
        gtk_widget_add_css_class(button, "flat");
        setTooltip(button, hint);
        g_signal_connect_swapped(button, "clicked", G_CALLBACK(clicked), this);
        gtk_box_append(GTK_BOX(menuBox), button);
        return button;
    };
    m_createProxyButton = item(
        "Create Proxy", "media.create-proxy", +[](gpointer self) {
            auto *window = static_cast<AppWindow *>(self);
            gtk_popover_popdown(window->m_mediaBrowserContextMenu);
            window->createProxies({window->m_contextMenuAssetId}, window->m_settings->proxyHeight());
        });
    m_conformProxyButton = item(
        "Create Conformed Proxy", "media.conform-proxy", +[](gpointer self) {
            auto *window = static_cast<AppWindow *>(self);
            gtk_popover_popdown(window->m_mediaBrowserContextMenu);
            window->createProxies({window->m_contextMenuAssetId}, 0);
        });
    m_removeProxyButton = item(
        "Remove Proxy", "media.remove-proxy", +[](gpointer self) {
            auto *window = static_cast<AppWindow *>(self);
            gtk_popover_popdown(window->m_mediaBrowserContextMenu);
            window->removeProxy(window->m_contextMenuAssetId);
        });
}

void AppWindow::updateProxyMenuItems()
{
    const bool known = m_model.hasAsset(m_contextMenuAssetId);
    const bool can = known && proxyable(m_model.asset(m_contextMenuAssetId));
    const bool has = known && !m_model.asset(m_contextMenuAssetId).proxyPath.empty();
    const bool making = m_proxyQueue && m_proxyQueue->progressOf(m_contextMenuAssetId).has_value();
    gtk_widget_set_visible(m_createProxyButton, can && !has && !making);
    gtk_widget_set_visible(m_conformProxyButton, can && !has && !making);
    gtk_widget_set_visible(m_removeProxyButton, has || making);
    gtk_button_set_label(GTK_BUTTON(m_removeProxyButton), making ? "Stop Making Proxy" : "Remove Proxy");
}

void AppWindow::createProxies(const std::vector<core::AssetId> &assets, int height)
{
    if (!m_proxyQueue)
        return;
    const std::string tool = locateRenderTool();
    if (tool.empty()) {
        showStatus("Can't make proxies: u-studio-render isn't installed next to the editor.");
        return;
    }
    size_t queued = 0;
    for (core::AssetId id : assets) {
        if (!m_model.hasAsset(id) || !proxyable(m_model.asset(id)))
            continue;
        const core::Asset &asset = m_model.asset(id);
        ProxyQueue::Job job{id, asset.path, proxyPathFor(asset.fileFingerprint, asset.path, height), height,
                            m_model.sequence().profile.fps};
        if (asset.info.isImageSequence) {
            job.sequenceBegin = asset.info.sequenceBegin;
            job.sequenceCount = static_cast<int>(asset.info.lengthInSequenceFrames);
        }
        m_proxyQueue->add(std::move(job));
        ++queued;
    }
    if (queued > 0)
        showStatus(queued == 1 ? "Making a proxy…" : "Making " + std::to_string(queued) + " proxies…");
    queueRefresh();
}

void AppWindow::onProxyDone(core::AssetId asset, const std::string &output)
{
    if (!m_model.hasAsset(asset)) {
        std::error_code ec;
        std::filesystem::remove(core::pathFromUtf8(output), ec); // its clip went meanwhile
        return;
    }
    m_model.setAssetProxy(asset, output);
    // Not an undo step, but saved with the project: dirty, which also
    // publishes the snapshot to the engine (UndoStack::changed).
    m_undoStack.markDirty();
    showStatus("Proxy ready: " + m_model.asset(asset).displayName);
    queueRefresh();
}

void AppWindow::removeProxy(core::AssetId asset)
{
    if (m_proxyQueue && m_proxyQueue->progressOf(asset)) {
        m_proxyQueue->cancel(asset);
        queueRefresh();
        return;
    }
    if (!m_model.hasAsset(asset) || m_model.asset(asset).proxyPath.empty())
        return;
    const std::string path = m_model.asset(asset).proxyPath;
    m_model.setAssetProxy(asset, "");
    m_undoStack.markDirty();
    std::error_code ec;
    std::filesystem::remove(core::pathFromUtf8(path), ec); // ours, in the cache
    showStatus("Removed the proxy of " + m_model.asset(asset).displayName + ".");
    queueRefresh();
}

void AppWindow::offerProxiesAfterImport()
{
    std::vector<core::AssetId> tall;
    for (const core::Asset &asset : m_model.project().bin)
        if (proxyable(asset) && asset.info.height > kProxyThreshold && asset.proxyPath.empty() &&
            !(m_proxyQueue && m_proxyQueue->progressOf(asset.id)))
            tall.push_back(asset.id);
    if (tall.empty())
        return;
    const auto &settings = m_model.project().settings;
    const auto policy = settings.find(kProxySetting);
    if (policy != settings.end()) {
        if (policy->second == "always")
            createProxies(tall, m_settings->proxyHeight());
        return;
    }
    // Once per project (doc 13, Q8), remembered with it.
    const std::string body = std::to_string(tall.size()) + (tall.size() == 1 ? " clip is" : " clips are") +
                             " larger than 1080p. Proxies are smaller copies that play smoothly while you edit; "
                             "renders always use the originals.";
    AdwDialog *dialog = adw_alert_dialog_new("Create proxies for smoother editing?", body.c_str());
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "never", "Not for This Project", "create",
                                   "Create Proxies", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "create", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "create");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "never");
    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog), GTK_WIDGET(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer self) {
            auto *window = static_cast<AppWindow *>(self);
            const std::string response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
            window->m_model.setProjectSetting(kProxySetting, response == "create" ? "always" : "never");
            window->m_undoStack.markDirty();
            if (response == "create")
                window->offerProxiesAfterImport(); // now "always": makes them
        },
        this);
}

void AppWindow::addProxySettingsRow(AdwPreferencesGroup *group)
{
    const char *labels[] = {"540p", "720p", "1080p", "Source size (conformed)", nullptr};
    static constexpr int kHeights[] = {540, 720, 1080, 0};
    AdwComboRow *row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_combo_row_set_model(row, G_LIST_MODEL(gtk_string_list_new(labels)));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Proxy size");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "For proxies made from now on");
    setTooltip(GTK_WIDGET(row), "settings.proxy-size");
    const int current = m_settings->proxyHeight();
    guint selected = 0;
    for (guint i = 0; i < 4; ++i)
        if (kHeights[i] == current)
            selected = i;
    adw_combo_row_set_selected(row, selected);
    g_signal_connect(row, "notify::selected", G_CALLBACK(+[](AdwComboRow *combo, GParamSpec *, gpointer self) {
                         const guint index = adw_combo_row_get_selected(combo);
                         if (index < 4)
                             static_cast<AppWindow *>(self)->m_settings->setProxyHeight(kHeights[index]);
                     }),
                     this);
    adw_preferences_group_add(group, GTK_WIDGET(row));
}

} // namespace ustudio::app
