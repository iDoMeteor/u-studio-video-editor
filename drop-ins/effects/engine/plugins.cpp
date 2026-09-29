#include "engine/plugins.h"

#include "core/json.h"
#include "core/log.h"
#include "platform/files.h"

#include <glib.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

namespace ustudio::effects {

namespace fs = std::filesystem;

namespace {

// factory.c's FREI0R_PLUGIN_PATH for a non-relocatable Unix build.
constexpr const char *kMltDefaultFrei0rPath =
    "/usr/lib/frei0r-1:/usr/lib64/frei0r-1:/opt/local/lib/frei0r-1:/usr/local/lib/frei0r-1:$HOME/.frei0r-1/lib";

#ifdef _WIN32
constexpr char kListSeparator = ';';
constexpr const char *kPluginSuffix = ".dll";
#else
constexpr char kListSeparator = ':';
constexpr const char *kPluginSuffix = ".so";
#endif

std::vector<std::string> split(const std::string &text, char separator)
{
    std::vector<std::string> parts;
    std::string part;
    std::istringstream in(text);
    while (std::getline(in, part, separator))
        if (!part.empty())
            parts.push_back(part);
    return parts;
}

uint64_t fnv1a(const std::string &text)
{
    uint64_t hash = 1469598103934665603ULL;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hex(uint64_t value)
{
    static const char *kHex = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<size_t>(i)] = kHex[value & 0xF];
        value >>= 4;
    }
    return out;
}

std::string fileKey(const Frei0rPlugin &plugin)
{
    return plugin.file.string() + "|" + std::to_string(plugin.size) + "|" + std::to_string(plugin.modified);
}

std::optional<Json> readJsonFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parseJson(text);
}

void writeJsonFile(const fs::path &path, const Json &json)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // Temp file + rename: a reader never sees half a file.
    const fs::path temp = path.string() + ".part";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << toJson(json) << '\n';
        if (!out)
            return;
    }
    fs::rename(temp, path, ec);
}

// The directory of links, removed when the process ends (it must outlive
// every frei0r filter: MLT dlopens a plugin by the linked path each time it
// creates one).
class CuratedDirCleanup
{
  public:
    void add(const fs::path &dir)
    {
        m_dirs.push_back(dir);
    }
    ~CuratedDirCleanup()
    {
        for (const fs::path &dir : m_dirs) {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
    }

  private:
    std::vector<fs::path> m_dirs;
};

CuratedDirCleanup &curatedDirCleanup()
{
    static CuratedDirCleanup cleanup;
    return cleanup;
}

constexpr const char *kSearchPathVariable = "USTUDIO_FREI0R_SEARCH_PATH";

} // namespace

void rememberFrei0rSearchPath()
{
    if (std::getenv(kSearchPathVariable))
        return;
    std::string list;
    for (const fs::path &dir : frei0rSearchDirs()) {
        if (!list.empty())
            list += kListSeparator;
        list += dir.string();
    }
    g_setenv(kSearchPathVariable, list.c_str(), TRUE);
}

std::vector<fs::path> frei0rSearchDirs()
{
    // Set by rememberFrei0rSearchPath() before this process replaced
    // FREI0R_PATH, and inherited by its children (the probe), so they see
    // the system's plugins, not the parent's curated directory.
    const char *fromEnv = std::getenv(kSearchPathVariable);
    if (!fromEnv || !*fromEnv)
        fromEnv = std::getenv("FREI0R_PATH");
    if (!fromEnv || !*fromEnv)
        fromEnv = std::getenv("MLT_FREI0R_PLUGIN_PATH");
    const std::string list = fromEnv && *fromEnv ? fromEnv : kMltDefaultFrei0rPath;
    std::vector<fs::path> dirs;
    // A package's own plugins first (the Flatpak extension's; no
    // FREI0R_PATH in the sandbox), when it has them. The remembered list
    // (a child's) already starts with them.
    std::error_code ec;
    const fs::path packaged = EFFECTS_FREI0R_INSTALL_DIR;
    if (fs::is_directory(packaged, ec))
        dirs.push_back(packaged);
    const char *home = g_get_home_dir();
    for (const std::string &entry : split(list, kListSeparator)) {
        fs::path dir = entry.starts_with("$HOME") ? fs::path(home ? home : "") / fs::path(entry.substr(5)).relative_path()
                                                  : fs::path(entry);
        if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end())
            dirs.push_back(std::move(dir));
    }
    return dirs;
}

