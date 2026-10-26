"""ORBIT DAW wordmark for the top bar: our own lettering, pure Python, no packages.

0.4.9: a slanted speed style (the look the user picked from a logo sheet): wide letters leaning forward, heavy
horizontal bars with cuts parallel to the lean, thin stems, sharp corners, a long tapering blade out of the first
letter's foot and another out of the last letter's top, thin breaks through some bars. (Earlier: a heavy blade
style in the look of the UNICAT logo; 0.4.8 a wide, thin techno style.)

    python -I tools/logogen/logogen.py [--preview DIR] [--no-header]

Writes firmware/main/ui/logo_orbit_daw.h (an LVGL A8 image, recoloured with the theme ink) and, with --preview,
logo_orbit_daw_preview.png (x4, ink on the light and the dark top bar) into DIR.
"""
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT_H = os.path.join(ROOT, "firmware", "main", "ui", "logo_orbit_daw.h")

CAP_PX = 22      # height of the letters on the screen (0.4.8: 18)
SS = 6           # supersampling per axis (36 samples a pixel)
H = 10.0         # design units: letter height (y down: 0 = top, H = baseline)
TV, TH, T = 1.5, 1.9, 1.7   # thin stems, heavy horizontal bars, diagonals (before the lean)
SLANT = 0.42     # the lean: x moves right by SLANT per unit of height
GAP = 1.6        # between letters
SPACE = 4.4      # word space


def inset(poly, d):
    """Convex polygon (clockwise on screen, y down) moved inward by d (one value, or one per edge)."""
    n = len(poly)
    ds = d if isinstance(d, (list, tuple)) else [d] * n
    lines = []
    for i in range(n):
        (x0, y0), (x1, y1) = poly[i], poly[(i + 1) % n]
        ex, ey = x1 - x0, y1 - y0
        ln = (ex * ex + ey * ey) ** 0.5
        nx, ny = -ey / ln, ex / ln          # inward normal of a clockwise (screen) polygon
        lines.append((x0 + nx * ds[i], y0 + ny * ds[i], ex, ey))
    out = []
    for i in range(n):
        ax, ay, adx, ady = lines[i - 1]
        bx, by, bdx, bdy = lines[i]
        den = adx * bdy - ady * bdx
        t = ((bx - ax) * bdy - (by - ay) * bdx) / den
        out.append((ax + adx * t, ay + ady * t))
    return out


def stroke_of(dx, dy):
    if abs(dy) <= abs(dx) * 0.2:
        return TH
    if abs(dx) <= abs(dy) * 0.2:
        return TV
    return T


def inset_auto(poly, keep=()):
    """inset() with the stroke of each edge's direction; edges listed in keep stay where they are."""
    n = len(poly)
    return inset(poly, [0.0 if i in keep else
                        stroke_of(poly[(i + 1) % n][0] - poly[i][0], poly[(i + 1) % n][1] - poly[i][1])
                        for i in range(n)])


def brk(x, y0, y1, w=0.45):
    """A thin break through a bar at x (upright before the lean, so it leans with the letters)."""
    return [(x, y0), (x + w, y0), (x + w, y1), (x, y1)]


# A glyph: (advance, groups, cuts), drawn upright (the lean comes in layout). A group is a list of contours filled
# even-odd; groups are united; cuts (convex polygons) are taken out of the whole glyph afterwards.
def glyph_O():
    w = 11.0
    outer = [(1.8, 0), (w, 0), (w, H - 1.8), (w - 1.8, H), (0, H), (0, 1.8)]
    return w, [[outer, inset_auto(outer)]], [brk(7.6, H - 2.6, H + 0.5)]


def glyph_R():
    w = 11.0
    bowl = [(0, 0), (w - 1.6, 0), (w, 1.6), (w, 4.0), (w - 1.4, 5.4), (0, 5.4)]
    stem = rect(0, 0, TV, H)
    leg = [(5.6, 5.3), (7.7, 5.3), (12.8, H), (10.0, H)]   # the leg runs out to a point
    return 12.0, [[bowl, inset_auto(bowl)], [stem], [leg]], []


