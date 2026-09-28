// Template packs (doc 20, T4b): the manifest, the validator against a
// corpus of malicious and broken packs (each rejected with a reason, and
// nothing written), install with its version rules, remove, and a pack
// built from a template folder surviving the round trip.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/media/utf8_path.h"
#include "core/template_library.h"
#include "core/title_xml.h"
#include "package/pack.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ustudio;
using namespace ustudio::titles;
namespace fs = std::filesystem;

namespace {

const std::string kPng = std::string("\x89PNG\r\n\x1a\n", 8) + "not really a picture";
const std::string kJpeg = std::string("\xff\xd8\xff\xe0", 4) + "jpeg";
const std::string kTtf = std::string("\x00\x01\x00\x00", 4) + "font";

constexpr const char *kTemplate = R"(<?xml version="1.0"?>
<ustitle version="1" name="Guest" category="Lower thirds" width="1920" height="1080" fps="30/1">
  <timing intro="12" hold="60" outro="12"/>
  <field name="name" default="Jay Doe"/>
  <layer id="logo" kind="image" x="10" y="10" w="100" h="100" src="../images/logo.png"/>
  <layer id="name" kind="text" x="140" y="20" w="600"><text>{{name}}</text><font family="Anton" size="64"/></layer>
</ustitle>
)";

fs::path scratch()
{
    static const fs::path dir =
        fs::temp_directory_path() /
        ("ustudio-titles-pack-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    return dir;
}

// A good pack's entries, with the manifest describing them. `edit` may
// change the files before the manifest is computed; `after` after.
std::vector<pack::Entry> goodPack(const std::string &version = "1.0.0")
{
    std::vector<pack::Entry> files = {
        {"templates", pack::Entry::Kind::Directory, {}},
        {"templates/guest.ustitle", pack::Entry::Kind::File, kTemplate},
        {"previews/guest.png", pack::Entry::Kind::File, kPng},
        {"images/logo.png", pack::Entry::Kind::File, kPng},
        {"fonts/Anton-Regular.ttf", pack::Entry::Kind::File, kTtf},
        {"LICENSE.txt", pack::Entry::Kind::File, "CC-BY-4.0: share and adapt, with credit."},
    };
    pack::Manifest m;
    m.id = "unicorn-tears/streamer-basics";
    m.version = version;
    m.title = "Streamer basics";
    m.licence = "CC-BY-4.0";
    m.author = "Unicorn Tears";
    m.tags = {"lower thirds", "live"};
    m.fonts = {{"Anton", "fonts/Anton-Regular.ttf", "OFL-1.1"}};
    for (const pack::Entry &e : files)
        if (e.kind == pack::Entry::Kind::File)
            m.files.push_back({e.path, e.data.size(), pack::sha256(e.data)});
    files.insert(files.begin(), {"pack.xml", pack::Entry::Kind::File, pack::writeManifest(m)});
    return files;
}

uint64_t sizeOf(const std::vector<pack::Entry> &entries)
{
    uint64_t total = 0;
    for (const pack::Entry &e : entries)
        total += e.data.size();
    return total;
}

pack::Entry &find(std::vector<pack::Entry> &entries, const std::string &path)
{
    for (pack::Entry &e : entries)
        if (e.path == path)
            return e;
    FAIL("no entry " << path);
    return entries.front();
}

// The manifest again after `entries` changed (so only the change under test fails).
void relist(std::vector<pack::Entry> &entries)
{
    auto m = pack::parseManifest(find(entries, "pack.xml").data);
    REQUIRE(m.has_value());
    m->files.clear();
    for (const pack::Entry &e : entries)
        if (e.kind == pack::Entry::Kind::File && e.path != "pack.xml")
            m->files.push_back({pack::normalisePath(e.path), e.data.size(), pack::sha256(e.data)});
    find(entries, "pack.xml").data = pack::writeManifest(*m);
}

std::string rejection(const std::vector<pack::Entry> &entries, const pack::Limits &limits = {})
{
    auto result = pack::validate(entries, sizeOf(entries), limits);
    return result ? std::string() : result.error();
}

} // namespace