std::vector<Frei0rPlugin> findFrei0rPlugins(const std::vector<fs::path> &dirs)
{
    std::vector<Frei0rPlugin> plugins;
    std::set<std::string> seen;
    for (auto dir = dirs.rbegin(); dir != dirs.rend(); ++dir) {
        std::error_code ec;
        std::vector<fs::path> files;
        for (const fs::directory_entry &entry : fs::directory_iterator(*dir, ec))
            if (entry.path().extension() == kPluginSuffix)
                files.push_back(entry.path());
        std::sort(files.begin(), files.end()); // MLT's dir listing is sorted too
        for (const fs::path &file : files) {
            const std::string filename = file.filename().string();
            const std::string name = filename.substr(0, filename.find('.'));
            if (name.empty() || !seen.insert(name).second)
                continue;
            Frei0rPlugin plugin{name, file, 0, 0};
            std::error_code statEc;
            plugin.size = fs::file_size(file, statEc);
            const auto modified = fs::last_write_time(file, statEc);
            if (!statEc)
                plugin.modified = static_cast<int64_t>(modified.time_since_epoch().count());
            plugins.push_back(std::move(plugin));
        }
    }
    std::sort(plugins.begin(), plugins.end(),
              [](const Frei0rPlugin &a, const Frei0rPlugin &b) { return a.name < b.name; });
    return plugins;
}

std::string pluginSetFingerprint(const std::vector<Frei0rPlugin> &plugins, const std::string &salt)
{
    std::string text = salt;
    for (const Frei0rPlugin &plugin : plugins)
        text += "\n" + plugin.name + "|" + fileKey(plugin);
    return hex(fnv1a(text));
}

bool mentionsQt(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return false;
    static constexpr std::string_view kNeedle = "libQt";
    // Read in chunks, carrying the needle's length minus one across
    // boundaries.
    std::string carry;
    std::vector<char> buffer(1 << 16);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = static_cast<size_t>(in.gcount());
        if (got == 0)
            break;
        std::string chunk = carry + std::string(buffer.data(), got);
        if (chunk.find(kNeedle) != std::string::npos)
            return true;
        carry = chunk.substr(chunk.size() - std::min(chunk.size(), kNeedle.size() - 1));
    }
    return false;
}

fs::path effectsCacheDir()
{
    const char *cache = g_get_user_cache_dir();
    return fs::path(cache ? cache : ".") / "ustudio";
}

fs::path healthFilePath()
{
    return effectsCacheDir() / "effect-health.json";
}

