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
