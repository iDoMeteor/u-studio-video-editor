// Captions (doc 16, T5): SRT and VTT read alike; tags, speakers and
// entities; the malformed-file policy; encodings; frame rounding at the
// common rates; lanes for overlapping cues; the import as one undo step;
// and 1,000 cues in time.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/captions.h"
#include "core/clip_fields.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ustudio;
using namespace ustudio::titles;
using namespace ustudio::titles::captions;

namespace {

constexpr const char *kSrt = "\xEF\xBB\xBF"
                             "1\r\n"
                             "00:00:01,000 --> 00:00:02,500\r\n"
                             "Hello there\r\n"
                             "\r\n"
                             "2\r\n"
                             "00:00:02,500 --> 00:00:04,000\r\n"
                             "<i>Two</i> lines,\r\n"
                             "<b>bold</b> &amp; <u>under</u>\r\n"
                             "\r\n"
                             "3\r\n"
                             "00:00:05,000 --> 00:00:06,000\r\n"
                             "<v Jay>Speaking</v>\r\n";

constexpr const char *kVtt = "WEBVTT - the same cues\n"
                             "\n"
                             "NOTE a comment, ignored\n"
                             "\n"
                             "STYLE\n"
                             "::cue { color: yellow }\n"
                             "\n"
                             "intro\n"
                             "00:01.000 --> 00:02.500 align:start position:10%\n"
                             "Hello there\n"
                             "\n"
                             "00:00:02.500 --> 00:00:04.000\n"
                             "<i.loud>Two</i> lines,\n"
                             "<b>bold</b> &amp; <u>under</u>\n"
                             "\n"
                             "00:05.000 --> 00:06.000\n"
                             "<v Jay>Speaking</v>\n";

bool sameCues(const std::vector<Cue> &a, const std::vector<Cue> &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].startMs != b[i].startMs || a[i].endMs != b[i].endMs || a[i].text != b[i].text ||
            a[i].speaker != b[i].speaker)
            return false;
    return true;
}

} // namespace

TEST_CASE("SRT and VTT of the same cues read the same")
{
    auto srt = parse(kSrt);
    auto vtt = parse(kVtt);
    REQUIRE_MESSAGE(srt.has_value(), (srt ? "" : srt.error()));
    REQUIRE_MESSAGE(vtt.has_value(), (vtt ? "" : vtt.error()));
    REQUIRE(srt->cues.size() == 3);
    CHECK(sameCues(srt->cues, vtt->cues));
    CHECK(srt->cues[0].text == "Hello there");
    CHECK(srt->cues[1].text == "<i>Two</i> lines,\n<b>bold</b> & <u>under</u>");
    CHECK(srt->cues[2].text == "Speaking");
    CHECK(srt->cues[2].speaker == "Jay");
    CHECK(srt->skipped == 0);
    CHECK(srt->warnings.empty());
}

TEST_CASE("tags: b, i, u and named colours kept; everything else dropped with its words kept")
{
    auto p = parse("1\n00:00:01,000 --> 00:00:02,000\n"
                   "{\\an8}<font color=\"#ff0000\">Red</font> <c.yellow>and</c> <00:00:01.500>timed "
                   "<ruby>漢<rt>kan</rt></ruby>\n"
                   "a < b, <i>unclosed\n\n"
                   "2\n00:00:03,000 --> 00:00:04,000\n&lt;b&gt; &#x263A; &#9731; &nbsp;x &bogus;\n");
    REQUIRE(p.has_value());
    CHECK(p->cues[0].text == "<c.red>Red</c> <c.yellow>and</c> timed 漢kan\na < b, <i>unclosed</i>");
    CHECK(p->cues[0].top); // {\an8}
    CHECK_FALSE(p->cues[1].top);
    CHECK(p->cues[1].text == "<b> ☺ ☃  x &bogus;");
}

