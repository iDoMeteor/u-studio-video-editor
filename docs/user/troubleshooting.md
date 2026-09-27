# Troubleshooting

[Docs home](../README.md) › [User guide](README.md) › Troubleshooting

## Reporting a bug

1. Open **Help › About** and click **Copy Diagnostics**. This copies the app,
   MLT, GTK and libadwaita versions, whether you're running the Flatpak,
   the log folder and the last 50 log lines. It never includes your
   environment or file contents.
2. Paste that into your report, along with what you did and what you
   expected.

**Open Log Folder** (same tab) opens the full logs:

| Install | Log folder |
|---|---|
| Flatpak | `~/.var/app/com.ustudio.VideoEditor/.local/state/ustudio/logs/` |
| Built from source | `~/.local/state/ustudio/logs/` (or `$XDG_STATE_HOME/ustudio/logs/`) |

## Common problems

### Playback stutters on 4K or phone footage

Set the preview scale to **Half** or **Quarter**, or make a
[proxy](importing-media.md#proxies-smooth-editing-of-4k-and-phone-footage).
A conformed proxy also fixes phone recordings with a variable frame rate.

### Playback looks wrong or glitches, or GPU acceleration turned itself off

Turn off **GPU acceleration** (Settings › Performance › Hardware): playback
then runs on the processor. If that fixes it, your graphics driver is the
cause; please report it with **Copy Diagnostics** from Help › About, which
includes what the GPU row says. If U Stu turned GPU acceleration off by
itself, it closed unexpectedly while using the graphics card last time;
turn it back on to try again.

### No sound

- Check the transport volume and that the track isn't muted (its name
  strip says "Muted").
- Check that the clip has sound: an audio-less video clip has no
  waveform.
- U Stu plays through PipeWire or PulseAudio. Check that the right
  output device is selected in your system's sound settings.

### Clips are red and striped

The media file has moved. Click **Relink…** in the banner
([Missing media](importing-media.md#missing-media)).

### The app closed unexpectedly

Start it again and accept the recovery offer. At most about two minutes of
work is lost ([Autosave](projects-and-saving.md#autosave-and-crash-recovery)).

### A drop or move is refused

Red while dragging means the edit isn't allowed: the clip would overlap
another, the track is locked, or the track is the wrong kind (audio vs
video).

### Flatpak installed into the wrong place from a VS Code terminal

Terminals inside the VS Code snap set `XDG_DATA_HOME` to a private folder,
so `flatpak --user` installs somewhere your desktop can't see. Install from
a normal terminal, or run `env -u XDG_DATA_HOME flatpak install …`.

## Known limitations

These are planned; see the [roadmap](../../README.md#roadmap).

- No effects or colour correction yet. They're coming as a drop-in.
- Titles are early (the Titles drop-in, not yet in the packages): `.ustitle`
  files play and follow edits to the file, but there's no title designer,
  and a moved title can't be relinked yet ([Titles](titles.md)).
- Renders are MP4 (H.264 + AAC) only.
- Markers can't be named, and the loop region isn't drawn on the timeline.
- Keyboard shortcuts can't be changed.
- Image sequences need their own command (Import Image Sequence…).
- Linux only for now. Windows is planned later; macOS isn't planned.
