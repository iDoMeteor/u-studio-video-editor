#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace ustudio::app::autosave {

struct Meta
{
    std::string originalPath; // empty = untitled
    int64_t timestampUnix = 0;

    bool operator==(const Meta &) const = default;
};

// $XDG_STATE_HOME/ustudio/autosave, created if missing. Empty on failure.
std::string directory();

// The filename base shared by "<base>.ustudio" (the autosaved project) and
// "<base>.meta" (its sidecar) -- sha1(originalPath), or
// sha1("untitled-" + sessionId) when there's no saved target yet (doc 09).
// sessionId should be a fresh id per app launch (e.g. a UUID), so two
// untitled sessions running at once don't autosave over each other.
std::string baseNameFor(const std::string &originalPath, const std::string &sessionId);

bool writeMeta(const std::string &metaPath, const Meta &meta);
std::optional<Meta> readMeta(const std::string &metaPath);

struct Recoverable
{
    std::string autosavePath;
    std::string metaPath;
    Meta meta;
};

// Scans directory() for a *.meta file whose paired *.ustudio autosave is
// newer than the target it names, or names no target at all (doc 09:
// "autosaves newer than their targets, or with no target, are offered for
// recovery"). Returns the first one found, or nullopt if there's nothing
// to recover.
std::optional<Recoverable> findRecoverable();

} // namespace ustudio::app::autosave
