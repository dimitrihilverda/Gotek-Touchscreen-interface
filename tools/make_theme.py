#!/usr/bin/env python3
"""
Parametric theme renderer for the Gotek Touchscreen.

A theme is twelve button PNGs. Rather than hand-draw them, a theme is described
by a handful of style parameters — fill, bevel, text colour, corner radius —
and every button is rendered from that description. That keeps a theme
internally consistent by construction, and it is the same model the in-browser
theme editor uses, so what you design in the web UI and what ships in the
firmware are produced the same way.

Text is drawn with the firmware's own 6x8 font, parsed straight out of
Gotek_Touchscreen.ino. Baked-in labels therefore match the fallback labels the
firmware draws when a theme is missing a button, instead of drifting from them.

Usage:
    python tools/make_theme.py --header              # regenerate default_theme.h
    python tools/make_theme.py --out DIR             # write PNGs to a folder
    python tools/make_theme.py --out DIR --preset PAPER_WHITE
    python tools/make_theme.py --list-presets
"""
from __future__ import annotations

import argparse
import io
import os
import re
import sys

from PIL import Image

REPO   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKETCH = os.path.join(REPO, "Gotek_Touchscreen", "Gotek_Touchscreen.ino")
HEADER = os.path.join(REPO, "Gotek_Touchscreen", "default_theme.h")

# ── Panels ────────────────────────────────────────────────────────────────
#
# Button geometry is a function of panel width, so a theme is not tied to one
# display. This is the reason a theme is stored as STYLE PARAMETERS rather than
# as artwork: re-rendering for a different panel is re-running this script,
# whereas hand-drawn buttons would be locked to whatever size they were drawn
# at. It is the same benefit SVG would give, without putting a rasteriser in
# the firmware — PNGdec is already there and the device never has to scale.
PANELS = {
    "JC3248":    {"gW": 480, "gH": 320},   # Guition 480x320
    "WAVESHARE": {"gW": 320, "gH": 240},   # Waveshare 2.8" 320x240
}


def buttons_for(gW: int):
    """The BTN_* set with the size the firmware actually draws each one at.

    Mirrors Gotek_Touchscreen.ino:
      main list bar : btnW = (gW - 20 - 3*8) / 4, btnH = 36
      detail bar    : 148 x 36
      compact       : 40 x 36 (SD/INFO), 44 x 36 (DAV/UP/DOWN)

    Each asset is rendered at the SMALLEST size it appears at, because
    drawThemedButton centres the artwork inside the button rect: undersized art
    is framed correctly, oversized art spills past the edge.
    """
    bar = (gW - 20 - 3 * 8) // 4          # 109 on JC3248, 69 on Waveshare
    det = min(148, gW - 20)               # detail bar can't exceed the panel
    return [
        ("BTN_ADF",    bar, 36, "ADF",    None),
        ("BTN_DSK",    bar, 36, "DSK",    None),
        ("BTN_THEME",  bar, 36, "THEME",  None),
        ("BTN_WIFI",   bar, 36, "WIFI",   None),
        ("BTN_BACK",   bar, 36, "BACK",   None),
        ("BTN_LOAD",   det, 36, "INSERT", None),
        ("BTN_UNLOAD", det, 36, "EJECT",  None),
        ("BTN_DAV",     44, 36, "DAV",    None),
        ("BTN_SD",      40, 36, "SD",     None),
        ("BTN_INFO",    40, 36, "i",      None),
        ("BTN_UP",      44, 36, "",       "up"),
        ("BTN_DOWN",    44, 36, "",       "down"),
    ]

# ── Presets ───────────────────────────────────────────────────────────────
#
# bevel: "raised" (Amiga/Workbench 3D edge), "flat" (single-colour border),
#        "none". radius rounds the corners; 0 keeps them square.
PRESETS = {
    # Workbench 2.0: grey panel, white top-left highlight, black bottom-right
    # shadow, black label. The look the device was designed around.
    "AMIGA_WB2": {
        "fill": (168, 168, 168), "light": (255, 255, 255), "dark": (0, 0, 0),
        "text": (0, 0, 0), "bevel": "raised", "bevel_w": 2, "radius": 0,
        "scale": 2, "accent": (90, 110, 160),
    },
    # Workbench 1.3: the blue/orange era.
    "AMIGA_WB13": {
        "fill": (0, 85, 170), "light": (255, 255, 255), "dark": (0, 0, 68),
        "text": (255, 255, 255), "bevel": "raised", "bevel_w": 2, "radius": 0,
        "scale": 2, "accent": (255, 136, 0),
    },
    # High-contrast light theme — easiest to read at arm's length.
    "PAPER_WHITE": {
        "fill": (238, 238, 234), "light": (255, 255, 255), "dark": (120, 120, 116),
        "text": (20, 20, 20), "bevel": "raised", "bevel_w": 2, "radius": 3,
        "scale": 2, "accent": (40, 90, 190),
    },
    # Dark, flat, modern. Sits well against the black UI background.
    "MIDNIGHT": {
        "fill": (34, 38, 48), "light": (72, 80, 98), "dark": (12, 14, 20),
        "text": (222, 228, 240), "bevel": "flat", "bevel_w": 1, "radius": 4,
        "scale": 2, "accent": (90, 170, 255),
    },
    # Green phosphor terminal.
    "PHOSPHOR": {
        "fill": (8, 24, 12), "light": (40, 200, 90), "dark": (4, 12, 6),
        "text": (80, 255, 130), "bevel": "flat", "bevel_w": 1, "radius": 0,
        "scale": 2, "accent": (80, 255, 130),
    },
}


