# App shell notes

[Developer docs](../README.md) › [Implementation notes](README.md)

GTK/GLib findings from the window shell (`src/app/`): the inline name editor, the recent-projects menu, Move File to Trash, the playhead overlay and the ruler.

The inline track/clip name editor (`showInlineNameEditor()`, a `GtkPopover`
holding a `GtkText`) had the same problem an earlier audit found in the
preview-scale dropdown and the volume/seek sliders (audit A7, see their
`focusable=FALSE` comments in `app_window.cpp`): the transport actions
`installActions()` binds to bare letters and Left/Right/Home/End
(+Ctrl/Alt variants) are `win.*` accelerators installed via
`gtk_application_set_accels_for_action`, which fire as global window
shortcuts regardless of which widget has keyboard focus — typing "a" to
rename a track sought to the previous cut instead of inserting the letter
(audit A1, 2026-09-23). `GtkDropDown`/`GtkRange` could opt out by setting
`focusable=FALSE` (A7); a text entry can't, since it needs focus to accept
input at all. Fix: `showInlineNameEditor()` disables every one of those
`GSimpleAction`s (`g_simple_action_set_enabled`, looked up by name via
`g_action_map_lookup_action`) for the popover's lifetime, and
`onInlineNameEditClosed()` — wired to the popover's own `"closed"` signal,
which fires on every dismissal path (Escape, Enter, or clicking away) —
re-enables them unconditionally as its first statement. `GSimpleAction`
ignores `activate()` entirely while disabled (confirmed via a live gdb
session: `g_action_get_enabled()` read 0 for the duration the popover was
open, and a `win.step-forward` activation over D-Bus during that window had
no effect), so this holds even though this app doesn't control the order
GTK's shortcut controller and the entry's own key handling see the event.
Undo/redo (`Ctrl+Z`/`Ctrl+Shift+Z`) are deliberately left enabled: they're
modifier combos a text entry never needs to consume for itself.

The recent-projects menu (`refreshRecentProjectsMenu()`) hit two GLib/GTK
findings worth recording. `GtkRecentInfo` (what `gtk_recent_manager_get_items()`
returns) is its own refcounted boxed type with `gtk_recent_info_ref()`/
`_unref()` — **not** a `GObject`, despite looking like one; freeing the
returned list with `g_object_unref` as the element destructor segfaults
inside GObject's own type-check machinery on the very first real call
(confirmed live via gdb: `g_type_check_instance_is_fundamentally_a`),
not something a quick glance at the type name would catch. And calling
`gtk_recent_manager_get_items()` immediately after
`gtk_recent_manager_add_item()`, in the same call stack, does **not**
see the just-added item — confirmed live (the menu showed "No recent
projects" right after a save that had just added one) that
`GtkRecentManager` updates its in-memory list and emits `"changed"`
asynchronously, not synchronously inside `add_item()`. Fixed by
connecting `refreshRecentProjectsMenu()` to the manager's own
`"changed"` signal instead of calling it directly after `add_item()` —
the correct source of truth regardless of that timing, confirmed live
(the same save-then-check sequence then showed the entry correctly).
A third finding in the same area: sorting the recent-projects list with
`g_list_sort()` and a comparator that calls back into
`gtk_recent_info_get_modified()`/`g_date_time_compare()` on every
comparison crashed deep inside GLib's own `g_date_time_compare`
(`g_time_zone_get_offset`) against a large, real `recently-used.xbel`
history — reproduced twice against the owner's actual recent-files list,
never against this session's own small synthetic test histories. Fixed
by extracting each entry's modified time to a plain `gint64` once, up
front, and sorting a `std::vector<std::pair<gint64, GtkRecentInfo*>>`
with `std::sort` instead — no further GLib calls happen during the
comparison itself.

"Move File to Trash…" (`onDeleteAssetFileClicked()`) used to permanently
unlink the file with `std::filesystem::remove` despite the confirmation
dialog only asking about removing it from the *project* (audit A3,
2026-09-23) — fixed by moving it to the desktop's Trash with GIO's
`g_file_trash()` instead. Confirmed empirically (a standalone repro
calling the same GLib function directly) that this only succeeds for a
file on the same filesystem as `$XDG_DATA_HOME/Trash`: a file under
`/tmp` fails outright with `G_IO_ERROR_NOT_SUPPORTED` ("Trashing on
system internal mounts is not supported" — GIO treats tmpfs/internal
mounts as ineligible for trashing regardless of `$XDG_DATA_HOME`), and a
file on a genuinely different *device* from `$XDG_DATA_HOME/Trash` fails
with the same error code but "across filesystem boundaries" instead. A
real media file living under the user's home directory (this feature's
actual use case) hits neither case — confirmed by trashing a real file
there and finding it land at `$XDG_DATA_HOME/Trash/files/`, matching the
XDG trash spec. `onDeleteAssetFileClicked()`'s existing error path
(`showStatus()` with the failure message) already surfaces either error
to the owner rather than silently doing nothing, but a source asset that
somehow ends up on `/tmp` or a removable/network mount too small to hold
a Trash copy will report a failure here where the previous
`std::filesystem::remove` implementation would have just deleted it.

