#pragma once

// The test drop-in's render subcommand (IP6's consumer), a small cousin of
// effects' --probe-effect (doc 15, "Health and cost probe"):
//   --<drop-in>-probe <service> [frames]
// pulls `frames` (default 10) frames of color:red through the MLT filter
// `service` and prints one JSON line: {"service":…,"status":"ok","frames":N},
// or status "unknown_service" (exit 1), "bad_output" (exit 1) or "usage"
// (exit 2).

#include <iosfwd>
#include <string>
#include <vector>

namespace ustudio::testdropin {

int runProbe(const std::vector<std::string> &args, std::ostream &out);

} // namespace ustudio::testdropin
