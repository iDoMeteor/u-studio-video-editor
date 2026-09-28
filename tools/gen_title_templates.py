#!/usr/bin/env python3
"""Writes the titles drop-in's built-in templates (doc 16, T4.2).

    tools/gen_title_templates.py drop-ins/titles/data/templates

Each template is an ordinary .ustitle with a name, a category and fields,
in the Unicorn Tears brand kit's colours and fonts (drop-ins/titles/data/
brand.xml; a font that isn't installed falls back). The output is
committed; edit this script and run it again rather than editing the
files. titles-render checks every one (reads cleanly, fills its fields,
draws, stays on the canvas).

1080p sizes (owner review, 2026-09-28): a primary line at least 60 px, a
secondary at least 40, a tag or label at least 30.
"""
import os
import sys
from xml.sax.saxutils import escape, quoteattr

MAG, CYAN, VIOLET, LIME = "#ff2bd6", "#19e3ff", "#9d4eff", "#4dff5a"
WHITE, PINKW = "#ffffff", "#ffeffb"
INK9, INK8, INK7 = "#07060d", "#120c1f", "#1b1230"
RED = "#ff3b5c"
SANS, DISPLAY, MONO = "Space Grotesk", "Anton", "JetBrains Mono"
TEARS = dict(gradient="linear", frm=MAG, via=VIOLET, to=CYAN, angle=0)


def attrs(**kw):
    return "".join(f" {k.replace('_', '-')}={quoteattr(str(v))}" for k, v in kw.items() if v is not None)


def fill(color=None, opacity=None, gradient=None, frm=None, via=None, to=None, angle=None):
    if gradient:
        return f"<fill{attrs(gradient=gradient, **{'from': frm}, via=via, to=to, angle=angle, opacity=opacity)}/>"
    return f"<fill{attrs(color=color, opacity=opacity)}/>"


def beh(slot, id, duration=None):
    return f"<behavior{attrs(slot=slot, id=id, duration=duration)}/>"


def in_out(entrance, exit="fade"):
    return beh("in", entrance) + beh("out", exit)


def shape(id, kind, x, y, w, h, fills, radius=None, extra="", stroke=None):
    s = f"  <layer{attrs(id=id, kind='shape', shape=kind, x=x, y=y, w=w, h=h, radius=radius)}>\n    {fills}\n"
    if stroke:
        s += f"    <stroke{attrs(color=stroke[0], width=stroke[1])}/>\n"
    if extra:
        s += f"    {extra}\n"
    return s + "  </layer>\n"


def text(id, x, y, w, content, family=SANS, size=64, weight=None, fills=None, align=None, fit="shrink",
         tracking=None, shadow=False, extra="", tags=None, stroke=None):
    s = f"  <layer{attrs(id=id, kind='text', x=x, y=y, w=w, fit=fit, align=align, tags=tags)}>\n"
    s += f"    <text>{escape(content)}</text>\n"
    s += f"    <font{attrs(family=family, weight=weight, size=size, tracking=tracking)}/>\n"
    s += f"    {fills or fill(WHITE)}\n"
    if stroke:
        s += f"    <stroke{attrs(color=stroke[0], width=stroke[1])}/>\n"
    if shadow:
        s += f"    <shadow{attrs(dx=0, dy=4, blur=12, color='#000000', opacity=0.5)}/>\n"
    if extra:
        s += f"    {extra}\n"
    return s + "  </layer>\n"


TEMPLATES = []


def title(id, name, category, fields, layers, intro=18, hold=120, outro=15, background=None):
    x = '<?xml version="1.0" encoding="UTF-8"?>\n'
    x += f"<ustitle{attrs(version=1, name=name, category=category, width=1920, height=1080, fps='30/1')}>\n"
    x += f"  <timing{attrs(intro=intro, hold=hold, outro=outro)}/>\n"
    if background:
        x += f"  {background}\n"
    for field, label, default in fields:
        x += f"  <field{attrs(name=field, label=label, default=default)}/>\n"
    TEMPLATES.append((id, x + "".join(layers) + "</ustitle>\n"))


RADIAL = '<background gradient="radial" from="#342357" to="#07060d"/>'
INK_BG = '<background color="#120c1f"/>'
BLACK_BG = '<background color="#07060d"/>'

