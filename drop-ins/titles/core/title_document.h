#pragma once

// A title (doc 16, "The document: .ustitle"): a small canvas of layers with
// an intro / hold / outro timeline. Pure data; std and src/core only
// (drop-ins' core/ rule, tools/dropin_boundary_check.sh).

#include "core/model/types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ustudio::titles {

// Straight (not premultiplied) colour, each channel 0..1.
struct Rgba
{
    double r = 0.0, g = 0.0, b = 0.0, a = 1.0;

    bool operator==(const Rgba &) const = default;
};

// "#rgb", "#rrggbb" or "#rrggbbaa"; nullopt otherwise.
std::optional<Rgba> parseColor(std::string_view text);
// "#rrggbb", or "#rrggbbaa" when not opaque.
std::string formatColor(const Rgba &colour);

enum class FillKind
{
    None,
    Solid,
    Linear, // `from` to `to` across the layer's box at `angle`
    Radial, // `from` at the box's centre to `to` at its farthest edge
};

struct Fill
{
    FillKind kind = FillKind::Solid;
    Rgba color{1.0, 1.0, 1.0, 1.0}; // Solid
    Rgba from, to;                  // Linear, Radial
    std::optional<Rgba> via;        // an optional middle stop, halfway (the brand's three-colour "tears")
    double angle = 0.0;             // degrees, Linear; 0 = left to right, 90 = top to bottom
    double opacity = 1.0;

    bool operator==(const Fill &) const = default;
};

inline Fill noFill()
{
    Fill fill;
    fill.kind = FillKind::None;
    return fill;
}

// An outline outside the fill (drawn under it), `width` canvas pixels wide.
struct Stroke
{
    Rgba color{0.0, 0.0, 0.0, 1.0};
    double width = 0.0; // 0: none
    double opacity = 1.0;

    bool operator==(const Stroke &) const = default;
};

// A blurred copy of the layer's shape under it; `blur` is roughly the
// Gaussian's sigma, in canvas pixels.
struct Shadow
{
    bool enabled = false;
    double dx = 0.0, dy = 4.0, blur = 8.0;
    Rgba color{0.0, 0.0, 0.0, 1.0};
    double opacity = 0.5;

    bool operator==(const Shadow &) const = default;
};

struct Font
{
    std::string family = "Sans"; // a generic fallback is always appended when rendering
    int weight = 400;            // 100..1000, CSS-style
    bool italic = false;
    double size = 48.0;      // canvas pixels (the em size)
    double tracking = 0.0;   // extra space between letters, in em
    double lineHeight = 1.0; // a factor of the font's own line height

    bool operator==(const Font &) const = default;
};

enum class Align
{
    Left,
    Center,
    Right
};

// How text meets its box's width `w` (0: no box, one line per paragraph).
enum class Fit
{
    None,   // lines as written, may overflow
    Wrap,   // wrap at `w`
    Shrink, // one size smaller until it fits `w` (and `h` when set)
};

enum class LayerKind
{
    Text,
    Shape,
    Image, // a PNG, drawn into the box
};

enum class ShapeKind
{
    Rect,
    RoundedRect,
    Ellipse,
    Line, // (x, y) to (x + w, y + h), stroke only
};

// Where a keyframe's `at` counts from. Keys in the outro stay with the
// outro when the hold is lengthened, in the file as on the clip.
enum class Zone
{
    Intro, // frames from the title's start
    Hold,  // frames from the hold's start
    Outro, // frames from the outro's start
};

struct TitleKey
{
    Zone zone = Zone::Intro;
    core::Keyframe key; // core's model (docs/developer/notes/animation.md)

    bool operator==(const TitleKey &) const = default;
};

// The properties a keyframe track may animate.
enum class Property
{
    X,
    Y,
    Opacity,
    Scale,
    Rotation, // degrees, clockwise, about the box's centre
    Blur,     // the whole layer blurred, about a Gaussian's sigma in canvas pixels
    Tracking, // a text layer's letter spacing, in em
    Reveal,   // how much of the box shows, 0..1 from its left edge (a wipe)
    Shift,    // a gradient fill slid along its axis, -1..1 of its length (a shimmer)
    FillR,    // a solid fill's colour, one channel each, 0..1
    FillG,
    FillB,
    FillA,
    ShadowOpacity, // multiplies the shadow's opacity (a glow's breathing)
};
const char *propertyName(Property property);
std::optional<Property> propertyFromName(std::string_view name);

struct PropertyTrack
{
    Property property = Property::Opacity;
    std::vector<TitleKey> keys; // sorted by their position in the title

    bool operator==(const PropertyTrack &) const = default;
};

