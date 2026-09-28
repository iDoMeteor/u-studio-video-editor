# Effects

[Docs home](../README.md) › [User guide](README.md) › Effects

Effects change how a clip looks or sounds: colour, blur, glow, grain,
keying, distortion, audio clean-up and hundreds more. You can put them on
one clip, several clips, a whole track, or the whole finished picture, make
them change over time, and undo anything.

> **Early days.** Effects come with the *Effects* add-on. Builds made from
> source include it; for the Flatpak it's a separate add-on
> ([Installing › Add-ons](installing.md#add-ons)). Without it, a project
> that uses effects still opens and saves; it just plays without them.

## Opening the Effects pages

Click the round **Inspector** button at the top right of the picture. Two
pages appear on the right: **Effects** (the effects on what you've
selected) and **Add** (every effect you can add). On a wide window the
panel sits beside the picture; on a narrow one it slides over it.

Press **E** to jump straight to **Add**.

## Adding an effect

1. Select a clip on the timeline (or several; see below).
2. Press **E**. The **Add** page shows a tile for each effect: your clip's
   current frame with that effect on it.
3. Point at a tile to try it: the picture shows your clip with that effect,
   without changing anything yet. Move away, or press **Esc**, and it goes
   back.
4. Click the tile, or press **Enter**, to add it.

You can also:

- **Search.** Type in **Search effects**: "blur", "glow", "key", "warm".
  **Enter** adds the best match.
- **Browse by section.** **Featured** picks, **Recent** (what you've added
  lately), **Looks**, **All**, or a category (Colour, Light, Stylise, …).
- **Drag a tile** onto the picture (it goes on the clip at the playhead) or
  onto the **Effects** page (it goes on what that page shows).

Each tile has a badge: **light**, **medium** or **heavy** is how much work
the effect is for each frame, and a heavy one may slow playback on a busy
project. **checking…** means U Stu hasn't finished checking that effect is
safe on this computer yet; it can't be tried on the picture until then
(see [Unstable effects](#unstable-effects)).

## Changing an effect

The **Effects** page shows one card per effect, top to bottom in the order
they apply. At the top, choose what the page is about: **Selected clip**,
**Its track** (everything on that track), or **Whole sequence** (the
finished picture).

On each card:

- The **switch** turns the effect off and on, keeping its settings.
- **↑ / ↓** move it earlier or later in the order.
- The **bin** removes it.
- **Mix** blends the effect with the picture without it: 100% is the full
  effect, 50% half of it.
- Below that, a control for each setting: drag a slider, type a number,
  pick a colour or choose from a list.

Every change can be undone with **Ctrl+Z**. A drag of a slider is one undo
step, however long you drag.

## Making an effect change over time (keyframes)

Beside each number there are three small buttons: **‹**, a **star** (the
pin), and **›**.

1. Move the playhead to where the change should start and set the value.
2. Click the **star** (or press **P**) to pin that value there. The star
   lights up: this setting now changes over time.
3. Move the playhead to where the change should end and set the new value.
   It's pinned there automatically.

Now the effect moves from the first value to the second as the clip plays.
**‹** and **›** jump to the previous and next pinned point. Click a lit
star to remove that point; removing the last one keeps the value as it is.

When the playhead is on a pinned point, **Feel** chooses how the value
moves to the next point: **Linear** (steady), **Smooth**, **Ease in**,
**Ease out**, **Snap**, **Bounce**, **Elastic**, or **Hold** (it stays put,
then jumps), plus every other curve in the list.

## Several clips at once

Select several clips (click one, then **Ctrl**-click or **Shift**-click
others). The **Effects** page then shows the effects they all have, and a
change applies to every one of them as one undo step. Adding an effect or a
look adds it to all of them. (Moving effects and keyframes are one clip at
a time.)

## Copy and paste

**Ctrl+Shift+C** copies the effects on the **Effects** page.
**Ctrl+Shift+V** pastes them onto the selected clips: **after these**
(added to what's there) or **instead of these** (replacing them). Both are
also in the page's **⋮** menu.

## Looks

A look is a set of effects saved together, applied in one go.

- **Brand looks** come with U Stu: *Unicorn Glow*, *Neon Night*,
  *Stream Punch*, *Pastel Dream* and *Film Grain*.
- **Save your own:** set up a clip's effects, then **⋮ › Save as a look…**
  and give it a name. It's saved in the project.

Find looks under **Looks** on the **Add** page (or by name in the search).
Try one by pointing at it, and apply it by clicking or dragging, like an
effect.

## Before and after

- **Hold \\** (backslash): the picture without the selected clip's effects,
  until you let go.
- **Compare** (the two-panes button on the **Effects** page): the picture
  is split, without the effects on the left and with them on the right.
  Drag the dividing line. Click **Compare** again to turn it off.

Both show the frame the playhead is on; pause to compare a moment.

## Unstable effects

When you first run U Stu with the Effects add-on (and after installing new
effects), it checks each effect in the background, safely apart from your
project: that it doesn't crash, hang or ruin the picture. This takes a few
minutes and doesn't need you. Most effects pass.

An effect that fails is **turned off**: it's hidden from the **Add** page,
and a project that already uses it plays without it (its card says so).
Tick **Unstable** on the **Add** page to see them anyway; they may take the
editor down. See [Troubleshooting](troubleshooting.md#an-effect-is-missing-or-turned-off).
