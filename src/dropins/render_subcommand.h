#pragma once

// IP6 (doc 15): a `u-studio-render --<name> args...` subcommand a drop-in
// registers (effects: --probe-effect; analysis passes, doc 17). It runs in
// the render tool's own process after Mlt::Factory::init() with every
// drop-in's factory paths, so a crashing plugin takes down the child, never
// the editor.

#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace ustudio::dropins {

struct RenderSubcommand
{
    std::string name;    // without the leading "--": lowercase letters, digits, '-'
    std::string summary; // one line, for --help
    // `args` are what follows the option. Results go to `out` (the editor
    // reads the child's stdout); the return value is the exit status.
    std::function<int(const std::vector<std::string> &args, std::ostream &out)> run;
};

} // namespace ustudio::dropins
