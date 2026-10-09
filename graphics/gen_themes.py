#!/usr/bin/env python3
"""Generate the alternate-theme graphics for the bundled base modules.

RackDroid's touch UI already has selectable color themes (AppTheme.kt). This
script produces matching recolored SVGs for the RACK itself: the rail (which
also paints the dark rack background), the ComponentLibrary (knobs/ports/
accents), Core panels, Fundamental panels, and RackDroid Drums panels.

Amber is the canonical/default look and is NOT emitted here (it lives in the
committed graphics/system-res, graphics/fundamental-res, drums/res). Only the
non-amber themes are written, into graphics/themes/<theme>/, mirroring the
same res/ layout so the packaging step (app/build.gradle.kts packSystemAssets)
and the on-device apply (native/port/asset_extract.cpp) can copy a theme's
files over the canonical paths verbatim.

Reuses regen_graphics (its drawing functions read module-level color globals,
swapped per theme via set_palette) and gen_drums_panels (its PANELS table).
Third-party .rdmod packs are intentionally left untouched — they keep their
own artwork.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import regen_graphics as R
import gen_drums_panels as D

TP = os.path.normpath(os.path.join(HERE, "..", "third_party"))
THEMES = ["blue", "emerald", "violet", "cream"]  # amber = canonical, not emitted

# Upstream source dir -> per-theme output subdir (under graphics/themes/<t>/)
# -> generation mode. Mirrors the base-bundle subset of regen_graphics.jobs.
JOBS = [
    (f"{TP}/Rack/res/ComponentLibrary", "system-res/ComponentLibrary", "component"),
    (f"{TP}/Rack/res/Core", "system-res/Core", "panel"),
    (f"{TP}/Fundamental/res", "fundamental-res", "panel"),
]


def gen_drums(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    for name, texts in D.PANELS.items():
        svg = R.gen_panel(name, D.W, D.H, texts)
        fn = name.replace("-", "") + ".svg"
        with open(os.path.join(out_dir, fn), "w", encoding="utf-8") as f:
            f.write(svg)


if __name__ == "__main__":
    for theme in THEMES:
        R.set_palette(theme)
        base = os.path.join(HERE, "themes", theme)
        total = 0
        for src, sub, mode in JOBS:
            if not os.path.isdir(src):
                print(f"skip (missing): {src}")
                continue
            total += R.process_dir(src, os.path.join(base, sub), mode)
        gen_drums(os.path.join(base, "drums-res"))
        total += len(D.PANELS)
        print(f"{theme:8s}: {total:4d} files -> {base}")
    R.set_palette("amber")  # leave module globals back at canonical
    print("done")
