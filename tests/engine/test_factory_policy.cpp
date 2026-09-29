#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine/factory_policy.h"
#include "platform/process.h"

#include <mlt++/Mlt.h>

// mlt_factory.h doesn't wrap its own declarations in extern "C" (only
// mlt_types.h, which it includes, wraps *its own* content) -- without this,
// the compiler mangles mlt_factory_repository() as a C++ symbol, which
// doesn't match the plain-C symbol the library actually exports (confirmed
// via `nm -D libmlt-7.so`: exported as `mlt_factory_repository`,
// unmangled).
extern "C" {
#include <framework/mlt_factory.h>
}

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <string>

using namespace ustudio::engine;

namespace {

// Confirmed by a standalone repro before this was written: after
// FactoryPolicy's curated init, /proc/self/maps must contain no libQt
// mapping (measured 0/27 on the dev machine, vs. 32 mappings with a plain
// argument-less Factory::init()).
bool hasMapping(const std::string &library)
{
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find(library) != std::string::npos)
            return true;
    }
    return false;
}

bool hasLibQtMapping()
{
    return hasMapping("libQt");
}

bool hasService(Mlt::Properties *services, const std::string &name)
{
    for (int i = 0; i < services->count(); ++i) {
        if (name == services->get_name(i))
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("FactoryPolicy: no Qt loaded, required services still present")
{
    // Constructing FactoryPolicy calls Mlt::Factory::init() with a curated
    // module directory (ADR-007); its destructor calls Factory::close() at
    // the end of this scope. mlt_factory_repository() (plain C API; mlt++
    // has no equivalent accessor) fetches the already-initialized
    // process-global repository without re-running init() a second time,
    // which would silently undo the curation.
    FactoryPolicy policy;
    CHECK_FALSE(policy.moduleDirectoryUsed().empty());

    CHECK_FALSE(hasLibQtMapping());

    // Denied by default: openfx loads every .ofx under /usr/OFX/Plugins at
    // init (ADR-007 note, 2026-09-28). MLT hard-codes that folder, so the
    // test checks the module itself stays out, linked and mapped.
    for (const auto &entry : std::filesystem::directory_iterator(policy.moduleDirectoryUsed()))
        CHECK_MESSAGE(entry.path().filename().string().find("openfx") == std::string::npos,
                      "openfx module linked: " << entry.path().filename().string());
    CHECK_FALSE(hasMapping("libmltopenfx"));

    // ADR-022: only the editor's modules are linked (no drop-ins here), and
    // no other MLT module is mapped into the process.
    auto listed = [](const std::string &file) {
        for (const std::string &name : editorModules())
            if (file == "libmlt" + name + ".so")
                return true;
        return false;
    };
    for (const auto &entry : std::filesystem::directory_iterator(policy.moduleDirectoryUsed()))
        CHECK_MESSAGE(listed(entry.path().filename().string()),
                      "unlisted module linked: " << entry.path().filename().string());
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        const size_t at = line.find("libmlt");
        if (at == std::string::npos || line.find("/mlt-7/") == std::string::npos)
            continue; // libmlt-7.so itself and libmlt++ aren't modules
        const std::string file = line.substr(at, line.find(".so", at) + 3 - at);
        // `just asan` preloads avformat's and frei0r's modules from the
        // system directory so their leaks can be suppressed by name
        // (justfile, asan_modules): mapped, but not by the curated dir.
        const char *preload = std::getenv("LD_PRELOAD");
        if (preload && std::string(preload).find("/" + file) != std::string::npos)
            continue;
        CHECK_MESSAGE(listed(file), "unlisted module mapped: " << file);
    }

    Mlt::Repository repo(mlt_factory_repository());

    // Verified present on the dev machine (2026-09-17) both before this
    // policy existed and after the curated dir was applied -- these are
    // the services this app actually uses; see also CLAUDE.md's "MLT
    // empirical-knowledge rule" and doc 00's verified-facts table.
    Mlt::Properties *consumers = repo.consumers();
    for (const char *name : {"xml", "sdl2", "sdl2_audio", "rtaudio", "avformat", "null", "multi"})
        CHECK_MESSAGE(hasService(consumers, name), "missing required consumer: ", name);
    delete consumers;

    Mlt::Properties *transitions = repo.transitions();
    for (const char *name : {"affine", "composite", "luma", "mix", "matte"})
        CHECK_MESSAGE(hasService(transitions, name), "missing required transition: ", name);
    delete transitions;

    // Filters, including "deinterlace" (MLT's xine module), the loader's
    // deinterlace normaliser: without it the loader falls back to
    // avdeinterlace, which turns every producer's frame into BT.601
    // limited-range YUV 4:2:2, and on the GPU pipeline a colour comes out
    // wrong (#2080c0 as 44,128,191; the Flatpak's first build, ADR-019 G5).
    Mlt::Properties *filters = repo.filters();
    for (const char *name : {"affine", "crop", "mirror", "volume", "deinterlace"})
        CHECK_MESSAGE(hasService(filters, name), "missing required filter: " << std::string(name));
    delete filters;
}

TEST_CASE("FactoryPolicy: a start sweeps curated dirs whose process is gone, and nothing else")
{
    namespace fs = std::filesystem;
    const fs::path base =
        fs::temp_directory_path() / ("ustudio-sweep-" + std::to_string(ustudio::platform::currentProcessId()));
    fs::create_directories(base);
    auto make = [&](const std::string &name) {
        fs::create_directories(base / name / "inner");
        return base / name;
    };
    // A pid above any kernel's pid_max: never a running process.
    const fs::path dead = make("ustudio-mlt-modules-999999999-aaaaaa");
    const fs::path mine =
        make("ustudio-mlt-modules-" + std::to_string(ustudio::platform::currentProcessId()) + "-bbbbbb");
    const fs::path oldForm = make("ustudio-mlt-modules-XyZ123");
    fs::last_write_time(oldForm, fs::file_time_type::clock::now() - std::chrono::hours(48));
    const fs::path recentOldForm = make("ustudio-mlt-modules-XyZ456");
    const fs::path other = make("somebody-elses-dir");

    CHECK(sweepStaleCuratedDirs(base.string()) == 2);
    CHECK_FALSE(fs::exists(dead));
    CHECK_FALSE(fs::exists(oldForm));
    CHECK(fs::exists(mine));
    CHECK(fs::exists(recentOldForm)); // an older editor may still be running with it
    CHECK(fs::exists(other));
    fs::remove_all(base);
}

// FX5: a drop-in may lift a default-denied module (the effects drop-in's
// OpenFX opt-in lifts "openfx"), never a Qt one.
TEST_CASE("FactoryPolicy: a drop-in lifts a denied module, never a Qt one")
{
    const std::vector<std::string> none = effectiveDenylist({});
    CHECK(std::find(none.begin(), none.end(), "openfx") != none.end());
    const std::vector<std::string> lifted = effectiveDenylist({"openfx"});
    CHECK(std::find(lifted.begin(), lifted.end(), "openfx") == lifted.end());
    CHECK(std::find(lifted.begin(), lifted.end(), "qt6") != lifted.end());
    const std::vector<std::string> qt = effectiveDenylist({"qt6", "glaxnimate-qt6"});
    CHECK(std::find(qt.begin(), qt.end(), "qt6") != qt.end());
    CHECK(std::find(qt.begin(), qt.end(), "glaxnimate-qt6") != qt.end());
}
