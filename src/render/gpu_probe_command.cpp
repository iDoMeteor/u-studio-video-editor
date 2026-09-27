#include "render/gpu_probe_command.h"

#include "engine/gpu_probe.h"
#include "render/proxy_command.h"

#include <ostream>

namespace ustudio::render {

dropins::RenderSubcommand gpuProbeSubcommand()
{
    return {
        .name = "gpu-probe",
        .summary = "check that this machine can run the GPU pipeline",
        .run =
            [](const std::vector<std::string> &args, std::ostream &out) {
                if (!args.empty()) {
                    out << R"json({"status":"error","message":"usage: --gpu-probe (no arguments)"})json" << "\n";
                    return 2;
                }
                const engine::GpuProbeResult result = engine::probeGpu();
                out << R"({"status":")" << (result.ok ? "ok" : "error") << '"';
                if (!result.ok)
                    out << R"(,"message":")" << jsonEscape(result.message) << '"';
                out << R"(,"renderer":")" << jsonEscape(result.renderer) << R"(","version":")"
                    << jsonEscape(result.version) << "\"}\n";
                return result.ok ? 0 : 1;
            },
    };
}

} // namespace ustudio::render
