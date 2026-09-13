#include "mlt_engine.h"

#include "util/log.h"

#include <mlt++/Mlt.h>

#include <pulse/error.h>
#include <pulse/simple.h>

#include <algorithm>
#include <chrono>

namespace {
constexpr int kAudioRate = 48000;
constexpr int kAudioChannels = 2;
} // namespace

MltEngine::MltEngine()
{
    // Repository* is intentionally not retained: it lives for the process
    // lifetime and Factory::close() releases MLT's global state regardless.
    Mlt::Factory::init();

    m_profile = std::make_unique<Mlt::Profile>("atsc_1080p_25");
    m_tractor = std::make_unique<Mlt::Tractor>(*m_profile);
    addTrack(); // track 0 always exists

    pa_sample_spec spec;
    spec.format = PA_SAMPLE_S16LE;
    spec.rate = kAudioRate;
    spec.channels = kAudioChannels;

    // A blocking pa_simple_write() only paces us to real time once the
    // server's buffer is full enough to push back. With the default (no
    // explicit attr), that buffer can be large enough that the first
    // stretch of playback decodes/writes far ahead of real time before
    // backpressure kicks in, which plays back like the whole clip is
    // running fast. Pin the buffer to ~150ms so backpressure starts
    // immediately.
    pa_buffer_attr attr;
    size_t targetBytes = pa_usec_to_bytes(150000, &spec);
    attr.maxlength = static_cast<uint32_t>(targetBytes);
    attr.tlength = static_cast<uint32_t>(targetBytes);
    // NOT 0: per <pulse/def.h>, prebuf=0 means "manual start/stop control" —
    // playback then never starts on its own, since only pa_stream_cork()
    // would trigger it, which the blocking simple API never calls. -1 is
    // the documented "same as tlength" default: normal auto-start once
    // ~150ms is buffered.
    attr.prebuf = static_cast<uint32_t>(-1);
    attr.minreq = static_cast<uint32_t>(targetBytes / 4);
    attr.fragsize = static_cast<uint32_t>(-1);

    int paError = 0;
    m_audioStream = pa_simple_new(
        nullptr, "u Studio Video Editor", PA_STREAM_PLAYBACK, nullptr, "preview", &spec, nullptr, &attr, &paError);
    if (!m_audioStream) {
        Log::warn(std::string("Audio output unavailable (") + pa_strerror(paError) + "); preview will be silent.");
    } else {
        Log::info("Audio output opened (48kHz stereo S16LE, ~150ms buffer)");
    }

    m_worker = std::thread([this] { pullLoopMain(); });
    Log::debug("MltEngine constructed, worker thread started");
}

MltEngine::~MltEngine()
{
    Log::debug("MltEngine shutting down");
    m_quit.store(true);
    if (m_worker.joinable())
        m_worker.join();

    if (m_audioStream)
        pa_simple_free(m_audioStream);

    // Destroy MLT objects before tearing down the factory.
    m_tractor.reset();
    m_tracks.clear();
    m_profile.reset();

    Mlt::Factory::close();
}

int MltEngine::addTrack()
{
    std::lock_guard<std::mutex> lock(m_mltMutex);

    auto track = std::make_unique<Mlt::Playlist>(*m_profile);
    int newIndex = static_cast<int>(m_tracks.size());
    m_tracks.push_back(std::move(track));
    rebuildTractor();
    Log::info("Added track " + std::to_string(newIndex));
    return newIndex;
}

int MltEngine::trackCount() const
{
    std::lock_guard<std::mutex> lock(m_mltMutex);
    return static_cast<int>(m_tracks.size());
}

bool MltEngine::removeTrack(int trackIndex)
{
    std::lock_guard<std::mutex> lock(m_mltMutex);

    if (trackIndex < 0 || trackIndex >= static_cast<int>(m_tracks.size()))
        return false;
    if (m_tracks.size() <= 1) {
        Log::warn("Refusing to remove the last remaining track");
        return false;
    }

    m_tracks.erase(m_tracks.begin() + trackIndex);
    rebuildTractor();
    Log::info("Removed track " + std::to_string(trackIndex));
    return true;
}

