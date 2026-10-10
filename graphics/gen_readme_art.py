#!/usr/bin/env python3
"""The artwork of README.md / README.it.md, in the app's own look.

GitHub shows a README with no stylesheet of ours, so everything that should
look like the app is an image: the banner (the rack's perforated back, a
glass card like the toolbar, the logo), one heading per section in each
language, a rail as a divider and the five themes side by side. All SVG,
drawn from the palette in regen_graphics.py. The text is turned into outlines
from the app's font (docs/assets/fonts/Geomini.ttf): an SVG shown as an image
cannot load a font, and a fallback face would not be the app's.

    python3 graphics/gen_readme_art.py      # writes graphics/readme/*.svg
"""
import os

from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

import regen_graphics as R

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "readme")
FONT = TTFont(os.path.join(HERE, "..", "docs", "assets", "fonts", "Geomini.ttf"))
GLYPHS = FONT.getGlyphSet()
CMAP = FONT.getBestCmap()
UPM = FONT["head"].unitsPerEm

TEXT = "#EDE6D8"
MUTED = "#9A9284"
SURFACE = "#221F1A"
CABLES = ["#D9534F", "#2ED573", "#9B7FD4", "#5B9BD5"]
W = 1280


def text_width(s, size, spacing=0.0):
    k = size / UPM
    return sum(GLYPHS[CMAP.get(ord(c), CMAP[ord("?")])].width * k + spacing for c in s) - spacing


def text(s, x, y, size, fill, spacing=0.0, anchor="start", opacity=1.0):
    """`s` as one <path>, baseline at y."""
    k = size / UPM
    if anchor == "middle":
        x -= text_width(s, size, spacing) / 2
    elif anchor == "end":
        x -= text_width(s, size, spacing)
    d = []
    for c in s:
        name = CMAP.get(ord(c), CMAP[ord("?")])
        pen = SVGPathPen(GLYPHS)
        GLYPHS[name].draw(TransformPen(pen, (k, 0, 0, -k, x, y)))
        d.append(pen.getCommands())
        x += GLYPHS[name].width * k + spacing
    op = f' fill-opacity="{opacity}"' if opacity < 1 else ""
    return f'<path d="{"".join(d)}" fill="{fill}"{op}/>'


def grate(w, h, bg, rim, hole):
    """The rack's back, as regen_graphics.rail() draws it."""
    out = [f'<rect width="{w}" height="{h}" fill="{bg}"/>']
    step, r, row, y = 19.0, 3.2, 0, 9.5
    rims, holes = [], []
    while y < h:
        x = step / 2 + (step / 2 if row % 2 else 0.0)
        while x < w:
            rims.append(R.circle_subpath(x, y, r))
            holes.append(R.circle_subpath(x, y - 0.6, 1.8))
            x += step
        y += step
        row += 1
    out.append(f'<path d="{"".join(rims)}" fill="{rim}"/>')
    out.append(f'<path d="{"".join(holes)}" fill="{hole}"/>')
    return "".join(out)


def rail(w, y, h=26.0):
    out = [f'<rect x="0" y="{y}" width="{w}" height="{h}" fill="{R.RAIL_COL}"/>',
           f'<rect x="0" y="{y}" width="{w}" height="2" fill="{R.RAIL_HI}"/>',
           f'<rect x="0" y="{y + h - 2}" width="{w}" height="2" fill="{R.RAIL_HOLE}"/>']
    holes = "".join(R.circle_subpath(x, y + h / 2, 4.5) for x in range(15, int(w), 30))
    out.append(f'<path d="{holes}" fill="{R.RAIL_HOLE}"/>')
    out.append(f'<path d="{holes}" fill="none" stroke="{R.RAIL_HI}" stroke-width="0.8"/>')
    return "".join(out)


def jack(cx, cy, r=15.0, plug=None):
    out = [f'<circle cx="{cx}" cy="{cy + 1}" r="{r}" fill="#000" fill-opacity="0.4"/>',
           f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="{R.METAL_LIGHT}"/>',
           f'<circle cx="{cx}" cy="{cy}" r="{r * 0.82}" fill="{R.METAL_DARK}"/>',
           f'<circle cx="{cx}" cy="{cy}" r="{r * 0.5}" fill="#0C0A08"/>']
    if plug:
        out.append(f'<circle cx="{cx}" cy="{cy}" r="{r * 0.9}" fill="{plug}"/>')
        out.append(f'<circle cx="{cx}" cy="{cy}" r="{r * 0.42}" fill="#000" fill-opacity="0.35"/>')
    return "".join(out)


