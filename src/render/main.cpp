#include "core/log.h"
#include "drop_ins.h"
#include "dropins/registry.h"
#include "engine/factory_policy.h"
#include "platform/process.h"
#include "render/proxy_command.h"
#include "render/render_cli.h"

#include <algorithm>
#include <atomic>
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

    // Core subcommands first (M4 C: --proxy), then the drop-ins'; a drop-in
    // can't take a core one's name.
    static std::atomic<bool> cancelled{false};
    ustudio::platform::onTerminationRequest(&cancelled);
    std::vector<ustudio::dropins::RenderSubcommand> subcommands{ustudio::render::proxySubcommand(&cancelled)};
    for (const ustudio::dropins::RenderSubcommand &subcommand : host.renderSubcommands()) {
        if (std::any_of(subcommands.begin(), subcommands.end(),
                        [&](const auto &core) { return core.name == subcommand.name; })) {
            ustudio::core::Log::warn("[render] a drop-in's --" + subcommand.name + " was refused: that's a core one");
            continue;
        }
        subcommands.push_back(subcommand);
    }
    return ustudio::render::runRenderCli(subcommands, std::vector<std::string>(argv + 1, argv + argc), std::cout,
                                         std::cerr);
}
