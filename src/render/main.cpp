#include "core/log.h"
#include "drop_ins.h"
#include "dropins/registry.h"
#include "engine/factory_policy.h"
#include "render/render_cli.h"

#include <iostream>
#include <string>
#include <vector>

// Placeholder: the real headless render CLI is milestone M6
// (docs/plans/v2/12-roadmap-and-milestones.md). It already finds and
// registers drop-ins the way the editor does (ADR-014) and runs their
// subcommands (IP6, render_cli.h).
int main(int argc, char **argv)
{
    ustudio::core::Log::init("u-studio-render");
    ustudio::dropins::DropInRegistry dropIns;
    for (const UStudioDropInDescription *builtin : builtinDropIns())
        dropIns.addBuiltin(builtin);
    dropIns.loadModules();
    ustudio::dropins::FactoryPaths factoryPaths;
    dropIns.contributeFactoryPaths(factoryPaths);
    ustudio::engine::FactoryPolicy factoryPolicy(factoryPaths);
    ustudio::dropins::BasicDropInHost host("render");
    dropIns.registerAll(host);

    return ustudio::render::runRenderCli(host.renderSubcommands(), std::vector<std::string>(argv + 1, argv + argc),
                                         std::cout, std::cerr);
}
