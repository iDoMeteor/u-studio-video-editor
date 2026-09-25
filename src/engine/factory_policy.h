#pragma once

#include <string>
#include <vector>

// ADR-007: Mlt::Factory::init() with no directory argument dlopens every
// module in the system MLT module directory, including libmltqt6.so and
// libmltglaxnimate-qt6.so — pulling Qt6 into the process regardless of
// what the link line says. Measured on this machine (2026-09-17, Fedora
// 44, mlt 7.40.0): 27 modules total, of which exactly 2 match "qt6"
// (libmltqt6.so, libmltglaxnimate-qt6.so). A curated module directory
// (symlinks to every module except the denylist) passed to
// Mlt::Factory::init(dir) was verified, via a standalone repro, to give
// zero libQt mappings in /proc/self/maps while every consumer/transition
// this app actually needs (xml, sdl2, sdl2_audio, rtaudio, avformat,
// composite, mix, ...) remains available.
//
// FactoryPolicy owns Mlt::Factory::init()/close() for the whole process:
// exactly one instance, constructed in main() before any window or
// PlaybackController, destroyed after g_application_run() returns (RAII
// brackets this automatically — see src/app/main.cpp). PlaybackController
// itself never calls Factory::init()/close(); doing so from two places
// would silently re-run init() with the wrong (default, non-curated)
// directory the second time, undoing the whole policy.
namespace ustudio::engine {

// IP4 (doc 15): what drop-ins add before Mlt::Factory::init() -- plugin
// search paths (the effects drop-in's curated FREI0R_PATH, OFX_PLUGIN_PATH)
// and extra MLT module directories (the titles drop-in's
// libmltustudio.so). Collected by dropins::DropInRegistry.
struct FactoryPaths
{
    std::vector<std::string> frei0rPaths;
    std::vector<std::string> ofxPaths;
    std::vector<std::string> mltModuleDirs;
};

class FactoryPolicy
{
  public:
    // `paths` from the drop-ins: the modules in `mltModuleDirs` join the
    // curated directory under the same denylist (so ADR-007 holds for them
    // too; a system module of the same name wins), and FREI0R_PATH /
    // OFX_PLUGIN_PATH are set to exactly the contributed lists before
    // init, when MLT's frei0r and OpenFX modules read them. With none, the
    // environment is left alone.
    explicit FactoryPolicy(const FactoryPaths &paths = {});
    ~FactoryPolicy();

    // Raises MLT's process-wide limit on live avformat decoders to fit
    // `tracks` plus every thread that may decode (factory_policy.cpp says
    // why). Only ever raises. Any thread; EngineSync calls it per rebuild.
    static void raiseAvformatDecoderLimit(size_t tracks);

    FactoryPolicy(const FactoryPolicy &) = delete;
    FactoryPolicy &operator=(const FactoryPolicy &) = delete;

    // The directory actually passed to Factory::init() — the curated
    // directory, or empty if it fell back to the system default. Exposed
    // for the factory-policy test and for diagnostics.
    const std::string &moduleDirectoryUsed() const
    {
        return m_moduleDirectoryUsed;
    }

  private:
    std::string m_moduleDirectoryUsed;
};

} // namespace ustudio::engine
