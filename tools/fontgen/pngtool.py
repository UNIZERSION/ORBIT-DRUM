#!/usr/bin/env python3
"""Tiny PNG crop/zoom/stack tool (Python stdlib only) used to compare host screenshots with
reference/shots crops while tuning fonts, icons and widgets.

  python pngtool.py out.png  SRC.png:x,y,w,h[@zoom]  [SRC2.png:x,y,w,h[@zoom] ...]  [--h]

Crops are stacked vertically (or horizontally with --h) on a grey background with 6 px gaps.
"""
import struct
import sys
import zlib


def read_png(path):
    with open(path, 'rb') as f:
        data = f.read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    pos, idat, w, h, ct = 8, b'', 0, 0, 0
    while pos < len(data):
        n, = struct.unpack('>I', data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if typ == b'IHDR':
            w, h, bd, ct, _, _, il = struct.unpack('>IIBBBBB', body)
            assert bd == 8 and ct in (2, 6) and il == 0, 'only 8-bit RGB/RGBA, non-interlaced'
        elif typ == b'IDAT':
            idat += body
        elif typ == b'IEND':
            break
    bpp = 3 if ct == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray(w * h * 3)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ft = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if ft == 1:
                line[x] = (line[x] + a) & 255
            elif ft == 2:
                line[x] = (line[x] + b) & 255
            elif ft == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif ft == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        prev = line
        if bpp == 3:
            out[y * w * 3:(y + 1) * w * 3] = line
        else:
            for x in range(w):
                out[(y * w + x) * 3:(y * w + x) * 3 + 3] = line[x * 4:x * 4 + 3]
    return w, h, out


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


_cache = {}


def crop(spec):
    path, rect = spec.rsplit(':', 1)
    zoom = 1
    if '@' in rect:
        rect, z = rect.split('@')
        zoom = int(z)
    x, y, cw, ch = [int(v) for v in rect.split(',')]
    if path not in _cache:
        _cache[path] = read_png(path)
    w, h, img = _cache[path]
    out = bytearray(cw * zoom * ch * zoom * 3)
    for yy in range(ch * zoom):
        sy = min(h - 1, y + yy // zoom)
        for xx in range(cw * zoom):
            sx = min(w - 1, x + xx // zoom)
            o = (yy * cw * zoom + xx) * 3
            s = (sy * w + sx) * 3
            out[o:o + 3] = img[s:s + 3]
    return cw * zoom, ch * zoom, out


def main():
    args = [a for a in sys.argv[2:] if a != '--h']
    horiz = '--h' in sys.argv
    parts = [crop(a) for a in args]
    gap = 6
    if horiz:
        W = sum(p[0] for p in parts) + gap * (len(parts) + 1)
        H = max(p[1] for p in parts) + 2 * gap
    else:
        W = max(p[0] for p in parts) + 2 * gap
        H = sum(p[1] for p in parts) + gap * (len(parts) + 1)
    canvas = bytearray([120, 124, 128] * (W * H))
    ox, oy = gap, gap
    for pw, ph, px in parts:
        for yy in range(ph):
            o = ((oy + yy) * W + ox) * 3
            canvas[o:o + pw * 3] = px[yy * pw * 3:(yy + 1) * pw * 3]
        if horiz:
            ox += pw + gap
        else:
            oy += ph + gap
    write_png(sys.argv[1], W, H, canvas)


if __name__ == '__main__':
    main()