TEST_CASE("malformed files: bad cues skipped and reported; hopeless files refused")
{
    auto some = parse("1\n00:00:01,000 --> 00:00:02,000\nGood\n\n"
                      "2\n00:00:0X,000 --> 00:00:03,000\nBad time\n\n"
                      "3\n00:00:05,000 --> 00:00:04,000\nBackwards\n\n"
                      "4\n00:00:06,000 --> 00:00:07,000\n<i></i>\n\n"
                      "just words, no timing\n\n"
                      "xx\n00:00:08,000 --> 00:00:09,000\nIndex isn't a number: fine\n\n"
                      "6\n25:00:00,000 --> 25:00:01,000\nPast a day\n");
    REQUIRE(some.has_value());
    CHECK(some->cues.size() == 2);
    CHECK(some->skipped == 5);
    CHECK(some->firstSkippedLine == 6);
    CHECK(some->firstSkippedWhy == "a timestamp isn't one");

    CHECK(parse("no cues at all").error() == "it has no cues");
    CHECK(parse("1\n00:00:0X,000 --> 00:00:01,000\nx\n").error().starts_with("none of its cues can be read"));
    CHECK(parse(std::string("1\n00:00:01,000 --> 00:00:02,000\nnul\0here\n", 42)).error() == "the file isn't text");
    CHECK(parse(std::string(captions::kMaxBytes + 1, 'a')).error().find("MB") != std::string::npos);
    std::string many;
    for (size_t i = 0; i <= captions::kMaxCues; ++i)
        many += "00:00:01,000 --> 00:00:02,000\nx\n\n";
    CHECK(parse(many).error().find("cues") != std::string::npos);
}

TEST_CASE("encodings: UTF-16 with a BOM, and Windows-1252 with a warning")
{
    const std::string text = "1\n00:00:01,000 --> 00:00:02,000\nCafé ☺\n";
    std::string utf16 = "\xFF\xFE";
    for (size_t i = 0; i < text.size();) {
        // Encode the UTF-8 text as UTF-16LE (BMP only, enough here).
        uint32_t cp;
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            cp = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = ((c & 0x1F) << 6) | (text[i + 1] & 0x3F);
            i += 2;
        } else {
            cp = ((c & 0x0F) << 12) | ((text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F);
            i += 3;
        }
        utf16 += static_cast<char>(cp & 0xFF);
        utf16 += static_cast<char>(cp >> 8);
    }
    auto wide = parse(utf16);
    REQUIRE(wide.has_value());
    CHECK(wide->cues[0].text == "Café ☺");
    CHECK(wide->warnings.empty());

    auto legacy = parse("1\n00:00:01,000 --> 00:00:02,000\nCaf\xE9 \x93quoted\x94\n");
    REQUIRE(legacy.has_value());
    CHECK(legacy->cues[0].text == "Café “quoted”");
    REQUIRE(legacy->warnings.size() == 1);
    CHECK(legacy->warnings[0].find("Windows-1252") != std::string::npos);
}

TEST_CASE("timing: the nearest frame at the exact rate; touching cues touch; a frame at least")
{
    const std::vector<core::Rational> rates = {{24000, 1001}, {25, 1}, {30000, 1001}, {30, 1}, {60000, 1001}};
    for (const core::Rational &fps : rates) {
        CAPTURE(fps.num);
        CAPTURE(fps.den);
        for (int64_t ms : {0ll, 1ll, 40ll, 1001ll, 33367ll, 3600000ll, 86399999ll}) {
            const double exact =
                static_cast<double>(ms) * static_cast<double>(fps.num) / (1000.0 * static_cast<double>(fps.den));
            CHECK(std::abs(static_cast<double>(frameAt(ms, fps)) - exact) <= 0.5 + 1e-9);
        }
        auto p = parse(kSrt);
        const auto placed = place(p->cues, fps);
        REQUIRE(placed.size() == 3);
        CHECK(placed[0].position + placed[0].length == placed[1].position); // 2.5 s shared: no gap, no overlap
        CHECK(placed[0].lane == 0);
        CHECK(placed[1].lane == 0);
    }
    // 10 ms at 25 fps rounds to nothing: one frame.
    std::vector<Cue> tiny = {{1000, 1010, "x", {}, 1}};
    CHECK(place(tiny, {25, 1})[0].length == 1);
}

