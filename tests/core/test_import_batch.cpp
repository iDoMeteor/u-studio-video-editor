// M4 A: a multi-file import is one undo step (CompositeCommand's merge key),
// and the fingerprint and UTF-8 path helpers the import uses.

#include "doctest.h"

#include "core/commands/composite_command.h"
#include "core/commands/primitives.h"
#include "core/commands/undo_stack.h"
#include "core/media/fingerprint.h"
#include "core/media/utf8_path.h"
#include "core/model/model.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

using namespace ustudio::core;

namespace {

std::unique_ptr<Command> importOne(const std::string &path, uint64_t key)
{
    Asset asset;
    asset.path = path;
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = 30;
    std::vector<std::unique_ptr<Command>> steps;
    steps.push_back(std::make_unique<AddAsset>(asset));
    return std::make_unique<CompositeCommand>("Import media", std::move(steps), key, "Import files");
}

} // namespace

TEST_CASE("import batch: files sharing a merge key are one undo step; another key is another")
{
    Model model = Model::createEmpty();
    UndoStack undo(model);
    for (const char *path : {"a.mp4", "b.mp4", "c.mp4"})
        REQUIRE(undo.execute(importOne(path, 7)));
    CHECK(model.project().bin.size() == 3);
    CHECK(undo.undoLabel() == "Import files");

    REQUIRE(undo.undo());
    // All three at once: the same as undoing three separate imports (ids
    // are never reused, so nextId stays where the imports left it).
    Model separate = Model::createEmpty();
    UndoStack separateUndo(separate);
    for (const char *path : {"a.mp4", "b.mp4", "c.mp4"})
        REQUIRE(separateUndo.execute(importOne(path, 0)));
    for (int i = 0; i < 3; ++i)
        REQUIRE(separateUndo.undo());
    CHECK(model == separate);
    CHECK(model.project().bin.empty());
    CHECK_FALSE(undo.canUndo());
    REQUIRE(undo.redo());
    CHECK(model.project().bin.size() == 3);

    REQUIRE(undo.execute(importOne("d.mp4", 8))); // the next import
    CHECK(undo.undoLabel() == "Import media");
    REQUIRE(undo.undo());
    CHECK(model.project().bin.size() == 3);
}

TEST_CASE("import batch: a single-file import keeps its own label, and the clean point isn't merged into")
{
    Model model = Model::createEmpty();
    UndoStack undo(model);
    REQUIRE(undo.execute(importOne("a.mp4", 3)));
    CHECK(undo.undoLabel() == "Import media");
    undo.setCleanPoint(); // saved mid-import
    REQUIRE(undo.execute(importOne("b.mp4", 3)));
    REQUIRE(undo.undo());
    CHECK(undo.isClean()); // back to exactly what was saved
    CHECK(model.project().bin.size() == 1);
}

TEST_CASE("fileFingerprint: size and modification time, changing when the file does")
{
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / pathFromUtf8("ustudio-fingerprint-ñ日.bin");
    std::ofstream(path, std::ios::binary) << "12345";
    const std::string first = fileFingerprint(utf8String(path));
    CHECK(first.starts_with("5:"));
    CHECK(first.size() > 12); // nanoseconds since the epoch
    fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(2));
    CHECK(fileFingerprint(utf8String(path)) != first);
    fs::remove(path);
    CHECK(fileFingerprint(utf8String(path)).empty());
}

TEST_CASE("utf8 paths: non-ASCII names survive the round trip")
{
    const std::string name = "clips/ñandú 日本 😀.mp4";
    CHECK(utf8String(pathFromUtf8(name)) == name);
    CHECK(utf8String(pathFromUtf8(name).filename()) == "ñandú 日本 😀.mp4");
    CHECK(utf8String(pathFromUtf8(name).extension()) == ".mp4");
}