def knob(cx, cy, r, angle, accent=None, body=None):
    import math
    accent = accent or R.ACCENT
    a = math.radians(angle - 90)
    x1, y1 = cx + r * 0.30 * math.cos(a), cy + r * 0.30 * math.sin(a)
    x2, y2 = cx + r * 0.78 * math.cos(a), cy + r * 0.78 * math.sin(a)
    return (f'<circle cx="{cx}" cy="{cy + r * 0.07}" r="{r}" fill="#000" fill-opacity="0.35"/>'
            f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="{R.KNOB_RING}"/>'
            f'<circle cx="{cx}" cy="{cy}" r="{r * 0.85}" fill="{body or R.KNOB_BODY}"/>'
            f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{accent}" '
            f'stroke-width="{r * 0.13:.1f}" stroke-linecap="round"/>')


def cable(x1, y1, x2, y2, color, sag=90):
    mx = (x1 + x2) / 2
    return (f'<path d="M{x1},{y1} C{x1},{y1 + sag} {x2},{y2 + sag} {x2},{y2}" fill="none" '
            f'stroke="#000" stroke-opacity="0.35" stroke-width="9" stroke-linecap="round" transform="translate(0,3)"/>'
            f'<path d="M{x1},{y1} C{x1},{y1 + sag} {x2},{y2 + sag} {x2},{y2}" fill="none" '
            f'stroke="{color}" stroke-opacity="0.9" stroke-width="7" stroke-linecap="round"/>')


def logo(x, y, s):
    """docs/assets/logo.svg, the knob over two patched jacks."""
    return (f'<g transform="translate({x},{y}) scale({s})" fill="none" stroke-linecap="round">'
            f'<circle cx="54" cy="47" r="17" stroke="{TEXT}" stroke-width="6"/>'
            f'<path d="M54,47 L43,36" stroke="{R.ACCENT}" stroke-width="6"/>'
            f'<circle cx="38" cy="79" r="7" stroke="{TEXT}" stroke-width="4.5"/>'
            f'<circle cx="38" cy="79" r="3" fill="{R.ACCENT}"/>'
            f'<circle cx="70" cy="79" r="7" stroke="{TEXT}" stroke-width="4.5"/>'
            f'<circle cx="70" cy="79" r="3" fill="{R.ACCENT}"/>'
            f'<path d="M38,86 C42,100 66,100 70,86" stroke="{R.ACCENT}" stroke-width="4"/></g>')


def svg(w, h, body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">'
            f'<defs><clipPath id="r"><rect width="{w}" height="{h}" rx="14"/></clipPath></defs>'
            f'<g clip-path="url(#r)">{body}</g></svg>')


def hero(tagline):
    h = 400
    b = [grate(W, h, R.RAIL_BG, R.RAIL_GRATE, R.RAIL_HOLE), rail(W, 0), rail(W, h - 26)]
    # Patch points either side, wired across behind the card.
    left = [(70, 120), (70, 200), (70, 280), (150, 160), (150, 240)]
    right = [(W - 70, 120), (W - 70, 200), (W - 70, 280), (W - 150, 160), (W - 150, 240)]
    wires = [(0, 3, 0), (1, 0, 1), (3, 2, 2), (4, 4, 3)]
    for a, c, col in wires:
        b.append(cable(left[a][0], left[a][1], right[c][0], right[c][1], CABLES[col], sag=150))
    for i, (x, y) in enumerate(left):
        b.append(jack(x, y, plug=next((CABLES[col] for a, c, col in wires if a == i), None)))
    for i, (x, y) in enumerate(right):
        b.append(jack(x, y, plug=next((CABLES[col] for a, c, col in wires if c == i), None)))
    for x, ang in ((250, -40), (W - 250, 60)):
        b.append(knob(x, 200, 34, ang))
    # The toolbar's glass card.
    cw, ch = 640, 236
    cx, cy = (W - cw) / 2, (h - ch) / 2
    b.append(f'<rect x="{cx}" y="{cy + 4}" width="{cw}" height="{ch}" rx="26" fill="#000" fill-opacity="0.35"/>')
    b.append(f'<rect x="{cx}" y="{cy}" width="{cw}" height="{ch}" rx="26" fill="{SURFACE}" fill-opacity="0.93" '
             f'stroke="#FFFFFF" stroke-opacity="0.15"/>')
    b.append(logo(W / 2 - 54 * 0.95, cy + 6, 0.95))
    b.append(text("RackDroid", W / 2, cy + 154, 62, TEXT, spacing=1.5, anchor="middle"))
    b.append(f'<rect x="{W / 2 - 150}" y="{cy + 172}" width="300" height="3" rx="1.5" fill="{R.ACCENT}"/>')
    b.append(text(tagline.upper(), W / 2, cy + 207, 17, R.ACCENT, spacing=2.6, anchor="middle"))
    return svg(W, h, "".join(b))