bool MltEngine::moveTrack(int fromIndex, int toIndex)
{
    std::lock_guard<std::mutex> lock(m_mltMutex);

    int count = static_cast<int>(m_tracks.size());
    if (fromIndex < 0 || fromIndex >= count || toIndex < 0 || toIndex >= count || fromIndex == toIndex)
        return false;

    auto track = std::move(m_tracks[fromIndex]);
    m_tracks.erase(m_tracks.begin() + fromIndex);
    m_tracks.insert(m_tracks.begin() + toIndex, std::move(track));
    rebuildTractor();
    Log::info("Moved track " + std::to_string(fromIndex) + " to " + std::to_string(toIndex));
    return true;
}

void MltEngine::rebuildTractor()
{
    int preservedPosition = m_tractor ? m_tractor->position() : 0;

    auto newTractor = std::make_unique<Mlt::Tractor>(*m_profile);
    for (size_t i = 0; i < m_tracks.size(); ++i) {
        int index = static_cast<int>(i);
        newTractor->set_track(*m_tracks[i], index);

        if (index > 0) {
            // Video: plain "composite" already does a full-frame
            // top-track-wins overlay by default (verified empirically — no
            // PIP geometry gotcha). Chained pairwise (i-1, i) so N tracks
            // stack correctly, and a pure-audio clip on the upper track
            // leaves the video below untouched (also verified).
            Mlt::Transition composite(*m_profile, "composite");
            newTractor->field()->plant_transition(composite, index - 1, index);

            // Audio: tracks do NOT auto-mix — verified empirically that
            // without an explicit "mix" transition, only track 0's audio is
            // audible at all. "start=1" sets a constant (non-crossfade)
            // full mix level; "sum=1" is required too — the default
            // halve-then-add algorithm measured no different from not
            // mixing at all in testing, while sum=1 measurably summed both
            // tracks' energy correctly.
            Mlt::Transition mix(*m_profile, "mix");
            mix.set("start", 1.0);
            mix.set("sum", 1);
            mix.set("always_active", 1);
            newTractor->field()->plant_transition(mix, index - 1, index);
        }
    }

    m_tractor = std::move(newTractor);
    m_tractor->refresh();
    if (preservedPosition > 0)
        m_tractor->seek(preservedPosition);
    m_totalFramesCache.store(m_tractor->get_length());
}

bool MltEngine::importClip(const std::string &path, int trackIndex, std::string &error)
{
    std::lock_guard<std::mutex> lock(m_mltMutex);

    if (trackIndex < 0 || trackIndex >= static_cast<int>(m_tracks.size())) {
        error = "Invalid track index: " + std::to_string(trackIndex);
        Log::error(error);
        return false;
    }

    Mlt::Producer producer(*m_profile, path.c_str());
    if (!producer.is_valid()) {
        error = "Could not open media file: " + path;
        Log::error(error);
        return false;
    }

    m_tracks[trackIndex]->append(producer);
    m_tractor->refresh();
    m_totalFramesCache.store(m_tractor->get_length());
    Log::info(
        "Imported clip to track " + std::to_string(trackIndex) + ": " + path + " ("
        + std::to_string(producer.get_length()) + " frames)");
    return true;
}

bool MltEngine::splitAt(int trackIndex, int absoluteFrame)
{
    std::lock_guard<std::mutex> lock(m_mltMutex);

    if (trackIndex < 0 || trackIndex >= static_cast<int>(m_tracks.size()))
        return false;
    if (absoluteFrame <= 0 || absoluteFrame >= m_tractor->get_length())
        return false;

    // Mlt::Playlist::split_at() does not return a status code — it returns
    // the (possibly clamped) position it operated at, even when nothing
    // actually split (e.g. the position already sits on a clip boundary).
    // Confirmed empirically: splitting at an existing boundary returns the
    // same value as a real split. The only reliable success signal is
    // whether a new clip entry actually appeared.
    Mlt::Playlist &playlist = *m_tracks[trackIndex];
    int before = playlist.count();
    playlist.split_at(absoluteFrame, true);
    bool didSplit = playlist.count() > before;
    if (didSplit) {
        m_tractor->refresh();
        m_totalFramesCache.store(m_tractor->get_length());
        Log::info("Split track " + std::to_string(trackIndex) + " at frame " + std::to_string(absoluteFrame));
    } else {
        Log::debug(
            "Split track " + std::to_string(trackIndex) + " at frame " + std::to_string(absoluteFrame)
            + " was a no-op (clip boundary)");
    }
    return didSplit;
}

