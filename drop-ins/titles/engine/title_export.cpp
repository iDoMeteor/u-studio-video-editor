#include "title_export.h"

#include "core/export_formats.h"
#include "core/media/utf8_path.h"
#include "core/title_xml.h"
#include "engine/title_extension.h"

#include <mlt++/Mlt.h>

#include <charconv>
#include <chrono>
#include <thread>
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

std::expected<core::FrameIndex, std::string> exportTitle(const TitleExportRequest &request,
                                                         const std::atomic<bool> *cancel)
{
    const Settings *chosen = nullptr;
    for (const Settings &s : settings())
        if (request.format == s.format)
            chosen = &s;
    const ExportFormat *format = exportFormat(request.format);
    if (!chosen || !format)
        return std::unexpected("unknown format " + request.format);
    auto read = readTitle(request.title);
    if (!read)
        return std::unexpected(read.error());
    const TitleDocument &doc = read->document;
    const core::Rational fps = request.fps.value_or(core::Rational{doc.fpsNum, doc.fpsDen});
    if (fps.num <= 0 || fps.den <= 0)
        return std::unexpected("the frame rate must be positive");
    // The title's own size, square pixels, progressive.
    Mlt::Profile profile;
    profile.set_width(doc.width);
    profile.set_height(doc.height);
    profile.set_frame_rate(static_cast<int>(fps.num), static_cast<int>(fps.den));
    profile.set_sample_aspect(1, 1);
    profile.set_display_aspect(doc.width, doc.height);
    profile.set_progressive(1);
    profile.set_colorspace(709);
    profile.set_explicit(1);
    const core::FrameIndex frames = std::max<core::FrameIndex>(1, request.frames.value_or(doc.timing.length()));
    auto producer = makeTitleProducer(profile, request.title, frames, request.fields);
    if (!producer)
        return std::unexpected("the title doesn't load (is the titles MLT module installed?)");
    producer->set("timeline_start", request.timelineStart);
    if (!format->alpha) {
        // Flattened: the title draws its own background, or black.
        producer->set("background",
                      doc.background.kind == FillKind::None ? "#000000" : formatColor(doc.background.color).c_str());
    }

    // Written beside the output, then renamed over it.
    namespace fs = std::filesystem;
    const fs::path final = core::pathFromUtf8(request.output);
    fs::path part = final;
    part += ".part";
    std::error_code ec;
    fs::remove_all(part, ec);
    std::string target;
    const bool sequence = std::string(format->extension).empty();
    if (sequence) {
        fs::create_directories(part, ec);
        if (ec)
            return std::unexpected("can't make " + core::utf8String(part) + " (" + ec.message() + ")");
        target = core::utf8String(part / (core::utf8String(final.filename()) + "_%05d.png"));
    } else {
        target = core::utf8String(part);
    }

    Mlt::Consumer consumer(profile, "avformat", target.c_str());
    if (!consumer.is_valid())
        return std::unexpected("MLT's avformat consumer isn't available");
    for (const auto &[name, value] : chosen->properties)
        consumer.set(name, value);
    consumer.set("an", 1);                    // no audio track
    consumer.set("mlt_image_format", "rgba"); // the title's own pixels, alpha and all
    consumer.set("real_time", -1);            // every frame, none dropped
    consumer.set("terminate_on_pause", 1);    // stop at the title's end
    consumer.connect(*producer);
    if (!cancel) {
        consumer.run();
    } else {
        // Its own thread, so a cancel (the editor quitting) can stop it.
        consumer.start();
        while (!consumer.is_stopped()) {
            if (cancel->load()) {
                consumer.stop();
                fs::remove_all(part, ec);
                return std::unexpected("cancelled");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    // Finished reads true once avformat's thread clears "running", while its
    // render-ahead thread may still be pulling a frame; only stop() joins it.
    // Without this, that thread can outlive the producer and the profile
    // (VE Core, 2026-09-29: a render crashed 5 of 6 times, and 0 with the
    // stop; docs/developer/notes/render.md). renderProject() does the same.
    consumer.stop();

    const bool wrote = sequence ? !fs::is_empty(part, ec) : fs::exists(part, ec) && fs::file_size(part, ec) > 0;
    if (!wrote) {
        fs::remove_all(part, ec);
        return std::unexpected("the encoder wrote nothing");
    }
    fs::remove_all(final, ec);
    fs::rename(part, final, ec);
    if (ec)
        return std::unexpected("can't replace " + request.output + " (" + ec.message() + ")");
    return frames;
}

int runTitleExport(const std::vector<std::string> &args, std::ostream &out)
{
    if (args.size() < 3) {
        error(out, "usage: --title-export <title.ustitle> <output> <png|prores|qtrle|webm|h264> "
                   "[--seconds S | --frames N] [--fps N/D] [--timeline-start F] [--field name=value]...");
        return 2;
    }
    TitleExportRequest request;
    request.title = args[0];
    request.output = args[1];
    request.format = args[2];
    double seconds = -1.0;
    const auto number = [](const std::string &text, auto &value) {
        auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        return ec == std::errc() && ptr == text.data() + text.size();
    };
    for (size_t i = 3; i < args.size(); ++i) {
        if (args[i] == "--seconds" && i + 1 < args.size()) {
            if (!number(args[++i], seconds) || seconds <= 0 || seconds > 3600) {
                error(out, "--seconds needs a length up to an hour");
                return 2;
            }
        } else if (args[i] == "--frames" && i + 1 < args.size()) {
            core::FrameIndex frames = 0;
            if (!number(args[++i], frames) || frames <= 0 || frames > 1'000'000) {
                error(out, "--frames needs a positive frame count");
                return 2;
            }
            request.frames = frames;
        } else if (args[i] == "--fps" && i + 1 < args.size()) {
            const std::string &text = args[++i];
            const size_t slash = text.find('/');
            core::Rational fps{0, 1};
            if (slash == std::string::npos
                    ? !number(text, fps.num)
                    : !number(text.substr(0, slash), fps.num) || !number(text.substr(slash + 1), fps.den) ||
                          fps.num <= 0 || fps.den <= 0) {
                error(out, "--fps needs a rate like 30 or 30000/1001");
                return 2;
            }
            request.fps = fps;
        } else if (args[i] == "--timeline-start" && i + 1 < args.size()) {
            if (!number(args[++i], request.timelineStart) || request.timelineStart < 0) {
                error(out, "--timeline-start needs a frame number");
                return 2;
            }
        } else if (args[i] == "--field" && i + 1 < args.size()) {
            const std::string &pair = args[++i];
            const size_t eq = pair.find('=');
            if (eq == std::string::npos || eq == 0) {
                error(out, "--field needs name=value");
                return 2;
            }
            request.fields.push_back({"field." + pair.substr(0, eq), pair.substr(eq + 1), {}});
        } else {
            error(out, "unknown option " + args[i]);
            return 2;
        }
    }
    if (seconds > 0) {
        auto read = readTitle(request.title);
        if (!read) {
            error(out, read.error());
            return 1;
        }
        const core::Rational fps = request.fps.value_or(core::Rational{read->document.fpsNum, read->document.fpsDen});
        request.frames = static_cast<core::FrameIndex>(
            std::lround(seconds * static_cast<double>(fps.num) / static_cast<double>(fps.den)));
    }
    auto written = exportTitle(request);
    if (!written) {
        error(out, written.error());
        return written.error().starts_with("unknown format") ? 2 : 1;
    }
    out << R"({"output":")" << request.output << R"(","frames":)" << *written << "}\n";
    return 0;
}

} // namespace ustudio::titles