The timeline's playhead line used to be drawn as the last few lines of
`onTimelineDraw()` itself (`AppWindow::onTimelineDraw()`), so every
displayed frame during playback -- `refreshTransport()`, called once per
frame via `onFrameReady()` -- queued a full redraw of the whole timeline:
every track's Pango label layout, every clip's waveform-cache lookup
(mutex + key-string build), all ~30 times a second (audit A5,
2026-09-23). Fixed by splitting the playhead onto its own
`GtkDrawingArea` (`m_playheadOverlay`), stacked on top of `m_timeline`
inside a `GtkOverlay`, `gtk_widget_set_can_target(..., FALSE)` so it
never intercepts the clicks/drags/drops/tooltips every existing
controller is still attached to `m_timeline` for.
`refreshTransport()` now queues only that overlay; everything that
queues a full `m_timeline` redraw for an actual content change is
unchanged. Confirmed via gdb breakpoints on both draw functions that a
`step-forward` action now hits `onPlayheadOverlayDraw()` and *not*
`onTimelineDraw()`, while a real edit (adding a track) still hits
`onTimelineDraw()` as before -- GTK4's per-widget `GskRenderNode` caching
means the overlay's last-drawn playhead position stays correctly
composited on top even on a frame where only `m_timeline` redrew, so
nothing needed to force both together.

The timecode ruler (`onRulerDraw()`, enhancement #12, 2026-09-23) is its
own fixed-height widget stacked ABOVE `m_timeline` in the layout, not
overlapping it -- unlike the playhead overlay, it doesn't need to sit on
top of anything, so it's simplest as a separate widget rather than
another `GtkOverlay` child, and it never has to touch any of the row/
y-coordinate math `onTimelineClicked()`/`onTrackDragBegin()`/
`onTrackDragUpdate()`/`onTimelineRightClicked()` already do. One finding
worth recording: `gtk_widget_set_size_request()`'s height and the
widget's actual allocated height aren't always equal -- confirmed live
via gdb that a `kRulerHeight` of 20 requested came out as 18 actually
allocated (CSS padding from the shared `"timeline-area"` class both this
and `m_timeline` use). Anchoring the tick marks to the *requested*
constant instead of the real `height` parameter `onRulerDraw()` receives
would have drawn them a couple of pixels past the widget's real bottom
edge -- fixed by using `height` for the drawing math, keeping the
constant only for the original size request.

**The first second's stalls are GTK's and Mesa's first-use costs, not
ours (2026-09-27).** Measured on the real Wayland desktop (Intel Iris Xe,
Mesa 26.1, GTK 4.22's Vulkan renderer) with a 3-track, 80-clip project,
using the stall monitor plus `perf record --call-graph fp` (Fedora builds
GTK and GLib with frame pointers). Each figure is one main-loop iteration:

| Stall | Size | What it is |
|---|---|---|
| First iteration after activation | 150–190 ms | `gtk_window_present()` realising the window and starting the renderer 90–120 ms; `adw_style_manager_set_color_scheme(FORCE_DARK)` re-parsing libadwaita's dark stylesheet 32–38 ms; building our window 20–32 ms |
| First frame, cold Mesa shader cache | 650–1,040 ms | GTK compiling its Vulkan pipelines (`vk_create_graphics_pipeline`): first launch, a driver update, or a cleared `~/.cache/mesa_shader_cache` (a Flatpak has its own cache) |
| First frame, warm cache | 55–90 ms | The first text layout (fontconfig matching, HarfBuzz shaping) and the render |
| A later frame with new content | ~55 ms | A pipeline variant GTK hadn't built yet, compiled once |
| Quit | 120–180 ms | Shutting down, window already closing |

With the cairo renderer the first two are 60–80 ms, but it's slower
afterwards, so leave the renderer choice to GTK. Before activation,
`Mlt::Factory::init()` takes about 140 ms of the ~450 ms before the window
exists (start-up time, not a stall). `main.cpp` marks the start-up
phases (`startup: dark scheme`, `startup: build window`, `startup: present
window`) so the stall monitor names them.

To measure on the desktop without touching its session, run the app under
a private `dbus-run-session`, with scratch `XDG_*_HOME`,
`GSETTINGS_BACKEND=keyfile` and `SDL_AUDIODRIVER=dummy`. Give activated
services a private runtime dir (`dbus-update-activation-environment
XDG_RUNTIME_DIR=<private dir>`, as `tools/titles-smoke/run.sh` does);
otherwise the private session's document portal unmounts the desktop's
`/run/user/<uid>/doc`. Reuse one scratch cache dir between runs, or every
run pays the cold-cache first frame.