def glyph_B():
    w = 11.0
    outer = [(0, 0), (9.6, 0), (10.6, 1.0), (10.6, 3.8), (9.6, 4.9), (w, 6.3), (w, 9.0), (10.0, H), (0, H)]
    top = rect(TV, TH, 9.1, 4.1)
    low = rect(TV, 5.8, 9.5, H - TH)
    return w, [[outer, top, low]], [brk(4.0, -0.5, TH + 0.01)]


def glyph_I():
    return TV, [[[(0, 1.2), (TV, 0), (TV, H - 1.2), (0, H)]]], []   # ends cut on the slant


def glyph_T():
    w = 12.0
    bar = [(-1.6, 0), (w, 0), (w - 0.8, TH), (0.4, TH)]   # the left end runs out to a point
    c = w / 2 + 0.6
    stem = rect(c - TV / 2, TH + 0.55, c + TV / 2, H)     # a break under the bar
    return w, [[bar], [stem]], []


def glyph_D():
    w = 11.0
    outer = [(0, 0), (w - 2.0, 0), (w, 2.0), (w, H - 2.0), (w - 2.0, H), (0, H)]
    return w, [[outer, inset_auto(outer)]], [brk(3.6, -0.5, TH + 0.01)]


def glyph_A():
    w = 11.0
    bar0 = 5.0
    outer = [(0, H), (0, 2.0), (2.0, 0), (w, 0), (w, H)]
    top = [(0, bar0), (0, 2.0), (2.0, 0), (w, 0), (w, bar0)]
    counter = inset_auto(top, keep=(4,))   # the crossbar edge stays: the bar is below it
    opening = rect(TV, bar0 + TH, w - TV, H + 0.5)
    return w, [[outer, counter, opening]], [brk(7.4, bar0 - 0.2, bar0 + TH + 0.2)]


def glyph_W():
    w = 15.0
    c = w / 2
    sil = [(0, 0), (TV, 1.0), (TV, H - TH), (c - TV / 2, H - TH), (c - TV / 2, 3.4), (c + TV / 2, 2.4),
           (c + TV / 2, H - TH), (w - TV, H - TH), (w - TV, 1.0), (w, 0), (w, H - 1.8), (w - 1.8, H), (0, H)]
    return w, [[sil]], []


def rect(x0, y0, x1, y1):
    return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]


GLYPHS = {"O": glyph_O, "R": glyph_R, "B": glyph_B, "I": glyph_I, "T": glyph_T, "D": glyph_D, "A": glyph_A,
          "W": glyph_W}

# The blades (letter units, upright): out of the first letter's foot to the left along the baseline and a short
# one along its top; out of the last letter's top to the right and a short one along its foot.
WING_L = [[[(0.01, H - TH), (0.01, H), (-7.5, H)]], [[(-3.0, 0), (4.0, 0), (4.0, 1.1)]]]
WING_R = [[[(15.0, 0), (22.5, 0), (14.99, TH)]], [[(11.0, H - 1.1), (11.0, H), (17.5, H)]]]


def layout(text):
    shapes, x = [], 0.0
    letters = [ch for ch in text if ch != " "]
    for ch in text:
        if ch == " ":
            x += SPACE - GAP
            continue
        w, groups, cuts = GLYPHS[ch]()
        if not shapes:
            groups = groups + WING_L
        if len(shapes) == len(letters) - 1:
            groups = groups + WING_R
        place = lambda cont: [(px + x + SLANT * (H - py), py) for px, py in cont]   # noqa: E731
        shapes.append(([[place(c) for c in g] for g in groups], [place(c) for c in cuts]))
        x += w + GAP
    return shapes


def inside(cont, px, py):
    c = False
    n = len(cont)
    j = n - 1
    for i in range(n):
        xi, yi = cont[i]
        xj, yj = cont[j]
        if (yi > py) != (yj > py) and px < (xj - xi) * (py - yi) / (yj - yi) + xi:
            c = not c
        j = i
    return c


def bbox(conts):
    xs = [p[0] for c in conts for p in c]
    ys = [p[1] for c in conts for p in c]
    return min(xs), min(ys), max(xs), max(ys)


