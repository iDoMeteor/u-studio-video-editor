#include "render/proxy_command.h"

#include "engine/proxy.h"

#include <charconv>
#include <cstdio>
#include <ostream>

namespace ustudio::render {

std::string jsonEscape(const std::string &text)
{
    std::string escaped;
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') {
            escaped += '\\';
            escaped += static_cast<char>(c);
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            escaped += buf;
        } else {
            escaped += static_cast<char>(c);
        }
    }
    return escaped;
}

namespace {

bool parseInt(const std::string &text, int &value)
{
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc() && end == text.data() + text.size();
}

bool parseRate(const std::string &text, core::Rational &rate)
{
    const size_t slash = text.find('/');
    int num = 0, den = 1;
    if (slash == std::string::npos ? !parseInt(text, num)
                                   : !parseInt(text.substr(0, slash), num) || !parseInt(text.substr(slash + 1), den))
        return false;
    if (num <= 0 || den <= 0)
        return false;
    rate = {num, den};
    return true;
}

int usage(std::ostream &out, const std::string &why)
{
    out << R"({"status":"error","message":")"
        << jsonEscape("usage: --proxy <source> <output> [--height N] "
                      "[--fps n/d] [--threads N]: " +
                      why)
        << "\"}\n";
    return 2;
}

} // namespace

dropins::RenderSubcommand proxySubcommand(const std::atomic<bool> *cancel)
{
    return {"proxy", "Render an editing proxy of a media file (the editor runs this)",
            [cancel](const std::vector<std::string> &args, std::ostream &out) {
                if (args.size() < 2)
                    return usage(out, "a source and an output are needed");
                engine::ProxyRequest request;
                request.source = args[0];
                request.output = args[1];
                for (size_t i = 2; i < args.size(); i += 2) {
                    if (i + 1 >= args.size())
                        return usage(out, args[i] + " needs a value");
                    const std::string &name = args[i], &value = args[i + 1];
                    const bool ok = name == "--height" ? parseInt(value, request.height) && request.height >= 0
                                    : name == "--fps"  ? parseRate(value, request.fps)
                                    : name == "--threads"
                                        ? parseInt(value, request.threadBudget) && request.threadBudget >= 0
                                        : false;
                    if (!ok)
                        return usage(out, "bad " + name + " " + value);
                }
                std::string error;
                const bool done = engine::renderProxy(
                    request, error,
                    [&out](int current, int total) {
                        if (total > 0)
                            out << "{\"progress\":" << static_cast<double>(current) / total << "}\n" << std::flush;
                    },
                    cancel);
                if (!done) {
                    out << R"({"status":"error","message":")" << jsonEscape(error) << "\"}\n";
                    return 1;
                }
                out << R"({"status":"ok","output":")" << jsonEscape(request.output) << "\"}\n";
                return 0;
            }};
}

} // namespace ustudio::render
