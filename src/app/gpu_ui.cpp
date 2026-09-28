// ADR-019: the GPU pipeline in the shell. GpuAcceleration decides; this
// wires it to the engine, the Settings rows and the status line.

#include "app_window.h"

#include "gpu_acceleration.h"
#include "proxy_launcher.h"
#include "engine/factory_policy.h"
#include "platform/gpu.h"

#include <filesystem>

namespace ustudio::app {

void AppWindow::setUpGpu()
{
    const char *stateDir = g_get_user_state_dir();
    const std::filesystem::path sentinel =
        std::filesystem::path(stateDir && *stateDir ? stateDir : ".") / "ustudio" / "gpu-pipeline-live";
    m_gpu = std::make_unique<GpuAcceleration>(
        *m_settings, std::make_unique<GioLauncher>(), locateRenderTool(), sentinel,
        gpuProbeCacheKey(USTUDIO_VERSION, engine::FactoryPolicy::mltVersion()), platform::hardwareDecodeApi(),
        GpuAcceleration::Hooks{
            .setPipeline = [this](bool on, const std::string &api) { m_engine->setGpuPipeline(on, api); },
            .setHardwareDecode = [this](const std::string &api) { m_engine->setHardwareDecode(api); },
            .notify = [this](const std::string &message) { showStatus(message); },
            .changed = [this] { refreshGpuSettingsRow(); },
        });
    m_engine->gpuChanged.connect([this](bool on, const std::string &detail) { m_gpu->engineChanged(on, detail); });
    m_gpu->start();
}

void AppWindow::refreshGpuSettingsRow()
{
    if (m_gpuSettingsRow && m_gpu)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(m_gpuSettingsRow), m_gpu->statusText().c_str());
}

} // namespace ustudio::app
