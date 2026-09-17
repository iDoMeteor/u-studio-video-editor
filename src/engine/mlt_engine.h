#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <glib.h>

// Forward-declared so nothing outside this module needs an MLT include.
namespace Mlt {
class Profile;
class Playlist;
class Tractor;
} // namespace Mlt

// Forward-declared so nothing outside this module needs a PulseAudio include.
struct pa_simple;

// The only class in the codebase that touches MLT types directly. Everything
// above this (app/, ui/) talks to plain C++ types only, so the MLT engine
// could be swapped or the UI toolkit could change without the other side
// noticing.
//
// Playback model: rather than a push-style custom Mlt::Consumer, a dedicated
// worker thread pulls one frame at a time from the Tractor (which is itself
// a Producer), and hands the decoded RGBA buffer to the GTK main thread via
// g_idle_add(). MLT decodes on its own thread; GTK widgets may only be
// touched from the main thread, so every frame crosses that boundary through
// the GLib main loop rather than a raw callback. Playback is paced against a
// wall-clock schedule (re-anchored on every play/resume and every seek) —
// audio is written via a blocking PulseAudio call but is not trusted as the
// clock, since a buffer underrun makes the next write return instantly and
// would otherwise cause a runaway-fast feedback loop.
//
// Multi-track: each track is its own Mlt::Playlist set on the Tractor.
// Additional tracks (index > 0) get a "composite" transition (video,
// full-frame top-track-wins) and a "mix" transition (audio, constant full
// level via start=1/sum=1 — tracks do not mix by default) chained to the
// track directly below them.
class MltEngine
{
public:
    struct ClipInfo
    {
        std::string name;
        std::string resource; // source file path, for waveform lookups
        int trackIndex = 0;
        int startFrame = 0;
        int frames = 0;
        int in = 0; // source in/out, for waveform lookups (identifies the
        int out = 0; // exact trim, distinguishing re-trims of the same file)
    };

    // Invoked already marshaled onto the GLib main thread — safe to touch
    // GTK widgets directly from inside the callback.
    using FrameCallback = std::function<void(std::vector<uint8_t> rgba, int width, int height, int frameNumber)>;

    MltEngine();
    ~MltEngine();

    MltEngine(const MltEngine &) = delete;
    MltEngine &operator=(const MltEngine &) = delete;

    // Adds a new track on top of the existing ones (composited over them
    // for video, mixed in for audio) and returns its index. Track 0 always
    // exists from construction.
    int addTrack();
    int trackCount() const;

    // Removes a track and its clips. Refuses to remove the last remaining
    // track. Returns false if the index is invalid or it's the last track.
    bool removeTrack(int trackIndex);

    // Reorders a track (drag-to-reorder in the UI). Returns false if either
    // index is invalid.
    bool moveTrack(int fromIndex, int toIndex);

    // Appends a media file as a new clip on the given track. Returns false
    // and fills `error` if the file could not be opened by MLT or the track
    // index is invalid.
    bool importClip(const std::string &path, int trackIndex, std::string &error);

    // Splits the given track's playlist at an absolute timeline frame
    // position. Returns false if the position isn't inside the timeline.
    bool splitAt(int trackIndex, int absoluteFrame);

    // Moves the clip currently at (trackIndex, startFrame) to start at
    // destStartFrame on destTrack (same track allowed — a same-track
    // reposition). Only supported when the destination span is empty
    // (blank or past the track's current end) or is the clip's own current
    // span; this pass refuses to overwrite other clips, ripple-shift, or
    // otherwise touch unrelated content. Returns false and changes nothing
    // if the clip can't be found or the destination isn't free.
    bool moveClip(int trackIndex, int startFrame, int destTrack, int destStartFrame);

    // Drags the clip's left edge: newStartFrame becomes its new timeline
    // start while its timeline *end* stays fixed (so its length changes by
    // the same amount, and the source in-point shifts to compensate).
    // Clamped to the source media's own bounds. Subject to the same
    // destination-must-be-free rule as moveClip.
    bool trimClipStart(int trackIndex, int startFrame, int newStartFrame);

    // Drags the clip's right edge: newEndFrame (exclusive) becomes its new
    // timeline end while its start stays fixed. Clamped to the source
    // media's own bounds. Subsequent clips on the same track ripple to
    // accommodate the length change (ordinary Mlt::Playlist::resize_clip
    // behavior) — unlike moveClip/trimClipStart, this one does shift
    // later content, matching a standard ripple-trim.
    bool trimClipEnd(int trackIndex, int startFrame, int newEndFrame);

    // "Lift": removes the clip at (trackIndex, startFrame), leaving a blank
    // gap of the same length in its place — nothing else on the track
    // moves. Pairs with closeGap() as a deliberate two-step "cut a piece
    // out, then separately decide whether to ripple the rest of the track
    // up to fill the hole" workflow. Returns false if there's no clip
    // there.
    bool deleteClip(int trackIndex, int startFrame);

    // Removes the blank gap at (trackIndex, frame) — a right-click-in-a-gap
    // "Close Gap" action — rippling everything after it on that track
    // earlier to fill the space. Returns false if that position isn't a
    // blank.
    bool closeGap(int trackIndex, int frame);

