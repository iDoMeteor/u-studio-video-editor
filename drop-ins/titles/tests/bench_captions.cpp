// Doc 16, T5: 1,000 subtitle cues read, placed and imported in well under
// two seconds, as an absolute time. Not a test (a loaded machine fails any
// fixed limit): `meson test -C builddir --benchmark titles-captions-bench`,
// or run the binary. Prints the time; exits 1 when it's two seconds or more.
// titles-captions checks the load-proof part (how the time grows).

#include "core/captions.h"

#include <chrono>
#include <cstdio>
#include <string>

using namespace ustudio;
using namespace ustudio::titles::captions;

int main()
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
    auto parsed = parse(srt);
    if (!parsed || parsed->cues.size() != 1000) {
        std::fprintf(stderr, "the file didn't read as 1,000 cues\n");
        return 1;
    }
    core::Model model = core::Model::createEmpty();
    core::Asset title;
    title.path = "/t.ustitle";
    title.info.hasVideo = true;
    title.info.isStillImage = true;
    ImportCaptions import(title, place(parsed->cues, {30000, 1001}), "long.srt");
    if (!import.apply(model)) {
        std::fprintf(stderr, "the import was refused\n");
        return 1;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    std::printf("1,000 cues read, placed and imported: %.0f ms (limit 2000)\n", ms);
    return ms < 2000.0 ? 0 : 1;
}