LT = "Lower thirds"
title("lower-third-one-line", "Lower third, one line", LT, [("name", "Name", "Jay Doe")], [
    shape("bar", "rounded-rect", 160, 830, 700, 124, fill(INK7, 0.92), radius=16, extra=in_out("wipe")),
    shape("accent", "rect", 160, 830, 12, 124, fill(**TEARS), extra=in_out("fade")),
    text("name", 206, 850, 630, "{{name}}", size=72, weight=700, extra=in_out("rise")),
])
title("lower-third-two-lines", "Lower third, two lines", LT, [("name", "Name", "Jay Doe"), ("role", "Role", "Host")], [
    shape("bar", "rounded-rect", 160, 790, 760, 180, fill(INK7, 0.92), radius=16, extra=in_out("wipe")),
    shape("accent", "rect", 160, 790, 12, 180, fill(**TEARS), extra=in_out("fade")),
    text("name", 206, 806, 690, "{{name}}", size=70, weight=700, extra=in_out("rise")),
    text("role", 206, 896, 690, "{{role}}", size=46, fills=fill(CYAN), extra=in_out("fade")),
])
title("lower-third-gradient", "Lower third, gradient", LT, [("name", "Name", "Jay Doe"), ("role", "Role", "Guest")], [
    shape("bar", "rect", 160, 790, 820, 180, fill(**TEARS, opacity=0.95), extra=in_out("wipe")),
    text("name", 200, 804, 750, "{{name}}", size=70, weight=700, shadow=True, extra=in_out("rise")),
    text("role", 200, 894, 750, "{{role}}", size=46, weight=500, fills=fill(PINKW), shadow=True, extra=in_out("fade")),
])
title("lower-third-minimal", "Lower third, minimal", LT, [("name", "Name", "Jay Doe"), ("role", "Role", "Host")], [
    text("name", 180, 790, 1000, "{{name}}", size=76, weight=700, shadow=True, extra=in_out("word-by-word")),
    shape("rule", "rect", 184, 888, 260, 6, fill(MAG), extra=in_out("wipe")),
    text("role", 180, 906, 1000, "{{role}}", size=46, fills=fill(PINKW), shadow=True, extra=in_out("fade")),
])
title("lower-third-right", "Lower third, right", LT, [("name", "Name", "Jay Doe"), ("role", "Role", "Host")], [
    shape("bar", "rounded-rect", 1000, 790, 760, 180, fill(INK7, 0.92), radius=16, extra=in_out("wipe")),
    shape("accent", "rect", 1748, 790, 12, 180, fill(**TEARS), extra=in_out("fade")),
    text("name", 1030, 806, 690, "{{name}}", size=70, weight=700, align="right", extra=in_out("rise")),
    text("role", 1030, 896, 690, "{{role}}", size=46, fills=fill(CYAN), align="right", extra=in_out("fade")),
])
title("lower-third-tag", "Lower third with tag", LT,
      [("tag", "Tag", "GUEST"), ("name", "Name", "Jay Doe"), ("role", "Role", "Streamer")], [
    shape("tag-bg", "rounded-rect", 160, 730, 190, 56, fill(MAG), radius=10, extra=in_out("pop")),
    text("tag", 170, 737, 170, "{{tag}}", size=32, weight=700, align="center", tracking=0.08, fills=fill(INK9),
         extra=in_out("fade")),
    shape("bar", "rounded-rect", 160, 796, 760, 176, fill(INK7, 0.9), radius=16, extra=in_out("wipe")),
    text("name", 200, 810, 690, "{{name}}", size=70, weight=700, extra=in_out("rise")),
    text("role", 200, 898, 690, "{{role}}", size=46, fills=fill(PINKW), extra=in_out("fade")),
])

