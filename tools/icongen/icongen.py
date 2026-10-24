#!/usr/bin/env python3
"""icongen: rasterises the prototype's ico() stroke icons (+ a few drawn to match the source
graphic) into LVGL 9 A8 image descriptors.

    python tools/icongen/icongen.py            # writes firmware/main/ui/icons/orbit_icons.c + icons.h
    python tools/icongen/icongen.py --preview tools/icongen/preview.png

SVG model of ico(): viewBox 0 0 24 24, fill none, stroke-width 1.7, round caps and joins. A stroke
with round caps/joins is exactly the union of capsules around every flattened segment, so coverage
is the distance to the nearest segment compared with half the stroke width. Anti-aliasing: 4x4
sub-samples per pixel, each with a 1-sub-sample-wide linear edge ramp (smooth, no 16-level banding).
Python 3 stdlib only.
"""
import math
import os
import re
import struct
import sys
import zlib

SIZES = [14, 16, 18, 20, 22, 26, 30, 34, 40, 48]   # 30+: maximized panels (0.3.2)
STROKE = 1.7
SS = 4

# Order == enum class orbit::ui::Icon in firmware/main/ui/widgets.h.
ICONS = [
    ('play', '<path d="m7 4 13 8-13 8z"/>'),
    ('pause', '<rect x="5" y="4" width="5" height="16" rx="1"/><rect x="14" y="4" width="5" height="16" rx="1"/>'),
    ('stop', '<rect x="5" y="5" width="14" height="14" rx="2"/>'),
    ('record', '<circle cx="12" cy="12" r="8"/>'),
    ('folder', '<path d="M3 6a2 2 0 0 1 2-2h5l2 3h7a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>'),
    ('settings', '<path d="M12 3v3m0 12v3M3 12h3m12 0h3M5.6 5.6l2.1 2.1m8.6 8.6 2.1 2.1M18.4 5.6l-2.1 2.1m-8.6 8.6-2.1 2.1"/><circle cx="12" cy="12" r="4"/>'),
    ('search', '<circle cx="10.5" cy="10.5" r="6.5"/><path d="m16 16 5 5"/>'),
    ('grid', '<rect x="3" y="3" width="7" height="7" rx="1"/><rect x="14" y="3" width="7" height="7" rx="1"/><rect x="3" y="14" width="7" height="7" rx="1"/><rect x="14" y="14" width="7" height="7" rx="1"/>'),
    ('wave', '<path d="M2 12h3l2-8 3 16 3-12 2 7 2-4h5"/>'),
    ('mixer', '<path d="M5 3v18m7-18v18m7-18v18"/><rect x="2" y="8" width="6" height="4" rx="1"/><rect x="9" y="14" width="6" height="4" rx="1"/><rect x="16" y="6" width="6" height="4" rx="1"/>'),
    ('spark', '<path d="m12 2 2.7 7.3L22 12l-7.3 2.7L12 22l-2.7-7.3L2 12l7.3-2.7z"/>'),
    ('close', '<path d="M5 5 19 19M19 5 5 19"/>'),
    ('chev', '<path d="m9 5 7 7-7 7"/>'),
    ('back', '<path d="m15 5-7 7 7 7"/>'),
    ('save', '<path d="M4 3h13l4 4v14H3V3zm3 0v7h10V3M7 21v-8h10v8"/>'),
    ('help', '<circle cx="12" cy="12" r="9"/><path d="M9.5 9a3 3 0 0 1 5.5 1.7c0 2-3 2-3 4M12 18v.1"/>'),
    ('layers', '<path d="m12 2 9 5-9 5-9-5zM3 12l9 5 9-5M3 17l9 5 9-5"/>'),
    ('key', '<rect x="3" y="5" width="18" height="14" rx="2"/><path d="M7 5v8m5-8v8m5-8v8"/>'),
    ('home', '<path d="m3 11 9-8 9 8v10H3z"/>'),
    ('minus', '<path d="M4 12h16"/>'),
    ('plus', '<path d="M4 12h16M12 4v16"/>'),
    ('headphones', '<path d="M3 13v-2a9 9 0 0 1 18 0v2m-18 0v7h5v-7H3m18 0v7h-5v-7h5"/>'),
    ('menu', '<path d="M3 6h18M3 12h18M3 18h18"/>'),
    ('check', '<path d="m4 12 5 5L20 6"/>'),
    ('reload', '<path d="M20 8V3l-4 4a9 9 0 1 0 4 5"/>'),
    ('trash', '<path d="M4 6h16M9 6V4h6v2m-9 0 1 15h10l1-15M10 10v7m4-7v7"/>'),
    ('info', '<circle cx="12" cy="12" r="9"/><path d="M12 10v7m0-10v.1"/>'),
    ('keyboard', '<rect x="2" y="6" width="20" height="12" rx="1"/><path d="M7 6v7m5-7v7m5-7v7"/>'),
    ('eq', '<path d="M2 16c3 0 3-10 6-10s3 12 6 12 3-7 8-7"/>'),
    ('volume', '<path d="M3 9v6h5l5 5V4L8 9zm13-2c3 3 3 7 0 10"/>'),
    ('dots', '<circle cx="5" cy="12" r="1"/><circle cx="12" cy="12" r="1"/><circle cx="19" cy="12" r="1"/>'),
    # Not in ico(): drawn in the same stroke language after reference/ORBIT_LIGHT_GRAFIKA_ZRODLOWA.png.
    ('pencil', '<path d="M4 20l1.1-4.4L15.6 5.1a2 2 0 0 1 2.8 0l.5.5a2 2 0 0 1 0 2.8L8.4 18.9z"/><path d="M13.6 7.1l3.3 3.3"/>'),
    ('envelope', '<path d="M4 5C5.5 13 9.5 18 20 18"/>'),
    ('slice', '<path d="M5 19 19 5"/>'),
    ('rewind', '<path d="M11 7v10l-7-5z"/><path d="M20 7v10l-7-5z"/>'),
    ('link', '<path d="M10.6 13.4a3.5 3.5 0 0 0 4.95 0l3.2-3.2a3.5 3.5 0 0 0-4.95-4.95l-1.1 1.1"/>'
             '<path d="M13.4 10.6a3.5 3.5 0 0 0-4.95 0l-3.2 3.2a3.5 3.5 0 0 0 4.95 4.95l1.1-1.1"/>'),
]
ENUM_COUNT = len(ICONS)
# Kit-internal extras after the enum entries (index ORBIT_ICON_<NAME>).
ICONS += [
    ('chev_down', '<path d="m6 9 6 6 6-6"/>'),
]

