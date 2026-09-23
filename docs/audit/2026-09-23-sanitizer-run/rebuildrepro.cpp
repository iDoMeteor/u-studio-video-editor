// Mimics EngineSync::rebuildAll(): black track + 3 playlists, composite+mix planted
// between each adjacent pair via Tractor::field(). Pulls a few frames per rebuild so
// the mix transitions allocate their audio buffers, as they do under a real consumer.
#include <mlt++/Mlt.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
static long rssKb() { std::ifstream f("/proc/self/status"); std::string l; while (std::getline(f, l)) if (l.rfind("VmRSS:", 0) == 0) return std::stol(l.substr(6)); return 0; }
int main(int, char **argv) {
    Mlt::Factory::init(argv[1]);
    bool deleteField = !std::strcmp(argv[2], "delete");
    {
        Mlt::Profile profile("atsc_1080p_30");
        Mlt::Producer master(profile, "tone:"); master.set_in_and_out(0, 299);
        for (int r = 1; r <= 300; ++r) {
            auto tractor = std::make_shared<Mlt::Tractor>(profile);
            Mlt::Producer black(profile, "colour", "black"); black.set_in_and_out(0, 299);
            tractor->set_track(black, 0);
            for (int t = 1; t <= 3; ++t) {
                Mlt::Playlist pl(profile);
                std::unique_ptr<Mlt::Producer> cut(master.cut(0, 99)); pl.append(*cut);
                tractor->set_track(pl, t);
            }
            for (int i = 1; i < tractor->count(); ++i) {
                Mlt::Transition composite(profile, "composite");
                Mlt::Transition mix(profile, "mix"); mix.set("start", 1.0); mix.set("sum", 1); mix.set("always_active", 1);
                if (deleteField) {
                    std::unique_ptr<Mlt::Field> f1(tractor->field()); f1->plant_transition(composite, i - 1, i);
                    std::unique_ptr<Mlt::Field> f2(tractor->field()); f2->plant_transition(mix, i - 1, i);
                } else {
                    tractor->field()->plant_transition(composite, i - 1, i);
                    tractor->field()->plant_transition(mix, i - 1, i);
                }
            }
            for (int k = 0; k < 3; ++k) {
                std::unique_ptr<Mlt::Frame> fr(tractor->get_frame());
                mlt_audio_format af = mlt_audio_s16; int freq = 48000, ch = 2, samples = 1600;
                fr->get_audio(af, freq, ch, samples);
            }
            if (r % 100 == 0) std::printf("%s rebuild %3d: RSS %ld MB\n", argv[2], r, rssKb() / 1024);
        }
    }
    Mlt::Factory::close();
}
