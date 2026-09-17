#pragma once

#include <string>

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
// MltEngine, destroyed after g_application_run() returns (RAII brackets
// this automatically — see src/app/main.cpp). MltEngine itself no longer
// calls Factory::init()/close(); doing so from two places would silently
// re-run init() with the wrong (default, non-curated) directory the
// second time, undoing the whole policy.
namespace ustudio::engine {

class FactoryPolicy
{
public:
    FactoryPolicy();
    ~FactoryPolicy();

    FactoryPolicy(const FactoryPolicy &) = delete;
    FactoryPolicy &operator=(const FactoryPolicy &) = delete;

    // The directory actually passed to Factory::init() — the curated
    // directory, or empty if it fell back to the system default. Exposed
    // for the factory-policy test and for diagnostics.
    const std::string &moduleDirectoryUsed() const { return m_moduleDirectoryUsed; }

private:
    std::string m_moduleDirectoryUsed;
};

} // namespace ustudio::engine
