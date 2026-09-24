// Writes the doc 12 M2 A/V sync test clip (see sync_clip.h) for checking by
// eye: import it, play at 1x, and the beep should land on the flash.
//   make_sync_clip <output.mp4> [seconds]
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "sync_clip.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <output.mp4> [seconds]\n", argv[0]);
        return 2;
    }
    int seconds = argc > 2 ? std::atoi(argv[2]) : 10;
    ustudio::engine::FactoryPolicy policy;
    ustudio::core::Model model = ustudio::core::Model::createEmpty();
    ustudio::engine::EngineSync sync(model); // the project's own default profile
    ustudio::testing::renderSyncClip(sync.profile(), argv[1], seconds);
    if (!std::filesystem::exists(argv[1])) {
        std::fprintf(stderr, "render failed: %s was not written\n", argv[1]);
        return 1;
    }
    std::printf("wrote %s (%d s, flash + 1 kHz beep on frame 0 of every second)\n", argv[1], seconds);
    return 0;
}