BB = "Bugs and badges"
title("live-now-bug", "Live now bug", BB, [("label", "Label", "LIVE")], [
    shape("pill", "rounded-rect", 1560, 60, 240, 76, fill(INK7, 0.9), radius=38, extra=in_out("pop")),
    shape("dot", "ellipse", 1588, 84, 28, 28, fill(RED), extra=beh("in", "pop") + beh("loop", "pulse") + beh("out", "fade")),
    text("label", 1630, 70, 150, "{{label}}", size=42, weight=700, tracking=0.1, extra=in_out("fade")),
], hold=300)
title("name-tag", "Name tag", BB, [("name", "Name", "Jay Doe")], [
    shape("pill", "rounded-rect", 160, 860, 600, 104, fill(INK7, 0.92), radius=52, stroke=(CYAN, 4), extra=in_out("pop")),
    text("name", 196, 876, 528, "{{name}}", size=58, weight=700, align="center", extra=in_out("fade")),
])
title("social-handle", "Social handle", BB, [("handle", "Handle", "unicorntears")], [
    shape("pill", "rounded-rect", 160, 870, 660, 100, fill(**TEARS), radius=50, extra=in_out("wipe")),
    text("handle", 196, 884, 588, "@{{handle}}", size=56, weight=700, align="center", shadow=True,
         extra=in_out("typewriter")),
])
title("channel-bug", "Channel bug", BB, [("channel", "Channel", "UNICORN TEARS")], [
    text("channel", 1260, 60, 560, "{{channel}}", family=DISPLAY, size=56, weight=700, align="right", tracking=0.06,
         fills=fill(WHITE, 0.9), shadow=True, extra=in_out("fade") + beh("loop", "glow-breathe")),
], hold=300)
title("new-badge", "New badge", BB, [("label", "Label", "NEW")], [
    shape("badge", "rounded-rect", 1580, 70, 220, 90, fill(LIME), radius=14, extra=in_out("pop")),
    text("label", 1590, 80, 200, "{{label}}", family=DISPLAY, size=60, weight=700, align="center", fills=fill(INK9),
         extra=in_out("fade")),
])
title("subscribe-badge", "Subscribe reminder", BB, [("label", "Label", "Subscribe")], [
    shape("pill", "rounded-rect", 160, 860, 440, 104, fill(RED), radius=52,
          extra=beh("in", "pop") + beh("loop", "pulse") + beh("out", "fade")),
    text("label", 180, 876, 400, "{{label}}", size=58, weight=700, align="center", extra=in_out("fade")),
])

CARDS = "Cards"
title("chapter-card", "Chapter card", CARDS, [("number", "Number", "1"), ("title", "Title", "The beginning")], [
    text("chapter", 360, 360, 1200, "CHAPTER {{number}}", size=60, weight=700, align="center", tracking=0.2,
         fills=fill(CYAN), extra=in_out("fade")),
    shape("rule", "rect", 760, 468, 400, 6, fill(**TEARS), extra=in_out("wipe")),
    text("title", 200, 500, 1520, "{{title}}", family=DISPLAY, size=140, weight=700, align="center", extra=in_out("rise")),
], background=INK_BG, intro=24, hold=90, outro=18)
title("title-card", "Title card", CARDS, [("title", "Title", "Unicorn Tears"), ("subtitle", "Subtitle", "Episode 12")], [
    text("title", 160, 370, 1600, "{{title}}", family=DISPLAY, size=170, weight=700, align="center",
         fills=fill(gradient="linear", frm=WHITE, via="#66efff", to=MAG, angle=90), extra=in_out("kinetic-stack")),
    text("subtitle", 360, 630, 1200, "{{subtitle}}", size=60, align="center", fills=fill(PINKW), extra=in_out("fade")),
], background=RADIAL, intro=30, hold=90, outro=18)
title("quote-card", "Quote card", CARDS,
      [("quote", "Quote", "Make the thing you wish existed."), ("author", "Author", "Jay Doe")], [
    text("mark", 220, 180, 240, "“", family=DISPLAY, size=300, weight=700, fills=fill(MAG, 0.9), fit="none",
         extra=in_out("pop")),
    text("quote", 320, 360, 1280, "{{quote}}", size=84, weight=500, fit="wrap", extra=in_out("word-by-word")),
    text("author", 320, 760, 1280, "— {{author}}", size=52, weight=500, fills=fill(CYAN), extra=in_out("fade")),
], background=INK_BG, intro=30, hold=150, outro=18)
title("stat-card", "Big number", CARDS, [("stat", "Number", "1,000,000"), ("label", "Label", "views and counting")], [
    text("stat", 160, 330, 1600, "{{stat}}", family=DISPLAY, size=220, weight=700, align="center", fills=fill(**TEARS),
         extra=in_out("scramble")),
    text("label", 360, 640, 1200, "{{label}}", size=64, align="center", fills=fill(PINKW), extra=in_out("fade")),
], background=BLACK_BG, intro=30, hold=90, outro=15)
title("section-divider", "Section divider", CARDS, [("section", "Section", "Q&A")], [
    shape("left", "rect", 200, 536, 440, 8, fill(MAG), extra=in_out("wipe")),
    shape("right", "rect", 1280, 536, 440, 8, fill(CYAN), extra=in_out("wipe")),
    text("section", 660, 470, 600, "{{section}}", family=DISPLAY, size=110, weight=700, align="center",
         extra=in_out("pop")),
], intro=18, hold=75, outro=15)