def heading(title):
    h = 64
    b = [f'<rect width="{W}" height="{h}" fill="{SURFACE}"/>',
         f'<rect width="{W}" height="{h}" fill="none" stroke="#FFFFFF" stroke-opacity="0.12" rx="14"/>',
         f'<rect x="0" y="0" width="8" height="{h}" fill="{R.ACCENT}"/>',
         jack(46, h / 2, 13, plug=None),
         text(title.upper(), 76, h / 2 + 8, 23, TEXT, spacing=3.2)]
    tw = 76 + text_width(title.upper(), 23, 3.2) + 24
    b.append(f'<rect x="{tw}" y="{h / 2 - 1}" width="{W - tw - 28}" height="2" rx="1" fill="{R.ACCENT}" fill-opacity="0.35"/>')
    return svg(W, h, "".join(b))


def divider():
    return svg(W, 26, rail(W, 0))


def themes(names):
    """The five themes: a strip of panel each, its own rack behind it."""
    h, n = 190, 5
    order = ["amber", "blue", "emerald", "violet", "cream"]
    cw = W / n
    b = []
    for i, theme in enumerate(order):
        R.set_palette(theme)
        x = i * cw
        ink = R.TEXT
        b.append(f'<g transform="translate({x},0)"><svg width="{cw}" height="{h}">'
                 + grate(cw, h, R.RAIL_BG, R.RAIL_GRATE, R.RAIL_HOLE)
                 + f'<rect x="28" y="14" width="{cw - 56}" height="{h - 28}" fill="{R.PANEL_GRAD1}"/>'
                 + f'<rect x="28" y="14" width="{cw - 56}" height="5" fill="{R.ACCENT}"/>'
                 + f'<rect x="28" y="{h - 19}" width="{cw - 56}" height="5" fill="{R.PANEL_FOOT}"/>'
                 + knob(cw / 2 - 42, 82, 26, -35) + knob(cw / 2 + 42, 82, 26, 50)
                 + jack(cw / 2 - 42, 140, 12) + jack(cw / 2 + 42, 140, 12, plug=R.ACCENT)
                 + text(names[i].upper(), cw / 2, 46, 15, ink, spacing=2.4, anchor="middle")
                 + '</svg></g>')
    R.set_palette("amber")
    return svg(W, h, "".join(b))


LANG = {
    "en": {
        "tagline": "Your modular rack, in your pocket",
        "themes": ["Amber", "Blue Night", "Emerald", "Violet", "Cream"],
        "heads": {"why": "Why RackDroid", "includes": "What it includes", "modules": "Additional modules",
                  "requirements": "Requirements", "performance": "Performance", "build": "Build",
                  "structure": "Structure", "licenses": "Licenses and trademarks", "themes": "Five themes"},
    },
    "it": {
        "tagline": "Il tuo rack modulare, in tasca",
        "themes": ["Ambra", "Blu notte", "Verde smeraldo", "Violetto", "Crema"],
        "heads": {"why": "Perché RackDroid", "includes": "Cosa include", "modules": "Moduli aggiuntivi",
                  "requirements": "Requisiti", "performance": "Prestazioni", "build": "Build",
                  "structure": "Struttura", "licenses": "Licenze e marchi", "themes": "Cinque temi"},
    },
}

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    files = {"divider.svg": divider()}
    for lang, t in LANG.items():
        files[f"hero-{lang}.svg"] = hero(t["tagline"])
        files[f"themes-{lang}.svg"] = themes(t["themes"])
        for slug, title in t["heads"].items():
            files[f"head-{slug}-{lang}.svg"] = heading(title)
    for name, content in files.items():
        with open(os.path.join(OUT, name), "w", encoding="utf-8") as f:
            f.write(content)
    print(f"{len(files)} files -> {OUT}")