// Text animators (doc 16, "Animation model"): the text split into units,
// each on its own clock, offset by a small keyframed curve.
enum class AnimatorUnit
{
    Character,
    Word,
    Line,
};

enum class AnimatorOrder
{
    Forward,
    Reverse,
    CentreOut, // the middle unit first, outwards
    Random,    // a shuffle fixed by the animator's seed
};

// One point of an animator's curve, on a unit's own clock. Offsets add to
// the layer (dx, dy, rotation, blur) or multiply it (scale, opacity).
struct AnimatorKey
{
    core::FrameIndex at = 0;
    core::Easing easing = core::Easing::Linear;
    double dx = 0.0, dy = 0.0, scale = 1.0, rotation = 0.0, opacity = 1.0, blur = 0.0;

    bool operator==(const AnimatorKey &) const = default;
};

struct Animator
{
    AnimatorUnit unit = AnimatorUnit::Character;
    AnimatorOrder order = AnimatorOrder::Forward;
    uint32_t seed = 1;
    double stagger = 1.0;   // frames between one unit's start and the next's
    double spread = 0.0;    // > 0: frames from the first unit's start to the last's (stagger follows the count)
    bool alternate = false; // every other unit mirrors dx (split lines)
    // When the first unit starts.
    Zone zone = Zone::Intro;
    core::FrameIndex at = 0;
    std::vector<AnimatorKey> keys; // sorted by at

    bool operator==(const Animator &) const = default;
};

// A behaviour (doc 16): a named preset kept as data and expanded when the
// title is drawn (core/animation.h), so its timing follows the zones; "Detach
// to keyframes" turns one into plain keyframes and animators.
enum class BehaviorSlot
{
    In,   // starts at the intro's start
    Out,  // ends at the outro's end
    Loop, // repeats through the hold
};

struct Behavior
{
    BehaviorSlot slot = BehaviorSlot::In;
    std::string id;                 // "fade", "rise", "typewriter", "float" ... (behaviorIds())
    core::FrameIndex duration = 12; // In, Out: how long; Loop: its period
    core::Easing easing = core::Easing::CubicOut;
    uint32_t seed = 1;   // scramble, wiggle, random orders
    double amount = 1.0; // how strong: distance, size or depth as a factor

    bool operator==(const Behavior &) const = default;
};

struct Layer
{
    std::string id;
    LayerKind kind = LayerKind::Text;
    bool visible = true;
    bool locked = false; // can't be picked on the canvas (the layers list still selects it)
    // The box, in canvas pixels. A text layer with h = 0 is as tall as its
    // text; with w = 0, x is the anchor its alignment is relative to.
    double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
    double opacity = 1.0, scale = 1.0, rotation = 0.0;

    // Text
    std::string text; // may hold {{field}} references
    Font font;
    Align align = Align::Left;
    Fit fit = Fit::None;
    // Shape
    ShapeKind shape = ShapeKind::Rect;
    double radius = 0.0; // RoundedRect
    // Image: a PNG, relative to the title's folder (or absolute). With w or
    // h 0, that side follows the picture's aspect; both 0, its own size.
    std::string src;

    Fill fill;
    Stroke stroke;
    Shadow shadow;
    double blur = 0.0; // the whole layer, like Property::Blur's base
    std::vector<PropertyTrack> animation;
    std::vector<Animator> animators; // text layers
    std::vector<Behavior> behaviors;

    bool operator==(const Layer &) const = default;
};

// A template's slot, filled per clip (`field.<name>` on the producer).
struct Field
{
    std::string name;
    std::string label;
    std::string defaultValue;

    bool operator==(const Field &) const = default;
};

struct Timing
{
    int64_t intro = 0, hold = 60, outro = 0; // frames at the title's fps

    int64_t length() const
    {
        return intro + hold + outro;
    }
    bool operator==(const Timing &) const = default;
};

struct TitleDocument
{
    // What the template gallery shows (doc 16, T4): "Lower third, two
    // lines" under "Lower thirds". Empty on an ordinary title.
    std::string name, category;
    int width = 1920, height = 1080;
    int fpsNum = 30, fpsDen = 1;
    Timing timing;
    // Behind every layer. None (the default): the title is transparent
    // where it has no layers, for overlays; a colour or gradient bakes a
    // background in (a full-frame card).
    Fill background = noFill();
    std::vector<Field> fields;
    std::vector<Layer> layers; // bottom first: later layers draw over earlier ones

    // Not saved: the folder relative image paths resolve against (the
    // title file's own; readTitle() sets it).
    std::string baseDirectory;

    bool operator==(const TitleDocument &) const = default;
};

} // namespace ustudio::titles