TEST_CASE("pack manifest: round trip, and what it refuses")
{
    auto entries = goodPack();
    auto m = pack::parseManifest(entries.front().data);
    REQUIRE(m.has_value());
    CHECK(m->id == "unicorn-tears/streamer-basics");
    CHECK(m->tags.size() == 2);
    CHECK(m->fonts.size() == 1);
    CHECK(pack::parseManifest(pack::writeManifest(*m)).value() == *m);

    CHECK_FALSE(pack::parseManifest("<pack format=\"2\" id=\"a\" version=\"1.0.0\"><title>t</title></pack>"));
    CHECK_FALSE(pack::parseManifest("<pack format=\"1\" id=\"../x\" version=\"1.0.0\"><title>t</title></pack>"));
    CHECK_FALSE(pack::parseManifest("<pack format=\"1\" id=\"a\" version=\"one\"><title>t</title></pack>"));
    CHECK_FALSE(pack::parseManifest("<pack format=\"1\" id=\"a\" version=\"1.0.0\"><title>t</title>"
                                    "<licence>MIT</licence><script/></pack>"));
    CHECK_FALSE(pack::parseManifest("not xml"));
}

TEST_CASE("paths and versions")
{
    CHECK(pack::normalisePath("./templates//a.ustitle") == "templates/a.ustitle");
    CHECK(pack::normalisePath("/etc/passwd").empty());
    CHECK(pack::normalisePath("templates/../../x").empty());
    CHECK(pack::normalisePath("a\\b").empty());
    CHECK(pack::normalisePath("C:x").empty());
    CHECK(pack::normalisePath("").empty());
    CHECK(pack::compareVersions("1.2.0", "1.10.0") < 0);
    CHECK(pack::compareVersions("2.0.0", "2.0.0-beta.1") > 0);
    CHECK(pack::compareVersions("1.0.0+build.5", "1.0.0") == 0);
    CHECK(pack::compareVersions("1.0.0-alpha", "1.0.0-beta") < 0);
}

TEST_CASE("a good pack passes")
{
    const auto entries = goodPack();
    auto pack = pack::validate(entries, sizeOf(entries));
    REQUIRE_MESSAGE(pack.has_value(), (pack ? "" : pack.error()));
    CHECK(pack->files.size() == 6);
    CHECK(pack->manifest.title == "Streamer basics");
}

