#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/autosave.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace ustudio::app::autosave;
namespace fs = std::filesystem;

namespace {

// meson.build points XDG_STATE_HOME at a build-dir-local scratch path for
// this test binary, so directory() never touches a real session's
// autosaves; the "doctest-" filename prefixes are defense in depth on top
// of that. removeIfExists() is still used mid-test (to clear state before
// writing), but final cleanup goes through Guard below so it runs even
// when a REQUIRE fails partway through (REQUIRE throws, per doctest,
// which would otherwise skip a plain removeIfExists() at the end of the
// test body -- exactly the case where leftover files matter most).
void removeIfExists(const std::string &path)
{
    std::error_code ec;
    fs::remove(path, ec);
}

struct Guard
{
    std::vector<std::string> paths;
    ~Guard()
    {
        for (const auto &path : paths)
            removeIfExists(path);
    }
};

} // namespace

TEST_CASE("autosave: directory() returns an existing, creatable path")
{
    std::string dir = directory();
    REQUIRE_FALSE(dir.empty());
    CHECK(fs::exists(dir));
    CHECK(fs::is_directory(dir));
}

TEST_CASE("autosave: baseNameFor is stable for the same input and differs for different input")
{
    std::string a = baseNameFor("/home/user/project.ustudio", "session-1");
    std::string b = baseNameFor("/home/user/project.ustudio", "session-1");
    CHECK(a == b);
    CHECK_FALSE(a.empty());

    std::string c = baseNameFor("/home/user/other.ustudio", "session-1");
    CHECK(a != c);

    // Untitled: the session id is what makes two untitled projects distinct.
    std::string untitledA = baseNameFor("", "session-1");
    std::string untitledB = baseNameFor("", "session-2");
    CHECK(untitledA != untitledB);
}

TEST_CASE("autosave: meta round-trips through write/read exactly")
{
    std::string metaPath = directory() + "/doctest-meta-roundtrip.meta";
    Guard guard{{metaPath}};
    removeIfExists(metaPath);

    Meta meta;
    meta.originalPath = "/home/user/a \"quoted\" \\ path.ustudio";
    meta.timestampUnix = 1234567890;

    REQUIRE(writeMeta(metaPath, meta));
    auto loaded = readMeta(metaPath);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == meta);
}

TEST_CASE("autosave: meta round-trips an empty (untitled) original path")
{
    std::string metaPath = directory() + "/doctest-meta-untitled.meta";
    Guard guard{{metaPath}};
    removeIfExists(metaPath);

    Meta meta;
    meta.originalPath.clear();
    meta.timestampUnix = 42;

    REQUIRE(writeMeta(metaPath, meta));
    auto loaded = readMeta(metaPath);
    REQUIRE(loaded.has_value());
    CHECK(loaded->originalPath.empty());
    CHECK(loaded->timestampUnix == 42);
}

TEST_CASE("autosave: readMeta fails cleanly on a missing or malformed file")
{
    CHECK_FALSE(readMeta(directory() + "/doctest-does-not-exist.meta").has_value());

    std::string badPath = directory() + "/doctest-malformed.meta";
    Guard guard{{badPath}};
    {
        std::ofstream out(badPath);
        out << "not json at all";
    }
    CHECK_FALSE(readMeta(badPath).has_value());
}

TEST_CASE("autosave: meta round-trips ownerPid (audit A5)")
{
    std::string metaPath = directory() + "/doctest-meta-pid.meta";
    Guard guard{{metaPath}};
    removeIfExists(metaPath);

    Meta meta;
    meta.originalPath = "/home/user/project.ustudio";
    meta.timestampUnix = 555;
    meta.ownerPid = 424242;

    REQUIRE(writeMeta(metaPath, meta));
    auto loaded = readMeta(metaPath);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == meta);
}

TEST_CASE("autosave: readMeta defaults ownerPid to 0 for a meta file written before the field existed (audit A5)")
{
    std::string metaPath = directory() + "/doctest-meta-legacy-pid.meta";
    Guard guard{{metaPath}};
    removeIfExists(metaPath);
    {
        std::ofstream out(metaPath);
        out << "{\"path\":\"\",\"timestamp\":10}"; // no "pid" key at all, matching a pre-A5 file
    }
    auto loaded = readMeta(metaPath);
    REQUIRE(loaded.has_value());
    CHECK(loaded->ownerPid == 0);
}

