// Captions (doc 16, T5): SRT and VTT read alike; tags, speakers and
// entities; the malformed-file policy; encodings; frame rounding at the
// common rates; lanes for overlapping cues; the import as one undo step;
// and 1,000 cues in time.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/captions.h"
#include "core/clip_fields.h"

#include <chrono>
#include <string>

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

TEST_CASE("tags: b, i and u kept; everything else dropped with its words kept")
{
    auto p = parse("1\n00:00:01,000 --> 00:00:02,000\n"
                   "{\\an8}<font color=\"#ff0000\">Red</font> <c.yellow>and</c> <00:00:01.500>timed "
                   "<ruby>漢<rt>kan</rt></ruby>\n"
                   "a < b, <i>unclosed\n\n"
                   "2\n00:00:03,000 --> 00:00:04,000\n&lt;b&gt; &#x263A; &#9731; &nbsp;x &bogus;\n");
    REQUIRE(p.has_value());
    CHECK(p->cues[0].text == "Red and timed 漢kan\na < b, <i>unclosed</i>");
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

TEST_CASE("1,000 cues read, place and import in well under two seconds")
{
    std::string srt;
    for (int i = 0; i < 1000; ++i) {
        const int start = i * 2000, end = start + 1800;
        char line[64];
        std::snprintf(line, sizeof line, "%02d:%02d:%02d,%03d --> %02d:%02d:%02d,%03d", start / 3600000,
                      start / 60000 % 60, start / 1000 % 60, start % 1000, end / 3600000, end / 60000 % 60,
                      end / 1000 % 60, end % 1000);
        srt += std::to_string(i + 1) + "\n" + line + "\nLine " + std::to_string(i) + "\n\n";
    }
    const auto begin = std::chrono::steady_clock::now();
    auto p = parse(srt);
    REQUIRE(p.has_value());
    REQUIRE(p->cues.size() == 1000);
    core::Model model = core::Model::createEmpty();
    core::Asset title;
    title.path = "/t.ustitle";
    title.info.hasVideo = true;
    title.info.isStillImage = true;
    ImportCaptions import(title, place(p->cues, {30000, 1001}), "long.srt");
    REQUIRE(import.apply(model));
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
    MESSAGE("1,000 cues: " << ms << " ms");
    CHECK(ms < 2000);
    CHECK(import.tracks().size() == 1);
}
