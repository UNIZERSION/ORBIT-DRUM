"""Lossless packing of the built-in kits (0.4.9): firmware/main/core/kits_808.c / kits_909.c hold int16 PCM as
written by kitgen (and trimmed for the flash copy); this rewrites them as "KPK1" streams that the firmware
unpacks into PSRAM on first use (core/kits.cpp builtinPcm). Bit-exact: every stream is decoded again here and
compared before a file is written. Pure Python, no packages.

    python -I tools/kitgen/kitpack.py            # pack both files in place (already packed files are skipped)
    python -I tools/kitgen/kitpack.py --stats    # sizes only, nothing written

KPK1 stream (MSB-first bit order), one per sound, `frames` samples (the table holds frames):
  blocks of 256 samples (the last one shorter); per block one header byte: order << 5 | k
    order 0..2: fixed predictor (0: 0, 1: x[n-1], 2: 2 x[n-1] - x[n-2]); the history runs on across blocks and
                starts at zero (the guard frames before the sound are zero)
    k 0..20:    Rice parameter
  per sample: e = x[n] - prediction, u = zigzag(e) = (e << 1) ^ (e >> 31); q = u >> k:
    q < 24: q one bits, a zero bit, then the k low bits of u
    else:   24 one bits, then u in 20 bits
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FILES = [os.path.join(ROOT, "firmware", "main", "core", f) for f in ("kits_808.c", "kits_909.c")]
BLOCK = 256
ESC_Q = 24
ESC_BITS = 20
GUARD = 4


def zig(e):
    return (e << 1) ^ (e >> 31) if e < 0 else e << 1


def unzig(u):
    return (u >> 1) ^ -(u & 1)


class BitWriter:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, value, bits):
        self.acc = (self.acc << bits) | (value & ((1 << bits) - 1))
        self.n += bits
        while self.n >= 8:
            self.n -= 8
            self.out.append((self.acc >> self.n) & 0xFF)
        self.acc &= (1 << self.n) - 1

    def ones(self, count):
        while count >= 16:
            self.put(0xFFFF, 16)
            count -= 16
        if count:
            self.put((1 << count) - 1, count)

    def finish(self):
        if self.n:
            self.out.append((self.acc << (8 - self.n)) & 0xFF)
            self.acc = self.n = 0
        return bytes(self.out)


class BitReader:
    def __init__(self, data):
        self.d = data
        self.pos = 0  # bit position

    def bit(self):
        b = (self.d[self.pos >> 3] >> (7 - (self.pos & 7))) & 1
        self.pos += 1
        return b

    def get(self, bits):
        v = 0
        for _ in range(bits):
            v = (v << 1) | self.bit()
        return v


def predict(order, h1, h2):
    return 0 if order == 0 else (h1 if order == 1 else 2 * h1 - h2)


def rice_cost(us, k):
    c = 0
    for u in us:
        q = u >> k
        c += (q + 1 + k) if q < ESC_Q else (ESC_Q + ESC_BITS)
    return c


def encode(samples):
    w = BitWriter()
    h1 = h2 = 0
    for b0 in range(0, len(samples), BLOCK):
        blk = samples[b0:b0 + BLOCK]
        best = None
        for order in (0, 1, 2):
            a1, a2 = h1, h2
            us = []
            for x in blk:
                us.append(zig(x - predict(order, a1, a2)))
                a2, a1 = a1, x
            mean = sum(us) / len(us)
            k0 = max(0, int(mean).bit_length() - 1)
            for k in range(max(0, k0 - 2), min(20, k0 + 3) + 1):
                c = rice_cost(us, k)
                if best is None or c < best[0]:
                    best = (c, order, k, us)
        _, order, k, us = best
        w.put((order << 5) | k, 8)
        for u in us:
            q = u >> k
            if q < ESC_Q:
                w.ones(q)
                w.put(0, 1)
                if k:
                    w.put(u, k)
            else:
                w.ones(ESC_Q)
                w.put(u, ESC_BITS)
        for x in blk:
            h2, h1 = h1, x
    return w.finish()


def decode(data, frames):
    r = BitReader(data)
    out = []
    h1 = h2 = 0
    n = 0
    while n < frames:
        hdr = r.get(8)
        order, k = hdr >> 5, hdr & 31
        for _ in range(min(BLOCK, frames - n)):
            q = 0
            while q < ESC_Q and r.bit():
                q += 1
            u = r.get(ESC_BITS) if q == ESC_Q else (q << k) | (r.get(k) if k else 0)
            x = unzig(u) + predict(order, h1, h2)
            out.append(x)
            h2, h1 = h1, x
            n += 1
    return out


ARRAY_RE = re.compile(r"static const int16_t (kit(\d+)_(\d+))\[(\d+)\] = \{(.*?)\};", re.S)
TABLE_RE = re.compile(r'\{"([^"]*)", "([^"]*)", (kit\d+_\d+) \+ 4, (\d+)u, (\d+), (\d+)\}')


def pack_file(path, write):
    text = open(path, encoding="utf-8").read()
    if "KPK1" in text:
        print(os.path.basename(path), "already packed")
        return 0, 0
    arrays = {}
    for m in ARRAY_RE.finditer(text):
        vals = [int(v) for v in m.group(5).replace("\n", "").split(",") if v.strip()]
        assert len(vals) == int(m.group(4)), m.group(1)
        assert vals[:GUARD] == [0] * GUARD and vals[-GUARD:] == [0] * GUARD, m.group(1)
        arrays[m.group(1)] = vals[GUARD:-GUARD]
    rows = TABLE_RE.findall(text)
    assert len(rows) == 16, (path, len(rows))
    kit = re.search(r"orbit_kit(\d+)_sounds", text).group(1)
    raw_total = packed_total = 0
    parts = []
    entries = []
    for i, (name, long_name, arr, frames, choke, bus) in enumerate(rows):
        pcm = arrays[arr]
        assert len(pcm) == int(frames), arr
        z = encode(pcm)
        assert decode(z, len(pcm)) == pcm, f"round trip {arr}"
        raw_total += 2 * (len(pcm) + 2 * GUARD)
        packed_total += len(z)
        # 32 values a line (never split inside a number)
        wrapped = "\n".join(",".join(str(v) for v in z[j:j + 32]) + "," for j in range(0, len(z), 32))
        nm = f"kit{kit}_{i:02d}z"
        parts.append(f"/* {i + 1:02d} {long_name}: {frames} frames, {len(z)} bytes packed "
                     f"({100.0 * len(z) / (2 * len(pcm)):.1f} %) */\nstatic const uint8_t {nm}[{len(z)}] = {{\n{wrapped}\n}};\n")
        entries.append(f'    {{"{name}", "{long_name}", {nm}, {len(z)}u, {frames}u, {choke}, {bus}}},')
    head_end = text.index("/* 01 ")
    head = text[:head_end]
    head = head.replace("Each array: 4 zero guard frames, the sound, 4 zero guard frames (core/sample.h). */",
                        "Each array: a KPK1 stream (tools/kitgen/kitpack.py, lossless), unpacked into PSRAM on first\n"
                        " * use by core/kits.cpp with 4 zero guard frames on both sides (core/sample.h). */")
    head = re.sub(r"typedef struct orbit_builtin_sound_c \{.*?\} orbit_builtin_sound_c;",
                  "typedef struct orbit_builtin_sound_c {\n    const char* name;\n    const char* long_name;\n"
                  "    const uint8_t* packed;\n    uint32_t packed_bytes;\n    uint32_t frames;\n    uint8_t choke;\n"
                  "    uint8_t bus;\n} orbit_builtin_sound_c;", head, flags=re.S)
    head = re.sub(r"ORBIT_KIT_SA\(offsetof\(orbit_builtin_sound_c, pcm\).*?\n(ORBIT_KIT_SA.*?\n)*",
                  "ORBIT_KIT_SA(offsetof(orbit_builtin_sound_c, packed) == 2 * sizeof(void*), \"layout\");\n"
                  "ORBIT_KIT_SA(offsetof(orbit_builtin_sound_c, packed_bytes) == 3 * sizeof(void*), \"layout\");\n"
                  "ORBIT_KIT_SA(offsetof(orbit_builtin_sound_c, frames) == 3 * sizeof(void*) + 4, \"layout\");\n"
                  "ORBIT_KIT_SA(offsetof(orbit_builtin_sound_c, choke) == 3 * sizeof(void*) + 8, \"layout\");\n"
                  "ORBIT_KIT_SA(offsetof(orbit_builtin_sound_c, bus) == 3 * sizeof(void*) + 9, \"layout\");\n",
                  head, flags=re.S)
    head = head.replace("#include <stdint.h>\n", "#include <stdint.h>\n/* KPK1 */\n", 1)
    tail = (f"\nextern const orbit_builtin_sound_c orbit_kit{kit}_sounds[16];\n"
            f"const orbit_builtin_sound_c orbit_kit{kit}_sounds[16] = {{\n" + "\n".join(entries) + "\n};\n\n"
            "#ifdef __cplusplus\n}\n#endif\n")
    print(f"{os.path.basename(path)}: PCM {raw_total} B -> packed {packed_total} B ({100.0 * packed_total / raw_total:.1f} %)")
    if write:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(head + "\n".join(parts) + tail)
    return raw_total, packed_total


def main():
    write = "--stats" not in sys.argv
    tr = tp = 0
    for p in FILES:
        r, z = pack_file(p, write)
        tr += r
        tp += z
    if tr:
        print(f"total: {tr} -> {tp} B, saves {tr - tp} B")


if __name__ == "__main__":
    main()
