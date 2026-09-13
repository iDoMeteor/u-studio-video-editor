# ADR-002: Consumer-based playback

**Status:** Proposed

## Context
v1 pulls frames on its own thread and paces with a blocking PulseAudio
write. It has a constant A/V offset (~150 ms, the Pulse buffer), drifts on
silent clips, cannot drop frames, and has no speed/reverse. MLT ships
consumers that solve all of that: `sdl2_audio` (audio-only SDL consumer that
emits `consumer-frame-show` for video) and `rtaudio`, both present on this
machine. Kdenlive has used this exact pattern for years.

## Decision
`PlaybackController` owns an `Mlt::Consumer` chosen from `sdl2_audio` →
`rtaudio` → `null`. Video reaches GTK through the `consumer-frame-show`
event, a single-slot latest-frame mailbox, and the main-thread dispatcher.
Pause = speed 0 + purge + refresh. The position shown is the position of the
frame shown.

## Consequences
- Removes `libpulse-simple` and ~150 lines of pacing code.
- Adds a hard dependency on the SDL2 or RtAudio MLT module being present at
  runtime; `null` keeps the app usable without sound.
- Frame delivery is now driven by MLT's threads; all our handling must be
  lock-free on that path (copy the frame, post, return).
- Preview scale (`scale` property) becomes the primary performance knob.