NUM = re.compile(r'[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?')
TOK = re.compile(r'[MmLlHhVvCcSsQqTtAaZz]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?')


def bez3(p0, p1, p2, p3, n=24):
    out = []
    for i in range(1, n + 1):
        t = i / n
        u = 1 - t
        out.append((u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t * t * t * p3[0],
                    u * u * u * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t * t * t * p3[1]))
    return out


def arc(p0, rx, ry, phi_deg, large, sweep, p1):
    """SVG elliptical arc (implementation notes F.6.5) flattened to points after p0."""
    if rx == 0 or ry == 0 or p0 == p1:
        return [p1]
    phi = math.radians(phi_deg)
    cp, sp = math.cos(phi), math.sin(phi)
    dx, dy = (p0[0] - p1[0]) / 2, (p0[1] - p1[1]) / 2
    x1p, y1p = cp * dx + sp * dy, -sp * dx + cp * dy
    rx, ry = abs(rx), abs(ry)
    lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry)
    if lam > 1:
        s = math.sqrt(lam)
        rx, ry = rx * s, ry * s
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    co = math.sqrt(max(0.0, num / den)) if den else 0.0
    if large == sweep:
        co = -co
    cxp, cyp = co * rx * y1p / ry, -co * ry * x1p / rx
    cx = cp * cxp - sp * cyp + (p0[0] + p1[0]) / 2
    cy = sp * cxp + cp * cyp + (p0[1] + p1[1]) / 2

    def ang(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a

    t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    n = max(4, int(abs(dt) / (math.pi / 24)) + 1)
    out = []
    for i in range(1, n + 1):
        t = t1 + dt * i / n
        x = cp * rx * math.cos(t) - sp * ry * math.sin(t) + cx
        y = sp * rx * math.cos(t) + cp * ry * math.sin(t) + cy
        out.append((x, y))
    out[-1] = p1
    return out


def parse_path(d):
    """Returns a list of (points, closed) polylines."""
    toks = TOK.findall(d)
    i = 0
    polys = []
    cur = None
    pos = (0.0, 0.0)
    start = (0.0, 0.0)
    cmd = None
    last_ctrl = None

    def num():
        nonlocal i
        v = float(toks[i])
        i += 1
        return v

    def has_num():
        return i < len(toks) and NUM.fullmatch(toks[i]) is not None

    while i < len(toks):
        if not NUM.fullmatch(toks[i]):
            cmd = toks[i]
            i += 1
        elif cmd is None:
            raise ValueError('path starts with a number: ' + d)
        rel = cmd.islower()
        c = cmd.upper()
        if c == 'Z':
            if cur is not None:
                polys.append((cur, True))
            cur = None
            pos = start
            last_ctrl = None
            continue
        if c == 'M':
            x, y = num(), num()
            if rel:
                x, y = pos[0] + x, pos[1] + y
            if cur is not None and len(cur) > 0:
                polys.append((cur, False))
            pos = start = (x, y)
            cur = [pos]
            cmd = 'l' if rel else 'L'   # following pairs are line-tos
            last_ctrl = None
            continue
        if cur is None:
            cur = [pos]
        if c == 'L':
            x, y = num(), num()
            if rel:
                x, y = pos[0] + x, pos[1] + y
            pos = (x, y)
            cur.append(pos)
            last_ctrl = None
        elif c == 'H':
            x = num()
            pos = (pos[0] + x if rel else x, pos[1])
            cur.append(pos)
            last_ctrl = None
        elif c == 'V':
            y = num()
            pos = (pos[0], pos[1] + y if rel else y)
            cur.append(pos)
            last_ctrl = None
        elif c in 'CS':
            if c == 'C':
                x1, y1 = num(), num()
                if rel:
                    x1, y1 = pos[0] + x1, pos[1] + y1
            else:
                x1, y1 = (2 * pos[0] - last_ctrl[0], 2 * pos[1] - last_ctrl[1]) if last_ctrl else pos
            x2, y2, x, y = num(), num(), num(), num()
            if rel:
                x2, y2, x, y = pos[0] + x2, pos[1] + y2, pos[0] + x, pos[1] + y
            cur.extend(bez3(pos, (x1, y1), (x2, y2), (x, y)))
            last_ctrl = (x2, y2)
            pos = (x, y)
        elif c == 'A':
            rx, ry, rot = num(), num(), num()
            large, sweep = int(num()), int(num())
            x, y = num(), num()
            if rel:
                x, y = pos[0] + x, pos[1] + y
            cur.extend(arc(pos, rx, ry, rot, large, sweep, (x, y)))
            pos = (x, y)
            last_ctrl = None
        else:
            raise ValueError('unsupported path command %s' % cmd)
    if cur is not None and len(cur) > 0:
        polys.append((cur, False))
    return polys


def rect_poly(x, y, w, h, rx):
    rx = min(rx, w / 2, h / 2)
    if rx <= 0:
        return [(x, y), (x + w, y), (x + w, y + h), (x, y + h)]
    pts = []
    for cx, cy, a0 in ((x + w - rx, y + rx, -90), (x + w - rx, y + h - rx, 0), (x + rx, y + h - rx, 90), (x + rx, y + rx, 180)):
        for k in range(9):
            a = math.radians(a0 + 90 * k / 8)
            pts.append((cx + rx * math.cos(a), cy + rx * math.sin(a)))
    return pts


def circle_poly(cx, cy, r):
    n = max(16, int(r * 8))
    return [(cx + r * math.cos(2 * math.pi * k / n), cy + r * math.sin(2 * math.pi * k / n)) for k in range(n)]


def parse_svg(svg):
    polys = []
    for tag, attrs in re.findall(r'<(\w+)([^>]*)/>', svg):
        a = dict(re.findall(r'([\w-]+)="([^"]*)"', attrs))
        if tag == 'path':
            polys += parse_path(a['d'])
        elif tag == 'rect':
            polys.append((rect_poly(float(a['x']), float(a['y']), float(a['width']), float(a['height']),
                                    float(a.get('rx', 0))), True))
        elif tag == 'circle':
            polys.append((circle_poly(float(a['cx']), float(a['cy']), float(a['r'])), True))
        else:
            raise ValueError('unsupported element ' + tag)
    return polys


def raster(polys, size):
    """A8 coverage (list of rows of 0..255) of the stroked polylines at size x size px."""
    s = size / 24.0
    hw = STROKE * s / 2.0          # half stroke in px
    n = size * SS
    sub = 1.0 / SS
    cov = [[0.0] * n for _ in range(n)]
    segs = []
    for pts, closed in polys:
        p = [(x * s, y * s) for x, y in pts]
        if closed and len(p) > 1:
            p = p + [p[0]]
        if len(p) == 1:
            p = p + p
        for k in range(len(p) - 1):
            segs.append((p[k], p[k + 1]))
    reach = hw + sub
    for (ax, ay), (bx, by) in segs:
        dx, dy = bx - ax, by - ay
        ll = dx * dx + dy * dy
        x0 = max(0, int((min(ax, bx) - reach) * SS))
        x1 = min(n - 1, int((max(ax, bx) + reach) * SS) + 1)
        y0 = max(0, int((min(ay, by) - reach) * SS))
        y1 = min(n - 1, int((max(ay, by) + reach) * SS) + 1)
        for j in range(y0, y1 + 1):
            py = (j + 0.5) * sub
            row = cov[j]
            for i in range(x0, x1 + 1):
                px = (i + 0.5) * sub
                if ll > 0:
                    t = ((px - ax) * dx + (py - ay) * dy) / ll
                    t = 0.0 if t < 0 else (1.0 if t > 1 else t)
                    qx, qy = ax + t * dx - px, ay + t * dy - py
                else:
                    qx, qy = ax - px, ay - py
                d = math.sqrt(qx * qx + qy * qy)
                c = (hw - d) / sub + 0.5
                if c > row[i]:
                    row[i] = 1.0 if c > 1 else c
    out = []
    for y in range(size):
        r = []
        for x in range(size):
            acc = 0.0
            for j in range(SS):
                row = cov[y * SS + j]
                for i in range(SS):
                    v = row[x * SS + i]
                    if v > 0:
                        acc += v
            r.append(min(255, int(round(acc / (SS * SS) * 255))))
        out.append(r)
    return out


def emit(images, out_c, out_h):
    lines = ['/* Generated by tools/icongen/icongen.py (do not edit). LVGL 9 A8 stroke icons, recolour with',
             ' * image_recolor. Sizes: %s px. Order matches enum class orbit::ui::Icon. */' % ', '.join(map(str, SIZES)),
             '#include "icons.h"', '']
    for name, size, px in images:
        sym = 'orbit_icon_%s_%d' % (name, size)
        lines.append('static const uint8_t %s_map[] = {' % sym)
        flat = [v for row in px for v in row]
        for k in range(0, len(flat), 24):
            lines.append('    ' + ','.join('0x%02x' % v for v in flat[k:k + 24]) + ',')
        lines.append('};')
        lines.append('static const lv_image_dsc_t %s = {' % sym)
        lines.append('    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .flags = 0, '
                     '.w = %d, .h = %d, .stride = %d},' % (size, size, size))
        lines.append('    .data_size = %d,' % (size * size))
        lines.append('    .data = %s_map,' % sym)
        lines.append('};')
    lines.append('')
    lines.append('const uint8_t orbit_icon_sizes[ORBIT_ICON_SIZES] = {%s};' % ', '.join(map(str, SIZES)))
    lines.append('const lv_image_dsc_t* const orbit_icons[ORBIT_ICON_TOTAL][ORBIT_ICON_SIZES] = {')
    for name, _ in ICONS:
        lines.append('    {%s},' % ', '.join('&orbit_icon_%s_%d' % (name, sz) for sz in SIZES))
    lines.append('};')
    with open(out_c, 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    h = ['#pragma once',
         '/* Stroke icons generated by tools/icongen (see orbit_icons.c). Index with (int)orbit::ui::Icon',
         ' * and the size slot whose px is nearest to the wanted size (orbit_icon_sizes). */',
         '#include "lvgl.h"', '',
         '#ifdef __cplusplus', 'extern "C" {', '#endif', '',
         '#define ORBIT_ICON_COUNT %d  /* == (int)orbit::ui::Icon::Count */' % ENUM_COUNT,
         '#define ORBIT_ICON_TOTAL %d  /* enum icons + kit extras */' % len(ICONS)] +         ['#define ORBIT_ICON_%s %d' % (n.upper(), ENUM_COUNT + k) for k, (n, _) in enumerate(ICONS[ENUM_COUNT:])] + [
         '#define ORBIT_ICON_SIZES %d' % len(SIZES), '',
         'extern const uint8_t orbit_icon_sizes[ORBIT_ICON_SIZES];',
         'extern const lv_image_dsc_t* const orbit_icons[ORBIT_ICON_TOTAL][ORBIT_ICON_SIZES];', '',
         '#ifdef __cplusplus', '}', '#endif', '']
    with open(out_h, 'w', newline='\n') as f:
        f.write('\n'.join(h))


def write_png(path, w, h, rgb):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgb[y * w * 3:(y + 1) * w * 3]

    def chunk(t, b):
        c = struct.pack('>I', len(b)) + t + b
        return c + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(bytes(raw), 6)))
        f.write(chunk(b'IEND', b''))


