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
    InsertClip A(t.trackId(), aa.assetId(), 0, 100, 199); A.apply(m);
    InsertClip B(t.trackId(), aa.assetId(), 100, 100, 199); B.apply(m);
    InsertClip C(t.trackId(), aa.assetId(), 200, 100, 199); C.apply(m);
    AddTransition d(t.trackId(), A.clipId(), B.clipId(), 10, 10); bool dok = d.apply(m);
    std::printf("dissolve A->B added: %d, transitions=%zu\n", dok, m.sequence().transitions.size());
    Model before = m;
    // The UI snaps a ripple drop inside C to C's start (200, pre-move timeline).
    RippleMove r(B.clipId(), t.trackId(), m.clip(C.clipId()).position);
    bool ok = r.apply(m);
    std::printf("ripple B -> C.start: %s; B at %lld C at %lld; transitions=%zu; model unchanged=%d\n",
                ok ? "applied" : "refused", static_cast<long long>(m.clip(B.clipId()).position),
                static_cast<long long>(m.clip(C.clipId()).position), m.sequence().transitions.size(),
                int(m.project() == before.project()));
}