bool MltEngine::isRangeFree(Mlt::Playlist &playlist, int start, int length, int ignoreIndex) const
{
    int existingLength = playlist.get_length();
    for (int f = start; f < start + length;) {
        if (f >= existingLength)
            break; // past current content -- fine, carveAndInsert pads with blank
        int idx = playlist.get_clip_index_at(f);
        if (idx < 0)
            break;
        if (!playlist.is_blank(idx) && idx != ignoreIndex)
            return false;
        f = playlist.clip_start(idx) + playlist.clip_length(idx);
    }
    return true;
}

bool MltEngine::carveAndInsert(
    Mlt::Playlist &playlist, int start, int length, const std::string &resource, int in, int out)
{
    int neededLength = start + length;
    if (neededLength > playlist.get_length()) {
        int pad = neededLength - playlist.get_length();
        playlist.blank(pad - 1); // blank(out) appends out+1 frames
    }

    // Carve an exact [start, start+length) hole: split at both boundaries
    // (works on blanks and real clips alike — verified empirically), then
    // remove the single resulting entry that now spans exactly that range.
    playlist.split_at(start, true);
    playlist.split_at(start + length, true);
    int holeIndex = playlist.get_clip_index_at(start);
    if (holeIndex < 0)
        return false;
    playlist.remove(holeIndex);

    Mlt::Producer fresh(*m_profile, resource.c_str());
    if (!fresh.is_valid())
        return false;
    playlist.insert(fresh, holeIndex, in, out);
    return true;
}

bool MltEngine::moveClip(int trackIndex, int startFrame, int destTrack, int destStartFrame)
{
    std::lock_guard<std::mutex> lock(m_mltMutex);

    if (trackIndex < 0 || trackIndex >= static_cast<int>(m_tracks.size()))
        return false;
    if (destTrack < 0 || destTrack >= static_cast<int>(m_tracks.size()))
        return false;

    Mlt::Playlist &src = *m_tracks[trackIndex];
    int srcIndex = src.get_clip_index_at(startFrame);
    if (srcIndex < 0 || src.is_blank(srcIndex))
        return false;

    std::unique_ptr<Mlt::Producer> clip(src.get_clip(srcIndex));
    if (!clip)
        return false;
    int in = clip->get_in();
    int out = clip->get_out();
    int length = out - in + 1;
    const char *resourceRaw = clip->parent().get("resource");
    if (!resourceRaw)
        return false;
    std::string resource = resourceRaw;

    Mlt::Playlist &dst = *m_tracks[destTrack];
    bool sameTrack = (trackIndex == destTrack);

    // Validate BEFORE mutating anything, so a refused move changes nothing.
    // When checking a same-track destination that overlaps the clip's own
    // current span, that overlap is fine (ignoreIndex) — it'll genuinely
    // be blank in a moment.
    if (!isRangeFree(dst, destStartFrame, length, sameTrack ? srcIndex : -1)) {
        Log::debug("moveClip: destination occupied, refusing");
        return false;
    }

    std::unique_ptr<Mlt::Producer> removedBlank(src.replace_with_blank(srcIndex));

    if (!carveAndInsert(dst, destStartFrame, length, resource, in, out)) {
        Log::error("moveClip: carve/insert failed after validation passed — this shouldn't happen");
        return false;
    }

    m_tractor->refresh();
    m_totalFramesCache.store(m_tractor->get_length());
    Log::info(
        "Moved clip: track " + std::to_string(trackIndex) + "@" + std::to_string(startFrame) + " -> track "
        + std::to_string(destTrack) + "@" + std::to_string(destStartFrame));
    return true;
}

