#pragma once

// frei0r plugin discovery and curation (IP4; doc 15, "Keeping ADR-007's
// no-Qt guarantee"). Runs before Mlt::Factory::init(), so no MLT calls:
// it finds the plugin files MLT's frei0r module would load, and when any of
// them must not be (it names a Qt library, or the health probe quarantined
// it) builds a private directory linking only the rest, for FREI0R_PATH.

#include "core/health.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ustudio::effects {

struct Frei0rPlugin
{
    std::string name; // the file name up to its first '.': MLT's service is "frei0r.<name>"
    std::filesystem::path file;
    uintmax_t size = 0;
    int64_t modified = 0; // an opaque stamp, compared for equality only
};

// Where MLT's frei0r module looks: FREI0R_PATH, else MLT_FREI0R_PLUGIN_PATH,
// else its built-in list (mlt/src/modules/frei0r/factory.c, MLT 7.40),
// "$HOME" expanded -- as they were before this process (or its parent)
// replaced FREI0R_PATH, see rememberFrei0rSearchPath().
std::vector<std::filesystem::path> frei0rSearchDirs();

// Records the search path in the environment before FREI0R_PATH is
// replaced, so this process and its children keep seeing the system's
// plugins (the same fingerprint, the same probe candidates). Before
// Mlt::Factory::init(), while the process has one thread.
void rememberFrei0rSearchPath();

// Every plugin in `dirs` MLT would register, once per name. MLT walks the
// list from its *last* directory and keeps the first registration of a
// name (factory.c's `while (dircount--)` and its "already registered"
// check), so a later directory wins; so does this.
std::vector<Frei0rPlugin> findFrei0rPlugins(const std::vector<std::filesystem::path> &dirs);

// A stable fingerprint of the plugin set plus `salt` (the MLT version, the
// drop-in's own version): changes when a plugin is added, removed or
// replaced, which re-runs the registry and the health scan.
std::string pluginSetFingerprint(const std::vector<Frei0rPlugin> &plugins, const std::string &salt);

// Whether a plugin file names a Qt library among its dependencies ("libQt"
// in its dynamic string table; searched in the whole file, which can only
// err towards leaving a plugin out). ADR-007: Qt must never be mapped.
bool mentionsQt(const std::filesystem::path &file);

// $XDG_CACHE_HOME/ustudio (created on demand by the writers).
std::filesystem::path effectsCacheDir();
// The health scan's results (effect-health.json in effectsCacheDir()).
std::filesystem::path healthFilePath();

struct Frei0rCuration
{
    std::vector<std::string> paths;    // FREI0R_PATH, exactly
    std::vector<std::string> excluded; // "<name>: <why>", for the log
    std::filesystem::path curatedDir;  // empty when the system directories are used as they are
};

// What FREI0R_PATH should be: the system directories when every plugin may
// load, else a private directory (under $XDG_RUNTIME_DIR) of links to the
// ones that may. `qtCache` remembers mentionsQt() per file, size and stamp,
// so each file is read once per change.
Frei0rCuration curateFrei0r(const std::vector<Frei0rPlugin> &plugins, const HealthFile &health,
                            const std::filesystem::path &qtCache);

// --- LADSPA and VST2 (MLT's jackrack module, libmltladspa) -----------------
//
// MLT's plugin manager opens every ".so" under LADSPA_PATH and VST_PATH,
// recursively, when the factory starts; unlike OpenFX's, setting them
// replaces its built-in lists (mlt/src/modules/jackrack/plugin_mgr.c:377,
// :990, MLT 7.40). So they're curated as FREI0R_PATH is: exactly the system
// folders when nothing there names Qt, else a private folder of links to
// the files that don't (ADR-007).

enum class AudioHost
{
    Ladspa,
    Vst2,
};

// Every ".so" the host would open under audioSearchDirs(), recursively.
std::vector<Frei0rPlugin> audioHostFiles(AudioHost host);

// The experimental families (doc 15, "Backend: plugin families"): VST2 and
// OpenFX, off unless chosen on the Add page. $XDG_CONFIG_HOME/ustudio/
// effects.ini; read before Mlt::Factory::init(), so a change applies at the
// next start.
struct ExperimentalFamilies
{
    bool vst2 = false;
    bool openfx = false;
};
ExperimentalFamilies loadExperimentalFamilies();
bool saveExperimentalFamilies(const ExperimentalFamilies &families);

// Where the host looks: its variable as before this process (or its
// parent) replaced it, else MLT's built-in list.
std::vector<std::filesystem::path> audioSearchDirs(AudioHost host);
// Records the search paths before they're replaced (see
// rememberFrei0rSearchPath()).
void rememberAudioSearchPaths();

struct PluginCuration
{
    std::vector<std::string> paths;    // the variable's value, exactly
    std::vector<std::string> excluded; // "<file>: <why>", for the log
    std::filesystem::path curatedDir;  // empty when the system folders are used as they are
};
// What `host`'s variable should be. `enabled` false (VST2 without its
// experimental preference): a folder that isn't there, so nothing loads.
// `qtCache` remembers mentionsQt() per file, size and stamp.
PluginCuration curateAudioHost(AudioHost host, bool enabled, const std::filesystem::path &qtCache);

// --- OpenFX -------------------------------------------------------------------
//
// MLT's openfx module dlopens every bundle in /usr/OFX/Plugins,
// /usr/local/OFX/Plugins and OFX_PLUGIN_PATH at factory start, whatever
// OFX_PLUGIN_PATH says (factory.c:314-352), so it can only be allowed when
// nothing it would open names Qt.

// Where it looks: the two fixed folders, then OFX_PLUGIN_PATH.
std::vector<std::filesystem::path> openfxSearchDirs();
// The bundles there that name Qt (any file under a "*.ofx.bundle"'s
// Contents, whatever its architecture: more than MLT opens, never less).
// MLT looks one folder deep, as this does.
std::vector<std::filesystem::path> openfxBundlesNamingQt(const std::vector<std::filesystem::path> &dirs);

} // namespace ustudio::effects