TEST_CASE("overlapping cues go to the next lane; a lane never overlaps")
{
    std::vector<Cue> cues = {
        {0, 3000, "a", {}, 1}, {1000, 2000, "b", {}, 2}, {1500, 4000, "c", {}, 3}, {3000, 5000, "d", {}, 4}};
    const auto placed = place(cues, {25, 1});
    REQUIRE(placed.size() == 4);
    CHECK(placed[0].lane == 0);
    CHECK(placed[1].lane == 1);
    CHECK(placed[2].lane == 2);
    CHECK(placed[3].lane == 0); // "a" ended at 3 s
}

TEST_CASE("the import: a track per lane on top, a clip per cue with its fields; one undo step, redone alike")
{
    core::Model model = core::Model::createEmpty();
    const core::TrackId video = model.addTrack(core::Track::Kind::Video, 0, "V1");
    core::Asset title;
    title.path = "/project/Titles/show captions.ustitle";
    title.displayName = "show captions.ustitle";
    title.info.hasVideo = true;
    title.info.isStillImage = true;
    auto p = parse(kSrt);
    std::vector<Cue> cues = p->cues;
    cues.push_back({1500, 2000, "overlaps the first", {}, 99});
    const auto placed = place(cues, {25, 1});

    ImportCaptions import(title, placed, "show.srt");
    REQUIRE(import.apply(model));
    CHECK(model.check().empty());
    REQUIRE(import.tracks().size() == 2);
    const auto &tracks = model.sequence().tracks;
    REQUIRE(tracks.size() == 3);
    CHECK(tracks[0].name == "Captions"); // on top
    CHECK(tracks[1].name == "Captions 2");
    CHECK(tracks[2].id == video);
    REQUIRE(import.clips().size() == 4);
    const core::Clip &first = model.clip(import.clips()[0]);
    CHECK(first.position == 25);
    CHECK(first.length() == 38); // 1 s to 2.5 s at 25 fps: frames 25..62
    CHECK(clipFieldValues(first) == std::map<std::string, std::string>{{"caption", "Hello there"}});
    CHECK(first.name == "Hello there");
    CHECK(model.clip(import.clips()[2]).name == "Two lines,"); // the first line, its tags gone (clips by start)
    CHECK(clipFieldValues(model.clip(import.clips()[3])).at("speaker") == "Jay");
    const std::vector<core::ClipId> ids = import.clips();

    import.revert(model);
    CHECK(model.sequence().tracks.size() == 1);
    CHECK(model.project().bin.empty());
    REQUIRE(import.apply(model)); // redo
    CHECK(import.clips() == ids);
    CHECK(model.check().empty());
    CHECK(model.sequence().tracks.size() == 3);
}

namespace {

// `count` cues, two seconds apart, as an SRT file.
std::string longSrt(int count)
{
    std::string srt;
    for (int i = 0; i < count; ++i) {
        const int start = i * 2000, end = start + 1800;
        char line[64];
        std::snprintf(line, sizeof line, "%02d:%02d:%02d,%03d --> %02d:%02d:%02d,%03d", start / 3600000,
                      start / 60000 % 60, start / 1000 % 60, start % 1000, end / 3600000, end / 60000 % 60,
                      end / 1000 % 60, end % 1000);
        srt += std::to_string(i + 1) + "\n" + line + "\nLine " + std::to_string(i) + "\n\n";
    }
    return srt;
}

// Reading, placing and importing `count` cues: the fastest of three runs, in
// milliseconds (the run a loaded machine disturbed least).
double importMs(int count)
{
    const std::string srt = longSrt(count);
    std::vector<double> runs;
    for (int run = 0; run < 3; ++run) {
        const auto begin = std::chrono::steady_clock::now();
        auto p = parse(srt);
        REQUIRE(p.has_value());
        REQUIRE(p->cues.size() == static_cast<size_t>(count));
        core::Model model = core::Model::createEmpty();
        core::Asset title;
        title.path = "/t.ustitle";
        title.info.hasVideo = true;
        title.info.isStillImage = true;
        ImportCaptions import(title, place(p->cues, {30000, 1001}), "long.srt");
        REQUIRE(import.apply(model));
        CHECK(import.tracks().size() == 1);
        runs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
    }
    return *std::min_element(runs.begin(), runs.end());
}

} // namespace

