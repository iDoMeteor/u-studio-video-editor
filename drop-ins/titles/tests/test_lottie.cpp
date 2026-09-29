// Animated layers (doc 16 T6, ADR-021), the core part: the check every
// Lottie file passes before ThorVG sees it (a good corpus accepted, a
// hostile one refused with reasons), the frame an animation shows at a
// title frame, and the animated layer in .ustitle (format 2 only when a
// title has one).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/lottie_check.h"
#include "core/media/utf8_path.h"
#include "core/template_library.h"
#include "core/title_xml.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ustudio;
using namespace ustudio::titles;

namespace {

// A header and `rest` (more root members, then the layers' contents).
std::string file(const std::string &header, const std::string &rest = "", const std::string &layers = "")
{
    return "{" + header + rest + R"(,"layers":[)" + layers + "]}";
}

const std::string kHeader = R"("v":"5.7.0","fr":30,"ip":0,"op":60,"w":200,"h":100)";

// A shape layer; `extra` goes into its position property.
std::string shapeLayer(const std::string &extra = "")
{
    return R"({"ty":4,"ind":1,"ip":0,"op":60,"st":0,"ks":{"p":{"a":1,"k":[{"t":0,"s":[50,50]},{"t":60,"s":[150,50]}])" +
           extra + R"(}},"shapes":[{"ty":"rc","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0}}]})";
}

std::string base64(const std::string &bytes)
{
    static const char *kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const auto n = (static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << 16) |
                       (static_cast<uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8) |
                       static_cast<unsigned char>(bytes[i + 2]);
        for (int k = 3; k >= 0; --k)
            out += kAlphabet[(n >> (6 * k)) & 63];
    }
    if (i + 1 == bytes.size()) {
        const auto n = static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << 16;
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const auto n = (static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << 16) |
                       (static_cast<uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8);
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += kAlphabet[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

// The start of a PNG of `w` x `h` (enough for the header check), padded to
// `size` bytes.
std::string pngBytes(uint32_t w, uint32_t h, size_t size = 64)
{
    std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string("\0\0\0\x0dIHDR", 8);
    for (uint32_t v : {w, h})
        for (int k = 3; k >= 0; --k)
            png += static_cast<char>((v >> (8 * k)) & 0xFF);
    png.resize(std::max(size, png.size()), '\0');
    return png;
}

// A JPEG's start with a baseline frame header of `w` x `h`.
std::string jpegBytes(int w, int h)
{
    std::string jpeg = "\xff\xd8";
    jpeg += std::string("\xff\xe0\x00\x04\x00\x00", 6); // an APP0 segment first
    jpeg += std::string("\xff\xc0\x00\x0b\x08", 5);
    jpeg += static_cast<char>((h >> 8) & 0xFF);
    jpeg += static_cast<char>(h & 0xFF);
    jpeg += static_cast<char>((w >> 8) & 0xFF);
    jpeg += static_cast<char>(w & 0xFF);
    jpeg += std::string("\x01\x01\x11\x00", 4);
    return jpeg;
}

std::string imageAsset(const std::string &path)
{
    return R"(,"assets":[{"id":"img","w":10,"h":10,"u":"","p":")" + path + R"(","e":1}])";
}

} // namespace

TEST_CASE("the check accepts good files and says what they are")
{
    auto plain = lottie::check(file(kHeader, "", shapeLayer()));
    REQUIRE_MESSAGE(plain.has_value(), (plain ? "" : plain.error()));
    CHECK(plain->fr == 30.0);
    CHECK(plain->ip == 0.0);
    CHECK(plain->op == 60.0);
    CHECK(plain->width == 200);
    CHECK(plain->height == 100);
    CHECK(plain->layers == 1);
    CHECK_FALSE(plain->hasText);

    // Embedded pictures, a precomposition used twice, a text layer with a
    // system font, unicode, escapes, and whitespace everywhere.
    const std::string good = "{ " + kHeader +
                             R"( , "nm": "café 😀 \"q\" \\ \/ ☕",
          "assets": [
            {"id":"p1","w":10,"h":10,"u":"","p":"data:image/png;base64,)" +
                             base64(pngBytes(10, 10)) + R"(","e":1},
            {"id":"j1","w":10,"h":10,"u":"","p":"data:image/jpeg;base64,)" +
                             base64(jpegBytes(32, 16)) + R"(","e":1},
            {"id":"comp","layers":[)" +
                             shapeLayer() + R"(]}
          ],
          "fonts": {"list": [{"fName":"Sans","fFamily":"Sans","fStyle":"Regular","fPath":"","origin":0}]},
          "layers": [ {"ty":0,"refId":"comp"}, {"ty":0,"refId":"comp"}, {"ty":2,"refId":"p1"},
                      {"ty":5,"t":{"d":{"k":[{"s":{"t":"Hi","f":"Sans"},"t":0}]}}} ] })";
    auto rich = lottie::check(good);
    REQUIRE_MESSAGE(rich.has_value(), (rich ? "" : rich.error()));
    CHECK(rich->layers == 5); // four at the top, one in the precomposition
    CHECK(rich->hasText);
}