Frei0rCuration curateFrei0r(const std::vector<Frei0rPlugin> &plugins, const HealthFile &health, const fs::path &qtCache)
{
    // Which files name Qt, remembered by file, size and stamp.
    std::map<std::string, bool> known;
    if (std::optional<Json> cached = readJsonFile(qtCache))
        for (const auto &[key, value] : (*cached)["files"].asObject())
            known[key] = value.asBool();
    std::map<std::string, bool> now;
    bool changed = false;
    Frei0rCuration curation;
    std::vector<const Frei0rPlugin *> allowed;
    for (const Frei0rPlugin &plugin : plugins) {
        const std::string key = fileKey(plugin);
        bool qt = false;
        if (auto it = known.find(key); it != known.end()) {
            qt = it->second;
        } else {
            qt = mentionsQt(plugin.file);
            changed = true;
        }
        now[key] = qt;
        if (qt)
            curation.excluded.push_back(plugin.name + ": links Qt (ADR-007)");
        else if (health.quarantined("frei0r." + plugin.name))
            curation.excluded.push_back(plugin.name + ": " + health.find("frei0r." + plugin.name)->reason);
        else
            allowed.push_back(&plugin);
    }
    if (changed || now.size() != known.size()) {
        Json files;
        for (const auto &[key, qt] : now)
            files.set(key, qt);
        Json out;
        out.set("files", files.isNull() ? Json(Json::Object{}) : files);
        writeJsonFile(qtCache, out);
    }

    if (curation.excluded.empty()) {
        for (const fs::path &dir : frei0rSearchDirs())
            curation.paths.push_back(dir.string());
        return curation;
    }

    // Beside FactoryPolicy's curated module directory, and for the same
    // reason private (its contents get loaded into the process).
    const char *runtime = g_get_user_runtime_dir();
    std::string error;
    fs::path dir = platform::makePrivateDirectory(fs::path(runtime ? runtime : "."), "ustudio-frei0r-", &error);
    if (dir.empty()) {
        // Without the directory nothing can be left out, so nothing loads:
        // safer than loading what must not be.
        core::Log::warn("[effects] no curated frei0r directory (" + error + "): frei0r effects are off this session");
        curation.excluded = {"all: no curated directory"};
        // FREI0R_PATH must still be set (an empty contribution leaves MLT
        // its defaults): to a directory that isn't there.
        curation.paths = {(effectsCacheDir() / "no-frei0r").string()};
        return curation;
    }
    curatedDirCleanup().add(dir);
    for (const Frei0rPlugin *plugin : allowed) {
        std::string linkError;
        if (!platform::linkFile(plugin->file, dir / plugin->file.filename(), &linkError))
            core::Log::warn("[effects] frei0r plugin " + plugin->name + " not linked: " + linkError);
    }
    curation.paths = {dir.string()};
    curation.curatedDir = dir;
    return curation;
}

// --- LADSPA and VST2 --------------------------------------------------------

namespace {

const char *hostVariable(AudioHost host)
{
    return host == AudioHost::Ladspa ? "LADSPA_PATH" : "VST_PATH";
}

const char *rememberedVariable(AudioHost host)
{
    return host == AudioHost::Ladspa ? "USTUDIO_LADSPA_SEARCH_PATH" : "USTUDIO_VST_SEARCH_PATH";
}

// MLT's built-in lists (plugin_mgr.c, Linux).
const char *builtInList(AudioHost host)
{
    return host == AudioHost::Ladspa ? "/usr/local/lib/ladspa:/usr/lib/ladspa:/usr/lib64/ladspa"
                                     : "/usr/local/lib/vst:/usr/lib/vst:/usr/lib64/vst";
}

std::string joined(const std::vector<fs::path> &dirs)
{
    std::string list;
    for (const fs::path &dir : dirs) {
        if (!list.empty())
            list += kListSeparator;
        list += dir.string();
    }
    return list;
}

} // namespace

std::vector<Frei0rPlugin> audioHostFiles(AudioHost host)
{
    // Every ".so" the host would open, recursively (plugin_mgr.c follows
    // folders), in its order.
    std::vector<Frei0rPlugin> files;
    for (const fs::path &dir : audioSearchDirs(host)) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code statEc;
            if (it->path().extension() != ".so" || !it->is_regular_file(statEc))
                continue;
            Frei0rPlugin file;
            file.name = it->path().filename().string();
            file.file = it->path();
            file.size = it->file_size(statEc);
            file.modified = static_cast<int64_t>(it->last_write_time(statEc).time_since_epoch().count());
            files.push_back(std::move(file));
        }
    }
    return files;
}

ExperimentalFamilies loadExperimentalFamilies()
{
    ExperimentalFamilies families;
    GKeyFile *file = g_key_file_new();
    const std::string path = (fs::path(g_get_user_config_dir()) / "ustudio" / "effects.ini").string();
    if (g_key_file_load_from_file(file, path.c_str(), G_KEY_FILE_NONE, nullptr)) {
        families.vst2 = g_key_file_get_boolean(file, "experimental", "vst2", nullptr);
        families.openfx = g_key_file_get_boolean(file, "experimental", "openfx", nullptr);
    }
    g_key_file_free(file);
    return families;
}

