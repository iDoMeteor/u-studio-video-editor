#include "title_export.h"

#include "core/export_formats.h"
#include "core/media/utf8_path.h"
#include "core/title_xml.h"
#include "engine/title_extension.h"

#include <mlt++/Mlt.h>

#include <charconv>
#include <cmath>
#include <filesystem>
#include <map>
#include <ostream>

namespace ustudio::titles {

namespace {

// Encoder settings, checked against the installed FFmpeg (`ffmpeg -h
// encoder=...`: prores_ks takes yuva444p10le, qtrle argb, libvpx-vp9
// yuva420p, png rgba) and MLT's avformat consumer (consumer_avformat.yml;
// a "v" prefix reaches the codec's own option, so vprofile=4 is ProRes's
// 4444 profile without clashing with MLT's `profile`, consumer_avformat.c
// apply_properties()).
struct Settings
{
    const char *format; // exportFormats()' name
    std::vector<std::pair<const char *, const char *>> properties;
};

const std::vector<Settings> &settings()
{
    static const std::vector<Settings> table = {
        {"png", {{"f", "image2"}, {"vcodec", "png"}, {"pix_fmt", "rgba"}}},
        {"prores", {{"f", "mov"}, {"vcodec", "prores_ks"}, {"vprofile", "4"}, {"pix_fmt", "yuva444p10le"}}},
        {"qtrle", {{"f", "mov"}, {"vcodec", "qtrle"}, {"pix_fmt", "argb"}}},
        {"webm",
         {{"f", "webm"},
          {"vcodec", "libvpx-vp9"},
          {"pix_fmt", "yuva420p"},
          {"crf", "20"},
          {"vb", "0"},
          {"auto-alt-ref", "0"}}},
        {"h264", {{"f", "mp4"}, {"vcodec", "libx264"}, {"pix_fmt", "yuv420p"}, {"crf", "18"}, {"preset", "medium"}}},
    };
    return table;
}

void error(std::ostream &out, const std::string &message)
{
    std::string escaped;
    for (char c : message) {
        if (c == '"' || c == '\\')
            escaped += '\\';
        escaped += c;
    }
    out << R"({"error":")" << escaped << "\"}\n";
}

} // namespace

int runTitleExport(const std::vector<std::string> &args, std::ostream &out)
{
    if (args.size() < 3) {
        error(out, "usage: --title-export <title.ustitle> <output> <png|prores|qtrle|webm|h264> [--seconds S] "
                   "[--field name=value]...");
        return 2;
    }
    const std::string titlePath = args[0], output = args[1], formatName = args[2];
    const Settings *chosen = nullptr;
    for (const Settings &s : settings())
        if (formatName == s.format)
            chosen = &s;
    const ExportFormat *format = exportFormat(formatName);
    if (!chosen || !format) {
        error(out, "unknown format " + formatName);
        return 2;
    }
    double seconds = -1.0;
    std::vector<core::Param> fields;
    for (size_t i = 3; i < args.size(); ++i) {
        if (args[i] == "--seconds" && i + 1 < args.size()) {
            const std::string &text = args[++i];
            auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), seconds);
            if (ec != std::errc() || ptr != text.data() + text.size() || seconds <= 0 || seconds > 3600) {
                error(out, "--seconds needs a length up to an hour");
                return 2;
            }
        } else if (args[i] == "--field" && i + 1 < args.size()) {
            const std::string &pair = args[++i];
            const size_t eq = pair.find('=');
            if (eq == std::string::npos || eq == 0) {
                error(out, "--field needs name=value");
                return 2;
            }
            fields.push_back({"field." + pair.substr(0, eq), pair.substr(eq + 1), {}});
        } else {
            error(out, "unknown option " + args[i]);
            return 2;
        }
    }

    auto read = readTitle(titlePath);
    if (!read) {
        error(out, read.error());
        return 1;
    }
    const TitleDocument &doc = read->document;
    // The title's own size and rate, square pixels, progressive.
    Mlt::Profile profile;
    profile.set_width(doc.width);
    profile.set_height(doc.height);
    profile.set_frame_rate(doc.fpsNum, doc.fpsDen);
    profile.set_sample_aspect(1, 1);
    profile.set_display_aspect(doc.width, doc.height);
    profile.set_progressive(1);
    profile.set_colorspace(709);
    profile.set_explicit(1);
    const double fps = static_cast<double>(doc.fpsNum) / doc.fpsDen;
    const auto frames = static_cast<core::FrameIndex>(seconds > 0 ? std::lround(seconds * fps)
                                                                  : std::max<int64_t>(1, doc.timing.length()));
    auto producer = makeTitleProducer(profile, titlePath, frames, fields);
    if (!producer) {
        error(out, "the title doesn't load (is the titles MLT module installed?)");
        return 1;
    }
    if (!format->alpha) {
        // Flattened: the title draws its own background, or black.
        producer->set("background",
                      doc.background.kind == FillKind::None ? "#000000" : formatColor(doc.background.color).c_str());
    }

    // Written beside the output, then renamed over it.
    namespace fs = std::filesystem;
    const fs::path final = core::pathFromUtf8(output);
    fs::path part = final;
    part += ".part";
    std::error_code ec;
    fs::remove_all(part, ec);
    std::string target;
    const bool sequence = std::string(format->extension).empty();
    if (sequence) {
        fs::create_directories(part, ec);
        if (ec) {
            error(out, "can't make " + core::utf8String(part) + " (" + ec.message() + ")");
            return 1;
        }
        target = core::utf8String(part / (core::utf8String(final.filename()) + "_%05d.png"));
    } else {
        target = core::utf8String(part);
    }

    Mlt::Consumer consumer(profile, "avformat", target.c_str());
    if (!consumer.is_valid()) {
        error(out, "MLT's avformat consumer isn't available");
        return 1;
    }
    for (const auto &[name, value] : chosen->properties)
        consumer.set(name, value);
    consumer.set("an", 1);                    // no audio track
    consumer.set("mlt_image_format", "rgba"); // the title's own pixels, alpha and all
    consumer.set("real_time", -1);            // every frame, none dropped
    consumer.set("terminate_on_pause", 1);    // stop at the title's end
    consumer.connect(*producer);
    consumer.run();

    const bool wrote = sequence ? !fs::is_empty(part, ec) : fs::exists(part, ec) && fs::file_size(part, ec) > 0;
    if (!wrote) {
        fs::remove_all(part, ec);
        error(out, "the encoder wrote nothing");
        return 1;
    }
    fs::remove_all(final, ec);
    fs::rename(part, final, ec);
    if (ec) {
        error(out, "can't replace " + output + " (" + ec.message() + ")");
        return 1;
    }
    out << R"({"output":")" << output << R"(","frames":)" << frames << "}\n";
    return 0;
}

} // namespace ustudio::titles