bool MltEngine::trimClipStart(int trackIndex, int startFrame, int newStartFrame)
{
    std::lock_guard<std::mutex> lock(m_mltMutex);

    if (trackIndex < 0 || trackIndex >= static_cast<int>(m_tracks.size()))
        return false;

    Mlt::Playlist &playlist = *m_tracks[trackIndex];
    int index = playlist.get_clip_index_at(startFrame);
    if (index < 0 || playlist.is_blank(index))
        return false;

    std::unique_ptr<Mlt::Producer> clip(playlist.get_clip(index));
    if (!clip)
        return false;
    int in = clip->get_in();
    int out = clip->get_out();
    const char *resourceRaw = clip->parent().get("resource");
    if (!resourceRaw)
        return false;
    std::string resource = resourceRaw;

    // The clip's timeline *end* (startFrame + length) stays fixed; only its
    // start moves, so its length changes by the same delta and the source
    // in-point shifts to compensate. Clamp to keep at least 1 frame and
    // stay within the source's own in/out bounds (out itself is untouched
    // here, so [0, out-1] is always valid regardless of the source's full
    // native length).
    int delta = newStartFrame - startFrame;
    int newIn = std::clamp(in + delta, 0, out - 1);
    int actualDelta = newIn - in;
    int actualNewStart = startFrame + actualDelta;
    int newLength = out - newIn + 1;

    if (actualDelta == 0)
        return false; // clamped to no-op

    if (!isRangeFree(playlist, actualNewStart, newLength, index)) {
        Log::debug("trimClipStart: destination occupied, refusing");
        return false;
    }

    std::unique_ptr<Mlt::Producer> removedBlank(playlist.replace_with_blank(index));

    if (!carveAndInsert(playlist, actualNewStart, newLength, resource, newIn, out)) {
        Log::error("trimClipStart: carve/insert failed after validation passed — this shouldn't happen");
        return false;
    }

    m_tractor->refresh();
    m_totalFramesCache.store(m_tractor->get_length());
    Log::info(
        "Trimmed start of clip on track " + std::to_string(trackIndex) + ": " + std::to_string(startFrame) + " -> "
        + std::to_string(actualNewStart));
    return true;
}

bool MltEngine::trimClipEnd(int trackIndex, int startFrame, int newEndFrame)
{
    std::lock_guard<std::mutex> lock(m_mltMutex);

    if (trackIndex < 0 || trackIndex >= static_cast<int>(m_tracks.size()))
        return false;

    Mlt::Playlist &playlist = *m_tracks[trackIndex];
    int index = playlist.get_clip_index_at(startFrame);
    if (index < 0 || playlist.is_blank(index))
        return false;

    std::unique_ptr<Mlt::Producer> clip(playlist.get_clip(index));
    if (!clip)
        return false;
    int in = clip->get_in();
    int out = clip->get_out();
    int maxOut = clip->parent().get_length() - 1;

    // Timeline start stays fixed; only the end (and thus length) changes.
    // resize_clip ripples subsequent clips on this track to accommodate —
    // that's the intended ripple-trim behavior here, unlike moveClip/
    // trimClipStart which refuse to disturb anything else.
    int newOut = std::clamp(newEndFrame - 1, in + 1, maxOut);
    if (newOut == out)
        return false; // clamped to no-op

    playlist.resize_clip(index, in, newOut);
    m_tractor->refresh();
    m_totalFramesCache.store(m_tractor->get_length());
    Log::info(
        "Trimmed end of clip on track " + std::to_string(trackIndex) + " at " + std::to_string(startFrame) + ": out "
        + std::to_string(out) + " -> " + std::to_string(newOut));
    return true;
}

