// IP6 (doc 15): u-studio-render's command line with drop-in subcommands.
// With none registered it behaves as the placeholder did; the test drop-in's
// --<name>-probe runs built in and as a module, and a taken or malformed
// subcommand name is refused.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dropins/registry.h"
#include "engine/engine_extension.h"
#include "engine/factory_policy.h"
#include "render/proxy_command.h"
#include "render/render_cli.h"

#include <atomic>
#include <sstream>
#include <string>
#include <vector>

using namespace ustudio;

extern "C" const UStudioDropInDescription *ustudio_dropin_testdropin_describe(void);

namespace {

engine::FactoryPolicy &sharedFactoryPolicy()
{
    static engine::FactoryPolicy policy;
    return policy;
}

struct Run
{
    int status;
    std::string out, err;
};

Run run(const std::vector<dropins::RenderSubcommand> &subcommands, const std::vector<std::string> &args)
{
    std::ostringstream out, err;
    int status = render::runRenderCli(subcommands, args, out, err);
    return {status, out.str(), err.str()};
}

int ok(const std::vector<std::string> &, std::ostream &)
{
    return 0;
}

} // namespace

TEST_CASE("IP6: with no drop-ins the render tool is the placeholder it was")
{
    Run none = run({}, {});
    CHECK(none.status == 0);
    CHECK(none.out == "u-studio-render: not yet implemented (see milestone M6)\n");

    Run help = run({}, {"--help"});
    CHECK(help.status == 0);
    CHECK(help.out.find("(none: no drop-ins installed)") != std::string::npos);

    Run unknown = run({}, {"--probe-effect", "brightness"});
    CHECK(unknown.status == 2);
    CHECK(unknown.out.empty());
    CHECK(unknown.err.find("unknown option --probe-effect") != std::string::npos);
}

TEST_CASE("IP6: the built-in test drop-in's subcommand runs, with its arguments")
{
    sharedFactoryPolicy();
    dropins::DropInRegistry registry;
    registry.addBuiltin(ustudio_dropin_testdropin_describe());
    dropins::BasicDropInHost host("render");
    registry.registerAll(host);
    engine::clearEngineExtensions();
    REQUIRE(host.renderSubcommands().size() == 1);

    Run help = run(host.renderSubcommands(), {"--help"});
    CHECK(help.out.find("--testdropin-probe") != std::string::npos);

    Run probed = run(host.renderSubcommands(), {"--testdropin-probe", "brightness", "5"});
    CHECK(probed.status == 0);
    CHECK(probed.out == R"({"service":"brightness","status":"ok","frames":5})"
                        "\n");

    Run missing = run(host.renderSubcommands(), {"--testdropin-probe", "no_such_filter"});
    CHECK(missing.status == 1);
    CHECK(missing.out == R"({"service":"no_such_filter","status":"unknown_service"})"
                         "\n");

    for (const std::vector<std::string> &bad :
         std::vector<std::vector<std::string>>{{"--testdropin-probe"},
                                               {"--testdropin-probe", "brightness", "0"},
                                               {"--testdropin-probe", "brightness", "5x"},
                                               {"--testdropin-probe", "a\"b"}}) {
        Run usage = run(host.renderSubcommands(), bad);
        CHECK(usage.status == 2);
        CHECK(usage.out == R"({"status":"usage"})"
                           "\n");
    }
}

TEST_CASE("IP6: the test drop-in built as a module runs its subcommand too")
{
    sharedFactoryPolicy();
    dropins::DropInRegistry registry;
    registry.loadModules({TEST_MODULE_DIR});
    REQUIRE(registry.entries().size() == 1);
    dropins::BasicDropInHost host("render");
    registry.registerAll(host);
    engine::clearEngineExtensions();
    REQUIRE(host.renderSubcommands().size() == 1);

    Run probed = run(host.renderSubcommands(), {"--moduledropin-probe", "brightness", "3"});
    CHECK(probed.status == 0);
    CHECK(probed.out == R"({"service":"brightness","status":"ok","frames":3})"
                        "\n");
}

TEST_CASE("IP6: a taken, reserved or malformed subcommand name is refused")
{
    dropins::BasicDropInHost host("render");
    host.addRenderSubcommand({"probe", "first", &ok});
    host.addRenderSubcommand({"probe", "second", &ok});  // taken
    host.addRenderSubcommand({"help", "reserved", &ok}); // --help is the tool's
    host.addRenderSubcommand({"Probe", "uppercase", &ok});
    host.addRenderSubcommand({"-x", "leading dash", &ok});
    host.addRenderSubcommand({"", "empty", &ok});
    host.addRenderSubcommand({"norun", "no function", nullptr});
    REQUIRE(host.renderSubcommands().size() == 1);
    CHECK(host.renderSubcommands()[0].summary == "first");
}

TEST_CASE("--proxy: bad arguments exit 2 with a JSON reason; strings are escaped")
{
    std::atomic<bool> cancel{false};
    const std::vector<dropins::RenderSubcommand> core{render::proxySubcommand(&cancel)};
    Run help = run(core, {"--help"});
    CHECK(help.out.find("--proxy") != std::string::npos);
    for (const std::vector<std::string> &bad : std::vector<std::vector<std::string>>{
             {"--proxy", "only-one"},
             {"--proxy", "a.mov", "b.mp4", "--height"},
             {"--proxy", "a.mov", "b.mp4", "--height", "tall"},
             {"--proxy", "a.mov", "b.mp4", "--fps", "0/1"},
             {"--proxy", "a.mov", "b.mp4", "--speed", "2"}}) {
        Run usage = run(core, bad);
        CHECK(usage.status == 2);
        CHECK(usage.out.starts_with(R"({"status":"error","message":"usage: )"));
    }
    Run missing = run(core, {"--proxy", "/nowhere/a \"b\".mov", "never.mp4"});
    CHECK(missing.status == 1);
    CHECK(missing.out.find(R"(\"b\")") != std::string::npos); // escaped, still one JSON line
    CHECK(render::jsonEscape("a\"b\\c\n") == "a\\\"b\\\\c\\u000a");
}
