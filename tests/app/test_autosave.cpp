#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/autosave.h"

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace ustudio::app::autosave;
namespace fs = std::filesystem;

namespace {

// Directory() is fixed at $XDG_STATE_HOME/ustudio/autosave -- these tests
// share it with a real session's autosaves if run on a dev machine, so
// every test cleans up only the specific files it created, named with a
// "doctest-" prefix baked into each test's own original-path/sessionId
// arguments to keep them from colliding with anything real.
void removeIfExists(const std::string &path)
{
    std::error_code ec;
    fs::remove(path, ec);
}

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
    removeIfExists(metaPath);

    Meta meta;
    meta.originalPath = "/home/user/a \"quoted\" \\ path.ustudio";
    meta.timestampUnix = 1234567890;

    REQUIRE(writeMeta(metaPath, meta));
    auto loaded = readMeta(metaPath);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == meta);

    removeIfExists(metaPath);
}

TEST_CASE("autosave: meta round-trips an empty (untitled) original path")
{
    std::string metaPath = directory() + "/doctest-meta-untitled.meta";
    removeIfExists(metaPath);

    Meta meta;
    meta.originalPath.clear();
    meta.timestampUnix = 42;

    REQUIRE(writeMeta(metaPath, meta));
    auto loaded = readMeta(metaPath);
    REQUIRE(loaded.has_value());
    CHECK(loaded->originalPath.empty());
    CHECK(loaded->timestampUnix == 42);

    removeIfExists(metaPath);
}

TEST_CASE("autosave: readMeta fails cleanly on a missing or malformed file")
{
    CHECK_FALSE(readMeta(directory() + "/doctest-does-not-exist.meta").has_value());

    std::string badPath = directory() + "/doctest-malformed.meta";
    {
        std::ofstream out(badPath);
        out << "not json at all";
    }
    CHECK_FALSE(readMeta(badPath).has_value());
    removeIfExists(badPath);
}

TEST_CASE("autosave: findRecoverable finds an untitled autosave with no target file")
{
    std::string base = baseNameFor("", "doctest-untitled-session");
    std::string autosavePath = directory() + "/" + base + ".ustudio";
    std::string metaPath = directory() + "/" + base + ".meta";
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

    removeIfExists(autosavePath);
    removeIfExists(metaPath);
}

TEST_CASE("autosave: findRecoverable offers an autosave newer than its target, not one older")
{
    std::string targetPath = (fs::temp_directory_path() / "doctest-autosave-target.ustudio").string();
    {
        std::ofstream(targetPath) << "<mlt/>";
    }

    std::string base = baseNameFor(targetPath, "unused-when-path-is-set");
    std::string autosavePath = directory() + "/" + base + ".ustudio";
    std::string metaPath = directory() + "/" + base + ".meta";
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

    removeIfExists(autosavePath);
    removeIfExists(metaPath);
    removeIfExists(targetPath);
}
