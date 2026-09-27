# Importing media

[Docs home](../README.md) › [User guide](README.md) › Importing media

## Ways to import

| Do this | What happens |
|---|---|
| `Ctrl+I` (Import…) | Pick one or more files. Each is placed after the previous one on the active track. |
| `Ctrl+Shift+I` (Import Folder…), or drop a folder | Adds every file in the folder and its subfolders to the media browser (hidden files are skipped). |
| Drag files onto the timeline | Imports them and places them one after another, starting where you dropped. |
| Drag files onto the media browser | Adds them to the project without placing a clip. |

- Files are opened in the background. The status bar shows progress
  ("Importing 3 of 12…") and the window stays responsive.
- However many files you import, it's **one undo step**.
- A file that can't be opened is skipped. At the end, a dialog lists each
  skipped file and why: empty, a folder, not media, or it took over 20
  seconds to open.
- **Still images** (PNG, JPEG, …) stretch from where they land to the end
  of the project, so a logo on an empty top track covers the whole video.

## Frame rates and sizes

- A new, empty project takes its size and frame rate from the first video
  you import.
- You can mix clips of any rate from 23.976 to 60 fps on one timeline. If
  you import a clip at a different rate, the import summary says whether
  frames will repeat or be skipped.
- To change the project's rate, click the title in the header bar. Every
  cut, dissolve, marker and keyframe keeps its time, and the change is one
  undo step.

## The media browser

Toggle it with the button next to **Add track** in the header bar. Each row
shows a thumbnail, name, length, frame rate and format, with badges:

| Badge | Meaning |
|---|---|
| IMAGE, SEQUENCE, AUDIO | What kind of file it is |
| 4K, 1440p, 1080p, 720p, SD | Its resolution. **Cyan** means it's larger than the project, so a proxy would help. |
| PROBING, FAILED | Still being read, or couldn't be opened |
| MISSING | The file has moved or been deleted |
| PROXY, PROXY n%, PROXY MISSING | Proxy ready, being made, or lost |

- **Double-click** a row to insert it at the playhead on the active track.
- **Drag** a row onto the timeline to place it exactly where you drop it.
  The drop is refused if there's no room or the track is locked.
- **Right-click** a row:
  - **Remove from Project** removes the file and every clip cut from it.
    You can undo it, and the file on disk is untouched.
  - **Move File to Trash…** asks first, then removes it from the project
    and moves the file to your desktop's Trash, where you can still
    restore it.
  - **Create Proxy** / **Create Conformed Proxy** (see below).

## Missing media

If files have moved since the project was saved, it still opens:

- The affected clips are drawn with red stripes and play as dark red.
- A banner says how many files are missing. Click **Relink…** to point each
  one at its new location, one at a time or by searching a folder. Relinking
  is one undo step and changes nothing else.
- Rendering with missing media asks you first.

## Proxies (smooth editing of 4K and phone footage)

A proxy is a smaller copy of a clip that plays smoothly while you edit.

- **Create Proxy** makes a 540p copy. Change the size in Settings ›
  Performance.
- **Create Conformed Proxy** makes a full-size copy at the project's frame
  rate. Use it for phone and screen recordings with variable frame rates.
- Footage taller than 1080p is offered a proxy once per project.
- Proxies are made in the background and stored in your user cache folder.
- The **Proxies** toggle beside the preview scale switches playback between
  proxies and originals.
- **Renders always use the originals**, so proxies never lower your
  export quality.

See also: [Editing on the timeline](editing-the-timeline.md) ·
[v2 doc 07: Media bin and assets](../plans/v2/07-media-bin-and-assets.md)
