#pragma once

// The effects drop-in's render-tool subcommands (IP6; doc 15, "Health and
// cost probe"). Both run in u-studio-render's own process, so a plugin that
// crashes takes down the child, never the editor.
//
//   --probe-effect <service> [frames]
//       One service on a 1080p noise: source: with its defaults, then every
//       number at its minimum, then at its maximum, then read at half size.
//       A stage line before each ({"service":..,"stage":..}), then one line
//       (core/health.h): ok, bad_output or unavailable, and the effect's
//       cost in milliseconds a 1080p frame (the source's own subtracted).
//       A crash or a hang is the parent's to see: no line, or a deadline.
//   --effects-registry
//       The registry (engine/registry.h) as one JSON line, for the editor's
//       cache: the editor never reads MLT metadata in its own process.

#include "core/health.h"

#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace ustudio::effects {

int runProbeEffect(const std::vector<std::string> &args, std::ostream &out);
int runEffectsRegistry(const std::vector<std::string> &args, std::ostream &out);

// The probe itself, for tests (runProbeEffect() prints it). `stage` is
// called as each stage starts.
HealthRecord probeEffect(
    const std::string &service, int frames, const std::function<void(const char *)> &stage = [](const char *) {});

} // namespace ustudio::effects
