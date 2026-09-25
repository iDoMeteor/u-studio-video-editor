#include "render/render_cli.h"

#include <algorithm>
#include <ostream>

namespace ustudio::render {

namespace {

void printUsage(const std::vector<dropins::RenderSubcommand> &subcommands, std::ostream &out)
{
    out << "Usage: u-studio-render [--help | --<subcommand> [args...]]\n"
           "\n"
           "Drop-in subcommands:\n";
    if (subcommands.empty())
        out << "  (none: no drop-ins installed)\n";
    for (const dropins::RenderSubcommand &subcommand : subcommands)
        out << "  --" << subcommand.name << "  " << subcommand.summary << "\n";
}

} // namespace

int runRenderCli(const std::vector<dropins::RenderSubcommand> &subcommands, const std::vector<std::string> &args,
                 std::ostream &out, std::ostream &err)
{
    if (args.empty()) {
        // Placeholder: the real headless render is milestone M6
        // (docs/plans/v2/12-roadmap-and-milestones.md).
        out << "u-studio-render: not yet implemented (see milestone M6)\n";
        return 0;
    }
    const std::string &option = args.front();
    if (option == "--help" || option == "-h") {
        printUsage(subcommands, out);
        return 0;
    }
    if (option.starts_with("--")) {
        const std::string name = option.substr(2);
        auto it = std::find_if(subcommands.begin(), subcommands.end(),
                               [&](const dropins::RenderSubcommand &subcommand) { return subcommand.name == name; });
        if (it != subcommands.end())
            return it->run(std::vector<std::string>(args.begin() + 1, args.end()), out);
    }
    err << "u-studio-render: unknown option " << option << " (a drop-in that isn't installed?)\n";
    printUsage(subcommands, err);
    return 2;
}

} // namespace ustudio::render
