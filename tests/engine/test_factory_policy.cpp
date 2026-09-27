#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine/factory_policy.h"

#include <mlt++/Mlt.h>

// mlt_factory.h doesn't wrap its own declarations in extern "C" (only
// mlt_types.h, which it includes, wraps *its own* content) -- without this,
// the compiler mangles mlt_factory_repository() as a C++ symbol, which
// doesn't match the plain-C symbol the library actually exports (confirmed
// via `nm -D libmlt-7.so`: exported as `mlt_factory_repository`,
// unmangled).
extern "C" {
#include <framework/mlt_factory.h>
}

#include <fstream>
#include <string>

using namespace ustudio::engine;

namespace {

// Confirmed by a standalone repro before this was written: after
// FactoryPolicy's curated init, /proc/self/maps must contain no libQt
// mapping (measured 0/27 on the dev machine, vs. 32 mappings with a plain
// argument-less Factory::init()).
bool hasLibQtMapping()
{
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find("libQt") != std::string::npos)
            return true;
    }
    return false;
}

bool hasService(Mlt::Properties *services, const std::string &name)
{
    for (int i = 0; i < services->count(); ++i) {
        if (name == services->get_name(i))
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("FactoryPolicy: no Qt loaded, required services still present")
{
    // Constructing FactoryPolicy calls Mlt::Factory::init() with a curated
    // module directory (ADR-007); its destructor calls Factory::close() at
    // the end of this scope. mlt_factory_repository() (plain C API; mlt++
    // has no equivalent accessor) fetches the already-initialized
    // process-global repository without re-running init() a second time,
    // which would silently undo the curation.
    FactoryPolicy policy;
    CHECK_FALSE(policy.moduleDirectoryUsed().empty());

    CHECK_FALSE(hasLibQtMapping());

    Mlt::Repository repo(mlt_factory_repository());

    // Verified present on the dev machine (2026-09-17) both before this
    // policy existed and after the curated dir was applied -- these are
    // the services this app actually uses; see also CLAUDE.md's "MLT
    // empirical-knowledge rule" and doc 00's verified-facts table.
    Mlt::Properties *consumers = repo.consumers();
    for (const char *name : {"xml", "sdl2", "sdl2_audio", "rtaudio", "avformat", "null", "multi"})
        CHECK_MESSAGE(hasService(consumers, name), "missing required consumer: ", name);
    delete consumers;

    Mlt::Properties *transitions = repo.transitions();
    for (const char *name : {"affine", "composite", "luma", "mix", "matte"})
        CHECK_MESSAGE(hasService(transitions, name), "missing required transition: ", name);
    delete transitions;

    // Filters, including "deinterlace" (MLT's xine module), the loader's
    // deinterlace normaliser: without it the loader falls back to
    // avdeinterlace, which turns every producer's frame into BT.601
    // limited-range YUV 4:2:2, and on the GPU pipeline a colour comes out
    // wrong (#2080c0 as 44,128,191; the Flatpak's first build, ADR-019 G5).
    Mlt::Properties *filters = repo.filters();
    for (const char *name : {"affine", "crop", "mirror", "volume", "deinterlace"})
        CHECK_MESSAGE(hasService(filters, name), "missing required filter: " << std::string(name));
    delete filters;
}
