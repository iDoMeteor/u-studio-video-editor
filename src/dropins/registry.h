#pragma once

#include "dropins/api.h"
#include "dropins/dropin_host.h"

#include <string>
#include <vector>

namespace ustudio::dropins {

// The drop-ins this run has: built-ins (from the generated drop_ins.h) and
// modules found in trusted locations. Main thread, at startup:
//   registry.addBuiltin(...);  registry.loadModules();
//   registry.contributeFactoryPaths(paths);   // before Mlt::Factory::init
//   registry.registerAll(host);               // after it
class DropInRegistry
{
  public:
    struct Entry
    {
        std::string name;
        std::string description;
        std::string path; // the module file; "" for a built-in
        // Null for a disabled module: it isn't opened, so its name comes
        // from its file (libustudio-dropin-<name>.so).
        const UStudioDropInDescription *describe = nullptr;
        std::string version; // the app release it was built for; "" when not opened
        bool enabled = true; // false: listed, but no factory paths and no registration
    };

    // $libdir/u-studio/drop-ins/ (baked in at build time), then, when the
    // build has a dropin_extension_dir (the Flatpak extension point, doc
    // 17), <that dir>/<extension>/lib/u-studio/drop-ins/ for each installed
    // extension. Never the project's folder or the user's home.
    static std::vector<std::string> trustedDirectories();
    // <extensionPoint>/<extension>/lib/u-studio/drop-ins/ for each
    // subdirectory of extensionPoint, sorted; none if it's "" or missing.
    static std::vector<std::string> extensionDirectories(const std::string &extensionPoint);
    // Where loadModules() looks: USTUDIO_DROPIN_PATH (colon-separated, a
    // development override that logs a warning) if set, else the trusted
    // directories.
    static std::vector<std::string> searchDirectories();

    // Settings > Drop-ins (GSettings "disabled-drop-ins"): set before
    // addBuiltin()/loadModules(). A disabled module isn't even opened, so a
    // drop-in that crashes on load can be switched off.
    void setDisabled(std::vector<std::string> names)
    {
        m_disabled = std::move(names);
    }
    // The drop-ins this build knows of (the dropin_<name> options), for
    // pointing at ones that aren't installed.
    void setKnown(std::vector<std::string> names)
    {
        m_known = std::move(names);
    }
    const std::vector<std::string> &known() const
    {
        return m_known;
    }

    void addBuiltin(const UStudioDropInDescription *describe);
    // Loads every libustudio-dropin-*.so in `directories` (default:
    // searchDirectories()). A module is refused -- logged and listed in
    // refusals() -- when it lacks ustudio_drop_in_describe, was built for
    // another DROPIN_API_VERSION or app release, has no name, has a name
    // other than its file's (disabling goes by the file name), or repeats
    // one already registered (a built-in wins).
    void loadModules(const std::vector<std::string> &directories = searchDirectories());
    // Why a single module would be refused, or "" (for tests and loadModules).
    static std::string problemWith(const UStudioDropInDescription *describe, const std::string &appVersion);

    void contributeFactoryPaths(FactoryPaths &paths) const;
    void registerAll(DropInHost &host) const;

    const std::vector<Entry> &entries() const
    {
        return m_entries;
    }
    // Loaded and enabled (so its effects play).
    bool has(const std::string &name) const;

    // The program's registry (main() sets it once, at startup), for code
    // that needs to know which drop-ins this run has; null in tests.
    static const DropInRegistry *current();
    static void setCurrent(const DropInRegistry *registry);
    const std::vector<std::string> &refusals() const
    {
        return m_refusals;
    }

  private:
    bool hasName(const std::string &name) const;
    bool isDisabled(const std::string &name) const;
    std::vector<Entry> m_entries;
    std::vector<std::string> m_refusals;
    std::vector<std::string> m_disabled;
    std::vector<std::string> m_known;
};

} // namespace ustudio::dropins
