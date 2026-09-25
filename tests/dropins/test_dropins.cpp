// Drop-in loading (ADR-014, doc 17 "Testing"): the test drop-in built in
// and as a module, and the modules that must be refused.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dropins/registry.h"

#include <cstdlib>
#include <string>
#include <vector>

using namespace ustudio::dropins;

extern "C" const UStudioDropInDescription *ustudio_dropin_testdropin_describe(void);

namespace {

struct RecordingHost : DropInHost
{
    std::vector<std::string> logged;
    std::string program() const override
    {
        return "test";
    }
    void log(const std::string &message) override
    {
        logged.push_back(message);
    }
    void addEngineExtension(ustudio::engine::EngineExtensionFactory) override
    {
        logged.push_back("engine extension");
    }
    void addRenderSubcommand(ustudio::dropins::RenderSubcommand subcommand) override
    {
        logged.push_back("render subcommand " + subcommand.name);
    }
    void addShellExtension(ustudio::dropins::ShellExtension) override
    {
        logged.push_back("shell extension");
    }
};

bool anyContains(const std::vector<std::string> &lines, const std::string &text)
{
    for (const std::string &line : lines)
        if (line.find(text) != std::string::npos)
            return true;
    return false;
}

} // namespace

TEST_CASE("drop-ins: a built-in drop-in contributes factory paths and registers")
{
    DropInRegistry registry;
    registry.addBuiltin(ustudio_dropin_testdropin_describe());
    REQUIRE(registry.entries().size() == 1);
    CHECK(registry.entries()[0].name == "testdropin");
    CHECK(registry.entries()[0].path.empty());

    FactoryPaths paths;
    registry.contributeFactoryPaths(paths);
    CHECK(paths.frei0rPaths == std::vector<std::string>{"/testdropin/frei0r"});

    RecordingHost host;
    registry.registerAll(host);
    CHECK(anyContains(host.logged, "[testdropin] registered in test"));
    CHECK(anyContains(host.logged, "render subcommand testdropin-probe"));
    CHECK(anyContains(host.logged, "shell extension"));
}

TEST_CASE("drop-ins: modules load from the given directory; bad ones are refused with a reason")
{
    DropInRegistry registry;
    registry.loadModules({TEST_MODULE_DIR});
    REQUIRE(registry.entries().size() == 1);
    CHECK(registry.entries()[0].name == "moduledropin");
    CHECK(registry.entries()[0].path.ends_with("libustudio-dropin-moduledropin.so"));

    const std::vector<std::string> &refusals = registry.refusals();
    CHECK(refusals.size() == 4);
    CHECK(anyContains(refusals, "calls itself \"othername\" but its file is named for \"misnamed\""));
    CHECK(anyContains(refusals, "built for drop-in API " + std::to_string(DROPIN_API_VERSION + 1)));
    CHECK(anyContains(refusals, "built for u Studio 0.0.0-elsewhere"));
    CHECK(anyContains(refusals, "doesn't export ustudio_drop_in_describe"));

    FactoryPaths paths;
    registry.contributeFactoryPaths(paths);
    CHECK(paths.mltModuleDirs == std::vector<std::string>{"/testdropin/mlt"});
    RecordingHost host;
    registry.registerAll(host);
    CHECK(anyContains(host.logged, "[moduledropin] registered in test"));
    CHECK(anyContains(host.logged, "render subcommand moduledropin-probe"));
}

TEST_CASE("drop-ins: disabled ones are listed but not loaded, registered or asked for paths")
{
    DropInRegistry registry;
    registry.setDisabled({"testdropin", "moduledropin", "wrongapi"});
    registry.setKnown({"testdropin", "effects"});
    registry.addBuiltin(ustudio_dropin_testdropin_describe());
    registry.loadModules({TEST_MODULE_DIR});
    REQUIRE(registry.entries().size() == 3);
    for (const DropInRegistry::Entry &entry : registry.entries())
        CHECK_FALSE(entry.enabled);
    CHECK(registry.entries()[0].name == "testdropin");
    CHECK(registry.entries()[1].name == "moduledropin");
    CHECK(registry.entries()[1].describe == nullptr); // never opened
    CHECK(registry.entries()[2].name == "wrongapi");  // nor this: no refusal, since it wasn't looked at
    CHECK_FALSE(anyContains(registry.refusals(), "drop-in API"));
    CHECK_FALSE(registry.has("testdropin"));
    CHECK(registry.known() == std::vector<std::string>{"testdropin", "effects"});

    FactoryPaths paths;
    registry.contributeFactoryPaths(paths);
    CHECK(paths.frei0rPaths.empty());
    CHECK(paths.mltModuleDirs.empty());
    RecordingHost host;
    registry.registerAll(host);
    CHECK(host.logged.empty());
}

TEST_CASE("drop-ins: a module can't take a built-in's name")
{
    DropInRegistry registry;
    UStudioDropInDescription sameName = *ustudio_dropin_testdropin_describe();
    sameName.name = "moduledropin";
    registry.addBuiltin(&sameName);
    registry.loadModules({TEST_MODULE_DIR});
    REQUIRE(registry.entries().size() == 1);
    CHECK(registry.entries()[0].path.empty()); // the built-in kept it
    CHECK(anyContains(registry.refusals(), "already registered"));
}

TEST_CASE("drop-ins: only trusted directories, unless the development override is set")
{
    ::unsetenv("USTUDIO_DROPIN_PATH");
    const std::vector<std::string> trusted = DropInRegistry::trustedDirectories();
    CHECK(DropInRegistry::searchDirectories() == trusted);
    for (const std::string &dir : trusted)
        CHECK(dir.find("/u-studio/drop-ins") != std::string::npos);
    // The build tree isn't trusted: nothing loads from it by default.
    DropInRegistry registry;
    registry.loadModules();
    CHECK(registry.entries().empty());

    ::setenv("USTUDIO_DROPIN_PATH", (std::string(TEST_MODULE_DIR) + ":/elsewhere").c_str(), 1);
    CHECK(DropInRegistry::searchDirectories() == std::vector<std::string>{TEST_MODULE_DIR, "/elsewhere"});
    DropInRegistry overridden;
    overridden.loadModules();
    CHECK(overridden.entries().size() == 1);
    ::unsetenv("USTUDIO_DROPIN_PATH");
}
