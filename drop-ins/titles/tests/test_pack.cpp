// Template packs (doc 20, T4b): the manifest, the validator against a
// corpus of malicious and broken packs (each rejected with a reason, and
// nothing written), install with its version rules, remove, and a pack
// built from a template folder surviving the round trip.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/media/utf8_path.h"
#include "core/template_library.h"
#include "core/title_xml.h"
#include "package/archive.h"
#include "package/pack.h"

#ifdef TITLES_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

#include <glib.h>

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
    // A title made from it gets the pack's fonts beside it.
    const fs::path titles = scratch() / "titles";
    fs::create_directories(titles);
    REQUIRE(newTitleFromTemplate(templates[0], core::utf8String(titles / "Guest 1.ustitle")).has_value());
    CHECK(fs::exists(titles / "fonts" / "Anton-Regular.ttf"));
    CHECK(fs::exists(titles / "Guest 1 images" / "logo.png"));

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

TEST_CASE("a pack with an animated template: built, validated, installed; a hostile animation refuses it whole (T6)")
{
    const std::string kSting =
        R"({"v":"5.7.0","fr":30,"ip":0,"op":30,"w":200,"h":100,"assets":[],"layers":[{"ty":4,"ind":1,"ip":0,)"
        R"("op":30,"st":0,"ks":{"p":{"a":0,"k":[100,50]}},"shapes":[]}]})";
    const fs::path work = scratch() / "animated";
    fs::create_directories(work);
    {
        std::ofstream(work / "sting.json", std::ios::binary) << kSting;
    }
    TitleDocument doc = parseTitle(kTemplate)->document;
    doc.layers.erase(doc.layers.begin()); // no picture
    Layer anim;
    anim.id = "sting";
    anim.kind = LayerKind::Lottie;
    anim.src = "sting.json";
    anim.w = 200;
    doc.layers.push_back(anim);
    doc.baseDirectory = core::utf8String(work);
    auto saved = saveTemplate(doc, core::utf8String(work / "library"), "Sting");
    REQUIRE_MESSAGE(saved.has_value(), (saved ? "" : saved.error()));

    pack::Manifest m;
    m.id = "me/animated";
    m.version = "0.1.0";
    m.title = "Animated";
    m.licence = "CC0-1.0";
    auto entries = pack::build(m, {{saved->folder, kPng}});
    REQUIRE_MESSAGE(entries.has_value(), (entries ? "" : entries.error()));
    auto valid = pack::validate(*entries, sizeOf(*entries));
    REQUIRE_MESSAGE(valid.has_value(), (valid ? "" : valid.error()));
    CHECK(valid->files.contains("lottie/sting.json"));
    CHECK(valid->files.at("lottie/sting.json") == kSting);
    auto installed = pack::install(*valid, core::utf8String(scratch() / "animated-library"), pack::Replace::Never);
    REQUIRE_MESSAGE(installed.has_value(), (installed ? "" : installed.error()));
    const auto templates = listTemplates(installed->folder, false);
    REQUIRE(templates.size() == 1);
    CHECK(fs::exists(fs::path(templates[0].folder) / "images" / "sting.json"));
    auto read = readTitle(templates[0].path);
    REQUIRE(read.has_value());
    CHECK(read->document.layers.back().src == "images/sting.json");

    // The same pack with a script in its animation: refused, and nothing
    // installed.
    auto hostile = *entries;
    std::string scripted = kSting;
    scripted.insert(scripted.find(R"("p":{"a":0)") + 10, R"(,"x":"$bm_rt = [0, 0];")");
    find(hostile, "lottie/sting.json").data = scripted;
    relist(hostile);
    const std::string why = rejection(hostile);
    CHECK_MESSAGE(why.find("isn't an animation the titles app can use") != std::string::npos, why);
    CHECK_MESSAGE(why.find("expressions") != std::string::npos, why);
    // A template pointing at an animation the pack doesn't carry.
    auto missing = *entries;
    std::erase_if(missing, [](const pack::Entry &e) { return e.path == "lottie/sting.json"; });
    relist(missing);
    CHECK(rejection(missing).find("animation") != std::string::npos);
    fs::remove_all(scratch() / "animated");
    fs::remove_all(scratch() / "animated-library");
}

