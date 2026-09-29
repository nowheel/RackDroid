#!/usr/bin/env python3
"""Generate ORIGINAL replacement graphics for RackDroid.

The upstream VCV Component Library, Core panels and Fundamental panels are
licensed CC BY-NC / CC BY-NC-ND (non-commercial). To make RackDroid
commercially distributable, this script produces original, geometric
replacements with the SAME canvas dimensions and filenames (drop-in), so the
module widget code — which positions controls by coordinate and loads these
SVGs by path — keeps working.

Only facts are read from the originals: canvas dimensions, and for panels the
functional text labels + their positions (short words like "FREQ", not
copyrightable artwork). All visual design here (shapes, colors, gradients) is
original to this project, released under the same GPLv3 as the app.
"""
import os
import re
import sys
import xml.sax.saxutils as sx

# --- RackDroid "warm studio" palette -----------------------------------------
# Quiet-luxury smart-home look: warm taupe/cream on near-black brown glass
# (key tones #AA907A / #FFDA9F / #0F0F0F), replacing the original blue-slate
# + amber scheme.
PANEL_BG = "#2B2721"
PANEL_BG2 = "#211E19"
ACCENT = "#FFDA9F"
ACCENT_DIM = "#C8985C"
METAL = "#8A8173"
METAL_LIGHT = "#A39A8A"
METAL_DARK = "#544E42"
KNOB_BODY = "#3A352D"
KNOB_BODY_HI = "#4A4438"
KNOB_RING = "#575046"
KNOB_RING_HI = "#6C6457"
HOLE = "#14120E"
TEXT = "#EDE6D8"

# Panel face gradient + header-strip highlight, and the rack rail/background
# palette. Previously inline literals in gen_panel()/rail(); lifted to
# globals so a color theme can reassign them (see THEMES / set_palette).
# These amber defaults keep the canonical output byte-identical.
PANEL_GRAD1 = "#3A342B"
PANEL_GRAD2 = "#292520"
STRIP_HI = "#FFE8C2"
RAIL_BG = "#141210"
RAIL_BG_DARK = "#100E0B"
RAIL_GRATE = "#1C1915"
RAIL_GRATE_DARK = "#191612"
RAIL_COL = "#332E26"
RAIL_HOLE = "#110F0C"
RAIL_HI = "#4C4539"

# Alternate color themes for the rack (background/rail + module panels).
# Amber is the canonical/default look (all values above). Each other theme
# overrides only the color-bearing globals; metal/text stay neutral. The
# accent pair drives every accent detail (knob pointers, switch nubs, LEDs,
# header strip) in cascade, so a small override recolors the whole rack.
THEMES = {
    "amber": {},  # canonical defaults — no overrides
    "blue": {
        "ACCENT": "#7FC4FF", "ACCENT_DIM": "#4E88BF", "STRIP_HI": "#C7E4FF",
        "PANEL_BG": "#262B33", "PANEL_BG2": "#1C2027",
        "PANEL_GRAD1": "#2E3440", "PANEL_GRAD2": "#23282F",
        "KNOB_BODY_HI": "#3B434E", "KNOB_RING": "#49525E", "KNOB_RING_HI": "#5B6673",
        "RAIL_BG": "#10141A", "RAIL_BG_DARK": "#0C0F14",
        "RAIL_GRATE": "#171C24", "RAIL_GRATE_DARK": "#141821",
        "RAIL_COL": "#2A313B", "RAIL_HOLE": "#0D1116", "RAIL_HI": "#3E4956",
    },
    "emerald": {
        "ACCENT": "#6FE0A0", "ACCENT_DIM": "#3E9E6C", "STRIP_HI": "#BEF2D4",
        "PANEL_BG": "#26302A", "PANEL_BG2": "#1B231D",
        "PANEL_GRAD1": "#2C3A30", "PANEL_GRAD2": "#212B24",
        "KNOB_BODY_HI": "#3B4A40", "KNOB_RING": "#47584C", "KNOB_RING_HI": "#586B5D",
        "RAIL_BG": "#101612", "RAIL_BG_DARK": "#0C110E",
        "RAIL_GRATE": "#161F19", "RAIL_GRATE_DARK": "#131A15",
        "RAIL_COL": "#29352D", "RAIL_HOLE": "#0D120F", "RAIL_HI": "#3C4A40",
    },
    "violet": {
        "ACCENT": "#C9A6F5", "ACCENT_DIM": "#8E6BC0", "STRIP_HI": "#E6D4FF",
        "PANEL_BG": "#2E2838", "PANEL_BG2": "#221C2B",
        "PANEL_GRAD1": "#362E40", "PANEL_GRAD2": "#29232F",
        "KNOB_BODY_HI": "#453B50", "KNOB_RING": "#52495E", "KNOB_RING_HI": "#645872",
        "RAIL_BG": "#14101A", "RAIL_BG_DARK": "#100C15",
        "RAIL_GRATE": "#1B1624", "RAIL_GRATE_DARK": "#171320",
        "RAIL_COL": "#322A3B", "RAIL_HOLE": "#110D16", "RAIL_HI": "#463E56",
    },
}


