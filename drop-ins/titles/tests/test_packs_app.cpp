// The designer's side of template packs (T4b): previews rendered for a
// pack are real PNGs, and a pack built from a template with them passes
// the same validation any opened pack does.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/packs.h"
#include "core/media/utf8_path.h"
#include "core/template_library.h"
#include "core/title_xml.h"
#include "package/archive.h"
#include "package/pack.h"
#include "render/title_renderer.h"

#include <chrono>
#include <filesystem>

using namespace ustudio;
using namespace ustudio::titles;
namespace fs = std::filesystem;

TEST_CASE("a pack's previews are PNGs of the template, and the pack validates")
{
    const auto builtIns = listTemplates(TITLES_TEMPLATES_DIR, true);
    REQUIRE_FALSE(builtIns.empty());
    const std::string png = app::previewPng(builtIns.front().path, 320, 180);
    REQUIRE(png.size() > 100);
    CHECK(png.starts_with(std::string("\x89PNG\r\n\x1a\n", 8)));

    const fs::path library =
        fs::temp_directory_path() /
        ("ustudio-titles-packs-app-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto copy = duplicateTemplate(builtIns.front(), core::utf8String(library), "Mine");
    REQUIRE(copy.has_value());
    pack::Manifest m;
    m.id = "me/mine";
    m.version = "1.0.0";
    m.title = "Mine";
    m.licence = "CC0-1.0";
    auto entries = pack::build(m, {{copy->folder, app::previewPng(copy->path)}});
    REQUIRE(entries.has_value());
    uint64_t total = 0;
    for (const pack::Entry &e : *entries)
        total += e.data.size();
    auto valid = pack::validate(*entries, total);
    CHECK_MESSAGE(valid.has_value(), (valid ? "" : valid.error()));

    // Saved here, opened "on another machine" (another library): the
    // template renders byte for byte as it did before it was packed.
    if (pack::archivesSupported()) {
        const fs::path file = library / "mine.tar.gz";
        REQUIRE(pack::writeArchive(*entries, core::utf8String(file)).has_value());
        auto installed = pack::openPackage(core::utf8String(file), core::utf8String(library / "other-machine"),
                                           pack::Replace::Never);
        REQUIRE_MESSAGE(installed.has_value(), (installed ? "" : installed.error()));
        const auto there = listTemplates(installed->folder, false);
        REQUIRE(there.size() == 1);
        auto before = readTitle(copy->path), after = readTitle(there[0].path);
        REQUIRE(before.has_value());
        REQUIRE(after.has_value());
        for (double t : {5.0, 40.0, 80.0}) {
            CAPTURE(t);
            CHECK(renderTitle(before->document, t, {}, 640, 360).frame.pixels ==
                  renderTitle(after->document, t, {}, 640, 360).frame.pixels);
        }
    }
    fs::remove_all(library);
}