bool MltEngine::saveProject(const std::string &path, std::string &error)
{
    std::lock_guard<std::mutex> lock(m_mltMutex);

    GKeyFile *keyFile = g_key_file_new();
    g_key_file_set_integer(keyFile, "Project", "TrackCount", static_cast<int>(m_tracks.size()));

    for (size_t t = 0; t < m_tracks.size(); ++t) {
        std::string group = "Track" + std::to_string(t);
        Mlt::Playlist &playlist = *m_tracks[t];
        int count = playlist.count();

        int savedCount = 0;
        for (int i = 0; i < count; ++i) {
            if (playlist.is_blank(i)) {
                // No UI path creates gaps today; skip rather than guess at
                // a round-trip format for something unreachable.
                Log::debug("Skipping blank entry on track " + std::to_string(t) + " while saving");
                continue;
            }

            std::unique_ptr<Mlt::Producer> clip(playlist.get_clip(i));
            if (!clip)
                continue;

            // playlist.get_clip() returns a "cut" producer — its own
            // "resource" property is a placeholder ("<producer>"); the real
            // file path lives on the cut's parent. Confirmed empirically
            // (this is exactly what put the same literal "<producer>"
            // string in both tracks' saved Resource field).
            const char *resource = clip->parent().get("resource");
            std::string key = "Clip" + std::to_string(savedCount);
            g_key_file_set_string(keyFile, group.c_str(), (key + "Resource").c_str(), resource ? resource : "");
            g_key_file_set_integer(keyFile, group.c_str(), (key + "In").c_str(), clip->get_in());
            g_key_file_set_integer(keyFile, group.c_str(), (key + "Out").c_str(), clip->get_out());
            ++savedCount;
        }
        g_key_file_set_integer(keyFile, group.c_str(), "ClipCount", savedCount);
    }

    GError *gerror = nullptr;
    bool ok = g_key_file_save_to_file(keyFile, path.c_str(), &gerror);
    if (!ok) {
        error = gerror ? gerror->message : "Failed to write project file";
        Log::error("Save failed: " + error);
        if (gerror)
            g_error_free(gerror);
    } else {
        Log::info("Saved project to " + path + " (" + std::to_string(m_tracks.size()) + " tracks)");
    }
    g_key_file_free(keyFile);
    return ok;
}

bool MltEngine::loadProject(const std::string &path, std::string &error)
{
    GKeyFile *keyFile = g_key_file_new();
    GError *gerror = nullptr;
    if (!g_key_file_load_from_file(keyFile, path.c_str(), G_KEY_FILE_NONE, &gerror)) {
        error = gerror ? gerror->message : "Failed to read project file";
        Log::error("Load failed: " + error);
        if (gerror)
            g_error_free(gerror);
        g_key_file_free(keyFile);
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mltMutex);

    int trackCount = g_key_file_get_integer(keyFile, "Project", "TrackCount", nullptr);
    std::vector<std::unique_ptr<Mlt::Playlist>> newTracks;

    for (int t = 0; t < trackCount; ++t) {
        std::string group = "Track" + std::to_string(t);
        auto playlist = std::make_unique<Mlt::Playlist>(*m_profile);
        int clipCount = g_key_file_get_integer(keyFile, group.c_str(), "ClipCount", nullptr);

        for (int i = 0; i < clipCount; ++i) {
            std::string key = "Clip" + std::to_string(i);
            gchar *resource = g_key_file_get_string(keyFile, group.c_str(), (key + "Resource").c_str(), nullptr);
            if (!resource)
                continue;

            int in = g_key_file_get_integer(keyFile, group.c_str(), (key + "In").c_str(), nullptr);
            int out = g_key_file_get_integer(keyFile, group.c_str(), (key + "Out").c_str(), nullptr);

            Mlt::Producer producer(*m_profile, resource);
            if (producer.is_valid()) {
                playlist->append(producer, in, out);
            } else {
                Log::warn(std::string("Load: could not reopen media file, skipping: ") + resource);
            }
            g_free(resource);
        }
        newTracks.push_back(std::move(playlist));
    }

    if (newTracks.empty())
        newTracks.push_back(std::make_unique<Mlt::Playlist>(*m_profile)); // always keep at least one track

    m_tracks = std::move(newTracks);
    rebuildTractor();
    m_tractor->seek(0); // rebuildTractor() preserves the *previous* project's playhead; a fresh load starts at 0
    m_lastKnownFrame.store(0);
    g_key_file_free(keyFile);
    Log::info("Loaded project from " + path + " (" + std::to_string(m_tracks.size()) + " tracks)");
    return true;
}