# ── Firmware font ─────────────────────────────────────────────────────────

def load_font6x8() -> list[list[int]]:
    """Parse font6x8[95][6] out of the sketch.

    Column-major: byte i is column i, bit r is row r — matching gfx_print's
    `bits & (1 << row)`.
    """
    src = open(SKETCH, encoding="utf-8", errors="replace").read()
    start = src.index("font6x8[95][6]")
    body = src[start:src.index("};", start)]
    rows = re.findall(r"\{([^{}]*)\}", body)
    glyphs = []
    for r in rows:
        cols = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", r)]
        if len(cols) == 6:
            glyphs.append(cols)
    if len(glyphs) != 95:
        sys.exit(f"error: expected 95 glyphs in font6x8, parsed {len(glyphs)}")
    return glyphs


FONT = None  # populated in main()


def text_width(text: str, scale: int) -> int:
    """Inked width, excluding the trailing inter-character gap.

    Every glyph in the 6x8 font reserves its sixth column as spacing, so a
    naive len*6*scale over-measures by one column and centres short labels
    visibly left of centre.
    """
    if not text:
        return 0
    return (len(text) * 6 - 1) * scale


def draw_text(px, x: int, y: int, text: str, colour, scale: int) -> None:
    for ci, ch in enumerate(text):
        code = ord(ch)
        if code < 32 or code > 126:
            continue
        glyph = FONT[code - 32]
        for col in range(6):
            bits = glyph[col]
            for row in range(8):
                if not (bits & (1 << row)):
                    continue
                bx = x + (ci * 6 + col) * scale
                by = y + row * scale
                for dy in range(scale):
                    for dx in range(scale):
                        px[bx + dx, by + dy] = colour


def draw_triangle(px, w: int, h: int, colour, pointing: str) -> None:
    """Solid arrow for the UP/DOWN buttons — drawn rather than typed, because
    the 6x8 font has no arrow glyphs."""
    size = min(w, h) // 3
    cx, cy = w // 2, h // 2
    for row in range(size):
        # An up arrow is a point at the top widening downwards, so its
        # half-width grows with the row; down is the mirror. Getting this
        # backwards silently swaps the two buttons.
        half = row if pointing == "up" else size - 1 - row
        yy = cy - size // 2 + row
        for xx in range(cx - half, cx + half + 1):
            if 0 <= xx < w and 0 <= yy < h:
                px[xx, yy] = colour


# ── Button rendering ──────────────────────────────────────────────────────

def in_rounded(x: int, y: int, w: int, h: int, r: int) -> bool:
    """False for pixels outside a rounded rectangle's corner arcs."""
    if r <= 0:
        return True
    for cx, cy in ((r, r), (w - 1 - r, r), (r, h - 1 - r), (w - 1 - r, h - 1 - r)):
        if (x < r and cx == r) or (x > w - 1 - r and cx == w - 1 - r):
            if (y < r and cy == r) or (y > h - 1 - r and cy == h - 1 - r):
                if (x - cx) ** 2 + (y - cy) ** 2 > r * r:
                    return False
    return True


def render_button(style: dict, w: int, h: int, label: str, glyph: str | None) -> Image.Image:
    # RGB, not RGBA: the firmware composites onto black and the PNG decoder's
    # alpha path costs an extra mask read per scanline.
    img = Image.new("RGB", (w, h), (0, 0, 0))
    px = img.load()
    r = style.get("radius", 0)
    bw = style.get("bevel_w", 2)
    fill, light, dark = style["fill"], style["light"], style["dark"]
    bevel = style.get("bevel", "raised")

    for y in range(h):
        for x in range(w):
            if not in_rounded(x, y, w, h, r):
                continue                      # leave the corner black
            c = fill
            if bevel == "raised":
                if y < bw or x < bw:
                    c = light
                if y >= h - bw or x >= w - bw:
                    c = dark
                # Keep the highlight winning on the top-left diagonal so the
                # edge reads as lit from the top-left, the Workbench convention.
                if (y < bw and x < w - bw) or (x < bw and y < h - bw):
                    c = light
            elif bevel == "flat":
                if x < bw or y < bw or x >= w - bw or y >= h - bw:
                    c = light
            px[x, y] = c

    if glyph in ("up", "down"):
        draw_triangle(px, w, h, style["text"], glyph)
    elif label:
        scale = style.get("scale", 2)
        tw = text_width(label, scale)
        # Shrink rather than overflow when a label outgrows a narrow button.
        while tw > w - 8 and scale > 1:
            scale -= 1
            tw = text_width(label, scale)
        tx = (w - tw) // 2
        ty = (h - 8 * scale) // 2
        draw_text(px, tx, ty, label, style["text"], scale)

    return img


