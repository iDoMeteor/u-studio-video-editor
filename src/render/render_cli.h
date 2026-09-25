#pragma once

#include "dropins/render_subcommand.h"

#include <iosfwd>
#include <string>
#include <vector>

namespace ustudio::render {

// u-studio-render's command line, after the drop-ins have registered (IP6,
// doc 15). `args` excludes the program name.
//   (none)              the M6 placeholder message, status 0
//   --help, -h          usage and the drop-ins' subcommands, status 0
//   --<name> args...    a drop-in's subcommand; its status
//   anything else       an error on `err`, status 2
int runRenderCli(const std::vector<dropins::RenderSubcommand> &subcommands, const std::vector<std::string> &args,
                 std::ostream &out, std::ostream &err);

} // namespace ustudio::render
