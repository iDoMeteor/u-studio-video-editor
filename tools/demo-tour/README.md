# Demo tour

Drives the editor through every user-facing feature on an off-screen X
display and records it: screen, the app's own audio, burned-in chapter
captions, and an 8× fast-forward through the render wait. It doubles as a
visual regression pass: rerun it after a milestone and watch the video.

Not built by meson and not part of `meson test`. Media is never committed.

## Needs

`Xvfb`, `ffmpeg` (x11grab, libx264, libass), `python3` with `gi`/Atspi,
`python-xlib` and Pillow, ImageMagick's `import`, and the AT-SPI bus
(`/usr/libexec/at-spi-bus-launcher`, `at-spi2-registryd`). Everything
runs under `env -i` with scratch XDG directories, so the owner's settings,
autosaves and desktop are untouched, and nothing plays on the speakers
(SDL's disk driver feeds `audiorec.py`, which keeps audio in step across
the consumer restarts every edit causes).

## Media

`TOUR_MEDIA` points at a folder with `unicorn/` and `zizzle/` (numbered
scene clips), `stills/` (a few PNGs) and `finals/unicorn-dj-final.mp4`
(a finished cut with a soundtrack). The 2026-09-25 recording used copies
of the owner's AI-generated projects (unicorn DJ at 1080p30, zizzle zap
zone at 1344×768@25).

## One command (the rolling series)

```sh
tools/demo-tour/make_demo.sh   # from the demo worktree (agent/strategist-demo)
```

Stages the media once into `~/.cache/ustudio-demo-media` (copies from the
owner's drive, read-only), builds a separate release `builddir-demo`,
records, and saves `u-studio-demo-<date>-v<version>.mp4` into
`/home/jj/projects/u-studio-video-editor-projects/demo-videos/`. It never
overwrites or removes a video there; a repeat run the same day gets `-2`,
`-3`, and so on.

## Run by hand

```sh
TOUR_MEDIA=/path/to/media ./run_tour.sh /tmp/tour-out 1   # 1 = record
python3 post.py /tmp/tour-out /tmp/tour-out/u-studio-demo.mp4
```

`TOUR_UPTO=N` stops after chapter N; without recording, each chapter
leaves screenshots in the output folder for checking.

## Notes

- GTK4 reports no widget positions over AT-SPI, so the timeline is found by
  pixel: clips are selected and their cyan selection outline gives exact
  edges (`selected_box`).
- There's no window manager: `fitwin.py` sizes the main window to the
  screen, and `center_dialogs()` centres dialogs.
- File choosers get their path set through AT-SPI (`set_location`), not
  typed, which avoids the location entry's autocomplete race.
- Playback on Xvfb is software-rendered, so it's choppier than on a real
  desktop.
