#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ustudio::app {

// Launched from a snap app's terminal (VS Code's, on the owner's machine),
// the editor inherits that snap's environment: GIO/GTK module paths, locale
// data and XDG dirs inside /snap or ~/snap. It then loads the snap's GIO
// modules itself, and everything it launches (the player behind "Open
// Render") fails the same way ("undefined symbol: fido_dev_has_uv"). Unless
// the editor is itself a snap, those values are undone before anything
// reads them: a variable VS Code saved as <NAME>_VSCODE_SNAP_ORIG gets its
// original back; module and data paths pointing into a snap are unset;
// search paths lose their snap entries; SNAP* variables go. Values that
// don't point into a snap are left alone (GSETTINGS_SCHEMA_DIR=builddir/data
// stays).

struct EnvChange
{
    std::string name;
    std::optional<std::string> value; // nullopt: unset
};

// What to change in `env` (pure, for tests). `home` is $HOME.
std::vector<EnvChange> snapEnvironmentFixes(const std::map<std::string, std::string> &env, const std::string &home);

// Applies them to this process unless /proc/self/exe is under /snap. Call
// first thing in main(), before GLib caches any XDG directory. Returns the
// names changed, for one log line once logging is up.
std::vector<std::string> scrubSnapEnvironment();

} // namespace ustudio::app