def render(text):
    shapes = layout(text)
    s = CAP_PX / H
    pad = 1
    x0, y0, x1, y1 = bbox([c for groups, _ in shapes for g in groups for c in g])
    wpx = int((x1 - x0) * s + 0.999) + 2 * pad
    hpx = int((y1 - y0) * s + 0.999) + 2 * pad
    # per glyph: its groups and cuts with bounding boxes (they speed up the per-sample tests)
    items = []
    for groups, cuts in shapes:
        items.append(([(g, bbox(g)) for g in groups], [(c, bbox([c])) for c in cuts]))
    px = bytearray(wpx * hpx)
    for y in range(hpx):
        for x in range(wpx):
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    ux = x0 + ((x - pad) + (sx + 0.5) / SS) / s
                    uy = y0 + ((y - pad) + (sy + 0.5) / SS) / s
                    ink = False
                    for groups, cuts in items:
                        for g, (bx0, by0, bx1, by1) in groups:
                            if ux < bx0 or ux > bx1 or uy < by0 or uy > by1:
                                continue
                            odd = False
                            for cont in g:
                                if inside(cont, ux, uy):
                                    odd = not odd
                            if odd:
                                ink = True
                                break
                        if ink:
                            for c, (bx0, by0, bx1, by1) in cuts:
                                if bx0 <= ux <= bx1 and by0 <= uy <= by1 and inside(c, ux, uy):
                                    ink = False
                                    break
                        if ink:
                            break
                    hits += 1 if ink else 0
            px[y * wpx + x] = (hits * 255 + SS * SS // 2) // (SS * SS)
    return wpx, hpx, px


def write_header(w, h, data):
    lines = []
    for i in range(0, len(data), 24):
        lines.append("    " + ", ".join("0x%02x" % b for b in data[i:i + 24]) + ",")
    body = "\n".join(lines)
    src = f"""#pragma once
// ORBIT DAW wordmark (top bar), generated by tools/logogen/logogen.py: do not edit by hand.
// Our own lettering in a slanted speed style. A8 (alpha only): the image is recoloured with the ink.
#include "lvgl.h"

namespace orbit::ui {{

static const uint8_t kLogoOrbitDawMap[{w * h}] = {{
{body}
}};

static const lv_image_dsc_t kLogoOrbitDaw = {{
    .header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .flags = 0, .w = {w}, .h = {h}, .stride = {w}, .reserved_2 = 0}},
    .data_size = sizeof kLogoOrbitDawMap,
    .data = kLogoOrbitDawMap,
    .reserved = nullptr,
    .reserved_2 = nullptr,
}};

}}  // namespace orbit::ui
"""
    with open(OUT_H, "w", encoding="utf-8", newline="\n") as f:
        f.write(src)


def write_png(path, w, h, rgb):
    def chunk(tag, d):
        return struct.pack(">I", len(d)) + tag + d + struct.pack(">I", zlib.crc32(tag + d) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def preview(path, w, h, a, zoom=4):
    themes = [((0xE9, 0xEB, 0xEC), (0x1F, 0x23, 0x25)), ((0x23, 0x27, 0x2C), (0xE6, 0xE8, 0xEA))]
    pw, ph = w * zoom + 16, (h * zoom + 16) * len(themes)
    rgb = bytearray(pw * ph * 3)
    for ti, (bg, ink) in enumerate(themes):
        y0 = ti * (h * zoom + 16)
        for y in range(h * zoom + 16):
            for x in range(pw):
                sx, sy = (x - 8) // zoom, (y - 8) // zoom
                al = a[sy * w + sx] / 255.0 if 0 <= sx < w and 0 <= sy < h and x >= 8 and y >= 8 else 0.0
                o = ((y0 + y) * pw + x) * 3
                for k in range(3):
                    rgb[o + k] = int(bg[k] + (ink[k] - bg[k]) * al + 0.5)
    write_png(path, pw, ph, rgb)


def main():
    w, h, a = render("ORBIT DAW")
    if "--preview" in sys.argv:
        d = sys.argv[sys.argv.index("--preview") + 1]
        p = os.path.join(d, "logo_orbit_daw_preview.png")
        preview(p, w, h, a)
        print("preview", p)
    if "--no-header" not in sys.argv:
        write_header(w, h, a)
        print(f"logo {w} x {h} px -> {os.path.relpath(OUT_H, ROOT)}")
    else:
        print(f"logo {w} x {h} px (header not written)")


if __name__ == "__main__":
    main()
