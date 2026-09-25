#pragma once

#include "core/render/render_profile.h"

#include <glib.h>

#include <optional>
#include <string>
#include <vector>

namespace ustudio::app {

// The user's render profiles, in a keyfile (one group per profile), plus
// the read-only built-ins. The default profile's name lives in GSettings
// (Settings::defaultRenderProfile()). Main thread only.
// One profile as a keyfile group (its name is the group's): shared by the
// store and pending-renders' files. readProfile() is nullopt for a group
// that isn't a valid profile.
void writeProfile(GKeyFile *file, const char *group, const core::RenderProfile &profile);
std::optional<core::RenderProfile> readProfile(GKeyFile *file, const char *group);

class RenderProfileStore
{
  public:
    // `path` is the keyfile; defaultPath() for the real one.
    explicit RenderProfileStore(std::string path);

    // $XDG_CONFIG_HOME/ustudio/render-profiles.ini
    static std::string defaultPath();

    // Built-ins first, then the user's in file order.
    std::vector<core::RenderProfile> all() const;
    const std::vector<core::RenderProfile> &userProfiles() const
    {
        return m_user;
    }
    std::optional<core::RenderProfile> find(const std::string &name) const;

    // Adds `profile`, or replaces the one called `previousName` ("" adds).
    // Returns why not, or "" once it's written.
    std::string save(const core::RenderProfile &profile, const std::string &previousName);
    std::string remove(const std::string &name);

  private:
    std::string write() const;

    std::string m_path;
    std::vector<core::RenderProfile> m_user;
};

} // namespace ustudio::app
