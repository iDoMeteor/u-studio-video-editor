# Rendering (exporting)

[Docs home](../README.md) › [User guide](README.md) › Rendering

## Render a video

Click **Render…** in the header bar and choose where to save. The file name
is filled in for you as `<project>-<profile>-YYYYMMDD-HHMMSS.mp4`.

- The render runs in the background, so you can keep editing and playing.
  It uses the project as it was when you started the render.
- The Render button fills magenta as it progresses and shows a badge
  counting queued renders.
- When it's done, the button turns cyan (**Open Render**), and clicking it
  opens the file.
- Clicking the button during a render offers **Cancel Render** (the partial
  file is deleted) or **Queue Another**.
- **Right-click** Render to render or queue with any profile, straight into
  the default export folder.
- Quitting during a render asks first. Unfinished renders are offered again,
  from the beginning, the next time you start the app.

## Output format

Every render is an **MP4** with H.264 video (yuv420p) and AAC audio at
48 kHz stereo, which plays everywhere and uploads to every platform. Other
container formats and codecs aren't available yet.

## Render profiles

Settings › Render manages profiles. Each one sets:

| Setting | Choices |
|---|---|
| Output height | Project, 2160p, 1440p, 1080p, 720p (the width follows the project's shape) |
| Quality | Draft, Good, High, Max, or an exact bitrate |
| Frame rate | Project, 23.976, 24, 25, 29.97, 30, 50, 59.94, 60 |

- The built-in profiles are **High quality** (the default) and **Draft
  (legacy)**. Duplicate one to make your own.
- Choosing a frame rate other than the project's renders a retimed copy,
  where every cut lands within half a frame of its original time.
- **Render threads** (Settings › Render) caps how much of your CPU a render
  uses. The default is 80%, so the desktop stays responsive.

## Where renders go

Renders are saved to your default export folder (Settings › Locations).
If that isn't set, they go next to the project, then to Videos, then to
your home folder. Renders always use the original media, never proxies.

See also: [Projects and saving](projects-and-saving.md#safety) ·
[v2 doc 10: Export and rendering](../plans/v2/10-export-and-rendering.md) ·
[Render notes](../developer/notes/render.md) (technical)
