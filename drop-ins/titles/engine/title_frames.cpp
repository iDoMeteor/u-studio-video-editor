#include "title_frames.h"

#include "core/xml/reader.h"
#include "engine/engine_sync.h"

#include <mlt++/Mlt.h>

#include <charconv>
#include <cstdio>
#include <memory>
#include <ostream>

namespace ustudio::titles {

std::string frameHash(const uint8_t *data, size_t size)
{
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

int runTitleFrames(const std::vector<std::string> &args, std::ostream &out)
{
    if (args.size() < 2) {
        out << R"({"error":"usage: --title-frames <project.ustudio> <frame>..."})" << "\n";
        return 2;
    }
    std::vector<int> frames;
    for (size_t i = 1; i < args.size(); ++i) {
        int frame = 0;
        const std::string &text = args[i];
        auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), frame);
        if (ec != std::errc() || ptr != text.data() + text.size() || frame < 0) {
            out << R"({"error":"not a frame number"})" << "\n";
            return 2;
        }
        frames.push_back(frame);
    }
    auto model = core::loadProject(args[0]);
    if (!model) {
        out << R"({"error":"the project doesn't load"})" << "\n";
        return 1;
    }
    engine::EngineSync sync(*model);
    Mlt::Tractor &tractor = sync.tractor();
    const int width = sync.profile().width(), height = sync.profile().height();
    out << R"({"width":)" << width << R"(,"height":)" << height << R"(,"frames":[)";
    for (size_t i = 0; i < frames.size(); ++i) {
        tractor.seek(frames[i]);
        std::unique_ptr<Mlt::Frame> frame(tractor.get_frame());
        mlt_image_format format = mlt_image_rgba;
        int w = width, h = height;
        const uint8_t *image = frame ? frame->get_image(format, w, h) : nullptr;
        const std::string hash = image && format == mlt_image_rgba
                                     ? frameHash(image, static_cast<size_t>(w) * static_cast<size_t>(h) * 4)
                                     : std::string("none");
        out << (i ? "," : "") << R"({"frame":)" << frames[i] << R"(,"hash":")" << hash << R"("})";
    }
    out << "]}\n";
    return 0;
}

} // namespace ustudio::titles