TEST_CASE("every malicious or broken pack is refused with a reason")
{
    struct Case
    {
        const char *name;
        std::function<void(std::vector<pack::Entry> &)> spoil;
        const char *reason; // part of the message
    };
    const std::vector<Case> cases = {
        {"path traversal", [](auto &e) { e.push_back({"../../.bashrc", pack::Entry::Kind::File, "evil"}); },
         "isn't a path inside the pack"},
        {"absolute path", [](auto &e) { e.push_back({"/tmp/x", pack::Entry::Kind::File, "evil"}); },
         "isn't a path inside the pack"},
        {"traversal hidden in a folder",
         [](auto &e) { e.push_back({"images/../../x.png", pack::Entry::Kind::File, kPng}); },
         "isn't a path inside the pack"},
        {"symlink", [](auto &e) { e.push_back({"images/link.png", pack::Entry::Kind::Link, {}}); }, "is a link"},
        {"device", [](auto &e) { e.push_back({"images/dev", pack::Entry::Kind::Other, {}}); }, "isn't a file"},
        {"unknown file",
         [](auto &e) {
             e.push_back({"run.sh", pack::Entry::Kind::File, "#!/bin/sh\nrm -rf ~"});
             relist(e);
         },
         "isn't a file a pack may hold"},
        {"unknown folder",
         [](auto &e) {
             e.push_back({"scripts/a.py", pack::Entry::Kind::File, "x"});
             relist(e);
         },
         "a folder a pack may not have"},
        {"file not in the manifest", [](auto &e) { e.push_back({"images/extra.png", pack::Entry::Kind::File, kPng}); },
         "not in pack.xml"},
        {"bad hash",
         [](auto &e) {
             find(e, "images/logo.png").data = std::string("\x89PNG\r\n\x1a\n", 8) + "swapped!!!!!!!!!!!!!";
         },
         "SHA-256"},
        {"listed but missing",
         [](auto &e) { std::erase_if(e, [](const pack::Entry &x) { return x.path == "LICENSE.txt"; }); },
         "isn't in the pack"},
        {"a picture that isn't one",
         [](auto &e) {
             find(e, "images/logo.png").data = "<svg onload=alert(1)>";
             relist(e);
         },
         "isn't a PNG or JPEG"},
        {"a title that isn't one",
         [](auto &e) {
             find(e, "templates/guest.ustitle").data = "<html/>";
             relist(e);
         },
         "isn't a title"},
        {"a picture from outside the pack",
         [](auto &e) {
             std::string t = kTemplate;
             t.replace(t.find("../images/logo.png"), 18, "/home/you/secret.png");
             find(e, "templates/guest.ustitle").data = t;
             relist(e);
         },
         "isn't in the pack"},
        {"a picture from the web",
         [](auto &e) {
             std::string t = kTemplate;
             t.replace(t.find("../images/logo.png"), 18, "https://example.com/x.png");
             find(e, "templates/guest.ustitle").data = t;
             relist(e);
         },
         "isn't in the pack"},
        {"no preview",
         [](auto &e) {
             std::erase_if(e, [](const pack::Entry &x) { return x.path == "previews/guest.png"; });
             relist(e);
         },
         "has no preview"},
        {"a font without a licence",
         [](auto &e) {
             e.push_back({"fonts/Other.ttf", pack::Entry::Kind::File, kTtf});
             relist(e);
         },
         "has no licence"},
        {"a font that may not be shared",
         [](auto &e) {
             auto m = pack::parseManifest(find(e, "pack.xml").data);
             m->fonts[0].licence = "LicenseRef-Proprietary";
             find(e, "pack.xml").data = pack::writeManifest(*m);
         },
         "doesn't allow sharing"},
        {"no manifest", [](auto &e) { std::erase_if(e, [](const pack::Entry &x) { return x.path == "pack.xml"; }); },
         "no pack.xml"},
        {"the same file twice", [](auto &e) { e.push_back({"./images/logo.png", pack::Entry::Kind::File, kPng}); },
         "twice"},
        {"no templates",
         [](auto &e) {
             std::erase_if(e, [](const pack::Entry &x) {
                 return x.path.starts_with("templates/") || x.path.starts_with("previews/");
             });
             relist(e);
         },
         "no templates"},
    };
    for (const Case &c : cases) {
        CAPTURE(c.name);
        auto entries = goodPack();
        c.spoil(entries);
        const std::string why = rejection(entries);
        CHECK_MESSAGE(why.find(c.reason) != std::string::npos, why);
    }

    // Size limits: too many files, one too big, a bomb.
    auto entries = goodPack();
    pack::Limits few;
    few.maxFiles = 3;
    CHECK(rejection(entries, few).find("more than 3 files") != std::string::npos);
    pack::Limits small;
    small.maxFileBytes = 30;
    CHECK(rejection(entries, small).find("too big") != std::string::npos);
    auto bomb = pack::validate(entries, 10); // 10 bytes on disk, far more unpacked
    REQUIRE_FALSE(bomb.has_value());
    CHECK(bomb.error().find("decompression bomb") != std::string::npos);
}

