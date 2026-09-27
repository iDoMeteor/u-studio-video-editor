# Titles drop-in

[Docs home](../../docs/README.md) › [Developer docs](../../docs/developer/README.md) › Titles drop-in

Animated text and lower thirds from `.ustitle` files: the plan is
[doc 16](../../docs/plans/v2/16-titles-tool.md), the decisions are
[ADR-012](../../docs/plans/v2/adr/012-titles-mlt-module.md) (our own
Pango/Cairo renderer behind an MLT producer) and
[ADR-013](../../docs/plans/v2/adr/013-effects-and-titles-as-drop-in-modules.md)
(one self-contained folder; nothing in `src/` includes from here).

Status: T1 done. `.ustitle` files import as title clips, play with alpha
through the `ustudio_title` MLT producer, fit any clip length, reload when
the file changes, and render the same in `u-studio-render`. Next: T2, the
`u-studio-titles` designer app.

## Layout

| Folder | What | May use |
|---|---|---|
| `core/` | `TitleDocument`, the `.ustitle` reader and writer, elastic timing, keyframe evaluation, `{{field}}` substitution | std, libxml2, `src/core` (keyframes are `core::Keyframe`, evaluated by `core::easedValue()`) |
| `render/` | `renderTitle()`: a title at a moment into premultiplied ARGB32, and `toStraightRgba()` for MLT | Pango, PangoCairo, Cairo, fontconfig |
| `mltmodule/` | `libmltustudio.so`: the `ustudio_title` MLT producer (`resource`, `length`, `field.<name>`). Self-contained, exports only `mlt_register` | MLT's C API, `core/`, `render/` |
| `engine/` | The engine extension (IP3: a producer per title clip) and `u-studio-render --title-frames` (IP6) | `src/engine`, mlt++ |
| `editor/` | The editor window's side (IP5): the `.ustitle` import handler and the file watch | `src/app/shell_host.h`, GIO |
| `register.cpp` | The drop-in's describe function; IP4 contributes the module's directory | the drop-in host API |
| `tests/` | `titles-core`, `titles-render`, `titles-engine`, `titles-shell` | doctest |

## Building and testing

```sh
meson configure builddir -Ddropin_titles=builtin   # or module
meson compile -C builddir
meson test -C builddir titles-core titles-render titles-engine titles-shell
```

The drop-in's tests build only when the option isn't `disabled`. Both
`builtin` and `module` must pass the whole suite (ADR-013):
`just dropins-builtin` and `just dropins-module`.

Installed, the MLT module goes to `$libdir/u-studio/mlt/` and the drop-in
module (in `module` mode) to `$libdir/u-studio/drop-ins/`. Run from a build
directory, the drop-in uses the build's own `libmltustudio` when nothing is
installed. `u-studio-render --title-frames <project> <frame>...` prints a
hash of each frame's pixels, which is how the tests compare the render tool
with the editor.

## The `.ustitle` format (version 1)

XML, UTF-8. Canvas pixels and title frames throughout. Layers draw in file
order, first at the bottom.

```xml
<ustitle version="1" width="1920" height="1080" fps="30/1">
  <timing intro="18" hold="60" outro="15" hold-mode="elastic"/>
  <field name="name" label="Name" default="Jay Doe"/>
  <layer id="bar" kind="shape" shape="rounded-rect" x="96" y="820" w="620" h="140" radius="12">
    <fill color="#1b1230" opacity="0.92"/>
    <stroke color="#19e3ff" width="2" opacity="0.6"/>
    <animate property="x">
      <key at="0" value="-700" easing="cubic_out"/>
      <key at="14" value="96"/>
      <key at="0" zone="outro" value="96" easing="cubic_in"/>
      <key at="15" zone="outro" value="-700"/>
    </animate>
  </layer>
  <layer id="name" kind="text" x="128" y="838" w="560" fit="shrink" align="left">
    <text>{{name}}</text>
    <font family="Space Grotesk" weight="700" size="64" tracking="0.02" line-height="1"/>
    <fill gradient="linear" from="#ff3cc7" to="#19e3ff" angle="0"/>
    <shadow dx="0" dy="4" blur="12" color="#000000" opacity="0.5"/>
  </layer>
</ustitle>
```

| Element | Attributes |
|---|---|
| `<ustitle>` | `version` (required), `width`, `height` (16–8192), `fps` (`num/den`) |
| `<timing>` | `intro`, `hold`, `outro` in title frames; `hold-mode` is always `elastic` |
| `<field>` | `name` (no braces), `label`, `default`; the text says `{{name}}` |
| `<layer>` | `id`, `kind` (`text`, `shape`), `x`, `y`, `w`, `h`, `opacity`, `scale`, `rotation` (degrees, about the box's centre), `visible` (`0`/`1`); text: `align` (`left`, `center`, `right`), `fit` (`none`, `wrap`, `shrink`); shape: `shape` (`rect`, `rounded-rect`, `ellipse`, `line`), `radius` |
| `<text>` | The text, plain (never Pango markup) |
| `<font>` | `family` (a generic fallback is always added), `weight` (100–1000), `size` (px), `style="italic"`, `tracking` (em), `line-height` (factor) |
| `<fill>` | `color`, or `gradient="linear"` / `"radial"` with `from`, `to` (and `angle` for linear); `kind="none"`; `opacity` |
| `<stroke>` | `color`, `width` (px shown outside the shape), `opacity` |
| `<shadow>` | `dx`, `dy`, `blur` (about a Gaussian's sigma, px), `color`, `opacity` |
| `<animate>` | `property` (`x`, `y`, `opacity`, `scale`, `rotation`), holding `<key at value easing zone>`; `zone` is `intro` (default), `hold` or `outro`, and `at` counts from that zone's start, so outro keys stay with the outro when the hold changes; `easing` is a `core::easingName()` (`linear` default) |

Colours are `#rgb`, `#rrggbb` or `#rrggbbaa`. A text layer with `h="0"` is
as tall as its text; with `w="0"`, `x` is the point the alignment refers
to. Unknown elements (T3's `<animator>`, `<behavior>`) and unknown layer
kinds are skipped with a warning; a newer `version` is refused.

**Elastic timing.** On a clip of any length the intro plays from its start
and the outro ends at its end, each at its designed speed; the hold, and
any keyframes in it, stretch or shrink to fill what's between. A clip
shorter than intro + outro plays both squeezed, with no hold. The title's
`fps` needn't match the project's.

**Untrusted input.** The reader never fetches anything (no network, no
external entities), refuses files over 16 MB or malformed values, and
clamps sizes and positions to sane ranges.
