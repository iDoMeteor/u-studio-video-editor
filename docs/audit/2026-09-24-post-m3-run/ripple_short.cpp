#include "core/commands/primitives.h"
#include "core/commands/timeline_edits.h"
#include "core/model/model.h"
#include <cstdio>
using namespace ustudio::core;
int main()
{
    Model m = Model::createEmpty();
    AddTrack t(Track::Kind::Video, 0, "V1"); t.apply(m);
    Asset a; a.path = "color:red"; a.displayName = "red"; a.status = Asset::Status::Ready;
    a.info.hasVideo = true; a.info.lengthInSequenceFrames = 1000;
    AddAsset aa(a); aa.apply(m);
    InsertClip A(t.trackId(), aa.assetId(), 0, 0, 99); A.apply(m);
    InsertClip B(t.trackId(), aa.assetId(), 100, 0, 99); B.apply(m);
    InsertClip C(t.trackId(), aa.assetId(), 200, 0, 99); C.apply(m);
    // Ripple mode, drag B 50 frames right (drop at 150, inside B's own old span).
    RippleMove r1(B.clipId(), t.trackId(), 150);
    bool ok1 = r1.apply(m);
    std::printf("ripple B -> 150 (A,B,C butted): %s\n", ok1 ? "applied" : "REFUSED");
    // Same drag, 150 frames right (drop at 250, past its own end).
    RippleMove r2(B.clipId(), t.trackId(), 250);
    bool ok2 = r2.apply(m);
    std::printf("ripple B -> 250: %s; B now at %lld, C at %lld\n", ok2 ? "applied" : "REFUSED",
                static_cast<long long>(m.clip(B.clipId()).position), static_cast<long long>(m.clip(C.clipId()).position));
}