// Doc 16's "1,000 cues in well under two seconds" as an absolute time is the
// captions benchmark (bench_captions.cpp), not a test: fixed limits fail on a
// loaded machine (2026-09-28: 4.2 s beside two other suites, 0.6 s alone).
// Here, the load-proof part, against 250 cues timed under the same load
// moments before: four times the cues take at most 16 times as long, linear
// with room. (Before core::Model placed clips by binary search, 0.74.1, each
// insert re-sorted the track and ten times the cues took 115 times as long;
// since, 1,000 cues take about 20 ms and 20,000 about 0.5 s.)
TEST_CASE("1,000 cues read, place and import in proportion to 250")
{
    const double small = importMs(250), large = importMs(1000);
    MESSAGE("250 cues: " << small << " ms; 1,000 cues: " << large << " ms");
    CHECK(large < 16.0 * std::max(small, 1.0));
}

namespace {

// Imports `cues` at `fps` into an empty project with a video track.
core::Model imported(const std::vector<Cue> &cues, core::Rational fps)
{
    core::Model model = core::Model::createEmpty();
    model.addTrack(core::Track::Kind::Video, 0, "V1");
    core::Asset title;
    title.path = "/project/Titles/captions.ustitle";
    title.info.hasVideo = true;
    title.info.isStillImage = true;
    ImportCaptions import(title, place(cues, fps), "captions");
    REQUIRE(import.apply(model));
    return model;
}

} // namespace

TEST_CASE("export: SRT and VTT round-trip with import at the common rates")
{
    auto p = parse(kSrt);
    REQUIRE(p.has_value());
    std::vector<Cue> cues = p->cues;
    cues.push_back({1500, 2000, "overlaps the first", {}, 99});
    cues.push_back({3601234, 3602999, "an hour in\nand two lines", "Sam", 100});
    const std::vector<core::Rational> rates = {{24000, 1001}, {25, 1}, {30000, 1001}, {30, 1}, {60000, 1001}};
    for (const core::Rational &fps : rates) {
        for (Format format : {Format::Srt, Format::Vtt}) {
            CAPTURE(fps.num);
            CAPTURE(fps.den);
            CAPTURE(format == Format::Vtt);
            const core::Model model = imported(cues, fps);
            const auto exported = captionCues(model);
            REQUIRE(exported.size() == cues.size());
            const std::string file = writeSubtitles(exported, fps, format);
            auto again = parse(file);
            REQUIRE(again.has_value());
            CHECK(again->skipped == 0);
            REQUIRE(again->cues.size() == cues.size());
            // Same frames, words and speakers once placed again.
            const auto first = place(cues, fps), second = place(again->cues, fps);
            for (size_t i = 0; i < first.size(); ++i) {
                CHECK(first[i].position == second[i].position);
                CHECK(first[i].length == second[i].length);
                CHECK(first[i].lane == second[i].lane);
                CHECK(first[i].cue->text == second[i].cue->text);
                CHECK(first[i].cue->speaker == second[i].cue->speaker);
            }
            // And a second export writes the same file.
            CHECK(writeSubtitles(captionCues(imported(again->cues, fps)), fps, format) == file);
        }
    }
}

TEST_CASE("export: the formats' headers, numbering and clocks")
{
    const std::vector<ExportCue> cues = {{25, 63, "Hello there", {}}, {90025, 90050, "Later", "Jay"}};
    CHECK(writeSubtitles(cues, {25, 1}, Format::Srt) == "1\n00:00:01,000 --> 00:00:02,520\nHello there\n\n"
                                                        "2\n01:00:01,000 --> 01:00:02,000\n<v Jay>Later\n\n");
    CHECK(writeSubtitles(cues, {25, 1}, Format::Vtt) == "WEBVTT\n\n00:00:01.000 --> 00:00:02.520\nHello there\n\n"
                                                        "01:00:01.000 --> 01:00:02.000\n<v Jay>Later\n\n");
    CHECK(msAt(1, {30000, 1001}) == 33); // 33.3667 ms
    CHECK(msAt(2, {60000, 1001}) == 33); // 33.3667 ms
    CHECK(msAt(3, {24000, 1001}) == 125);
}

