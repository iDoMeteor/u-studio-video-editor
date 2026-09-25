// IP4 (doc 15): drop-ins' factory paths reach MLT before Mlt::Factory::init
// without breaking ADR-007. Its own binary: FactoryPolicy is one per process.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine/factory_policy.h"

#include <mlt++/Mlt.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace ustudio::engine;
namespace fs = std::filesystem;

namespace {

bool mapped(const std::string &text)
{
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line))
        if (line.find(text) != std::string::npos)
            return true;
    return false;
}

} // namespace

TEST_CASE("FactoryPolicy: drop-ins' module directories and plugin paths, and still no Qt")
{
    // A drop-in module directory: our stand-in module, one named like a
    // system module (the system's must win), and one named like the Qt ones
    // (denied, like the system's).
    const fs::path dir = fs::temp_directory_path() / ("ustudio-factory-paths-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    const fs::path module = fs::path(TEST_MODULE_DIR) / "libmltustudiotest.so";
    fs::create_symlink(module, dir / "libmltustudiotest.so");
    fs::create_symlink(module, dir / "libmltcore.so");
    fs::create_symlink(module, dir / "libmltfakeqt6.so");

    FactoryPaths paths;
    paths.frei0rPaths = {"/opt/effects/frei0r", "/opt/effects/frei0r-extra"};
    paths.ofxPaths = {"/opt/effects/ofx"};
    paths.mltModuleDirs = {dir.string(), "/nonexistent/drop-in/modules"};
    FactoryPolicy policy(paths);

    CHECK(std::string(std::getenv("FREI0R_PATH")) == "/opt/effects/frei0r:/opt/effects/frei0r-extra");
    CHECK(std::string(std::getenv("OFX_PLUGIN_PATH")) == "/opt/effects/ofx");

    const fs::path curated = policy.moduleDirectoryUsed();
    REQUIRE_FALSE(curated.empty());
    CHECK(fs::canonical(curated / "libmltustudiotest.so") == fs::canonical(module));
    CHECK(fs::canonical(curated / "libmltcore.so") != fs::canonical(module)); // the system's
    CHECK_FALSE(fs::exists(curated / "libmltfakeqt6.so"));

    // init registered the contributed module (MLT unloads one that registers
    // nothing, so it marks the environment); nothing Qt came with it, and the
    // services the app needs are all still there.
    CHECK(std::getenv("USTUDIO_TEST_MODULE_REGISTERED") != nullptr);
    CHECK_FALSE(mapped("libQt"));
    Mlt::Profile profile;
    CHECK(Mlt::Producer(profile, "color:red").is_valid());
    CHECK(Mlt::Consumer(profile, "null").is_valid());

    fs::remove_all(dir);
}
