# Titles

[Docs home](../README.md) › [User guide](README.md) › Titles

Titles are animated text and shapes, such as lower thirds, name tags and
chapter cards. They play over your video with transparency. Each title is a
`.ustitle` file, and you use it on the timeline like any other clip.

> **Early days.** Titles come with the *Titles* drop-in, which isn't in the
> installed packages yet. The title designer below can place and arrange
> layers; styling, on-canvas typing, animation presets and a template
> gallery are on the way ([roadmap](../../README.md)).

## Putting a title on the timeline

1. Click **Import…** (or press **Ctrl+I**) and pick a `.ustitle` file. The
   Import dialog's **Titles** filter shows just those.
2. The title lands on the active track, after its last clip, at the length
   it was designed for.
3. Put it on a track **above** your video. Where the title is transparent,
   the picture below shows through.

## Designing a title

**U Stu Titles** is the title designer (`u-studio-titles`). It opens a
`.ustitle` file, or starts a new one.

- **Add** (the **+** button) puts text, a rectangle, a rounded rectangle,
  an ellipse, a line or a picture (PNG) in the middle of the frame.
  **Ctrl+T** adds text; **Ctrl+Shift+R** a rounded rectangle.
- **Double-click text** to type on the canvas, in the title's own font.
  **Enter** keeps it, **Shift+Enter** starts a new line, **Escape** cancels.
- **Click** a layer to select it. **Drag** it to move it. It snaps to the
  frame's centre and edges, the safe areas, the thirds and the other
  layers; the line it snapped to shows while you drag. Hold **Alt** to move
  freely.
- **Drag a handle** to resize a shape, or to give text a box to fit.
- **Arrow keys** nudge the selected layer; **Delete** removes it.
- The dashed boxes are the **safe areas**: keep text inside the inner one
  and it won't be cut off on any screen. Toggle them in the main menu.
- The canvas's background is only for designing, and isn't saved in the
  title: a checkerboard (showing where the title is transparent), a colour
  of your choice, or a picture. Pick one in the main menu; the app
  remembers it.
- **Save** writes the `.ustitle` file. If the editor is using it, the
  editor updates straight away.

**Layers** (left) lists every layer, the topmost first. Click one to select
it; the eye hides it; the lock stops it being picked on the canvas (handy
for a background shape). Drag a layer up or down the list, or use the
arrows at the bottom, to change what's in front.

**The inspector** (right) styles the selected layer: its text, font
(family, weight, size, italic, letter spacing, line height, alignment, and
how it fits its box), fill (a colour, or a linear or radial gradient with an
optional middle colour), outline, shadow, and exact position, size,
rotation, scale and opacity. With nothing selected, it shows the title
itself: its background (none, for a transparent overlay; or a colour or
gradient), timing and canvas size.

**The brand kit** is next to every colour and font: the Unicorn Tears
colours, gradients and fonts, one click each. **Apply Brand** (with nothing
selected) restyles the whole title in the brand, keeping its layout.

Animation is on the way.

## From the editor

**Double-click a title clip** on the timeline (or select it and press
**Ctrl+Shift+T**, Edit Title) to open it in U Stu Titles. The
designer shows the video at the playhead behind the title, without the title
itself, so you design over the real picture. Save there, and the editor
picks up the change.

## Any length you like

Every title has three parts: an **intro** (it animates in), a **hold** (it
stays up) and an **outro** (it animates out). When you make a title clip
longer or shorter, only the hold changes. The intro and outro keep their
timing. So one lower third works for a two-second mention and for a
two-minute segment.

A clip shorter than the intro and outro together plays both, faster, with
no hold.

## Editing a title

A title clip plays whatever the file says now. Save a change to the
`.ustitle` file (in the titles app, or any text editor) and the preview
updates within a second. You don't need to re-import it.

## Fonts

Titles use the fonts installed on your computer. If a title asks for a font
you don't have, it's drawn in a similar font instead, and the log names both
(for example "Space Grotesk isn't installed; using Noto Sans"). To bring
fonts with a title, put them in a `fonts` folder next to the `.ustitle`
file.

## When something's wrong

- **The clip shows as missing.** The `.ustitle` file was moved, deleted or
  can't be read. Put the file back where it was, or fix it, and it plays
  again. **Relink** doesn't work for titles yet.
- **"… isn't supported yet" after importing.** The title uses something
  this version can't draw yet, such as text animators, which arrive with the
  animation tools. The rest of the title still plays.
- **Rendering a project with titles in another program.** Other programs
  (including stock `melt`) can't play `.ustitle` files. Render from U Stu.