TEST_CASE("export: VTT escapes a literal <, & or > and keeps b, i and u; no blank line inside a cue")
{
    const std::vector<ExportCue> cues = {{0, 25, "<b>a</b> < b & c > d", {}}, {25, 50, "one\n\ntwo", {}}};
    const std::string vtt = writeSubtitles(cues, {25, 1}, Format::Vtt);
    CHECK(vtt.find("<b>a</b> &lt; b &amp; c &gt; d\n") != std::string::npos);
    auto p = parse(vtt);
    REQUIRE(p.has_value());
    REQUIRE(p->cues.size() == 2);
    CHECK(p->cues[0].text == "<b>a</b> < b & c > d");
    CHECK(p->cues[1].text == "one\ntwo");
}

TEST_CASE("export: only caption clips, by start then track; none is an empty list")
{
    core::Model model = core::Model::createEmpty();
    const core::TrackId video = model.addTrack(core::Track::Kind::Video, 0, "V1");
    core::Asset clipAsset;
    clipAsset.path = "/media/shot.mp4";
    clipAsset.info.hasVideo = true;
    const core::AssetId shot = model.addAsset(clipAsset);
    model.insertClip(video, shot, 0, 0, 99);
    CHECK(captionCues(model).empty());

    const core::Model withCaptions = imported(
        {{2000, 3000, "second", {}, 1}, {1000, 2500, "first", {}, 2}, {2000, 2600, "second, lane 2", {}, 3}}, {25, 1});
    const auto cues = captionCues(withCaptions);
    REQUIRE(cues.size() == 3);
    CHECK(cues[0].text == "first");
    CHECK(cues[1].text == "second"); // lane 0 ("Captions") is above lane 1
    CHECK(cues[2].text == "second, lane 2");
}

TEST_CASE("export: the file name picks the format; saved whole, no temporary left")
{
    CHECK(formatFor("/a/b.vtt") == Format::Vtt);
    CHECK(formatFor("/a/b.VTT") == Format::Vtt);
    CHECK(formatFor("/a/b.srt") == Format::Srt);
    CHECK(formatFor("/a/b") == Format::Srt);
    const auto folder = std::filesystem::temp_directory_path() / ("ustudio-captions-" + std::to_string(std::rand()));
    std::filesystem::create_directories(folder);
    const std::string path = (folder / "out.vtt").string();
    const std::vector<ExportCue> cues = {{25, 50, "Hi", {}}};
    CHECK(saveSubtitles(cues, {25, 1}, path).empty());
    std::ifstream in(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text == writeSubtitles(cues, {25, 1}, Format::Vtt));
    CHECK(!std::filesystem::exists(path + ".part"));
    CHECK(!saveSubtitles(cues, {25, 1}, (folder / "missing" / "out.srt").string()).empty());
    std::filesystem::remove_all(folder);
}

TEST_CASE("T5.2 colours: the eight names in both formats, other colours and classes dropped, words kept")
{
    for (const char *name : {"white", "lime", "cyan", "red", "yellow", "magenta", "blue", "black"}) {
        CAPTURE(name);
        const std::string hex = captionColourHex(name);
        const std::string want = std::string("<c.") + name + ">Hi</c> there";
        const std::string vtt =
            std::string("WEBVTT\n\n00:01.000 --> 00:02.000\n<c.bg_black.") + name + ".loud>Hi</c> there\n";
        const std::string srtName =
            std::string("1\n00:00:01,000 --> 00:00:02,000\n<font color=\"") + name + "\">Hi</font> there\n";
        const std::string srtHex =
            std::string("1\n00:00:01,000 --> 00:00:02,000\n<FONT COLOR=") + hex + ">Hi</FONT> there\n";
        for (const std::string &file : {vtt, srtName, srtHex}) {
            auto p = parse(file);
            REQUIRE(p.has_value());
            CHECK(p->cues[0].text == want);
        }
        // And back out, and in again.
        const std::vector<ExportCue> cues = {{25, 50, want, {}, false}};
        for (Format format : {Format::Srt, Format::Vtt}) {
            auto again = parse(writeSubtitles(cues, {25, 1}, format));
            REQUIRE(again.has_value());
            CHECK(again->cues[0].text == want);
        }
    }
    CHECK(writeSubtitles({{25, 50, "<c.red>Hi</c>", {}, false}}, {25, 1}, Format::Srt)
              .find("<font color=\"red\">Hi</font>") != std::string::npos);
    auto other = parse("WEBVTT\n\n00:01.000 --> 00:02.000\n<c.orange>Hi</c> <font color=\"#123456\">you</font> "
                       "<c.loud>there</c>\n");
    REQUIRE(other.has_value());
    CHECK(other->cues[0].text == "Hi you there");
    CHECK(captionWords("<c.red>a</c> <b>b</b> <c.nope>c</c>") == "a b <c.nope>c");
    CHECK(captionColourHex("orange") == nullptr);
    // A colour-only cue still has words; tags alone don't.
    CHECK_FALSE(parse("1\n00:00:01,000 --> 00:00:02,000\n<font color=red></font>\n").has_value());
}

