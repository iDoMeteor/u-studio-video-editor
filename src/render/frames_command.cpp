#include "render/frames_command.h"

#include "core/media/frame_hash.h"
#include "core/media/utf8_path.h"
#include "core/xml/reader.h"
#include "engine/engine_sync.h"
#include "render/proxy_command.h"

#include <mlt++/Mlt.h>

#include <charconv>
#include <filesystem>
#include <memory>
#include <ostream>

namespace ustudio::render {

namespace {

int usage(std::ostream &out, const std::string &why)
{
    out << R"({"status":"error","message":")"
        << jsonEscape("usage: --frames <project> [--range IN:OUT] [--ffv1 <output>]: " + why) << "\"}\n";
    return 2;
}

bool parseFrame(const std::string &text, int &value)
{
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc() && ptr == text.data() + text.size() && value >= 0;
}

int run(const std::vector<std::string> &args, std::ostream &out, const std::atomic<bool> *cancelled)
{
    if (args.empty())
        return usage(out, "a project is needed");
    std::string ffv1;
    int first = 0, last = -1;
    for (size_t i = 1; i < args.size(); ++i) {
        if (i + 1 >= args.size())
            return usage(out, args[i] + " needs a value");
        const std::string &name = args[i], &value = args[++i];
        if (name == "--ffv1") {
            ffv1 = value;
        } else if (name == "--range") {
            const size_t colon = value.find(':');
            if (colon == std::string::npos || !parseFrame(value.substr(0, colon), first) ||
                !parseFrame(value.substr(colon + 1), last) || last < first)
                return usage(out, "bad --range " + value);
        } else {
            return usage(out, "unknown " + name);
        }
    }
    auto model = core::loadProject(args[0]);
    if (!model) {
        out << R"({"status":"error","message":")" << jsonEscape(model.error()) << "\"}\n";
        return 1;
    }
    // The preview's graph (engine.cpp builds it the same way): Full, frames
    // read at the profile's size, CPU.
    engine::EngineSync sync(*model, engine::PreviewScale::Full, engine::EngineSync::FrameReads::ProfileSize);
    Mlt::Tractor &tractor = sync.tractor();
    const int length = std::max(1, tractor.get_length());
    if (last < 0)
        last = length - 1;
    if (last >= length)
        return usage(out, "the range ends past the sequence (" + std::to_string(length) + " frames)");
    if (cancelled && cancelled->load())
        return 1;

    if (!ffv1.empty()) {
        const std::filesystem::path target = core::pathFromUtf8(ffv1);
        const std::filesystem::path part = target.string() + ".part";
        tractor.set_in_and_out(first, last);
        {
            // Consumer properties as avformat's YAML names them (vcodec,
            // acodec, f, pix_fmt). FFV1 is lossless; yuv422p keeps the
            // graph's own 4:2:2 YUV exactly, the format an export's consumer
            // asks the graph for. A hash-for-hash check against the preview
            // uses the hashes above, not this file.
            Mlt::Consumer consumer(sync.profile(), "avformat", core::utf8String(part).c_str());
            consumer.set("f", "matroska");
            consumer.set("vcodec", "ffv1");
            consumer.set("pix_fmt", "yuv422p");
            consumer.set("acodec", "pcm_s16le");
            consumer.set("real_time", -1);
            consumer.set("terminate_on_pause", 1);
            consumer.connect(tractor);
            consumer.run();
            // run() returns once avformat's thread clears "running", but its
            // render-ahead thread (real_time -1) can still be pulling frames:
            // stop() joins it. Without it the graph, then the modules
            // (Factory::close), went away under that thread, a SIGSEGV or
            // SIGFPE in a filter (2026-09-29). renderProject() does the same.
            consumer.stop();
        }
        std::error_code ec;
        std::filesystem::rename(part, target, ec);
        if (ec) {
            std::filesystem::remove(part, ec);
            out << R"({"status":"error","message":"the render couldn't be written"})" << "\n";
            return 1;
        }
        out << R"({"status":"ok","output":")" << jsonEscape(ffv1) << R"(","frames":)" << (last - first + 1) << "}\n";
        return 0;
    }

    const int width = sync.profile().width(), height = sync.profile().height();
    out << R"({"width":)" << width << R"(,"height":)" << height << R"(,"fps":")" << sync.profile().frame_rate_num()
        << "/" << sync.profile().frame_rate_den() << R"(","frames":[)";
    for (int position = first; position <= last; ++position) {
        if (cancelled && cancelled->load())
            return 1;
        tractor.seek(position);
        std::unique_ptr<Mlt::Frame> frame(tractor.get_frame());
        mlt_image_format format = mlt_image_rgba;
        int w = width, h = height;
        const uint8_t *image = frame ? frame->get_image(format, w, h) : nullptr;
        const std::string hash = image && format == mlt_image_rgba
                                     ? core::frameHash(image, static_cast<size_t>(w) * static_cast<size_t>(h) * 4)
                                     : std::string("none");
        out << (position > first ? "," : "") << R"({"frame":)" << position << R"(,"hash":")" << hash << R"("})";
    }
    out << "]}\n";
    return 0;
}

} // namespace

dropins::RenderSubcommand framesSubcommand(const std::atomic<bool> *cancelled)
{
    return {
        .name = "frames",
        .summary = "hash every frame of a project as the preview draws it, or render it losslessly (--ffv1)",
        .run = [cancelled](const std::vector<std::string> &args,
                           std::ostream &out) { return run(args, out, cancelled); },
    };
}

} // namespace ustudio::render