def render_theme(style: dict, gW: int) -> dict[str, bytes]:
    out = {}
    for name, w, h, label, glyph in buttons_for(gW):
        buf = io.BytesIO()
        render_button(style, w, h, label, glyph).save(buf, format="PNG", optimize=True)
        out[name] = buf.getvalue()
    return out


# ── Outputs ───────────────────────────────────────────────────────────────

def emit_header(theme_name: str, assets: dict[str, bytes]) -> None:
    lines = [
        "// ==========================================================================",
        "// default_theme.h — Embedded default theme PNGs",
        "// ==========================================================================",
        "// Auto-generated by tools/make_theme.py — DO NOT EDIT BY HAND.",
        "//",
        "// Written to SD by firstBootScaffold() so a blank card still boots into a",
        "// complete, styled UI. Covers every BTN_* asset the firmware draws; a theme",
        "// missing one falls back to a plain rect, which is what the old placeholder",
        "// set did for BTN_DAV, BTN_SD and BTN_WIFI because it never included them.",
        "//",
        f"// Regenerate:  python tools/make_theme.py --header --preset {theme_name}",
        "//",
        "// Button sizes follow the JC3248 panel; see tools/make_theme.py PANELS.",
        "// ==========================================================================",
        "",
        "#pragma once",
        "#include <Arduino.h>",
        "",
    ]
    for name in sorted(assets):
        var = "btn_png_" + name.lower()
        data = assets[name]
        lines.append(f"static const uint8_t {var}[] PROGMEM = {{")
        for i in range(0, len(data), 12):
            chunk = data[i:i + 12]
            lines.append("  " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
        lines[-1] = lines[-1].rstrip(",")
        lines += ["};", f"static const size_t {var}_len = sizeof({var});", ""]

    lines += [
        "// Theme file mapping: filename on SD -> embedded data",
        "struct DefaultThemeFile {",
        "  const char *filename;",
        "  const uint8_t *data;",
        "  size_t len;",
        "};",
        "",
        "static const DefaultThemeFile default_theme_files[] = {",
    ]
    for name in sorted(assets):
        var = "btn_png_" + name.lower()
        lines.append(f'  {{ "/THEMES/{theme_name}/{name}.png", {var}, {var}_len }},')
    lines += [
        "};",
        "static const int default_theme_files_count = "
        "sizeof(default_theme_files) / sizeof(default_theme_files[0]);",
        "",
    ]
    # The style parameters that produced the artwork above. The PNGs are output;
    # this is the theme. firstBootScaffold writes it to SD next to them so the
    # shipped theme can be reopened in the browser editor like a user-made one.
    st = PRESETS[theme_name]
    hexc = lambda rgb: "#%02x%02x%02x" % rgb
    style = (
        '{"preset":"%s","fill":"%s","light":"%s","dark":"%s","text":"%s",'
        '"accent":"%s","bevel":"%s","bevelW":%d,"radius":%d,"scale":%d}'
        % (theme_name, hexc(st["fill"]), hexc(st["light"]), hexc(st["dark"]),
           hexc(st["text"]), hexc(st.get("accent", st["dark"])),
           st.get("bevel", "raised"), st.get("bevel_w", 2),
           st.get("radius", 0), st.get("scale", 2))
    )
    lines += [
        "// Style parameters behind the artwork above — see handleThemeStyleGet().",
        'static const char default_theme_style[] PROGMEM =',
        '  "' + style.replace('"', '\\"') + '";',
        f'static const char default_theme_name[] PROGMEM = "{theme_name}";',
        "",
    ]

    with open(HEADER, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))


def main() -> int:
    global FONT
    ap = argparse.ArgumentParser()
    ap.add_argument("--preset", default="AMIGA_WB2")
    ap.add_argument("--header", action="store_true", help="regenerate default_theme.h")
    ap.add_argument("--out", help="write PNGs into this directory")
    ap.add_argument("--panel", default="JC3248", choices=sorted(PANELS))
    ap.add_argument("--list-presets", action="store_true")
    args = ap.parse_args()

    if args.list_presets:
        for k in PRESETS:
            print(k)
        return 0
    if args.preset not in PRESETS:
        sys.exit(f"unknown preset {args.preset!r}; try --list-presets")
    if not args.header and not args.out:
        sys.exit("nothing to do: pass --header and/or --out DIR")

    FONT = load_font6x8()
    style = PRESETS[args.preset]
    gW = PANELS[args.panel]["gW"]
    assets = render_theme(style, gW)
    total = sum(len(v) for v in assets.values())

    if args.out:
        os.makedirs(args.out, exist_ok=True)
        for name, data in assets.items():
            open(os.path.join(args.out, name + ".png"), "wb").write(data)
        print(f"wrote {len(assets)} PNGs to {args.out}")

    if args.header:
        emit_header(args.preset, assets)
        print(f"wrote {HEADER}")

    print(f"preset {args.preset} @ {args.panel} ({gW}px): {len(assets)} assets, {total} bytes total")
    return 0


if __name__ == "__main__":
    sys.exit(main())
