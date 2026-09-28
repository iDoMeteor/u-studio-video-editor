// The designer's side of template packs (T4b): previews rendered for a
// pack are real PNGs, and a pack built from a template with them passes
// the same validation any opened pack does.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/packs.h"
#include "core/media/utf8_path.h"
#include "core/template_library.h"
#include "package/pack.h"

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
    fs::remove_all(library);
}
