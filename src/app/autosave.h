#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>

namespace ustudio::app::autosave {

struct Meta
{
    std::string originalPath; // empty = untitled
    int64_t timestampUnix = 0;
    // The pid of the process that wrote this autosave (audit A5); 0 =
    // unknown (a meta file from before this field existed). The caller
    // (AppWindow::performAutosave) is responsible for filling this in --
    // this module only serializes whatever it's given.
    int64_t ownerPid = 0;
    // That pid's own start time (processStartTime() below) at the moment
    // it wrote this autosave; 0 = unknown (a meta file from before this
    // field existed, audit A3). ownerPid alone can be reused by an
    // unrelated process well within an orphaned autosave's lifetime --
    // findRecoverable() cross-checks this against the CURRENT process at
    // that pid, if any, so a coincidental reuse doesn't read as "the
    // original owner is still alive" and hide the autosave forever.
    int64_t ownerStartTime = 0;

    bool operator==(const Meta &) const = default;
};

// Field 22 (starttime, in clock ticks since boot) of /proc/<pid>/stat --
// 0 if unavailable (no such process, /proc unsupported, or pid <= 0).
// comm (field 2) is parenthesized and can itself contain spaces or
// parentheses, so this finds the LAST ')' on the line and counts fields
// after it, rather than naively splitting on spaces (verified against
// /proc/self/stat's own real field layout).
int64_t processStartTime(int64_t pid);

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

// Scans directory() for every *.meta file whose paired *.ustudio autosave
// is newer than the target it names, or names no target at all (doc 09:
// "autosaves newer than their targets, or with no target, are offered for
// recovery"), AND whose recorded ownerPid (if any) does not name a
// currently-running process (audit A5) -- otherwise a second instance
// could offer, and the owner could choose to discard, another still-live
// instance's own in-progress autosave. That "still running" check also
// cross-checks ownerStartTime, not just ownerPid (audit A3): a pid alone
// can be reused by an unrelated process well within an orphaned
// autosave's realistic lifetime, which would otherwise make
// findRecoverable() treat a long-dead owner as still alive and never
// offer its autosave at all. `excludeMetaPaths` skips specific
// candidates by their .meta path -- how AppWindow's recovery loop moves
// on to the next one after the caller has already offered (and the owner
// has answered for) one, without re-offering it forever: recovering
// doesn't delete the file (see AppWindow::offerRecoveryIfAny's own
// comment on why), so without this the very next call would just find
// the same one again.
//
// Returns the most recently written qualifying candidate (by
// Meta::timestampUnix), or nullopt if there's nothing to recover. Picking
// "most recent" matters once more than one exists: recovering an
// untitled session, editing further, and quitting without ever doing an
// explicit Save leaves that session's own autosave behind (its cleanup
// is deferred to a Save that may never come) AND starts a brand new one
// next launch under a fresh session id -- repeat that pattern a few
// times (an agent bouncing the app through many test relaunches is
// exactly this) and several unrelated untitled autosaves accumulate.
// Before this, whichever one a directory_iterator's unspecified order
// happened to return first won -- confirmed to reproduce a real report
// of "recovered project only had one track and no edits" when a richer,
// but not literally newest by directory order, autosave existed
// alongside older ones (2026-09-23).
std::optional<Recoverable> findRecoverable(const std::set<std::string> &excludeMetaPaths = {});

} // namespace ustudio::app::autosave