TEST_CASE("the check refuses a hostile corpus, each with its reason")
{
    struct Case
    {
        const char *what;
        std::string json;
        const char *reason; // a part of the reason given
    };
    std::string deep = file(kHeader, R"(,"d":)" + std::string(100, '[') + std::string(100, ']'));
    std::string wide;
    for (int i = 0; i < 1001; ++i)
        wide += (i ? "," : "") + shapeLayer();
    std::string manyAssets = R"(,"assets":[)";
    for (int i = 0; i < 257; ++i)
        manyAssets += (i ? "," : "") + std::string(R"({"id":"a)") + std::to_string(i) + R"(","layers":[]})";
    manyAssets += "]";
    std::string manyValues = R"(,"d":[)";
    for (int i = 0; i < 1'000'001; ++i)
        manyValues += i ? ",0" : "0";
    manyValues += "]";
    const std::vector<Case> corpus = {
        {"not JSON", "hello", "not a JSON object"},
        {"an array", "[1,2]", "not a JSON object"},
        {"truncated", file(kHeader).substr(0, 40), "valid JSON"},
        {"trailing bytes", file(kHeader) + " x", "after the animation"},
        {"no header", file(R"("v":"5")"), "header is missing"},
        {"a string frame rate", file(R"("v":"5","fr":"30","ip":0,"op":60,"w":2,"h":2)"), "header is missing"},
        {"a huge number", file(R"("v":"5","fr":1e999,"ip":0,"op":60,"w":2,"h":2)"), "out of range"},
        {"a long number", file(kHeader, R"(,"d":1)" + std::string(100, '0')), "out of range"},
        {"frame rate 0", file(R"("v":"5","fr":0,"ip":0,"op":60,"w":2,"h":2)"), "frame rate"},
        {"frame rate 500", file(R"("v":"5","fr":500,"ip":0,"op":60,"w":2,"h":2)"), "frame rate"},
        {"ends before it starts", file(R"("v":"5","fr":30,"ip":60,"op":60,"w":2,"h":2)"), "ends before"},
        {"eleven minutes", file(R"("v":"5","fr":1,"ip":0,"op":660,"w":2,"h":2)"), "ten minutes"},
        {"zero wide", file(R"("v":"5","fr":30,"ip":0,"op":60,"w":0,"h":2)"), "size"},
        {"too big", file(R"("v":"5","fr":30,"ip":0,"op":60,"w":9000,"h":2)"), "size"},
        {"a fractional size", file(R"("v":"5","fr":30,"ip":0,"op":60,"w":2.5,"h":2)"), "size"},
        {"bad UTF-8", file(R"("v":"5","fr":30,"ip":0,"op":60,"w":2,"h":2,"nm":"a)" + std::string("\xc3\x28") + "\""),
         "UTF-8"},
        {"overlong UTF-8", file(kHeader, R"(,"nm":")" + std::string("\xc0\xaf") + "\""), "UTF-8"},
        {"a lone surrogate", file(kHeader, R"(,"nm":"\ud800")"), "surrogate"},
        {"a raw control character", file(kHeader, ",\"nm\":\"a\x01\""), "control character"},
        {"deep nesting", deep, "nests too deeply"},
        {"too many layers", file(kHeader, "", wide), "too many layers"},
        {"too many assets", file(kHeader, manyAssets), "too many assets"},
        {"too many values", file(kHeader, manyValues), "too complex"},
        {"an expression", file(kHeader, "", shapeLayer(R"(,"x":"var $bm_rt = [20, 50];")")), "expressions"},
        {"an expression elsewhere", file(kHeader, R"J(,"d":{"x":"thisComp.layer(1)"})J"), "expressions"},
        {"an external picture", file(kHeader, imageAsset("logo.png")), "outside the file"},
        {"a path out of the folder", file(kHeader, imageAsset("../../../../etc/hostname")), "outside the file"},
        {"an absolute path", file(kHeader, imageAsset("/etc/hostname")), "outside the file"},
        {"a URL", file(kHeader, imageAsset("https://example.com/a.png")), "outside the file"},
        {"an SVG data URI", file(kHeader, imageAsset("data:image/svg+xml;base64,PHN2Zz4=")), "outside the file"},
        {"a font from outside", file(kHeader, R"(,"fonts":{"list":[{"fName":"x","fPath":"https://evil/x.ttf"}]})"),
         "font from outside"},
        {"bad base64", file(kHeader, imageAsset("data:image/png;base64,!!!!")), "base64"},
        {"not the PNG it says", file(kHeader, imageAsset("data:image/png;base64," + base64("hello there"))),
         "isn't the PNG"},
        {"not the JPEG it says", file(kHeader, imageAsset("data:image/jpeg;base64," + base64(pngBytes(4, 4)))),
         "isn't the JPEG"},
        {"a picture too big", file(kHeader, imageAsset("data:image/png;base64," + base64(pngBytes(9000, 10)))),
         "picture's size"},
        {"a JPEG too big", file(kHeader, imageAsset("data:image/jpeg;base64," + base64(jpegBytes(10, 9000)))),
         "picture's size"},
        {"a picture over 16 MB",
         file(kHeader, imageAsset("data:image/png;base64," + base64(pngBytes(10, 10, (16u << 20) + 16)))), "8 MB"},
        {"a precomposition using itself",
         file(kHeader, R"(,"assets":[{"id":"a","layers":[{"ty":0,"refId":"a"}]}])", R"({"ty":0,"refId":"a"})"),
         "uses itself"},
        {"a precomposition cycle",
         file(kHeader,
              R"(,"assets":[{"id":"a","layers":[{"ty":0,"refId":"b"}]},{"id":"b","layers":[{"ty":0,"refId":"c"}]},)"
              R"({"id":"c","layers":[{"ty":0,"refId":"a"}]}])"),
         "uses itself"},
        {"over 8 MB", file(kHeader, R"(,"nm":")" + std::string((8u << 20) + 1, 'a') + "\""), "8 MB"},
    };
    for (const Case &c : corpus) {
        CAPTURE(c.what);
        auto result = lottie::check(c.json);
        REQUIRE_FALSE(result.has_value());
        CAPTURE(result.error());
        CHECK(result.error().find(c.reason) != std::string::npos);
    }
}

