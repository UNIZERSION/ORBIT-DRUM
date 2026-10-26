"""ORBIT DAW wordmark for the top bar (0.4.8): our own lettering in a wide, thin, angular techno style (chamfered
corners, blade-cut ends). Pure Python, no packages.

    python -I tools/logogen/logogen.py [--preview DIR]

Writes firmware/main/ui/logo_orbit_daw.h (an LVGL A8 image, recoloured with the theme ink) and, with --preview,
logo_orbit_daw_preview.png (x4, ink on the light and the dark top bar) into DIR.
"""
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT_H = os.path.join(ROOT, "firmware", "main", "ui", "logo_orbit_daw.h")

CAP_PX = 18      # cap height on the screen
SS = 6           # supersampling per axis (36 samples a pixel)
H = 10.0         # design units: cap height
T = 1.7          # stroke (diagonals)
TH, TV = 2.0, 1.25  # horizontal / vertical strokes: thick bars, thin stems (as in the reference style)
GAP = 2.6        # between letters
SPACE = 6.0      # word space
CB, CS = 3.0, 1.2  # big / small chamfer


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
    ds = []
    for i in range(n):
        (x0, y0), (x1, y1) = poly[i], poly[(i + 1) % n]
        ds.append(0.0 if i in keep else stroke_of(x1 - x0, y1 - y0))
    return inset(poly, ds)


def rect(x0, y0, x1, y1):
    return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]


# A glyph: (advance, [group, ...]); a group is a list of contours filled even-odd; groups are united.
def glyph_O():
    w = 15.0
    outer = [(CB, 0), (w - CS, 0), (w, CS), (w, H - CB), (w - CB, H), (CS, H), (0, H - CS), (0, CB)]
    return w, [[outer, inset_auto(outer)]]


def glyph_R():
    w = 15.0
    mid = 6.0
    bowl = [(0, 0), (w - CB, 0), (w, CB), (w, mid - CS), (w - CS, mid), (0, mid)]
    leg = [(8.6, mid - 0.01), (11.0, mid - 0.01), (w, H), (w - 2.6, H)]
    stem = [(0, 0), (TV, 0), (TV, H), (0, H - 1.4)]   # blade-cut foot
    return w, [[bowl, inset_auto(bowl)], [leg], [stem]]


def glyph_B():
    w = 15.0
    top = [(0, 0), (w - 1.4 - CB, 0), (w - 1.4, CB), (w - 1.4, 4.0 + TH), (0, 4.0 + TH)]
    low = [(0, 4.0), (w - CS, 4.0), (w, 4.0 + CS), (w, H - CB), (w - CB, H), (0, H)]
    return w, [[top, inset_auto(top)], [low, inset_auto(low)]]


def glyph_I():
    w = 1.6
    return w, [[[(0, 1.6), (w, 0), (w, H), (0, H - 0.0)]]]   # blade-cut top


def glyph_T():
    w = 15.0
    bar = [(0, 0), (w, 0), (w - 2.6, TH), (2.6, TH)]      # long blade ends
    c = w / 2
    stem = [(c - TV / 2, TH - 0.01), (c + TV / 2, TH - 0.01), (c + TV / 2, H), (c - TV / 2, H - 1.2)]
    return w, [[bar], [stem]]


def glyph_D():
    w = 15.0
    outer = [(0, 0), (w - CB, 0), (w, CB), (w, H - CB), (w - CB, H), (0, H)]
    return w, [[outer, inset_auto(outer)]]


def glyph_A():
    w = 15.0
    bar0, bar1 = 5.4, 5.4 + TH
    outer = [(0, H), (0, CB), (CB, 0), (w - CS, 0), (w, CS), (w, H)]
    top = [(0, bar0), (0, CB), (CB, 0), (w - CS, 0), (w, CS), (w, bar0)]
    counter = inset_auto(top, keep=(5,))   # the crossbar edge stays: the bar is below it
    opening = rect(TV, bar1, w - TV, H)
    return w, [[outer, counter, opening]]


def glyph_W():
    w = 16.0
    c = w / 2
    sil = [(0, 0), (TV, 1.4), (TV, H - TH), (c - TV / 2, H - TH), (c - TV / 2, 4.2), (c + TV / 2, 3.0),
           (c + TV / 2, H - TH), (w - TV, H - TH), (w - TV, 1.4), (w, 0), (w, H - CS), (w - CS, H), (CB, H),
           (0, H - CB)]
    return w, [[sil]]


GLYPHS = {"O": glyph_O, "R": glyph_R, "B": glyph_B, "I": glyph_I, "T": glyph_T, "D": glyph_D, "A": glyph_A,
          "W": glyph_W}


def layout(text):
    shapes, x = [], 0.0
    for i, ch in enumerate(text):
        if ch == " ":
            x += SPACE - GAP
            continue
        w, groups = GLYPHS[ch]()
        shapes.append([[[(px + x, py) for px, py in cont] for cont in g] for g in groups])
        x += w + GAP
    return shapes, x - GAP


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


def render(text):
    shapes, width_u = layout(text)
    s = CAP_PX / H
    pad = 1
    wpx = int(width_u * s + 0.999) + 2 * pad
    hpx = CAP_PX + 2 * pad
    groups = [g for glyph in shapes for g in glyph]
    # bounding boxes speed up the per-sample tests
    boxes = []
    for g in groups:
        xs = [p[0] for c in g for p in c]
        ys = [p[1] for c in g for p in c]
        boxes.append((min(xs), min(ys), max(xs), max(ys)))
    px = bytearray(wpx * hpx)
    for y in range(hpx):
        for x in range(wpx):
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    ux = ((x - pad) + (sx + 0.5) / SS) / s
                    uy = ((y - pad) + (sy + 0.5) / SS) / s
                    for g, (bx0, by0, bx1, by1) in zip(groups, boxes):
                        if ux < bx0 or ux > bx1 or uy < by0 or uy > by1:
                            continue
                        odd = False
                        for cont in g:
                            if inside(cont, ux, uy):
                                odd = not odd
                        if odd:
                            hits += 1
                            break
            px[y * wpx + x] = (hits * 255 + SS * SS // 2) // (SS * SS)
    return wpx, hpx, px


def write_header(w, h, data):
    lines = []
    for i in range(0, len(data), 24):
        lines.append("    " + ", ".join("0x%02x" % b for b in data[i:i + 24]) + ",")
    body = "\n".join(lines)
    src = f"""#pragma once
// ORBIT DAW wordmark (top bar), generated by tools/logogen/logogen.py: do not edit by hand.
// Our own lettering in a wide, thin, angular techno style. A8 (alpha only): the image is recoloured with the ink.
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
    write_header(w, h, a)
    print(f"logo {w} x {h} px -> {os.path.relpath(OUT_H, ROOT)}")
    if "--preview" in sys.argv:
        d = sys.argv[sys.argv.index("--preview") + 1]
        p = os.path.join(d, "logo_orbit_daw_preview.png")
        preview(p, w, h, a)
        print("preview", p)


if __name__ == "__main__":
    main()
