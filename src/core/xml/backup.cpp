#include "core/xml/backup.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace ustudio::core {

namespace {

constexpr const char *kBackupDir = ".ustudio-backups";
constexpr const char *kExtension = ".ustudio";

// `<stem>-YYYYMMDD-HHMMSS.ustudio` exactly, so project "foo" doesn't claim
// (and prune) project "foo-bar"'s backups.
bool isBackupOf(const std::string &name, const std::string &stem)
{
    const std::string ext = kExtension;
    const size_t stampLength = 15; // YYYYMMDD-HHMMSS
    if (name.size() != stem.size() + 1 + stampLength + ext.size())
        return false;
    if (name.compare(0, stem.size(), stem) != 0 || name[stem.size()] != '-')
        return false;
    if (name.compare(name.size() - ext.size(), ext.size(), ext) != 0)
        return false;
    const size_t start = stem.size() + 1;
    for (size_t i = 0; i < stampLength; ++i) {
        const char c = name[start + i];
        if (i == 8 ? c != '-' : !std::isdigit(static_cast<unsigned char>(c)))
            return false;
    }
    return true;
}

} // namespace

std::string backupBeforeOverwrite(const std::string &projectPath, std::time_t when)
{
    std::error_code ec;
    const fs::path project(projectPath);
    if (!fs::is_regular_file(project, ec))
        return {}; // a first save: nothing to keep

    const fs::path dir = project.parent_path() / kBackupDir;
    fs::create_directories(dir, ec);
    if (ec)
        return "Couldn't create " + dir.string() + ": " + ec.message();

    std::tm local{};
    localtime_r(&when, &local);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
    const std::string stem = project.stem().string();
    const fs::path target = dir / (stem + "-" + stamp + kExtension);
    // Two saves within one second: the backup already there holds the older
    // state, which is the one worth keeping.
    if (!fs::exists(target, ec)) {
        fs::copy_file(project, target, ec);
        if (ec)
            return "Couldn't back up " + projectPath + ": " + ec.message();
    }

    // The stamp sorts chronologically, so the newest are last by name.
    std::vector<fs::path> backups;
    for (const fs::directory_entry &entry : fs::directory_iterator(dir, ec))
        if (entry.is_regular_file() && isBackupOf(entry.path().filename().string(), stem))
            backups.push_back(entry.path());
    std::sort(backups.begin(), backups.end());
    for (size_t i = 0; i + kKeepBackups < backups.size(); ++i)
        fs::remove(backups[i], ec);
    return {};
}

} // namespace ustudio::core
