#include "core/log.h"
#include "drop_ins.h"
#include "dropins/registry.h"
#include "engine/factory_policy.h"

#include <cstdio>

// Placeholder: the real headless render CLI is milestone M6
// (docs/plans/v2/12-roadmap-and-milestones.md). It already finds and
// registers drop-ins the way the editor does (ADR-014), so their render
// subcommands (IP6) have somewhere to run.
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    ustudio::core::Log::init("u-studio-render");
    ustudio::dropins::DropInRegistry dropIns;
    for (const UStudioDropInDescription *builtin : builtinDropIns())
        dropIns.addBuiltin(builtin);
    dropIns.loadModules();
    ustudio::dropins::FactoryPaths factoryPaths;
    dropIns.contributeFactoryPaths(factoryPaths);
    ustudio::engine::FactoryPolicy factoryPolicy;
    ustudio::dropins::BasicDropInHost host("render");
    dropIns.registerAll(host);

    std::puts("u-studio-render: not yet implemented (see milestone M6)");
    return 0;
}
