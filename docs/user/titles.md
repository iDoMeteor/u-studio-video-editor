# Titles

[Docs home](../README.md) › [User guide](README.md) › Titles

Titles are animated text and shapes, such as lower thirds, name tags and
chapter cards. They play over your video with transparency. Each title is a
`.ustitle` file, and you use it on the timeline like any other clip.

> **Early days.** Titles come with the *Titles* drop-in, which isn't in the
> installed packages yet. For now you write `.ustitle` files by hand or get
> them from someone who has. The title designer app, animation presets and
> a template gallery are on the way ([roadmap](../../README.md)).

## Putting a title on the timeline

1. Click **Import…** (or press **Ctrl+I**) and pick a `.ustitle` file. The
   Import dialog's **Titles** filter shows just those.
2. The title lands on the active track, after its last clip, at the length
   it was designed for.
3. Put it on a track **above** your video. Where the title is transparent,
   the picture below shows through.

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