TEST_CASE("autosave: findRecoverable skips an entry owned by a still-live process (audit A5)")
{
    std::string base = baseNameFor("", "doctest-live-owner-session");
    std::string autosavePath = directory() + "/" + base + ".ustudio";
    std::string metaPath = directory() + "/" + base + ".meta";
    Guard guard{{autosavePath, metaPath}};
    removeIfExists(autosavePath);
    removeIfExists(metaPath);

    {
        std::ofstream(autosavePath) << "<mlt/>";
    }
    Meta meta;
    meta.originalPath.clear();
    meta.timestampUnix = 100;
    meta.ownerPid = static_cast<int64_t>(getpid()); // this very test process -- guaranteed alive
    REQUIRE(writeMeta(metaPath, meta));

    // Would otherwise be offered (same shape as the untitled-autosave test
    // above) -- the only difference is a live ownerPid.
    CHECK_FALSE(findRecoverable().has_value());
}

TEST_CASE("autosave: findRecoverable still offers an entry owned by a process that's no longer running (audit A5)")
{
    std::string base = baseNameFor("", "doctest-dead-owner-session");
    std::string autosavePath = directory() + "/" + base + ".ustudio";
    std::string metaPath = directory() + "/" + base + ".meta";
    Guard guard{{autosavePath, metaPath}};
    removeIfExists(autosavePath);
    removeIfExists(metaPath);

    {
        std::ofstream(autosavePath) << "<mlt/>";
    }

    // A forked-then-reaped child's pid is definitely not running any more
    // (its process table entry only exists until waitpid() reaps it) --
    // a deterministic stand-in for "a crashed session's now-dead owner".
    pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0)
        _exit(0);
    int status = 0;
    waitpid(child, &status, 0);

    Meta meta;
    meta.originalPath.clear();
    meta.timestampUnix = 100;
    meta.ownerPid = static_cast<int64_t>(child);
    REQUIRE(writeMeta(metaPath, meta));

    auto found = findRecoverable();
    REQUIRE(found.has_value());
    CHECK(found->autosavePath == autosavePath);
}

TEST_CASE("autosave: processStartTime returns a positive value for the current process (audit A3)")
{
    CHECK(processStartTime(static_cast<int64_t>(getpid())) > 0);
}

TEST_CASE("autosave: processStartTime returns 0 for a pid that no longer exists (audit A3)")
{
    pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0)
        _exit(0);
    int status = 0;
    waitpid(child, &status, 0);
    CHECK(processStartTime(static_cast<int64_t>(child)) == 0);
}

TEST_CASE("autosave: processStartTime returns 0 for pid <= 0 (audit A3)")
{
    CHECK(processStartTime(0) == 0);
    CHECK(processStartTime(-1) == 0);
}

TEST_CASE("autosave: meta round-trips ownerStartTime (audit A3)")
{
    std::string metaPath = directory() + "/doctest-meta-pid-start.meta";
    Guard guard{{metaPath}};
    removeIfExists(metaPath);

    Meta meta;
    meta.originalPath = "/home/user/project.ustudio";
    meta.timestampUnix = 555;
    meta.ownerPid = 424242;
    meta.ownerStartTime = 999999;

    REQUIRE(writeMeta(metaPath, meta));
    auto loaded = readMeta(metaPath);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == meta);
}

TEST_CASE("autosave: readMeta defaults ownerStartTime to 0 for a meta file written before the field existed "
         "(audit A3)")
{
    std::string metaPath = directory() + "/doctest-meta-legacy-pid-start.meta";
    Guard guard{{metaPath}};
    removeIfExists(metaPath);
    {
        std::ofstream out(metaPath);
        out << "{\"path\":\"\",\"timestamp\":10,\"pid\":123}"; // no "pid_start" key, matching a pre-A3 file
    }
    auto loaded = readMeta(metaPath);
    REQUIRE(loaded.has_value());
    CHECK(loaded->ownerPid == 123);
    CHECK(loaded->ownerStartTime == 0);
}

TEST_CASE("autosave: findRecoverable skips an entry whose pid is alive with a matching start time (audit A3)")
{
    std::string base = baseNameFor("", "doctest-matching-start-session");
    std::string autosavePath = directory() + "/" + base + ".ustudio";
    std::string metaPath = directory() + "/" + base + ".meta";
    Guard guard{{autosavePath, metaPath}};
    removeIfExists(autosavePath);
    removeIfExists(metaPath);
    { std::ofstream(autosavePath) << "<mlt/>"; }

    Meta meta;
    meta.originalPath.clear();
    meta.timestampUnix = 100;
    meta.ownerPid = static_cast<int64_t>(getpid());
    meta.ownerStartTime = processStartTime(static_cast<int64_t>(getpid())); // what performAutosave() itself writes
    REQUIRE(writeMeta(metaPath, meta));

    CHECK_FALSE(findRecoverable().has_value());
}