END = "End screens"
title("end-card-cta", "End card with call to action", END,
      [("headline", "Headline", "Thanks for watching"), ("cta", "Call to action", "Subscribe for more")], [
    text("headline", 160, 300, 1600, "{{headline}}", family=DISPLAY, size=140, weight=700, align="center",
         extra=in_out("rise")),
    shape("button", "rounded-rect", 610, 580, 700, 120, fill(**TEARS), radius=60,
          extra=beh("in", "pop") + beh("loop", "pulse") + beh("out", "fade")),
    text("cta", 640, 604, 640, "{{cta}}", size=56, weight=700, align="center", extra=in_out("fade")),
], background=RADIAL, intro=24, hold=240, outro=18)
title("end-card-follow", "Follow card", END, [("handle", "Handle", "unicorntears"), ("platform", "Where", "Everywhere")], [
    text("follow", 360, 320, 1200, "FOLLOW", size=60, weight=700, align="center", tracking=0.3, fills=fill(PINKW),
         extra=in_out("fade")),
    text("handle", 160, 410, 1600, "@{{handle}}", family=DISPLAY, size=150, weight=700, align="center",
         fills=fill(**TEARS), extra=in_out("typewriter")),
    text("platform", 360, 640, 1200, "{{platform}}", size=56, align="center", extra=in_out("fade")),
], background=BLACK_BG, intro=30, hold=180, outro=18)
title("up-next", "Up next", END, [("title", "Title", "The next episode")], [
    shape("panel", "rounded-rect", 1040, 720, 720, 250, fill(INK7, 0.92), radius=18, extra=in_out("wipe")),
    text("label", 1076, 742, 650, "UP NEXT", size=36, weight=700, tracking=0.2, fills=fill(MAG), extra=in_out("fade")),
    text("title", 1076, 800, 650, "{{title}}", size=64, weight=700, fit="wrap", extra=in_out("rise")),
], intro=18, hold=150, outro=15)

CD = "Countdowns"
title("starting-soon", "Starting soon", CD, [("headline", "Headline", "Starting soon")], [
    text("headline", 160, 300, 1600, "{{headline}}", family=DISPLAY, size=130, weight=700, align="center",
         extra=in_out("rise") + beh("loop", "float")),
    text("time", 560, 540, 800, "{{countdown:05:00}}", family=MONO, size=150, weight=700, align="center",
         fills=fill(CYAN), extra=in_out("fade")),
], background='<background gradient="linear" from="#120c1f" via="#1b1230" to="#07060d" angle="90"/>',
    intro=24, hold=9000, outro=18)
title("countdown-ten", "Ten-second countdown", CD, [], [
    shape("ring", "ellipse", 760, 340, 400, 400, fill(INK7, 0.9), stroke=(MAG, 12), extra=in_out("pop")),
    text("time", 760, 430, 400, "{{countdown:10}}", family=DISPLAY, size=190, weight=700, align="center",
         extra=in_out("fade")),
], intro=6, hold=300, outro=6)
title("be-right-back", "Be right back", CD, [("headline", "Headline", "Be right back")], [
    text("headline", 160, 330, 1600, "{{headline}}", family=DISPLAY, size=130, weight=700, align="center",
         extra=in_out("fade") + beh("loop", "glow-breathe")),
    text("time", 610, 570, 700, "{{countdown:03:00}}", family=MONO, size=110, align="center", fills=fill(PINKW),
         extra=in_out("fade")),
], background=INK_BG, intro=18, hold=5400, outro=18)