TEST_CASE("T5.2 placement: VTT line: and SRT {\\an7-9} put a cue at the top; import and export keep it")
{
    auto vtt = parse("WEBVTT\n\n"
                     "00:01.000 --> 00:02.000 line:0\nzero\n\n"
                     "00:02.000 --> 00:03.000 line:10%,start align:left\nten\n\n"
                     "00:03.000 --> 00:04.000 line:-1\nminus one\n\n"
                     "00:04.000 --> 00:05.000 line:80%\neighty\n\n"
                     "00:05.000 --> 00:06.000 position:10%\nnone\n\n"
                     "00:06.000 --> 00:07.000 line:auto\nauto\n");
    REQUIRE(vtt.has_value());
    REQUIRE(vtt->cues.size() == 6);
    CHECK(vtt->cues[0].top);
    CHECK(vtt->cues[1].top);
    CHECK_FALSE(vtt->cues[2].top);
    CHECK_FALSE(vtt->cues[3].top);
    CHECK_FALSE(vtt->cues[4].top);
    CHECK_FALSE(vtt->cues[5].top);
    auto srt = parse("1\n00:00:01,000 --> 00:00:02,000\n{\\an7}a\n\n2\n00:00:02,000 --> 00:00:03,000\n{\\an9}b\n\n"
                     "3\n00:00:03,000 --> 00:00:04,000\n{\\an2}c\n");
    REQUIRE(srt.has_value());
    CHECK(srt->cues[0].top);
    CHECK(srt->cues[1].top);
    CHECK_FALSE(srt->cues[2].top);

    // Import: top cues play the top title with placement=top; export and
    // import again keep them there, in both formats.
    core::Model model = core::Model::createEmpty();
    core::Asset bottom, top;
    bottom.path = "/p/talk captions.ustitle";
    top.path = "/p/talk captions (top).ustitle";
    for (core::Asset *a : {&bottom, &top}) {
        a->info.hasVideo = true;
        a->info.isStillImage = true;
    }
    ImportCaptions import(bottom, place(vtt->cues, {25, 1}), "talk.vtt", top);
    REQUIRE(import.apply(model));
    CHECK(model.project().bin.size() == 2);
    const core::Clip &first = model.clip(import.clips()[0]);
    CHECK(model.asset(first.asset).path == top.path);
    CHECK(clipFieldValues(first).at("placement") == "top");
    const core::Clip &third = model.clip(import.clips()[2]);
    CHECK(model.asset(third.asset).path == bottom.path);
    CHECK_FALSE(clipFieldValues(third).contains("placement"));
    const auto cues = captionCues(model);
    for (Format format : {Format::Srt, Format::Vtt}) {
        auto again = parse(writeSubtitles(cues, {25, 1}, format));
        REQUIRE(again.has_value());
        REQUIRE(again->cues.size() == 6);
        for (size_t i = 0; i < 6; ++i)
            CHECK(again->cues[i].top == vtt->cues[i].top);
    }
    import.revert(model);
    CHECK(model.project().bin.empty());

    // No top cue: no second title.
    core::Model plain = core::Model::createEmpty();
    auto noTop = parse(kSrt);
    REQUIRE(noTop.has_value());
    ImportCaptions only(bottom, place(noTop->cues, {25, 1}), "show.srt", top);
    REQUIRE(only.apply(plain));
    CHECK(plain.project().bin.size() == 1);
}
