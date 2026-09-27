// The titles drop-in's one entry point (ADR-013, ADR-014): built in, it
// exports ustudio_dropin_titles_describe() (listed in the generated
// drop_ins.h); as a module, ustudio_drop_in_describe().

#include "dropins/api.h"
#include "dropins/dropin_host.h"
#include "editor/title_shell.h"
#include "engine/title_extension.h"
#include "engine/title_frames.h"

#include <filesystem>
#include <string>

namespace {

// IP4: the directory holding libmltustudio, the installed one or, when
// running from a build directory (nothing installed), this build's.
void contributeFactoryPaths(ustudio::dropins::FactoryPaths *paths)
{
    std::error_code ec;
    const bool installed = std::filesystem::is_directory(TITLES_MLT_INSTALL_DIR, ec);
    paths->mltModuleDirs.push_back(installed ? TITLES_MLT_INSTALL_DIR : TITLES_MLT_BUILD_DIR);
}

void registerDropIn(ustudio::dropins::DropInHost *host)
{
    host->log("[titles] registered in " + host->program());
    // IP3: a ustudio_title producer per title clip.
    host->addEngineExtension([] { return ustudio::titles::makeTitleExtension(); });
    // IP6: u-studio-render --title-frames <project> <frame>...
    host->addRenderSubcommand({"title-frames", "Hash frames of a project with titles; one JSON line (T1 checks)",
                               &ustudio::titles::runTitleFrames});
    // IP5: .ustitle import and the file watch.
    host->addShellExtension([](ustudio::app::ShellHost &shell) { ustudio::titles::extendShell(shell); });
}

const UStudioDropInDescription kDescription = {
    DROPIN_API_VERSION,      "titles",
    USTUDIO_VERSION,         "Titles: animated text and lower thirds from .ustitle files",
    &contributeFactoryPaths, &registerDropIn,
};

} // namespace

extern "C" {
#ifdef TITLES_DROPIN_MODULE
__attribute__((visibility("default"))) const UStudioDropInDescription *ustudio_drop_in_describe(void)
#else
const UStudioDropInDescription *ustudio_dropin_titles_describe(void)
#endif
{
    return &kDescription;
}
}