TEST_CASE("archives: .zip and .tar.gz of one pack read back the same; real bad archives are refused")
{
    if (!pack::archivesSupported()) {
        CHECK_FALSE(pack::readArchive("x.zip").has_value());
        MESSAGE("built without libarchive: skipped");
        return;
    }
    const auto entries = goodPack();
    const fs::path zip = scratch() / "pack.zip", tgz = scratch() / "pack.tar.gz";
    REQUIRE(pack::writeArchive(entries, core::utf8String(zip)).has_value());
    REQUIRE(pack::writeArchive(entries, core::utf8String(tgz)).has_value());
    CHECK_FALSE(fs::exists(scratch() / "pack.zip.part"));
    auto fromZip = pack::inspectPackage(core::utf8String(zip));
    auto fromTgz = pack::inspectPackage(core::utf8String(tgz));
    REQUIRE_MESSAGE(fromZip.has_value(), (fromZip ? "" : fromZip.error()));
    REQUIRE_MESSAGE(fromTgz.has_value(), (fromTgz ? "" : fromTgz.error()));
    CHECK(fromZip->files == fromTgz->files);
    CHECK(fromZip->manifest == fromTgz->manifest);
    auto installed =
        pack::openPackage(core::utf8String(tgz), core::utf8String(scratch() / "lib"), pack::Replace::Never);
    REQUIRE(installed.has_value());
    CHECK(listTemplates(installed->folder, false).size() == 1);

    // A pack zipped by hand with the everyday tools (directory entries,
    // extra attributes and all) opens too.
    gchar *zipTool = g_find_program_in_path("zip");
    gchar *tarTool = g_find_program_in_path("tar");
    if (zipTool && tarTool) {
        const fs::path unpacked = scratch() / "unpacked";
        fs::create_directories(unpacked);
        const std::string tgzPath = core::utf8String(tgz), unpackedPath = core::utf8String(unpacked),
                          handZip = core::utf8String(scratch() / "by-hand.zip");
        const std::vector<std::vector<std::string>> commands = {
            {tarTool, "-xzf", tgzPath, "-C", unpackedPath},
            {zipTool, "-qr", handZip, "."},
        };
        for (const auto &command : commands) {
            std::vector<char *> argv;
            for (const std::string &arg : command)
                argv.push_back(const_cast<char *>(arg.c_str()));
            argv.push_back(nullptr);
            gint status = 0;
            REQUIRE(g_spawn_sync(unpackedPath.c_str(), argv.data(), nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, nullptr,
                                 nullptr, &status, nullptr));
            REQUIRE(g_spawn_check_wait_status(status, nullptr));
        }
        auto byHand = pack::inspectPackage(handZip);
        CHECK_MESSAGE(byHand.has_value(), (byHand ? "" : byHand.error()));
        if (byHand)
            CHECK(byHand->files == fromZip->files);
    }
    g_free(zipTool);
    g_free(tarTool);

#ifdef TITLES_HAVE_LIBARCHIVE
    // Archives libarchive itself writes, with what our writer never would.
    const auto raw = [](const fs::path &path, auto &&add) {
        archive *a = archive_write_new();
        archive_write_set_format_pax_restricted(a);
        archive_write_add_filter_gzip(a);
        archive_write_open_filename(a, path.string().c_str());
        add(a);
        archive_write_close(a);
        archive_write_free(a);
    };
    const auto file = [](archive *a, const char *name, const std::string &data) {
        archive_entry *e = archive_entry_new();
        archive_entry_set_pathname(e, name);
        archive_entry_set_size(e, static_cast<la_int64_t>(data.size()));
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_perm(e, 0644);
        archive_write_header(a, e);
        archive_write_data(a, data.data(), data.size());
        archive_entry_free(e);
    };
    const fs::path traversal = scratch() / "traversal.tar.gz";
    raw(traversal, [&](archive *a) {
        for (const pack::Entry &e : entries)
            if (e.kind == pack::Entry::Kind::File)
                file(a, e.path.c_str(), e.data);
        file(a, "../../escaped.txt", "evil");
    });
    auto t = pack::inspectPackage(traversal.string());
    REQUIRE_FALSE(t.has_value());
    CHECK_MESSAGE(t.error().find("isn't a path inside the pack") != std::string::npos, t.error());
    CHECK_FALSE(fs::exists(scratch().parent_path().parent_path() / "escaped.txt"));

    const fs::path link = scratch() / "link.tar.gz";
    raw(link, [&](archive *a) {
        for (const pack::Entry &e : entries)
            if (e.kind == pack::Entry::Kind::File)
                file(a, e.path.c_str(), e.data);
        archive_entry *e = archive_entry_new();
        archive_entry_set_pathname(e, "images/passwd.png");
        archive_entry_set_filetype(e, AE_IFLNK);
        archive_entry_set_symlink(e, "/etc/passwd");
        archive_entry_set_perm(e, 0777);
        archive_write_header(a, e);
        archive_entry_free(e);
    });
    auto l = pack::inspectPackage(link.string());
    REQUIRE_FALSE(l.has_value());
    CHECK_MESSAGE(l.error().find("is a link") != std::string::npos, l.error());

    // A bomb: 200 MB of zeros gzip down to a few hundred kB. Reading stops
    // at the limit, long before it's all in memory.
    const fs::path bomb = scratch() / "bomb.tar.gz";
    raw(bomb, [&](archive *a) { file(a, "images/zeros.png", std::string(200u << 20, '\0')); });
    CHECK(fs::file_size(bomb) < (4u << 20));
    auto b = pack::inspectPackage(bomb.string());
    REQUIRE_FALSE(b.has_value());
    CHECK((b.error().find("too big") != std::string::npos || b.error().find("more than") != std::string::npos));
#endif
}
