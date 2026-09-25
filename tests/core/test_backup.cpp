#include "core/xml/backup.h"

#include "doctest.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace ustudio;

namespace {

struct TempDir
{
    fs::path path = fs::temp_directory_path() / ("ustudio-backup-test-" + std::to_string(::getpid()));
    TempDir()
    {
        fs::create_directories(path);
    }
    ~TempDir()
    {
        fs::remove_all(path);
    }
};

void writeFile(const fs::path &path, const std::string &text)
{
    std::ofstream(path) << text;
}

std::string readFile(const fs::path &path)
{
    std::ostringstream text;
    text << std::ifstream(path).rdbuf();
    return text.str();
}

std::vector<std::string> backupNames(const fs::path &dir)
{
    std::vector<std::string> names;
    for (const auto &entry : fs::directory_iterator(dir / ".ustudio-backups"))
        names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

// 2026-09-25 12:00:00 local, whatever the test machine's zone.
std::time_t noonLocal()
{
    std::tm tm{};
    tm.tm_year = 126;
    tm.tm_mon = 8;
    tm.tm_mday = 25;
    tm.tm_hour = 12;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

} // namespace

TEST_CASE("backupBeforeOverwrite: nothing to keep on a first save")
{
    TempDir tmp;
    CHECK(core::backupBeforeOverwrite((tmp.path / "show.ustudio").string(), noonLocal()).empty());
    CHECK_FALSE(fs::exists(tmp.path / ".ustudio-backups"));
}

TEST_CASE("backupBeforeOverwrite: copies the old file, keeps the newest five")
{
    TempDir tmp;
    const fs::path project = tmp.path / "show.ustudio";
    const fs::path other = tmp.path / ".ustudio-backups" / "show-extra-20200101-000000.ustudio";
    for (int i = 0; i < 7; ++i) {
        writeFile(project, "version " + std::to_string(i));
        CHECK(core::backupBeforeOverwrite(project.string(), noonLocal() + i).empty());
        if (i == 0)
            writeFile(other, "another project's backup");
    }
    CHECK(readFile(project) == "version 6"); // untouched

    const std::vector<std::string> names = backupNames(tmp.path);
    REQUIRE(names.size() == 6); // five of ours plus show-extra's
    CHECK(names[0] == "show-20260925-120002.ustudio");
    CHECK(names[4] == "show-20260925-120006.ustudio");
    CHECK(names[5] == "show-extra-20200101-000000.ustudio");
    CHECK(readFile(tmp.path / ".ustudio-backups" / names[4]) == "version 6");
}

TEST_CASE("backupBeforeOverwrite: a second save in the same second keeps the older copy")
{
    TempDir tmp;
    const fs::path project = tmp.path / "show.ustudio";
    writeFile(project, "first");
    CHECK(core::backupBeforeOverwrite(project.string(), noonLocal()).empty());
    writeFile(project, "second");
    CHECK(core::backupBeforeOverwrite(project.string(), noonLocal()).empty());
    CHECK(readFile(tmp.path / ".ustudio-backups" / "show-20260925-120000.ustudio") == "first");
}