# Canonical (amber) values of every themeable global, snapshotted so that
# set_palette() can always restore the baseline before applying overrides
# (switching blue -> emerald must not leave blue's values behind).
_THEMEABLE = ("ACCENT", "ACCENT_DIM", "STRIP_HI", "PANEL_BG", "PANEL_BG2",
              "PANEL_GRAD1", "PANEL_GRAD2", "KNOB_BODY_HI", "KNOB_RING",
              "KNOB_RING_HI", "RAIL_BG", "RAIL_BG_DARK", "RAIL_GRATE",
              "RAIL_GRATE_DARK", "RAIL_COL", "RAIL_HOLE", "RAIL_HI")
_DEFAULTS = {k: globals()[k] for k in _THEMEABLE}


def set_palette(theme):
    """Reassign the module-level color globals the drawing functions read at
    call time: restore the amber baseline, then apply the theme's overrides.
    Idempotent and order-independent. Unknown/'amber' -> canonical."""
    globals().update(_DEFAULTS)
    globals().update(THEMES.get(theme, {}))

# nanosvg renders linear/radial gradients (no filters/blur), so all depth
# cues below are gradient + concentric-shape tricks.

def lgrad(gid, c1, c2, x1=0, y1=0, x2=0, y2=1):
    """Vertical-by-default linear gradient def (objectBoundingBox units)."""
    return (f'<linearGradient id="{gid}" x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}">'
            f'<stop offset="0" stop-color="{c1}"/>'
            f'<stop offset="1" stop-color="{c2}"/></linearGradient>')


def rgrad(gid, c1, c2, cx=0.4, cy=0.35, r=0.75):
    """Radial gradient def, highlight offset to the upper left by default."""
    return (f'<radialGradient id="{gid}" cx="{cx}" cy="{cy}" r="{r}">'
            f'<stop offset="0" stop-color="{c1}"/>'
            f'<stop offset="1" stop-color="{c2}"/></radialGradient>')


def dims(svg_text):
    """Return (width, height, unit) from width/height attrs or viewBox.

    The unit suffix ("mm", "px" or "") MUST be preserved on the output file:
    nanosvg converts mm to px at load DPI (1mm ~ 2.95px in Rack), so writing
    a bare "25.4" where the original said "25.4mm" renders the file at a
    third of its intended size (this was live-reported as modules
    "fuori misura": VCA/Unity panels drawn too small inside their slot and
    the mm-sized light components nearly invisible).
    """
    w = re.search(r'\bwidth="([0-9.]+)([a-z]*)"', svg_text)
    h = re.search(r'\bheight="([0-9.]+)', svg_text)
    if w and h:
        return float(w.group(1)), float(h.group(1)), w.group(2)
    vb = re.search(r'viewBox="[\d.]+ [\d.]+ ([\d.]+) ([\d.]+)"', svg_text)
    if vb:
        return float(vb.group(1)), float(vb.group(2)), ""
    return 20.0, 20.0, ""


def svg_open(w, h):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
            f'viewBox="0 0 {w} {h}">')


