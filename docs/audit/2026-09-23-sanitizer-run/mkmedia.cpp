#include <mlt++/Mlt.h>
#include <cstdio>
int main(int, char **argv) {
    Mlt::Factory::init(argv[1]);
    {
        Mlt::Profile profile("atsc_1080p_30");
        Mlt::Tractor tractor(profile);
        Mlt::Producer video(profile, "colour:#19e3ff"); video.set_in_and_out(0, 299);
        Mlt::Producer tone(profile, "tone:"); tone.set_in_and_out(0, 299);
        Mlt::Filter timer(profile, "timer"); video.attach(timer);   // burnt-in timecode, doc 12's M2 check
        tractor.set_track(video, 0); tractor.set_track(tone, 1);
        Mlt::Transition mix(profile, "mix"); mix.set("start", 1.0); mix.set("sum", 1); mix.set("always_active", 1);
        tractor.field()->plant_transition(mix, 0, 1);
        Mlt::Consumer c(profile, "avformat", argv[2]);
        c.set("vcodec", "libx264"); c.set("acodec", "aac"); c.set("ar", "48000"); c.set("channels", 2);
        c.set("pix_fmt", "yuv420p"); c.set("real_time", -1);
        c.connect(tractor);
        std::printf("render=%d\n", c.run());
    }
    Mlt::Factory::close();
}