def preview(images, path, zoom=3):
    cell = max(SIZES) + 6
    cols = len(SIZES)
    rows = len(ICONS)
    W, H = cols * cell * zoom, rows * cell * zoom
    bg = (0xE8, 0xEA, 0xEB)
    ink = (0x25, 0x26, 0x29)
    rgb = bytearray(bg * (W * H))
    for idx, (name, size, px) in enumerate(images):
        r, c = idx // cols, idx % cols
        ox, oy = c * cell * zoom + 3 * zoom, r * cell * zoom + 3 * zoom
        for y in range(size):
            for x in range(size):
                a = px[y][x] / 255.0
                col = tuple(int(round(bg[k] * (1 - a) + ink[k] * a)) for k in range(3))
                for yy in range(zoom):
                    o = ((oy + y * zoom + yy) * W + ox + x * zoom) * 3
                    rgb[o:o + 3 * zoom] = bytes(col) * zoom
    write_png(path, W, H, rgb)


def main():
    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    out_dir = os.path.join(root, 'firmware', 'main', 'ui', 'icons')
    os.makedirs(out_dir, exist_ok=True)
    images = []
    for name, svg in ICONS:
        polys = parse_svg(svg)
        for size in SIZES:
            images.append((name, size, raster(polys, size)))
    emit(images, os.path.join(out_dir, 'orbit_icons.c'), os.path.join(out_dir, 'icons.h'))
    if '--preview' in sys.argv:
        preview(images, sys.argv[sys.argv.index('--preview') + 1])
    print('%d icons x %d sizes -> %s' % (len(ICONS), len(SIZES), out_dir))


if __name__ == '__main__':
    main()