TEST_CASE("install, the version rules, remove; nothing written for a refused pack")
{
    const fs::path library = scratch() / "library";
    auto one = pack::validate(goodPack("1.0.0"), sizeOf(goodPack("1.0.0")));
    REQUIRE(one.has_value());
    auto installed = pack::install(*one, core::utf8String(library), pack::Replace::IfNewer);
    REQUIRE_MESSAGE(installed.has_value(), (installed ? "" : installed.error()));
    const fs::path folder = installed->folder;
    CHECK(folder.filename() == "unicorn-tears--streamer-basics");
    CHECK(fs::exists(folder / "pack.xml"));
    CHECK(fs::exists(folder / "fonts" / "Anton-Regular.ttf"));
    CHECK(fs::exists(folder / "guest" / "preview.png"));
    CHECK(fs::exists(folder / "guest" / "images" / "logo.png"));
    auto title = readTitle(core::utf8String(folder / "guest" / "template.ustitle"));
    REQUIRE(title.has_value());
    CHECK(title->document.layers.front().src == "images/logo.png");
    // Its templates list like My Templates do.
    const auto templates = listTemplates(core::utf8String(folder), false);
    REQUIRE(templates.size() == 1);
    CHECK(templates[0].name == "Guest");
    CHECK_FALSE(templates[0].preview.empty());

    CHECK_FALSE(pack::install(*one, core::utf8String(library), pack::Replace::IfNewer).has_value()); // same version
    CHECK_FALSE(pack::install(*one, core::utf8String(library), pack::Replace::Never).has_value());
    auto two = pack::validate(goodPack("1.1.0"), sizeOf(goodPack("1.1.0")));
    REQUIRE(pack::install(*two, core::utf8String(library), pack::Replace::IfNewer).has_value());
    auto packs = pack::installedPacks(core::utf8String(library));
    REQUIRE(packs.size() == 1);
    CHECK(packs[0].manifest.version == "1.1.0");
    CHECK_FALSE(pack::install(*one, core::utf8String(library), pack::Replace::IfNewer).has_value()); // older
    REQUIRE(pack::install(*one, core::utf8String(library), pack::Replace::Always).has_value());      // confirmed
    CHECK(pack::installedPacks(core::utf8String(library))[0].manifest.version == "1.0.0");
    // No temporary folders left behind.
    for (const auto &entry : fs::directory_iterator(library / "packs"))
        CHECK_FALSE(entry.path().filename().string().starts_with("."));

    REQUIRE(pack::remove(pack::installedPacks(core::utf8String(library))[0]).has_value());
    CHECK(pack::installedPacks(core::utf8String(library)).empty());
    CHECK_FALSE(pack::remove({{}, core::utf8String(library)}).has_value()); // not a pack folder
}

TEST_CASE("a pack built from template folders validates, installs and keeps its pictures")
{
    const fs::path work = scratch() / "build";
    fs::create_directories(work / "images");
    {
        std::ofstream(work / "images" / "logo.png", std::ios::binary) << kPng;
    }
    TitleDocument doc = parseTitle(kTemplate)->document;
    doc.layers.front().src = core::utf8String(work / "images" / "logo.png"); // an absolute picture
    doc.baseDirectory = core::utf8String(work);
    auto saved = saveTemplate(doc, core::utf8String(work / "library"), "Guest");
    REQUIRE(saved.has_value());

    pack::Manifest m;
    m.id = "me/one";
    m.version = "0.1.0";
    m.title = "One";
    m.licence = "CC0-1.0";
    auto entries = pack::build(m, {{saved->folder, kPng}});
    REQUIRE_MESSAGE(entries.has_value(), (entries ? "" : entries.error()));
    auto valid = pack::validate(*entries, sizeOf(*entries));
    REQUIRE_MESSAGE(valid.has_value(), (valid ? "" : valid.error()));
    CHECK(valid->files.contains("images/logo.png"));
    auto installed = pack::install(*valid, core::utf8String(scratch() / "elsewhere"), pack::Replace::Never);
    REQUIRE(installed.has_value());
    const auto templates = listTemplates(installed->folder, false);
    REQUIRE(templates.size() == 1);
    CHECK(fs::exists(fs::path(templates[0].folder) / "images" / "logo.png"));
    fs::remove_all(scratch());
}