void MltEngine::play()
{
    Log::debug("play() at frame " + std::to_string(currentFrame()));
    m_playing.store(true);
}

void MltEngine::pause()
{
    Log::debug("pause() at frame " + std::to_string(currentFrame()));
    m_playing.store(false);
}

void MltEngine::togglePlay()
{
    if (m_playing.load())
        pause();
    else
        play();
}

void MltEngine::seek(int frame)
{
    int length = totalFrames();
    if (frame < 0)
        frame = 0;
    if (length > 0 && frame >= length)
        frame = length - 1;
    m_seekRequest.store(frame);
}

int MltEngine::totalFrames() const
{
    return m_totalFramesCache.load();
}

double MltEngine::fps() const
{
    return m_profile->fps();
}

std::vector<MltEngine::ClipInfo> MltEngine::clips() const
{
    std::lock_guard<std::mutex> lock(m_mltMutex);

    std::vector<ClipInfo> result;
    for (size_t t = 0; t < m_tracks.size(); ++t) {
        Mlt::Playlist &playlist = *m_tracks[t];
        int count = playlist.count();
        for (int i = 0; i < count; ++i) {
            if (playlist.is_blank(i))
                continue;

            std::string name = "Clip";
            std::unique_ptr<Mlt::Producer> clip(playlist.get_clip(i));
            if (clip) {
                // See saveProject(): get_clip() is a "cut", read the real
                // path from its parent.
                const char *resource = clip->parent().get("resource");
                if (resource) {
                    std::string full(resource);
                    auto pos = full.find_last_of('/');
                    name = (pos == std::string::npos) ? full : full.substr(pos + 1);
                }
            }

            ClipInfo info;
            info.name = name;
            info.trackIndex = static_cast<int>(t);
            info.startFrame = playlist.clip_start(i);
            info.frames = playlist.clip_length(i);
            result.push_back(std::move(info));
        }
    }
    return result;
}

void MltEngine::setFrameCallback(FrameCallback cb)
{
    m_callback = std::move(cb);
}

