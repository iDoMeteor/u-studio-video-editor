#include "test_probe.h"

#include <mlt++/Mlt.h>

#include <algorithm>
#include <charconv>
#include <memory>
#include <ostream>

namespace ustudio::testdropin {

namespace {

// Service names are MLT identifiers; anything else is refused before it can
// reach the JSON (no escaping needed) or MLT.
bool isServiceName(const std::string &name)
{
    return !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
               c == '-';
    });
}

} // namespace

int runProbe(const std::vector<std::string> &args, std::ostream &out)
{
    int frames = 10;
    bool usable = (args.size() == 1 || args.size() == 2) && isServiceName(args[0]);
    if (usable && args.size() == 2) {
        const std::string &text = args[1];
        auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), frames);
        usable = error == std::errc() && end == text.data() + text.size() && frames > 0 && frames <= 1000;
    }
    if (!usable) {
        out << R"({"status":"usage"})" << "\n";
        return 2;
    }
    const std::string &service = args[0];

    Mlt::Profile profile("atsc_1080p_25");
    Mlt::Producer source(profile, "color:red");
    Mlt::Filter filter(profile, service.c_str());
    if (!filter.is_valid()) {
        out << R"({"service":")" << service << R"(","status":"unknown_service"})" << "\n";
        return 1;
    }
    source.set("length", frames);
    source.set_in_and_out(0, frames - 1);
    source.attach(filter);
    for (int position = 0; position < frames; ++position) {
        source.seek(position);
        std::unique_ptr<Mlt::Frame> frame(source.get_frame());
        mlt_image_format format = mlt_image_rgb;
        int width = 64, height = 36;
        if (!frame || !frame->get_image(format, width, height)) {
            out << R"({"service":")" << service << R"(","status":"bad_output"})" << "\n";
            return 1;
        }
    }
    out << R"({"service":")" << service << R"(","status":"ok","frames":)" << frames << "}\n";
    return 0;
}

} // namespace ustudio::testdropin
