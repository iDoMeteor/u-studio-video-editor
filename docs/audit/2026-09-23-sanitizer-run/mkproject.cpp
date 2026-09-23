#include "core/commands/primitives.h"
#include "core/xml/reader.h"
#include "core/xml/writer.h"
#include <cstdio>
using namespace ustudio::core;
int main(int, char **argv) {
    std::string media = argv[1], out = argv[2];
    Model m = Model::createEmpty();
    auto addTrack = [&](Track::Kind k, size_t i, const char *n) { AddTrack c(k, i, n); c.apply(m); return c.trackId(); };
    TrackId v1 = addTrack(Track::Kind::Video, 0, "V1");
    TrackId v2 = addTrack(Track::Kind::Video, 0, "V2");
    TrackId a1 = addTrack(Track::Kind::Audio, 2, "A1");
    Asset clip; clip.path = media + "/clip.mp4"; clip.displayName = "clip.mp4"; clip.status = Asset::Status::Ready;
    clip.info.hasVideo = true; clip.info.hasAudio = true; clip.info.lengthInSequenceFrames = 300;
    clip.info.fps = {30, 1}; clip.info.width = 1920; clip.info.height = 1080; clip.info.nativeDurationSeconds = 10; clip.info.container = "mp4";
    AddAsset ac(clip); ac.apply(m);
    Asset still; still.path = media + "/still.png"; still.displayName = "still.png"; still.status = Asset::Status::Ready;
    still.info.hasVideo = true; still.info.isStillImage = true; still.info.lengthInSequenceFrames = 120;
    AddAsset as(still); as.apply(m);
    InsertClip a(v1, ac.assetId(), 0, 0, 99); bool ok = a.apply(m);
    InsertClip b(v1, ac.assetId(), 100, 150, 249); ok &= b.apply(m);
    AddTransition t(v1, a.clipId(), b.clipId(), 10, 5); ok &= t.apply(m);
    InsertClip s(v2, as.assetId(), 30, 0, 119); ok &= s.apply(m);
    InsertClip au(a1, ac.assetId(), 0, 0, 199); ok &= au.apply(m);
    auto problems = m.check();
    std::printf("commands ok=%d check=%s\n", ok, problems.empty() ? "clean" : problems.front().c_str());
    std::string err = saveProject(m, out);
    auto back = loadProject(out);
    std::printf("save=%s reload=%s equal=%d\n", err.empty() ? "ok" : err.c_str(), back ? "ok" : back.error().c_str(),
                back ? int(back->project() == m.project()) : -1);
}