def knob(w, h, indicator=True, cap=False):
    cx, cy = w / 2, h / 2
    r = min(w, h) / 2 * 0.92
    out = [svg_open(w, h)]
    out.append('<defs>'
               + lgrad("ring", KNOB_RING_HI, "#3B362D")
               + rgrad("body", KNOB_BODY_HI, "#2C2820")
               + '</defs>')
    # Drop shadow under the knob (offset dark disc; no blur in nanosvg)
    out.append(f'<circle cx="{cx}" cy="{cy+r*0.06:.2f}" r="{r}" fill="#000000" fill-opacity="0.35"/>')
    out.append(f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="url(#ring)"/>')
    out.append(f'<circle cx="{cx}" cy="{cy}" r="{r*0.85}" fill="url(#body)"/>')
    # Thin light arc on the upper rim for a machined edge
    out.append(f'<circle cx="{cx}" cy="{cy}" r="{r*0.85}" fill="none" '
               f'stroke="#FFFFFF" stroke-opacity="0.10" stroke-width="{r*0.05:.2f}"/>')
    if cap:
        out.append(f'<circle cx="{cx}" cy="{cy}" r="{r*0.30}" fill="url(#ring)"/>')
    if indicator:
        # Soft amber glow behind the pointer, then the pointer itself
        out.append(f'<rect x="{cx-r*0.10:.2f}" y="{cy-r*0.86:.2f}" width="{r*0.20:.2f}" '
                   f'height="{r*0.56:.2f}" rx="{r*0.10:.2f}" fill="{ACCENT}" fill-opacity="0.28"/>')
        out.append(f'<rect x="{cx-r*0.055:.2f}" y="{cy-r*0.82:.2f}" width="{r*0.11:.2f}" '
                   f'height="{r*0.5:.2f}" rx="{r*0.055:.2f}" fill="{ACCENT}"/>')
    out.append('</svg>')
    return "\n".join(out)


def port(w, h):
    cx, cy = w / 2, h / 2
    r = min(w, h) / 2 * 0.95
    return "\n".join([
        svg_open(w, h),
        '<defs>'
        + lgrad("prim", METAL_LIGHT, "#3E382E")
        + lgrad("pface", METAL, METAL_DARK)
        + rgrad("phole", "#211E18", "#0C0A08", cx=0.5, cy=0.4, r=0.7)
        + '</defs>',
        f'<circle cx="{cx}" cy="{cy+r*0.05:.2f}" r="{r}" fill="#000000" fill-opacity="0.35"/>',
        f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="url(#prim)"/>',
        f'<circle cx="{cx}" cy="{cy}" r="{r*0.82}" fill="url(#pface)"/>',
        f'<circle cx="{cx}" cy="{cy}" r="{r*0.52}" fill="url(#phole)"/>',
        f'<circle cx="{cx}" cy="{cy}" r="{r*0.28}" fill="#0A0806"/>',
        '</svg>',
    ])


def screw(w, h):
    cx, cy = w / 2, h / 2
    r = min(w, h) / 2 * 0.8
    return "\n".join([
        svg_open(w, h),
        '<defs>' + rgrad("shead", METAL_LIGHT, METAL_DARK) + '</defs>',
        f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="url(#shead)"/>',
        f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="none" stroke="#000000" '
        f'stroke-opacity="0.3" stroke-width="{r*0.1:.2f}"/>',
        f'<rect x="{cx-r*0.75}" y="{cy-r*0.13}" width="{r*1.5}" height="{r*0.26}" '
        f'rx="{r*0.13:.2f}" fill="#332E26" transform="rotate(35 {cx} {cy})"/>',
        '</svg>',
    ])


def switch(w, h, state, nstates):
    out = [svg_open(w, h),
           '<defs>'
           + lgrad("swt", "#14120D", "#2E2922")
           + lgrad("swn", ACCENT, ACCENT_DIM)
           + '</defs>',
           f'<rect x="{w*0.28}" y="1" width="{w*0.44}" height="{h-2}" '
           f'rx="{w*0.18}" fill="url(#swt)"/>']
    # Nub position along the track by state index
    if nstates > 1:
        t = state / (nstates - 1)
    else:
        t = 0.5
    ny = (h - 2) * (0.12 + 0.72 * (1 - t)) + 1
    out.append(f'<rect x="{w*0.2}" y="{ny}" width="{w*0.6}" height="{h*0.24}" '
               f'rx="{w*0.1}" fill="url(#swn)"/>')
    out.append(f'<rect x="{w*0.24:.2f}" y="{ny + h*0.03:.2f}" width="{w*0.52:.2f}" '
               f'height="{h*0.05:.2f}" rx="{h*0.025:.2f}" fill="#FFFFFF" fill-opacity="0.35"/>')
    out.append('</svg>')
    return "\n".join(out)


def button(w, h, pressed):
    cx, cy = w / 2, h / 2
    r = min(w, h) / 2 * 0.9
    face = lgrad("bface", ACCENT, ACCENT_DIM) if pressed \
        else lgrad("bface", KNOB_BODY_HI, "#2E2922")
    return "\n".join([
        svg_open(w, h),
        '<defs>' + lgrad("bbez", KNOB_RING_HI, "#3B362D") + face + '</defs>',
        f'<rect x="{cx-r}" y="{cy-r}" width="{r*2}" height="{r*2}" rx="{r*0.35}" '
        f'fill="url(#bbez)"/>',
        f'<rect x="{cx-r*0.82}" y="{cy-r*0.82}" width="{r*1.64}" height="{r*1.64}" '
        f'rx="{r*0.28}" fill="url(#bface)"/>',
        f'<rect x="{cx-r*0.62:.2f}" y="{cy-r*0.68:.2f}" width="{r*1.24:.2f}" height="{r*0.3:.2f}" '
        f'rx="{r*0.15:.2f}" fill="#FFFFFF" fill-opacity="{0.30 if pressed else 0.06}"/>',
        '</svg>',
    ])


def light(w, h):
    cx, cy = w / 2, h / 2
    r = min(w, h) / 2 * 0.9
    return "\n".join([
        svg_open(w, h),
        '<defs>' + rgrad("lens", "#1B1813", "#0C0A08", cx=0.4, cy=0.35, r=0.8) + '</defs>',
        f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="url(#lens)"/>',
        f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="none" stroke="#FFFFFF" '
        f'stroke-opacity="0.08" stroke-width="{max(r*0.08, 0.3):.2f}"/>',
        '</svg>',
    ])


def slider_track(w, h):
    """Vertical fader track: recessed groove with side tick marks."""
    out = [svg_open(w, h)]
    slot_w = max(w * 0.16, 1.2)
    # Recessed well behind the groove
    out.append(f'<rect x="{w*0.5 - slot_w*2:.2f}" y="1" width="{slot_w*4:.2f}" '
               f'height="{h-2}" rx="{slot_w:.2f}" fill="{PANEL_BG2}"/>')
    # Tick marks along both sides
    ticks = 11
    for i in range(ticks):
        ty = 2 + (h - 4) * i / (ticks - 1)
        for tx in (w * 0.08, w * 0.72):
            out.append(f'<rect x="{tx:.2f}" y="{ty:.2f}" width="{w*0.20:.2f}" '
                       f'height="0.6" fill="{KNOB_RING}"/>')
    # The groove itself
    out.append(f'<rect x="{w*0.5 - slot_w/2:.2f}" y="2" width="{slot_w:.2f}" '
               f'height="{h-4}" rx="{slot_w/2:.2f}" fill="{HOLE}"/>')
    out.append('</svg>')
    return "\n".join(out)


def slider_handle(w, h):
    """Fader cap: metal body with a center accent grip line."""
    return "\n".join([
        svg_open(w, h),
        '<defs>' + lgrad("cap", KNOB_BODY_HI, "#2C2820") + '</defs>',
        f'<rect x="0.5" y="0.5" width="{w-1}" height="{h-1}" rx="{min(w,h)*0.18:.2f}" '
        f'fill="url(#cap)" stroke="{KNOB_RING}" stroke-width="0.8"/>',
        f'<rect x="1" y="{h*0.5 - h*0.08:.2f}" width="{w-2}" height="{h*0.16:.2f}" '
        f'rx="{h*0.08:.2f}" fill="{ACCENT}"/>',
        f'<rect x="1.5" y="{h*0.12:.2f}" width="{w-3}" height="{h*0.08:.2f}" '
        f'rx="{h*0.04:.2f}" fill="#FFFFFF" fill-opacity="0.08"/>',
        '</svg>',
    ])


def blank(w, h):
    return svg_open(w, h) + f'<rect width="{w}" height="{h}" fill="none"/></svg>'


def circle_subpath(cx, cy, r):
    """One circle as a closed subpath, for merging many into a single <path>."""
    return (f"M{cx - r:.2f},{cy:.2f}"
            f"A{r:.2f},{r:.2f} 0 1,0 {cx + r:.2f},{cy:.2f}"
            f"A{r:.2f},{r:.2f} 0 1,0 {cx - r:.2f},{cy:.2f}Z")


def rail(w, h, dark=False):
    """Rack background: perforated 'grate' metal tiled across the rack, with
    a mounting rail (holes) at the top and bottom of each 380px row.

    The holes are grouped one <path> per row and kind: ~50 shapes instead of
    one <circle> each (667). Rack draws an SVG one nanovg fill per shape, and
    the rail tile is re-rendered whenever the zoom changes, so fewer fills is
    cheaper -- but measured on a Nothing A024 and a OnePlus 8T it was NOT what
    made pinch-zoom stutter: the rebuild cost the same with 51 shapes, and
    grew with the framebuffer's pixel size. That was the texture being
    reallocated (see pinchFreezesFramebuffers() in window_android.cpp).
    Do not put all the holes in ONE path either: window::svgDraw tests every
    subpath of a shape against every segment of the others to tell holes from
    solids, and at 330 subpaths that is over a million tests per render. The
    holes never overlap, so none of this changes what is on screen.
    """
    bg = RAIL_BG_DARK if dark else RAIL_BG
    grate = RAIL_GRATE_DARK if dark else RAIL_GRATE
    railcol = RAIL_COL
    holecol = RAIL_HOLE
    hi = RAIL_HI
    ROW = 380.0  # RACK_GRID_HEIGHT
    out = [svg_open(w, h)]
    out.append(f'<rect width="{w}" height="{h}" fill="{bg}"/>')
    # Perforated grate: staggered small holes across the whole tile
    step = 19.0
    r = 3.2
    grate_rows = []
    row = 0
    y = step / 2
    while y < h:
        off = (step / 2) if (row % 2) else 0.0
        x = step / 2 + off
        rims, holes = [], []
        while x < w:
            rims.append(circle_subpath(x, y, r))
            holes.append(circle_subpath(x, y - 0.6, round(r * 0.55, 1)))
            x += step
        grate_rows.append(("".join(rims), "".join(holes)))
        y += step
        row += 1
    for rims, holes in grate_rows:
        out.append(f'<path d="{rims}" fill="{grate}"/>')
        out.append(f'<path d="{holes}" fill="{holecol}"/>')
    # Mounting rails at each row boundary (top and bottom of every 380px row)
    rail_h = 26.0
    mount_rows = []
    yb = 0.0
    while yb <= h + 1:
        out.append(f'<rect x="0" y="{yb:.1f}" width="{w}" height="{rail_h}" fill="{railcol}"/>')
        out.append(f'<rect x="0" y="{yb:.1f}" width="{w}" height="2" fill="{hi}"/>')
        out.append(f'<rect x="0" y="{yb+rail_h-2:.1f}" width="{w}" height="2" fill="{holecol}"/>')
        # Evenly spaced mounting holes along the rail
        hx = 15.0
        holes = []
        while hx < w:
            holes.append(circle_subpath(hx, yb + rail_h / 2, 4.5))
            hx += 30.0
        if holes:
            mount_rows.append("".join(holes))
        yb += ROW
    for holes in mount_rows:
        out.append(f'<path d="{holes}" fill="{holecol}"/>')
        out.append(f'<path d="{holes}" fill="none" stroke="{hi}" stroke-width="0.8"/>')
    out.append('</svg>')
    return "\n".join(out)


def classify_component(name):
    n = name.lower()
    if n.startswith("rail"):
        return ("rail", False, False, None)
    base = name
    for suf in ("_bg", "_fg"):
        if base.endswith(suf):
            base = base[:-3]
    state = None
    m = re.search(r'_(\d+)$', base)
    if m:
        state = int(m.group(1))
    if any(k in n for k in ("knob", "trimpot", "rogan", "davies", "trimmer", "pot")) and "slide" not in n:
        return ("knob", name.endswith("_bg"), name.endswith("_fg"), state)
    if any(k in n for k in ("port", "pj301", "pj341", "cl1362", "plug", "jack")):
        return ("port", False, False, state)
    if "screw" in n:
        return ("screw", False, False, state)
    if any(k in n for k in ("ckss", "switch", "nkk", "ckd6", "toggle")):
        return ("switch", False, False, state)
    if any(k in n for k in ("button", "tl1105", "push", "led")):
        if "slide" in n:
            return ("slider", False, False, state)
        return ("button", False, False, state)
    if ("slide" in n or "slider" in n or "fader" in n) and "light" not in n:
        return ("slider", False, False, state)
    if "light" in n:
        return ("light", False, False, state)
    return ("knob", name.endswith("_bg"), name.endswith("_fg"), state)


def gen_component(name, w, h):
    kind, is_bg, is_fg, state = classify_component(name)
    if kind == "rail":
        return rail(w, h, dark="dark" in name.lower())
    if kind == "knob":
        if is_fg:
            return blank(w, h)  # front highlight overlay: keep transparent
        return knob(w, h, indicator=not is_bg, cap=is_bg)
    if kind == "port":
        return port(w, h)
    if kind == "screw":
        return screw(w, h)
    if kind == "switch":
        # Count sibling states by scanning is hard here; assume 2 or 3 by state
        n = 3 if state is not None and state >= 2 else 2
        return switch(w, h, state or 0, n)
    if kind == "button":
        return button(w, h, pressed=(state == 1))
    if kind == "slider":
        # Track vs cap: the track is the tall piece (VCVSlider,
        # BefacoSlidePot); the cap is the small "handle" overlay that Rack
        # moves along it. Distinguish by name, falling back to aspect.
        if "handle" in name.lower() or h < w * 2.5:
            return slider_handle(w, h)
        return slider_track(w, h)
    if kind == "light":
        return light(w, h)
    return knob(w, h)


def extract_texts(svg_text):
    """Extract (x, y, size, content) for <text> elements — functional labels."""
    out = []
    for m in re.finditer(r'<text\b([^>]*)>(.*?)</text>', svg_text, re.S):
        attrs, body = m.group(1), m.group(2)
        x = re.search(r'\bx="([-0-9.]+)"', attrs)
        y = re.search(r'\by="([-0-9.]+)"', attrs)
        fs = re.search(r'font-size="([0-9.]+)"', attrs)
        # strip nested tspans/markup, collapse whitespace
        content = re.sub(r'<[^>]+>', '', body)
        content = re.sub(r'\s+', ' ', content).strip()
        if x and y and content:
            out.append((float(x.group(1)), float(y.group(1)),
                        float(fs.group(1)) if fs else 3.5, content))
    return out


def gen_panel(name, w, h, texts):
    out = [svg_open(w, h)]
    out.append('<defs>'
               + lgrad("panel", PANEL_GRAD1, PANEL_GRAD2)
               + lgrad("strip", STRIP_HI, ACCENT_DIM)
               # Diagonal glass sheen: soft white reflection fading out
               # before mid-panel (glassmorphism cue, no blur in nanosvg).
               + '<linearGradient id="sheen" x1="0" y1="0" x2="0.65" y2="1">'
                 '<stop offset="0" stop-color="#FFFFFF" stop-opacity="0.09"/>'
                 '<stop offset="0.4" stop-color="#FFFFFF" stop-opacity="0.02"/>'
                 '<stop offset="1" stop-color="#FFFFFF" stop-opacity="0"/>'
                 '</linearGradient>'
               + '</defs>')
    # Slightly translucent: the rack grate ghosts through like smoked glass.
    out.append(f'<rect width="{w}" height="{h}" fill="url(#panel)" fill-opacity="0.97"/>')
    out.append(f'<rect width="{w}" height="{h}" fill="url(#sheen)"/>')
    # Slim amber header strip with a light edge, dark footer band
    strip = max(2.0, h * 0.014)
    out.append(f'<rect x="0" y="0" width="{w}" height="{strip:.2f}" fill="url(#strip)"/>')
    out.append(f'<rect x="0" y="{strip:.2f}" width="{w}" height="0.8" '
               f'fill="#000000" fill-opacity="0.35"/>')
    out.append(f'<rect x="0" y="{h-strip:.2f}" width="{w}" height="{strip:.2f}" fill="#1B1813"/>')
    # Hairline inner border: the "milled edge" that separates modules
    out.append(f'<rect x="0.6" y="0.6" width="{w-1.2:.2f}" height="{h-1.2:.2f}" '
               f'fill="none" stroke="#FFFFFF" stroke-opacity="0.13" stroke-width="1"/>')
    out.append(f'<rect x="0" y="0" width="1" height="{h}" fill="#FFFFFF" fill-opacity="0.07"/>')
    out.append(f'<rect x="{w-1}" y="0" width="1" height="{h}" fill="#000000" fill-opacity="0.45"/>')
    # Module name across the top (from filename)
    title = re.sub(r'[-_]', ' ', name).upper()
    out.append(f'<text x="{w/2}" y="{h*0.045}" font-family="sans-serif" '
               f'font-size="{min(6, w*0.22)}" fill="{TEXT}" text-anchor="middle" '
               f'font-weight="bold">{sx.escape(title)}</text>')
    # Re-typeset the functional labels at their original positions
    for (x, y, fs, content) in texts:
        out.append(f'<text x="{x}" y="{y}" font-family="sans-serif" '
                   f'font-size="{fs}" fill="{TEXT}" text-anchor="middle">'
                   f'{sx.escape(content)}</text>')
    out.append('</svg>')
    return "\n".join(out)


def process_dir(src, dst, mode):
    os.makedirs(dst, exist_ok=True)
    count = 0
    for fn in sorted(os.listdir(src)):
        if not fn.endswith(".svg"):
            continue
        with open(os.path.join(src, fn), encoding="utf-8", errors="ignore") as f:
            text = f.read()
        w, h, unit = dims(text)
        name = fn[:-4]
        # Panels are large; anything small in a panel dir is a component
        # (e.g. Fundamental's VCVBezelBig button bezel). mm-sized panels are
        # numerically small (128.5mm) but still panels: compare in px.
        px_scale = 2.9527559 if unit == "mm" else 1.0
        is_component = mode == "component" or max(w, h) * px_scale < 60
        if is_component:
            new = gen_component(name, w, h)
        else:
            new = gen_panel(name, w, h, extract_texts(text))
        if unit:
            # Restore the original unit suffix on the root element only;
            # the viewBox (and all shape coordinates) stay in user units.
            new = re.sub(
                r'(<svg[^>]*?width=")([0-9.]+)(" height=")([0-9.]+)(")',
                rf'\g<1>\g<2>{unit}\g<3>\g<4>{unit}\g<5>',
                new, count=1)
        with open(os.path.join(dst, fn), "w", encoding="utf-8") as f:
            f.write(new)
        count += 1
    return count


if __name__ == "__main__":
    root = os.path.dirname(os.path.abspath(__file__))
    tp = os.path.normpath(os.path.join(root, "..", "third_party"))
    jobs = [
        (f"{tp}/Rack/res/ComponentLibrary", f"{root}/system-res/ComponentLibrary", "component"),
        (f"{tp}/Rack/res/Core", f"{root}/system-res/Core", "panel"),
        (f"{tp}/Fundamental/res", f"{root}/fundamental-res", "panel"),
        # Frozen Wasteland: code is GPLv3 but everything under res/ is
        # CC BY-NC-ND, so panels AND its custom component library are
        # regenerated (fonts are handled by copy_free_fonts below).
        (f"{tp}/FrozenWasteland/res", f"{root}/frozenwasteland-res", "panel"),
        (f"{tp}/FrozenWasteland/res/ComponentLibrary",
         f"{root}/frozenwasteland-res/ComponentLibrary", "component"),
        # Audible Instruments: GPLv3 code + MIT DSP (Mutable eurorack), but
        # the panels are (c) Emilie Gillet "distributed with permission" —
        # a personal grant to VCV that does not extend to us, so they are
        # regenerated. The CKSS_rot_* switch frames in the same dir are
        # auto-classified as components by size. The Segment14 display font
        # is OFL and copied as-is below.
        (f"{tp}/AudibleInstruments/res", f"{root}/audible-res", "panel"),
        # Impromptu Modular: GPL code, res/ art CC BY-NC-ND (+ complib
        # adapted from VCV's CC BY-NC components) -> all regenerated.
        # res/fonts (OFL faces) is skipped by the walk and copied below.
        (f"{tp}/ImpromptuModular/res", f"{root}/impromptu-res", "panel-r"),
        # Count Modula: GPL code, but LICENSE clause 18 reserves all rights
        # on logo/panels/components -> all 1000+ SVGs (7 panel themes)
        # regenerated.
        (f"{tp}/CountModula/res", f"{root}/countmodula-res", "panel-r"),
        # Grande: GPL code, art CC BY-NC-ND -> regenerated (flat dir).
        (f"{tp}/GrandeModular/res", f"{root}/grande-res", "panel"),
        # Bidoo: GPL code, art CC BY-NC-ND -> regenerated.
        (f"{tp}/Bidoo/res", f"{root}/bidoo-res", "panel-r"),
        # Befaco: GPL code; res/panels is (c) Befaco "with permission" (a
        # grant to VCV only) and res/components is VCV CC BY-NC -> both
        # regenerated. res/fonts (Miso free, Segment7 OFL) is copied by the
        # walk's font hook below; SpringReverbIR.f32 ships from the repo.
        (f"{tp}/Befaco/res", f"{root}/befaco-res", "panel-r"),
    ]
    def copy_free_fonts():
        """FW panels load fonts from res/fonts by filename. DejaVu and the
        OFL fonts are free and copied as-is (with their licenses); the two
        freeware-of-unknown-terms faces (01 Digit, ARABIAN KNIGHT) are
        substituted with a copy of DejaVuSansMono under the same filename
        so the code keeps finding a valid font."""
        import shutil
        src = f"{tp}/FrozenWasteland/res/fonts"
        dst = f"{root}/frozenwasteland-res/fonts"
        if not os.path.isdir(src):
            return
        os.makedirs(dst, exist_ok=True)
        free = ["DejaVuSansMono.ttf", "DejaVu-LICENSE.txt", "OFL.txt",
                "SUBWT___.ttf", "Sudo.ttf"]
        for fn in free:
            if os.path.exists(os.path.join(src, fn)):
                shutil.copy(os.path.join(src, fn), os.path.join(dst, fn))
        for fn in ["01 Digit.ttf", "ARABIAN KNIGHT.ttf"]:
            shutil.copy(os.path.join(src, "DejaVuSansMono.ttf"), os.path.join(dst, fn))
        print(f"fonts -> {dst}")

    def copy_fonts(src, dst):
        """Copy a res/fonts dir verbatim — used only for packs whose font
        files are free (OFL etc.) even when the surrounding art is not."""
        import shutil
        if not os.path.isdir(src):
            return
        shutil.copytree(src, dst, dirs_exist_ok=True)
        print(f"fonts -> {dst}")

    def copy_audible_font():
        """Segment14 (OFL 1.1) ships as-is, with its license."""
        import shutil
        src = f"{tp}/AudibleInstruments/res/hdad-segment14-1.002"
        dst = f"{root}/audible-res/hdad-segment14-1.002"
        if not os.path.isdir(src):
            return
        os.makedirs(dst, exist_ok=True)
        for fn in ["Segment14.ttf", "OFL.txt"]:
            shutil.copy(os.path.join(src, fn), os.path.join(dst, fn))
        print(f"fonts -> {dst}")

    def process_recursive(src, dst, mode):
        """Walk src, regenerating every .svg with relative paths preserved.
        Subdir names that clearly hold components (complib, components)
        force component mode; fonts dirs are skipped (copied separately
        when their faces are free)."""
        total = 0
        for cur, dirs, _files in os.walk(src):
            rel = os.path.relpath(cur, src)
            base = os.path.basename(cur).lower()
            if "font" in base:
                continue
            sub_mode = "component" if "comp" in base else "panel"
            total += process_dir(cur, os.path.join(dst, rel) if rel != "." else dst, sub_mode)
        return total

    only = sys.argv[1] if len(sys.argv) > 1 else None
    for src, dst, mode in jobs:
        if only and only not in src:
            continue
        if not os.path.isdir(src):
            print(f"skip (missing): {src}")
            continue
        if "FrozenWasteland/res" == src[-len("FrozenWasteland/res"):]:
            copy_free_fonts()
        if "AudibleInstruments/res" == src[-len("AudibleInstruments/res"):]:
            copy_audible_font()
        # OFL/free display fonts referenced by filename from panel code.
        if "ImpromptuModular/res" == src[-len("ImpromptuModular/res"):]:
            copy_fonts(f"{src}/fonts", f"{dst}/fonts")
        if "CountModula/res" == src[-len("CountModula/res"):]:
            copy_fonts(f"{src}/fonts", f"{dst}/fonts")
        if "Befaco/res" == src[-len("Befaco/res"):]:
            copy_fonts(f"{src}/fonts", f"{dst}/fonts")
        if mode == "panel-r":
            n = process_recursive(src, dst, mode)
        else:
            n = process_dir(src, dst, mode)
        print(f"{n:4d} files -> {dst}")