TEST_CASE("autosave: findRecoverable still offers an entry whose pid is alive but under a DIFFERENT process "
         "(reused pid, audit A3)")
{
    std::string base = baseNameFor("", "doctest-reused-pid-session");
    std::string autosavePath = directory() + "/" + base + ".ustudio";
    std::string metaPath = directory() + "/" + base + ".meta";
    Guard guard{{autosavePath, metaPath}};
    removeIfExists(autosavePath);
    removeIfExists(metaPath);
    { std::ofstream(autosavePath) << "<mlt/>"; }

    Meta meta;
    meta.originalPath.clear();
    meta.timestampUnix = 100;
    meta.ownerPid = static_cast<int64_t>(getpid()); // this test process -- genuinely alive
    // A start time that does not match this process's real one (this
    // suite runs well after boot, so a real starttime is never exactly
    // 1 tick) -- simulates an unrelated process having since reused
    // this same pid, the exact scenario audit A3 flagged: the OLD
    // pid-only check would have wrongly treated this as "owner still
    // alive" and hidden the autosave forever.
    meta.ownerStartTime = 1;
    REQUIRE(writeMeta(metaPath, meta));

    auto found = findRecoverable();
    REQUIRE(found.has_value());
    CHECK(found->autosavePath == autosavePath);
}

TEST_CASE("autosave: findRecoverable finds an untitled autosave with no target file")
{
    std::string base = baseNameFor("", "doctest-untitled-session");
    std::string autosavePath = directory() + "/" + base + ".ustudio";
    std::string metaPath = directory() + "/" + base + ".meta";
    Guard guard{{autosavePath, metaPath}};
    removeIfExists(autosavePath);
    removeIfExists(metaPath);

    {
        std::ofstream(autosavePath) << "<mlt/>";
    } // findRecoverable only checks existence + mtimes, not content
    Meta meta;
    meta.originalPath.clear();
    meta.timestampUnix = 100;
    REQUIRE(writeMeta(metaPath, meta));

    auto found = findRecoverable();
    REQUIRE(found.has_value());
    CHECK(found->autosavePath == autosavePath);
    CHECK(found->meta.originalPath.empty());
}

TEST_CASE("autosave: findRecoverable offers an autosave newer than its target, not one older")
{
    std::string targetPath = (fs::temp_directory_path() / "doctest-autosave-target.ustudio").string();
    Guard targetGuard{{targetPath}};
    {
        std::ofstream(targetPath) << "<mlt/>";
    }

    std::string base = baseNameFor(targetPath, "unused-when-path-is-set");
    std::string autosavePath = directory() + "/" + base + ".ustudio";
    std::string metaPath = directory() + "/" + base + ".meta";
    Guard guard{{autosavePath, metaPath}};
    removeIfExists(autosavePath);
    removeIfExists(metaPath);

    Meta meta;
    meta.originalPath = targetPath;
    meta.timestampUnix = 0;
    REQUIRE(writeMeta(metaPath, meta));

    // Pin both mtimes explicitly rather than relying on real-time
    // ordering between two writes, which is a real-clock race.
    auto targetTime = fs::file_time_type::clock::now();
    fs::last_write_time(targetPath, targetTime);

    // Older than the target: not recoverable.
    {
        std::ofstream(autosavePath) << "<mlt/>";
    }
    fs::last_write_time(autosavePath, targetTime - std::chrono::seconds(60));
    CHECK_FALSE(findRecoverable().has_value());

    // Newer than the target: recoverable.
    fs::last_write_time(autosavePath, targetTime + std::chrono::seconds(60));
    auto found = findRecoverable();
    REQUIRE(found.has_value());
    CHECK(found->autosavePath == autosavePath);
    CHECK(found->meta.originalPath == targetPath);
}

TEST_CASE("autosave: findRecoverable picks the most recently written of several qualifying candidates")
{
    // Reproduces a real report (2026-09-23): several independent untitled
    // sessions -- each left behind by "recover, edit further, quit
    // without ever doing an explicit Save" (cleanup is deliberately
    // deferred to a Save that may never come) -- can coexist in the
    // directory at once. Before this fix, findRecoverable() returned
    // whichever one a directory_iterator's UNSPECIFIED order happened to
    // yield first, not the one actually worth recovering.
    std::string baseOld = baseNameFor("", "doctest-multi-old-session");
    std::string baseNew = baseNameFor("", "doctest-multi-new-session");
    std::string oldAutosave = directory() + "/" + baseOld + ".ustudio";
    std::string oldMeta = directory() + "/" + baseOld + ".meta";
    std::string newAutosave = directory() + "/" + baseNew + ".ustudio";
    std::string newMeta = directory() + "/" + baseNew + ".meta";
    Guard guard{{oldAutosave, oldMeta, newAutosave, newMeta}};
    for (const auto &path : guard.paths)
        removeIfExists(path);

    { std::ofstream(oldAutosave) << "<mlt/>"; }
    Meta oldMetaData;
    oldMetaData.originalPath.clear();
    oldMetaData.timestampUnix = 100;
    REQUIRE(writeMeta(oldMeta, oldMetaData));

    { std::ofstream(newAutosave) << "<mlt/>"; }
    Meta newMetaData;
    newMetaData.originalPath.clear();
    newMetaData.timestampUnix = 200;
    REQUIRE(writeMeta(newMeta, newMetaData));

    auto found = findRecoverable();
    REQUIRE(found.has_value());
    CHECK(found->autosavePath == newAutosave);
    CHECK(found->meta.timestampUnix == 200);
}

