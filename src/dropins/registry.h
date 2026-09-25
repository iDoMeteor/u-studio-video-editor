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
        const UStudioDropInDescription *describe = nullptr;
    };

    // $libdir/u-studio/drop-ins/ (baked in at build time). The Flatpak
    // extension mount joins this list in M7 (doc 17). Never the project's
    // folder or the user's home.
    static std::vector<std::string> trustedDirectories();
    // Where loadModules() looks: USTUDIO_DROPIN_PATH (colon-separated, a
    // development override that logs a warning) if set, else the trusted
    // directories.
    static std::vector<std::string> searchDirectories();

    void addBuiltin(const UStudioDropInDescription *describe);
    // Loads every libustudio-dropin-*.so in `directories` (default:
    // searchDirectories()). A module is refused -- logged and listed in
    // refusals() -- when it lacks ustudio_drop_in_describe, was built for
    // another DROPIN_API_VERSION or app release, has no name, or repeats
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
    const std::vector<std::string> &refusals() const
    {
        return m_refusals;
    }

  private:
    bool hasName(const std::string &name) const;
    std::vector<Entry> m_entries;
    std::vector<std::string> m_refusals;
};

} // namespace ustudio::dropins