bool saveExperimentalFamilies(const ExperimentalFamilies &families)
{
    const fs::path dir = fs::path(g_get_user_config_dir()) / "ustudio";
    std::error_code ec;
    fs::create_directories(dir, ec);
    GKeyFile *file = g_key_file_new();
    g_key_file_set_boolean(file, "experimental", "vst2", families.vst2);
    g_key_file_set_boolean(file, "experimental", "openfx", families.openfx);
    const bool saved = g_key_file_save_to_file(file, (dir / "effects.ini").string().c_str(), nullptr);
    g_key_file_free(file);
    return saved;
}

std::vector<fs::path> audioSearchDirs(AudioHost host)
{
    const char *fromEnv = std::getenv(rememberedVariable(host));
    if (!fromEnv || !*fromEnv)
        fromEnv = std::getenv(hostVariable(host));
    const std::string list = fromEnv && *fromEnv ? fromEnv : builtInList(host);
    std::vector<fs::path> dirs;
    for (const std::string &entry : split(list, kListSeparator))
        if (!entry.empty())
            dirs.emplace_back(entry);
    return dirs;
}

void rememberAudioSearchPaths()
{
    for (AudioHost host : {AudioHost::Ladspa, AudioHost::Vst2})
        if (!std::getenv(rememberedVariable(host)))
            g_setenv(rememberedVariable(host), joined(audioSearchDirs(host)).c_str(), TRUE);
}

PluginCuration curateAudioHost(AudioHost host, bool enabled, const fs::path &qtCache)
{
    PluginCuration curation;
    const std::string what = host == AudioHost::Ladspa ? "ladspa" : "vst";
    if (!enabled) {
        curation.paths = {(effectsCacheDir() / ("no-" + what)).string()};
        return curation;
    }
    const std::vector<Frei0rPlugin> files = audioHostFiles(host);
    std::map<std::string, bool> known;
    if (std::optional<Json> cached = readJsonFile(qtCache))
        for (const auto &[key, value] : (*cached)["files"].asObject())
            known[key] = value.asBool();
    std::map<std::string, bool> now = known;
    bool changed = false;
    std::vector<const Frei0rPlugin *> allowed;
    for (const Frei0rPlugin &file : files) {
        const std::string key = fileKey(file);
        bool qt = false;
        if (auto it = known.find(key); it != known.end()) {
            qt = it->second;
        } else {
            qt = mentionsQt(file.file);
            now[key] = qt;
            changed = true;
        }
        if (qt)
            curation.excluded.push_back(file.file.string() + ": links Qt (ADR-007)");
        else
            allowed.push_back(&file);
    }
    if (changed) {
        Json cache;
        for (const auto &[key, qt] : now)
            cache.set(key, qt);
        Json out;
        out.set("files", cache.isNull() ? Json(Json::Object{}) : cache);
        writeJsonFile(qtCache, out);
    }
    if (curation.excluded.empty()) {
        for (const fs::path &dir : audioSearchDirs(host))
            curation.paths.push_back(dir.string());
        return curation;
    }
    const char *runtime = g_get_user_runtime_dir();
    std::string error;
    fs::path dir = platform::makePrivateDirectory(fs::path(runtime ? runtime : "."), "ustudio-" + what + "-", &error);
    if (dir.empty()) {
        core::Log::warn("[effects] no curated " + what + " directory (" + error + "): " + what +
                        " plugins are off this session");
        curation.paths = {(effectsCacheDir() / ("no-" + what)).string()};
        return curation;
    }
    curatedDirCleanup().add(dir);
    // Flat: names from different folders may clash, so the second
    // "delay.so" becomes "delay-2.so" (the host only wants ".so").
    std::set<std::string> taken;
    for (const Frei0rPlugin *file : allowed) {
        std::string name = file->file.filename().string();
        for (int n = 2; !taken.insert(name).second; ++n)
            name = file->file.stem().string() + "-" + std::to_string(n) + ".so";
        std::string linkError;
        if (!platform::linkFile(file->file, dir / name, &linkError))
            core::Log::warn("[effects] " + what + " plugin " + file->file.string() + " not linked: " + linkError);
    }
    curation.paths = {dir.string()};
    curation.curatedDir = dir;
    return curation;
}

} // namespace ustudio::effects