TEST_CASE("autosave: findRecoverable's exclude set skips a candidate to surface the next one")
{
    // How AppWindow's recovery loop moves on to another orphaned autosave
    // after the owner has already been asked about (and answered for) the
    // most recent one -- recovering/discarding doesn't remove it from a
    // single findRecoverable() call's perspective (discard does delete
    // the file, but only after the caller has decided to move on), so the
    // caller excludes it explicitly instead.
    std::string baseA = baseNameFor("", "doctest-exclude-a-session");
    std::string baseB = baseNameFor("", "doctest-exclude-b-session");
    std::string autosaveA = directory() + "/" + baseA + ".ustudio";
    std::string metaA = directory() + "/" + baseA + ".meta";
    std::string autosaveB = directory() + "/" + baseB + ".ustudio";
    std::string metaB = directory() + "/" + baseB + ".meta";
    Guard guard{{autosaveA, metaA, autosaveB, metaB}};
    for (const auto &path : guard.paths)
        removeIfExists(path);

    { std::ofstream(autosaveA) << "<mlt/>"; }
    Meta metaDataA;
    metaDataA.originalPath.clear();
    metaDataA.timestampUnix = 100;
    REQUIRE(writeMeta(metaA, metaDataA));

    { std::ofstream(autosaveB) << "<mlt/>"; }
    Meta metaDataB;
    metaDataB.originalPath.clear();
    metaDataB.timestampUnix = 200; // more recent -- found first
    REQUIRE(writeMeta(metaB, metaDataB));

    auto first = findRecoverable();
    REQUIRE(first.has_value());
    CHECK(first->autosavePath == autosaveB);

    auto second = findRecoverable({first->metaPath});
    REQUIRE(second.has_value());
    CHECK(second->autosavePath == autosaveA);

    // Excluding both leaves nothing.
    CHECK_FALSE(findRecoverable({first->metaPath, second->metaPath}).has_value());
}

// doc 12, M1: "kill -9 during editing -> next launch offers recovery with
// <= 2 min lost". Microseconds, like g_get_monotonic_time().
TEST_CASE("autosaveDue: nothing pending never triggers")
{
    constexpr int64_t kMinute = 60'000'000;
    CHECK_FALSE(autosaveDue(100 * kMinute, 0, 0, 2 * kMinute, 10'000'000));
}

TEST_CASE("autosaveDue: idle for the delay after the last edit triggers (doc 09)")
{
    constexpr int64_t kSecond = 1'000'000;
    const int64_t delay = 120 * kSecond, heartbeat = 10 * kSecond;
    CHECK_FALSE(autosaveDue(1000 * kSecond, 1000 * kSecond, 1000 * kSecond, delay, heartbeat));
    CHECK_FALSE(autosaveDue(1100 * kSecond, 1000 * kSecond, 1000 * kSecond, delay, heartbeat));
    CHECK(autosaveDue(1120 * kSecond, 1000 * kSecond, 1000 * kSecond, delay, heartbeat));
}

TEST_CASE("autosaveDue: steady editing still autosaves, losing at most the delay")
{
    // An edit every 30 s, never idle long enough for the idle rule: the
    // max-age rule must fire, and at a heartbeat granularity that keeps the
    // oldest unsaved edit younger than the delay when the write happens.
    constexpr int64_t kSecond = 1'000'000;
    const int64_t delay = 120 * kSecond, heartbeat = 10 * kSecond;
    const int64_t firstEdit = 5000 * kSecond;
    int64_t firedAt = -1;
    for (int64_t now = firstEdit; now <= firstEdit + 600 * kSecond; now += heartbeat) {
        int64_t lastEdit = firstEdit + ((now - firstEdit) / (30 * kSecond)) * (30 * kSecond);
        if (autosaveDue(now, lastEdit, firstEdit, delay, heartbeat)) {
            firedAt = now;
            break;
        }
    }
    REQUIRE(firedAt >= 0);
    CHECK(firedAt - firstEdit <= delay);
    CHECK(firedAt - firstEdit >= delay - heartbeat);
}