    // True if (trackIndex, frame) falls inside a blank gap in the track's
    // current content (not past its end). Used to decide what a
    // right-click on the timeline should offer.
    bool isGapAt(int trackIndex, int frame) const;

    // Saves/loads the track/clip layout (tracks, each clip's file + in/out
    // trim points) as a small GKeyFile-format project file — not MLT's own
    // XML format. MLT's "xml" producer, when reloaded, presents itself as a
    // plain producer (Service::type() == mlt_service_producer_type, not
    // mlt_service_tractor_type) rather than a live editable Tractor, so
    // there's no safe way to get back per-track Playlist access from it
    // (confirmed empirically — wrapping it as Mlt::Tractor silently yields
    // zero tracks and segfaults on playback). Owning our own minimal format
    // and reconstructing via the same addTrack()/append() primitives used
    // for normal editing sidesteps that entirely.
    bool saveProject(const std::string &path, std::string &error);
    bool loadProject(const std::string &path, std::string &error);

    // Renders the whole project to outputPath as H.264 (High, yuv420p,
    // 1920x1080, 30fps) + AAC (48kHz stereo) in an MP4 container — matching
    // this project's fixed working profile/format (see the profile comment
    // in the constructor). Blocking and potentially slow (real encode
    // time); the caller is expected to run this on its own thread, not the
    // GTK main thread. Builds a completely separate, throwaway
    // Profile/Tractor from a snapshot of the current tracks/clips rather
    // than rendering through the live m_tractor, so editing/playback isn't
    // blocked or disturbed for however long the render takes.
    bool renderProject(const std::string &outputPath, std::string &error);

    void play();
    void pause();
    void togglePlay();
    bool isPlaying() const { return m_playing.load(); }

    // Non-blocking: requests the worker thread seek and deliver one frame.
    void seek(int frame);

    int currentFrame() const { return m_lastKnownFrame.load(); }
    int totalFrames() const;
    double fps() const;

    std::vector<ClipInfo> clips() const;

    void setFrameCallback(FrameCallback cb);

private:
    struct PendingFrame
    {
        MltEngine *engine;
        std::vector<uint8_t> rgba;
        int width;
        int height;
        int frameNumber;
    };

    void pullLoopMain();
    static gboolean deliverOnMainThread(gpointer data);

    // Plants the composite (video, full-frame top-wins) + mix (audio,
    // start=1/sum=1 — see rebuildTractor) transition pair connecting track
    // `index` to `index - 1` on `tractor`/`profile`. Shared by
    // rebuildTractor (the live m_tractor) and renderProject (a throwaway
    // render-only tractor) so the two don't silently drift apart.
    static void plantTrackTransitions(Mlt::Profile &profile, Mlt::Tractor &tractor, int index);

    // Rebuilds m_tractor from scratch against the current m_tracks order:
    // re-registers every track and re-plants the composite/mix transition
    // chain. Used by addTrack/removeTrack/moveTrack rather than trying to
    // surgically patch the Field's transition graph — a transition is
    // planted between two specific track *indices*, so removing or
    // reordering a track shifts every index above/around it anyway.
    // Rebuilding is cheap (no decoding involved, just re-wiring services)
    // and avoids having to hand-verify yet another piece of MLT's internal
    // behavior for a surgical remove/reorder path. Caller must hold
    // m_mltMutex. Preserves the current playhead position.
    void rebuildTractor();

    // Read-only check for moveClip/trimClipStart: is [start, start+length)
    // on `playlist` entirely blank (or past its current end), aside from
    // `ignoreIndex` (the clip's own current entry, when checking a
    // same-track destination that overlaps where it already sits)? Caller
    // must hold m_mltMutex.
    bool isRangeFree(Mlt::Playlist &playlist, int start, int length, int ignoreIndex) const;

    // Pads `playlist` with blank if needed, then carves an exact
    // [start, start+length) hole (via split_at at both boundaries + remove)
    // and inserts a fresh producer for `resource` trimmed to [in, out].
    // Caller must hold m_mltMutex and must already have confirmed (via
    // isRangeFree, before making any of its own changes) that the range is
    // actually free — this does not re-check and will happily consume
    // whatever single entry now occupies the carved boundaries.
    bool carveAndInsert(Mlt::Playlist &playlist, int start, int length, const std::string &resource, int in, int out);

    std::unique_ptr<Mlt::Profile> m_profile;
    std::vector<std::unique_ptr<Mlt::Playlist>> m_tracks;
    std::unique_ptr<Mlt::Tractor> m_tractor;
    pa_simple *m_audioStream = nullptr;

    mutable std::mutex m_mltMutex;
    std::thread m_worker;
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_quit{false};
    std::atomic<int> m_seekRequest{-1};
    std::atomic<int> m_lastKnownFrame{0};
    // Mirrors m_tractor->get_length(), updated whenever it changes (import,
    // split). Lets seek()/totalFrames() answer without touching m_mltMutex —
    // that mutex can be held by the worker thread for a real stretch during
    // an expensive seek on a large file, and a UI callback blocking on it
    // (e.g. every "value-changed" tick while dragging the seek scale during
    // playback) is what froze the app.
    std::atomic<int> m_totalFramesCache{0};

    FrameCallback m_callback;
};
