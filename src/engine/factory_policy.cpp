#include "factory_policy.h"

#include "core/log.h"

#include <mlt++/Mlt.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifndef USTUDIO_MLT_MODULE_DIR
#error "USTUDIO_MLT_MODULE_DIR must be supplied by the build (see src/engine/meson.build)"
#endif

namespace ustudio::engine {

namespace Log = ustudio::core::Log;

namespace {

namespace fs = std::filesystem;

// Denylist is filename-substring matching, e.g. "qt6" matches both
// libmltqt6.so and libmltglaxnimate-qt6.so. Overridable via
// USTUDIO_MLT_DENYLIST (colon-separated, matching MLT's own
// MLT_REPOSITORY_DENY convention) for debugging.
std::vector<std::string> denylist()
{
    if (const char *override = std::getenv("USTUDIO_MLT_DENYLIST"); override && *override) {
        std::vector<std::string> result;
        std::string value = override;
        size_t start = 0;
        while (start <= value.size()) {
            size_t colon = value.find(':', start);
            if (colon == std::string::npos) {
                result.push_back(value.substr(start));
                break;
            }
            result.push_back(value.substr(start, colon - start));
            start = colon + 1;
        }
        return result;
    }
    return {"qt6", "glaxnimate-qt6"};
}

bool isDenied(const std::string &filename, const std::vector<std::string> &deny)
{
    for (const auto &pattern : deny) {
        if (!pattern.empty() && filename.find(pattern) != std::string::npos)
            return true;
    }
    return false;
}

// XDG_RUNTIME_DIR is only set inside a logind session; CI containers and
// other headless environments don't have one. Fall back to the process's
// system temp directory (std::filesystem::temp_directory_path(), which
// itself honours TMPDIR) rather than degrading to unsafe default MLT
// init -- ADR-007's whole point is that Qt6 never loads, and a temp-dir
// location is just as valid a place for the curated symlink farm.
fs::path moduleCacheBaseDir()
{
    if (const char *runtimeDir = std::getenv("XDG_RUNTIME_DIR"); runtimeDir && *runtimeDir)
        return fs::path(runtimeDir);

    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec);
    if (ec) {
        Log::warn("[engine] FactoryPolicy: XDG_RUNTIME_DIR not set and no system temp directory found (" +
                  ec.message() + "), falling back to default MLT module loading");
        return {};
    }

    Log::info("[engine] FactoryPolicy: XDG_RUNTIME_DIR not set, using " + tmp.string() + " instead");
    return tmp;
}

// Builds (or refreshes) a curated module directory containing symlinks to
// every module in USTUDIO_MLT_MODULE_DIR except the denylist. Returns the
// curated directory path, or empty on any failure (caller falls back to
// default init).
std::string buildCuratedModuleDir()
{
    fs::path base = moduleCacheBaseDir();
    if (base.empty())
        return {};

    fs::path curated = base / "ustudio-mlt-modules";
    fs::path source = USTUDIO_MLT_MODULE_DIR;

    std::error_code ec;
    fs::remove_all(curated, ec); // refresh: drop any stale symlinks from a previous run
    fs::create_directories(curated, ec);
    if (ec) {
        Log::warn("[engine] FactoryPolicy: could not create " + curated.string() + " (" + ec.message() +
                  "), falling back to default MLT module loading");
        return {};
    }

    if (!fs::exists(source, ec) || ec) {
        Log::warn("[engine] FactoryPolicy: MLT module source dir " + source.string() +
                  " not found, falling back to default MLT module loading");
        return {};
    }

    std::vector<std::string> deny = denylist();
    int linked = 0;
    int skipped = 0;
    for (const auto &entry : fs::directory_iterator(source, ec)) {
        std::string name = entry.path().filename().string();
        if (isDenied(name, deny)) {
            ++skipped;
            continue;
        }
        fs::create_symlink(entry.path(), curated / name, ec);
        if (ec) {
            Log::warn("[engine] FactoryPolicy: could not symlink " + name + ": " + ec.message());
            continue;
        }
        ++linked;
    }
    if (ec) {
        Log::warn("[engine] FactoryPolicy: error scanning " + source.string() + " (" + ec.message() +
                  "), falling back to default MLT module loading");
        return {};
    }

    Log::info("[engine] FactoryPolicy: curated MLT module dir " + curated.string() + " (" + std::to_string(linked) +
              " linked, " + std::to_string(skipped) + " denied)");
    return curated.string();
}

} // namespace

FactoryPolicy::FactoryPolicy()
{
    std::string curated = buildCuratedModuleDir();
    if (!curated.empty()) {
        Mlt::Factory::init(curated.c_str());
        m_moduleDirectoryUsed = curated;
    } else {
        Log::warn("[engine] FactoryPolicy: initializing MLT with default (uncurated) module directory — Qt6 may load");
        Mlt::Factory::init();
        m_moduleDirectoryUsed.clear();
    }
}

FactoryPolicy::~FactoryPolicy()
{
    Mlt::Factory::close();
}

} // namespace ustudio::engine
