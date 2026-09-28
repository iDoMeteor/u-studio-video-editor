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

} // namespace ustudio::effects