TEST_CASE("the check's time grows with the file, not faster (load-proof)")
{
    // A file of `count` numbers; the fastest of three checks of it.
    const auto timed = [](size_t count) {
        std::string numbers;
        numbers.reserve(count * 8);
        for (size_t i = 0; i < count; ++i)
            numbers += (i ? ",1234.5" : "1234.5");
        const std::string json = file(kHeader, R"(,"d":[)" + numbers + "]");
        double best = 1e9;
        for (int run = 0; run < 3; ++run) {
            const auto begin = std::chrono::steady_clock::now();
            REQUIRE(lottie::check(json).has_value());
            best = std::min(
                best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
        }
        return best;
    };
    // About 1 MB and about 7.5 MB (the limit is 8).
    const double small = timed(125'000), large = timed(950'000);
    MESSAGE("1 MB: " << small << " ms; 7.5 MB: " << large << " ms");
    CHECK(large < 20.0 * std::max(small, 1.0)); // 7.6x the bytes: linear, with room
}

TEST_CASE("frameAt: the title's time in the animation's frames, looping or held")
{
    lottie::Facts facts;
    facts.fr = 30.0;
    facts.ip = 10.0;
    facts.op = 70.0; // 60 frames: two seconds
    const std::vector<std::pair<int, int>> rates = {{24000, 1001}, {25, 1}, {30000, 1001}, {30, 1}, {60000, 1001}};
    for (auto [num, den] : rates) {
        for (double speed : {0.5, 1.0, 2.0}) {
            CAPTURE(num);
            CAPTURE(speed);
            for (double t : {0.0, 1.0, 17.0, 59.0, 60.0, 61.0, 150.0, 1000.0}) {
                CAPTURE(t);
                const double elapsed = t * den / num * facts.fr * speed; // the animation's frames since its start
                const double looped = lottie::frameAt(t, num, den, facts, speed, true);
                const double once = lottie::frameAt(t, num, den, facts, speed, false);
                CHECK(looped == doctest::Approx(10.0 + std::fmod(elapsed, 60.0)).epsilon(1e-9));
                CHECK(once == doctest::Approx(10.0 + std::min(elapsed, 59.0)).epsilon(1e-9));
                CHECK(looped >= 10.0);
                CHECK(looped < 70.0);
                // The same inputs, the same frame, bit for bit.
                CHECK(lottie::frameAt(t, num, den, facts, speed, true) == looped);
            }
        }
    }
    // At 30 fps and speed 1, title frame n is animation frame ip + n.
    CHECK(lottie::frameAt(45.0, 30, 1, facts, 1.0, true) == 55.0);
    CHECK(lottie::frameAt(75.0, 30, 1, facts, 1.0, true) == 25.0);  // looped
    CHECK(lottie::frameAt(75.0, 30, 1, facts, 1.0, false) == 69.0); // held at op - 1
    // Nonsense in, the first frame out.
    CHECK(lottie::frameAt(10.0, 0, 1, facts, 1.0, true) == 10.0);
    CHECK(lottie::frameAt(-5.0, 30, 1, facts, 1.0, true) == 10.0);
    lottie::Facts tiny = facts;
    tiny.op = 10.5; // shorter than a frame: held at ip, never before it
    CHECK(lottie::frameAt(100.0, 30, 1, tiny, 1.0, false) == 10.0);
}

TEST_CASE("an animated layer in .ustitle: format 2 only with one; round trip; older formats refuse it")
{
    TitleDocument doc;
    Layer text;
    text.id = "t";
    text.text = "Hi";
    doc.layers.push_back(text);
    CHECK(writeTitle(doc).find("version=\"1\"") != std::string::npos); // no animation: still format 1

    Layer anim;
    anim.id = "a";
    anim.kind = LayerKind::Lottie;
    anim.src = "Show images/sting.json";
    anim.x = 100;
    anim.y = 50;
    anim.w = 320;
    anim.h = 160;
    anim.loop = false;
    anim.speed = 1.5;
    doc.layers.push_back(anim);
    const std::string xml = writeTitle(doc);
    CHECK(xml.find("version=\"2\"") != std::string::npos);
    CHECK(xml.find("kind=\"lottie\"") != std::string::npos);
    auto again = parseTitle(xml);
    REQUIRE(again.has_value());
    CHECK(again->warnings.empty());
    REQUIRE(again->document.layers.size() == 2);
    const Layer &read = again->document.layers[1];
    CHECK(read.kind == LayerKind::Lottie);
    CHECK(read.src == anim.src);
    CHECK_FALSE(read.loop);
    CHECK(read.speed == 1.5);
    CHECK(read.w == 320);
    CHECK(again->document == doc);

    // The defaults aren't written.
    doc.layers[1].loop = true;
    doc.layers[1].speed = 1.0;
    const std::string plain = writeTitle(doc);
    CHECK(plain.find("loop=") == std::string::npos);
    CHECK(plain.find("speed=") == std::string::npos);

    // An out-of-range speed is clamped, like any number; a bad loop value
    // is refused.
    std::string fast = xml;
    fast.replace(fast.find("speed=\"1.5\""), 11, "speed=\"99\"");
    auto clamped = parseTitle(fast);
    REQUIRE(clamped.has_value());
    CHECK(clamped->document.layers[1].speed == 4.0);
    std::string badLoop = xml;
    badLoop.replace(badLoop.find("loop=\"once\""), 11, "loop=\"sometimes\"");
    CHECK_FALSE(parseTitle(badLoop).has_value());

    // A newer format than this reads is refused whole, never half-read.
    std::string newer = xml;
    newer.replace(newer.find("version=\"2\""), 11, "version=\"3\"");
    auto refused = parseTitle(newer);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().find("newer version") != std::string::npos);
}

TEST_CASE("an animation travels with a title made into a template, like a picture")
{
    namespace fs = std::filesystem;
    const fs::path root =
        fs::temp_directory_path() /
        ("ustudio-lottie-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "work");
    {
        std::ofstream(root / "work" / "sting.json") << file(kHeader, "", shapeLayer());
    }
    TitleDocument doc;
    Layer anim;
    anim.id = "a";
    anim.kind = LayerKind::Lottie;
    anim.src = "sting.json";
    doc.layers.push_back(anim);
    doc.baseDirectory = core::utf8String(root / "work");
    auto saved = saveTemplate(doc, core::utf8String(root / "library"), "Sting");
    REQUIRE_MESSAGE(saved.has_value(), (saved ? "" : saved.error()));
    CHECK(fs::exists(fs::path(saved->folder) / "images" / "sting.json"));
    auto read = readTitle(saved->path);
    REQUIRE(read.has_value());
    CHECK(read->document.layers[0].src == "images/sting.json");
    fs::remove_all(root);
}