void MltEngine::pullLoopMain()
{
    using namespace std::chrono;

    // Wall-clock playback schedule. A blocking pa_simple_write() looked like
    // a natural pacing signal, but it isn't a safe one: the moment the
    // server's buffer underruns (even briefly, e.g. audible as a click), the
    // very next write is accepted instantly instead of blocking, and the
    // loop races ahead pulling frames until the buffer re-fills — which
    // sounds exactly like alternating fast bursts and static. Anchoring to
    // a wall-clock schedule instead is immune to that feedback loop: audio
    // writes still happen, but they no longer decide our timing.
    bool wasPlaying = false;
    steady_clock::time_point playStartWallClock;
    int playStartFrame = 0;

    while (!m_quit.load()) {
        int seekTo = m_seekRequest.exchange(-1);
        bool playing = m_playing.load();

        if (seekTo < 0 && !playing) {
            wasPlaying = false;
            std::this_thread::sleep_for(milliseconds(30));
            continue;
        }

        // Re-anchor on any resume from idle AND on any seek while already
        // playing (a scrub during playback). Without the seekTo>=0 half of
        // this, a mid-playback scrub leaves the schedule anchored to
        // wherever playback originally started: elapsedFrames vs. real
        // wall-clock time then diverges by tens of seconds, "behind
        // schedule" never recovers, and the loop stops sleeping entirely
        // for the rest of the session — pegging a core and flooding
        // g_idle_add() fast enough to make the whole app unresponsive.
        // Confirmed via debug logs: frame numbers jumping around (scrubs)
        // immediately followed by "behind schedule" climbing without bound.
        if (playing && (!wasPlaying || seekTo >= 0)) {
            std::lock_guard<std::mutex> lock(m_mltMutex);
            playStartWallClock = steady_clock::now();
            playStartFrame = seekTo >= 0 ? seekTo : m_tractor->position();
        }
        wasPlaying = playing;

        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
        int frameNumber = 0;
        std::vector<uint8_t> audioPcm;

        {
            std::lock_guard<std::mutex> lock(m_mltMutex);

            if (seekTo >= 0)
                m_tractor->seek(seekTo);

            std::unique_ptr<Mlt::Frame> frame(m_tractor->get_frame());
            if (frame && frame->is_valid()) {
                mlt_image_format iFormat = mlt_image_rgba;
                int fw = 0;
                int fh = 0;
                uint8_t *image = frame->get_image(iFormat, fw, fh);
                if (image && fw > 0 && fh > 0) {
                    rgba.assign(image, image + static_cast<size_t>(fw) * fh * 4);
                    width = fw;
                    height = fh;
                }
                frameNumber = frame->get_position();

                // Only pull audio while actually playing — not on a
                // scrub-while-paused seek, which would otherwise play a
                // burst of stale audio for every frame dragged past.
                if (playing && m_audioStream) {
                    mlt_audio_format aFormat = mlt_audio_s16;
                    int frequency = kAudioRate;
                    int channels = kAudioChannels;
                    int samples =
                        mlt_audio_calculate_frame_samples(static_cast<float>(m_profile->fps()), frequency, frameNumber);
                    void *audio = frame->get_audio(aFormat, frequency, channels, samples);
                    if (audio && samples > 0) {
                        size_t bytes = static_cast<size_t>(samples) * channels * sizeof(int16_t);
                        auto *bytesPtr = static_cast<uint8_t *>(audio);
                        audioPcm.assign(bytesPtr, bytesPtr + bytes);
                    }
                }
            }

            // Deliberately no seek(position + 1) here for the normal
            // playing-forward case: get_frame() already auto-advances the
            // producer's internal position for the next sequential call.
            // Forcing an explicit seek before every frame instead makes the
            // producer treat each step as a random-access seek rather than
            // a cheap sequential decode — on a real file with long GOPs/
            // B-frames this desyncs audio badly (verified empirically: it
            // cut a real 88s clip's actual audio content down to the first
            // ~43s, silence after). seek() is still used above, but only
            // when there's an explicit seek request (scrub/jump).
            if (playing && frameNumber + 1 >= m_tractor->get_length())
                m_playing.store(false);
        }

        m_lastKnownFrame.store(frameNumber);

        if (!rgba.empty()) {
            auto *pending = new PendingFrame{this, std::move(rgba), width, height, frameNumber};
            g_idle_add(&MltEngine::deliverOnMainThread, pending);
        }

        // Write audio (still blocking — that's fine, it just contributes to
        // this iteration's real elapsed time now rather than being trusted
        // as the clock), then correct against the wall-clock schedule.
        if (playing && !audioPcm.empty() && m_audioStream) {
            int paError = 0;
            pa_simple_write(m_audioStream, audioPcm.data(), audioPcm.size(), &paError);
        }

        if (playing) {
            int elapsedFrames = frameNumber - playStartFrame + 1;
            double frameDurationSec = 1.0 / fps();
            auto target = playStartWallClock
                + duration_cast<steady_clock::duration>(duration<double>(elapsedFrames * frameDurationSec));
            auto now = steady_clock::now();
            if (now < target) {
                std::this_thread::sleep_for(target - now);
            } else {
                // Behind schedule — don't sleep (catching up without a
                // corrective delay is what avoids a runaway-fast feedback
                // loop), but log it: a large or growing lag here is exactly
                // the signal that decode/seek can't keep up with real time
                // for a given file.
                auto behindMs = duration_cast<milliseconds>(now - target).count();
                if (behindMs > 100) {
                    Log::debug(
                        "Playback behind schedule by " + std::to_string(behindMs) + "ms at frame "
                        + std::to_string(frameNumber));
                }
            }
        }
    }
}

gboolean MltEngine::deliverOnMainThread(gpointer data)
{
    std::unique_ptr<PendingFrame> pending(static_cast<PendingFrame *>(data));
    if (pending->engine->m_callback) {
        pending->engine->m_callback(std::move(pending->rgba), pending->width, pending->height, pending->frameNumber);
    }
    return G_SOURCE_REMOVE;
}
