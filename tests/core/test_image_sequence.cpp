// M4 E: finding a numbered image sequence from one of its files.

#include "doctest.h"

#include "core/media/image_sequence.h"

#include <filesystem>
#include <fstream>

using namespace ustudio::core;
namespace fs = std::filesystem;

namespace {

fs::path folder(const char *name)
{
    const fs::path dir = fs::temp_directory_path() / (std::string("ustudio-seq-") + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void touch(const fs::path &file)
{
    std::ofstream(file) << "x";
}

} // namespace

TEST_CASE("image sequence: the gapless run around the picked file")
{
    const fs::path dir = folder("run");
    for (int i = 3; i <= 12; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "shot_%04d.png", i);
        touch(dir / name);
    }
    touch(dir / "shot_0020.png");  // after a gap: not in it
    touch(dir / "shot_00005.png"); // another width
    touch(dir / "other_0004.png"); // another prefix
    touch(dir / "shot_0006.jpg");  // another suffix
    auto sequence = findImageSequence((dir / "shot_0007.png").string());
    REQUIRE(sequence);
    CHECK(sequence->pattern == (dir / "shot_%04d.png").string());
    CHECK(sequence->begin == 3);
    CHECK(sequence->count == 10);
    CHECK(sequence->displayName == "shot_[0003-0012].png");
    CHECK(imageSequenceFile(sequence->pattern, 3) == (dir / "shot_0003.png").string());
    fs::remove_all(dir);
}

TEST_CASE("image sequence: none without a number or a numbered neighbour, and '%' is escaped")
{
    const fs::path dir = folder("none");
    touch(dir / "cover.png");
    touch(dir / "IMG_0001.jpg");
    touch(dir / "50%_1.png");
    touch(dir / "50%_2.png");
    CHECK_FALSE(findImageSequence((dir / "cover.png").string()));
    CHECK_FALSE(findImageSequence((dir / "IMG_0001.jpg").string())); // alone
    auto percent = findImageSequence((dir / "50%_2.png").string());
    REQUIRE(percent);
    CHECK(percent->pattern == (dir / "50%%_%01d.png").string());
    CHECK(imageSequenceFile(percent->pattern, 1) == (dir / "50%_1.png").string());
    fs::remove_all(dir);
}