LIVE = "Live and social"
title("now-playing", "Now playing", LIVE, [("track", "Track", "Neon Drip"), ("artist", "Artist", "Unicorn Tears")], [
    shape("panel", "rounded-rect", 160, 810, 760, 160, fill(INK7, 0.9), radius=16, extra=in_out("wipe")),
    shape("disc", "ellipse", 190, 842, 96, 96, fill(**TEARS),
          extra=beh("in", "pop") + beh("loop", "pulse") + beh("out", "fade")),
    text("track", 316, 822, 580, "{{track}}", size=58, weight=700, extra=in_out("rise")),
    text("artist", 316, 896, 580, "{{artist}}", size=42, fills=fill(PINKW), extra=in_out("fade")),
], hold=300)
title("chat-highlight", "Chat highlight", LIVE,
      [("user", "User", "viewer42"), ("message", "Message", "This is the best stream!")], [
    shape("panel", "rounded-rect", 160, 730, 920, 240, fill(INK7, 0.92), radius=18, stroke=(VIOLET, 3),
          extra=in_out("pop")),
    text("user", 200, 752, 840, "{{user}}", size=44, weight=700, fills=fill(MAG), extra=in_out("fade")),
    text("message", 200, 816, 840, "{{message}}", size=56, fit="wrap", extra=in_out("typewriter")),
], hold=180)
title("thank-you-alert", "Thank-you alert", LIVE, [("name", "Name", "viewer42"), ("amount", "What", "5 gifted subs")], [
    text("name", 260, 160, 1400, "{{name}}", family=DISPLAY, size=120, weight=700, align="center", fills=fill(**TEARS),
         extra=in_out("pop")),
    text("amount", 260, 320, 1400, "thank you for {{amount}}!", size=64, align="center", shadow=True,
         extra=in_out("word-by-word")),
], intro=18, hold=150, outro=15)
title("on-air-timer", "On air timer", LIVE, [("label", "Label", "ON AIR")], [
    shape("pill", "rounded-rect", 1400, 60, 400, 84, fill(RED, 0.95), radius=42, extra=in_out("pop")),
    text("label", 1428, 72, 180, "{{label}}", size=40, weight=700, tracking=0.08, extra=in_out("fade")),
    text("time", 1610, 72, 164, "{{clip_time}}", family=MONO, size=40, weight=700, align="right", extra=in_out("fade")),
], hold=9000)
title("date-stamp", "Date stamp", LIVE, [("place", "Place", "Studio")], [
    text("date", 160, 850, 900, "{{date:%d %B %Y}}", family=MONO, size=52, weight=700, fills=fill(WHITE, 0.95),
         shadow=True, extra=in_out("typewriter")),
    text("place", 160, 920, 900, "{{place}}", size=42, fills=fill(PINKW), shadow=True, extra=in_out("fade")),
])
title("sponsor", "Sponsor mention", LIVE, [("sponsor", "Sponsor", "Your sponsor"), ("line", "Line", "Supported by")], [
    shape("bar", "rounded-rect", 1020, 800, 740, 170, fill(INK7, 0.92), radius=16, extra=in_out("wipe")),
    text("line", 1056, 816, 670, "{{line}}", size=40, fills=fill(PINKW), align="right", extra=in_out("fade")),
    text("sponsor", 1056, 876, 670, "{{sponsor}}", size=64, weight=700, align="right", extra=in_out("rise")),
])

CAP = "Captions"
FADE = beh("in", "fade", 4) + beh("out", "fade", 4)
CAPTION_FIELDS = [("caption", "Caption", "Caption text"), ("speaker", "Speaker", "")]


def caption_text(y, **kw):
    # Up to three lines at 1080p, wrapping in a centred box; <b>, <i> and
    # <u> from the subtitle file (tags="basic").
    return text("caption", 160, y, 1600, "{{caption}}", size=56, weight=600, align="center", fit="wrap",
                tags="basic", shadow=True, extra=FADE, **kw)


title("caption-plain", "Caption, plain", CAP, CAPTION_FIELDS, [
    text("speaker", 160, 770, 1600, "{{speaker}}", size=40, weight=700, align="center", fills=fill(CYAN),
         shadow=True, stroke=("#000000", 3), extra=FADE),
    caption_text(830, stroke=("#000000", 4)),
], intro=4, hold=60, outro=4)
title("caption-boxed", "Caption, boxed", CAP, CAPTION_FIELDS, [
    shape("band", "rect", 0, 800, 1920, 230, fill(INK9, 0.72), extra=FADE),
    text("speaker", 160, 808, 1600, "{{speaker}}", size=40, weight=700, align="center", fills=fill(CYAN),
         extra=FADE),
    caption_text(856),
], intro=4, hold=60, outro=4)
title("caption-top", "Caption, top", CAP, CAPTION_FIELDS, [
    text("speaker", 160, 70, 1600, "{{speaker}}", size=40, weight=700, align="center", fills=fill(CYAN),
         shadow=True, stroke=("#000000", 3), extra=FADE),
    caption_text(126, stroke=("#000000", 4)),
], intro=4, hold=60, outro=4)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "drop-ins", "titles",
                                                              "data", "templates")
    os.makedirs(out, exist_ok=True)
    for id, xml in TEMPLATES:
        with open(os.path.join(out, id + ".ustitle"), "w", encoding="utf-8") as f:
            f.write(xml)
    print(f"{len(TEMPLATES)} templates in {out}")


if __name__ == "__main__":
    main()
