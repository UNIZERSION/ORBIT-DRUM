"""ORBIT DRUM device test: drives the Tab5 over its USB console (firmware/main/ui/test_console.cpp, 0.4.4+).

The port is opened without touching DTR / RTS, so a running ORBIT keeps running (the usual open toggles them and
resets the Tab5 into the launcher; --reset does that on purpose and then waits for "T>ready"). It then taps, drags and holds fingers through the real touch path, reads the state and the audio
telemetry, takes screenshots of the glass (PNG) and checks the results. Everything the Tab5 prints (the ESP log
too) goes into the log file.

    python -I tools/device_test.py COM3 [--out DIR] [--only NAME,...] [--soak SECONDS] [--reset]

Needs pyserial (the ESP-IDF Python environment has it). What it cannot check: how things sound (speaker,
headphones, Bluetooth), plugging headphones, the feel of the touch, pairing Bluetooth devices.
"""
import argparse
import base64
import os
import re
import struct
import sys
import time
import zlib

import serial

ANSI = re.compile(r"\x1b\[[0-9;]*m")

# ---- screen coordinates (design px, the 1280x720 layout; host/render_ui.cpp uses the same)
TAB_SLOT = {"sample": 0, "sequence": 1, "bass": 2, "mixer": 3, "browser": 4, "fx": 5}
VIEW_ID = {"sample": 0, "sequence": 1, "mixer": 2, "browser": 3, "fx": 4, "bass": 5}
PAD_CX = [25 + 30, 93 + 30, 162 + 30, 230 + 30]
PAD_CY = [118 + 31, 187 + 31, 255 + 31, 324 + 31]
PLAY = (1153 + 16, 14 + 16)
GEAR = (1020 + 15, 31)
WORK_MAX = (377 + 6 * 92 + 2 + 13, 30)
# BASS view
KEYS_BTN = (589 + 35, 87 + 17)
BASS_FULL = (1039 - 26 + 13, 87 + 4 + 13)
KNOB_Y = 432 + 23


def tab(name):
    return (377 + TAB_SLOT[name] * 92 + 42, 30)


def pad(i):
    return (PAD_CX[i & 3], PAD_CY[i >> 2])


def white_key(i):   # KEYS keyboard: 15 white keys over 329..1039 x 176..424, low half
    return (329 + i * 710 // 15 + 710 // 30, 176 + 210)


def knob(i):        # BASS knobs: TUNE CUTOFF RESO ENV MOD DECAY ACCENT DRIVE FX SEND LEVEL
    return (329 + i * 79 + 39, KNOB_Y)


def work_full(x, y):   # design point -> screen point with the BASS view on the whole screen (ui.cpp maxScale)
    return (int(round((x - 311) * 1280 / 746)), int(round((y - 73) * 720 / 486)))


# ---- PNG (no extra packages)
def write_png(path, w, h, rgb_rows):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(r) for r in rgb_rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def rle_to_png(data, nw, nh, path):
    px = [0] * (nw * nh)
    pos = 0
    for i in range(0, len(data) - 2, 3):
        n = data[i] + 1
        v = data[i + 1] | (data[i + 2] << 8)
        px[pos:pos + n] = [v] * n
        pos += n
    # native portrait (nw x nh) -> landscape (nh x nw): landscape (x, y) = native (y, nh - 1 - x)
    lw, lh = nh, nw
    rows = []
    for y in range(lh):
        r = bytearray(lw * 3)
        for x in range(lw):
            v = px[(nh - 1 - x) * nw + y]
            r[3 * x] = ((v >> 11) & 0x1F) * 255 // 31
            r[3 * x + 1] = ((v >> 5) & 0x3F) * 255 // 63
            r[3 * x + 2] = (v & 0x1F) * 255 // 31
        rows.append(r)
    write_png(path, lw, lh, rows)


class Device:
    def __init__(self, port, log_path, reset=False):
        self.log = open(log_path, "w", encoding="utf-8")
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.1
        # both lines low from the start (and so at close): no reset. USB-Serial/JTAG resets the chip on an RTS / DTR
        # change, which pyserial's defaults (both high at open) cause at open and again at close.
        self.ser.dtr = reset
        self.ser.rts = reset
        self.ser.open()
        if reset:
            self.ser.dtr = False
            self.ser.rts = False
        self.buf = b""
        self.t0 = time.time()
        self.pongs = 0   # pings sent to push a stuck line out (their T>pong is skipped)

    def close(self):
        self.ser.close()
        self.log.close()

    def lines(self, timeout):
        """Yields received lines until timeout (seconds) passes without the caller stopping."""
        end = time.time() + timeout
        while time.time() < end:
            chunk = self.ser.read(4096)
            if chunk:
                self.buf += chunk
                while b"\n" in self.buf:
                    raw, self.buf = self.buf.split(b"\n", 1)
                    line = ANSI.sub("", raw.decode("utf-8", "replace").rstrip("\r"))
                    if not line.startswith("S:"):
                        self.log.write("%8.2f %s\n" % (time.time() - self.t0, line))
                    if self.pongs and line.startswith("T>pong"):
                        self.pongs -= 1
                        continue
                    yield line

    def wait_ready(self, timeout=60):
        """A running ORBIT answers ping; one that is starting prints T>ready."""
        end = time.time() + timeout
        while time.time() < end:
            self.ser.write(b"ping\n")
            for line in self.lines(2.0):
                if line.startswith("T>ready") or line.startswith("T>pong"):
                    return line
        return None

    def cmd(self, text, timeout=5.0):
        self.log.write("%8.2f >>> %s\n" % (time.time() - self.t0, text))
        self.ser.write((text + "\n").encode())
        end = time.time() + timeout
        nudged = False
        while time.time() < end:
            for line in self.lines(0.6 if not nudged else max(0.1, end - time.time())):
                if line.startswith("T>"):
                    return line[2:]
            if not nudged:
                # 0.4.4: a reply of exactly n x 64 bytes (with CR LF) waits in the USB pipe for the next packet;
                # a ping's answer pushes it out (later firmware pads such lines)
                self.ser.write(b"ping\n")
                self.pongs += 1
                nudged = True
        return None

    def state(self):
        r = self.cmd("state")
        if not r or not r.startswith("state"):
            return {}
        out = {}
        for kv in r.split()[1:]:
            if "=" in kv:
                k, v = kv.split("=", 1)
                try:
                    out[k] = float(v) if "." in v else int(v)
                except ValueError:
                    out[k] = v
        return out

    def audio(self):
        r = self.cmd("audio")
        out = {}
        for kv in (r or "").split()[1:]:
            k, v = kv.split("=", 1)
            out[k] = float(v)
        return out

    def tap(self, x, y, ms=80):
        return self.cmd("tap %d %d %d" % (x, y, ms), timeout=3)

    def touch(self, *points):
        return self.cmd("touch " + " ".join("%d %d" % p for p in points))

    def release(self):
        return self.cmd("release")

    def drag(self, x1, y1, x2, y2, ms=300):
        return self.cmd("drag %d %d %d %d %d" % (x1, y1, x2, y2, ms), timeout=3 + ms / 1000)

    def shot(self, path, step=2):
        self.log.write("%8.2f >>> shot %d\n" % (time.time() - self.t0, step))
        self.ser.write(("shot %d\n" % step).encode())
        head, parts = None, {}
        for line in self.lines(60):
            if line.startswith("T>err"):
                return False
            if line.startswith("T>shot end"):
                break
            if line.startswith("T>shot"):
                head = line.split()
            elif line.startswith("S:") and head:
                _, n, b64 = line.split(":", 2)
                parts[int(n)] = b64
        if not head:
            return False
        nw, nh, nbytes, crc = int(head[1]), int(head[2]), int(head[3]), int(head[4], 16)
        data = b"".join(base64.b64decode(parts[i]) for i in sorted(parts))
        if len(data) != nbytes or (zlib.crc32(data) & 0xFFFFFFFF) != crc:
            print("  ! shot %s: damaged (%d of %d bytes)" % (os.path.basename(path), len(data), nbytes))
            return False
        rle_to_png(data, nw, nh, path)
        return True


class Report:
    def __init__(self):
        self.rows = []

    def check(self, name, ok, detail=""):
        self.rows.append((name, bool(ok), detail))
        print("  %s %s%s" % ("OK  " if ok else "FAIL", name, (" - " + detail) if detail else ""))
        return ok

    def summary(self):
        bad = [r for r in self.rows if not r[1]]
        print("\n%d checks, %d failed" % (len(self.rows), len(bad)))
        for name, _, detail in bad:
            print("  FAIL %s %s" % (name, detail))
        return not bad


# ---------------------------------------------------------------- the tests
def t_views(d, rep, out):
    print("views")
    for name in ["sequence", "bass", "mixer", "browser", "fx", "sample"]:
        d.tap(*tab(name))
        time.sleep(0.7)
        s = d.state()
        rep.check("view " + name, s.get("view") == VIEW_ID[name], "view=%s" % s.get("view"))
        d.shot(os.path.join(out, "view_%s.png" % name))


def t_pads(d, rep, out):
    print("pads")
    d.tap(*tab("sequence"))
    time.sleep(0.6)
    d.tap(*pad(0))
    a = d.audio()
    rep.check("pad 1 sounds", a.get("peak_l", 0) > 0.01 or a.get("voices", 0) > 0, "peak %.3f voices %d" % (
        a.get("peak_l", 0), a.get("voices", 0)))
    time.sleep(0.8)
    d.touch(pad(9), pad(10))   # two fingers at once (Crash + Ride: long sounds, both still ringing when read)
    time.sleep(0.05)
    a = d.audio()
    d.release()
    rep.check("two pads at once", a.get("voices", 0) >= 2, "voices %d" % a.get("voices", 0))
    time.sleep(0.5)
    d.tap(*pad(0))
    s = d.state()
    rep.check("pad 1 selected", s.get("pad") == 0, "pad=%s" % s.get("pad"))


def t_transport(d, rep, out):
    print("transport")
    s0 = d.state()
    d.tap(*PLAY)
    time.sleep(0.4)
    a1 = d.audio()
    time.sleep(1.3)   # (not a whole bar: 2 s at 120 BPM is 16 steps and lands on the same step)
    a2 = d.audio()
    s1 = d.state()
    rep.check("play", s1.get("playing") == 1, "playing=%s" % s1.get("playing"))
    rep.check("steps advance", a1.get("step", -1) != a2.get("step", -1), "step %s -> %s" % (a1.get("step"), a2.get("step")))
    rep.check("no audio dropouts (2 s)", s1.get("xruns", 0) == s0.get("xruns", 0), "xruns %s -> %s" % (
        s0.get("xruns"), s1.get("xruns")))
    d.tap(*PLAY)
    time.sleep(0.4)
    rep.check("stop", d.state().get("playing") == 0)


def t_bass_keys(d, rep, out):
    print("BASS KEYS")
    d.tap(*tab("bass"))
    time.sleep(0.7)
    if d.state().get("keys") != 1:
        d.tap(*KEYS_BTN)
        time.sleep(0.4)
    s = d.state()
    rep.check("KEYS on", s.get("keys") == 1, "keys=%s" % s.get("keys"))
    d.shot(os.path.join(out, "bass_keys.png"))
    n0 = d.audio().get("bass_notes", 0)
    e2, g2 = white_key(2), white_key(4)
    d.touch(e2)
    time.sleep(0.15)
    n1 = d.audio().get("bass_notes", 0)
    d.touch(e2, g2)   # a second finger: G2 sounds
    time.sleep(0.15)
    n2 = d.audio().get("bass_notes", 0)
    d.shot(os.path.join(out, "bass_keys_two_fingers.png"))
    d.touch(e2)       # G2 lifted: back to E2
    time.sleep(0.15)
    n3 = d.audio().get("bass_notes", 0)
    d.release()
    time.sleep(0.2)
    rep.check("key plays", n1 == n0 + 1, "%d -> %d" % (n0, n1))
    rep.check("second finger plays", n2 == n1 + 1, "%d -> %d" % (n1, n2))
    rep.check("lifting returns to the held key", n3 == n2 + 1, "%d -> %d" % (n2, n3))
    # a knob turned while a key is held (the keyboard is the touch task's exclusive zone)
    c0 = d.state().get("cutoff", 0)
    kx, ky = knob(1)
    d.touch(e2, (kx, ky))
    for i in range(1, 9):
        d.touch(e2, (kx, ky - i * 8))
        time.sleep(0.03)
    d.touch(e2)
    d.release()
    time.sleep(0.3)
    c1 = d.state().get("cutoff", 0)
    rep.check("knob turns while a key is held", c1 > c0 + 0.02, "cutoff %.3f -> %.3f" % (c0, c1))
    # put CUTOFF back (drag down the same way)
    d.drag(kx, ky, kx, ky + 64, 300)
    time.sleep(0.3)


def t_bass_full(d, rep, out):
    print("BASS full screen")
    d.tap(*tab("bass"))
    time.sleep(0.6)
    d.tap(*BASS_FULL)
    time.sleep(0.8)
    s = d.state()
    rep.check("full screen", s.get("full") == 1, "full=%s" % s.get("full"))
    d.shot(os.path.join(out, "bass_full.png"))
    n0 = d.audio().get("bass_notes", 0)
    k = work_full(*white_key(9))
    d.touch(k)
    time.sleep(0.15)
    n1 = d.audio().get("bass_notes", 0)
    d.release()
    rep.check("full-screen key plays", n1 == n0 + 1, "%d -> %d" % (n0, n1))
    d.tap(*work_full(*BASS_FULL))
    time.sleep(0.8)
    rep.check("back from full screen", d.state().get("full") == 0)
    d.tap(*KEYS_BTN)   # KEYS off again
    time.sleep(0.4)
    rep.check("KEYS off", d.state().get("keys") == 0)


def t_maximize(d, rep, out):
    print("maximize panels")
    d.tap(*tab("sequence"))
    time.sleep(0.6)
    d.tap(*WORK_MAX)
    time.sleep(0.8)
    s = d.state()
    rep.check("work area maximized", s.get("max") == 2, "max=%s" % s.get("max"))
    d.shot(os.path.join(out, "max_work.png"))
    d.tap(*WORK_MAX)
    time.sleep(0.8)
    rep.check("work area restored", d.state().get("max") == -1)


SETTINGS_TABS = [("general", 125), ("audio", 168), ("midi", 211), ("bluetooth", 254), ("display", 297),
                 ("touch", 340), ("storage", 383), ("about", 426)]   # (name, y of the tab at x 300)


def t_settings(d, rep, out):
    print("settings")
    d.tap(*GEAR)
    time.sleep(0.8)
    s = d.state()
    rep.check("settings open", s.get("modal") == 1, "modal=%s" % s.get("modal"))
    for name, y in SETTINGS_TABS:   # every tab (nothing is applied: no APPLY)
        d.tap(300, y)
        time.sleep(0.5)
        d.shot(os.path.join(out, "settings_%s.png" % name))
    d.tap(300, 125)
    time.sleep(0.3)
    d.tap(1016, 63)   # the dialog's X
    time.sleep(0.6)
    rep.check("settings closed", d.state().get("modal") == 0)


def t_help(d, rep, out):
    print("help")
    d.tap(1153 + 2 * 37 + 16, 30)   # "?"
    time.sleep(0.8)
    rep.check("help open", d.state().get("modal") == 1)
    d.shot(os.path.join(out, "help.png"))
    d.tap(1016, 64)
    time.sleep(0.6)
    rep.check("help closed", d.state().get("modal") == 0)


def t_scenes(d, rep, out):
    print("scenes")
    s0 = d.state().get("scene", 0)
    d.tap(320 + 1 * 29 + 12, 621)   # scene B
    time.sleep(0.5)
    s1 = d.state().get("scene", -1)
    d.tap(320 + 0 * 29 + 12, 621)   # scene A
    time.sleep(0.5)
    s2 = d.state().get("scene", -1)
    rep.check("scene B", s1 == 1, "scene=%s" % s1)
    rep.check("scene A again", s2 == 0, "scene=%s (start %s)" % (s2, s0))


def t_soak(d, rep, out, seconds):
    print("soak: the sequencer plays %d s" % seconds)
    s0 = d.state()
    d.tap(*PLAY)
    t_end = time.time() + seconds
    worst_heap = s0.get("heap", 0)
    while time.time() < t_end:
        time.sleep(min(10, max(0.1, t_end - time.time())))
        s = d.state()
        worst_heap = min(worst_heap, s.get("heap", worst_heap))
    d.tap(*PLAY)
    s1 = d.state()
    rep.check("soak: no dropouts", s1.get("xruns", 0) == s0.get("xruns", 0), "xruns %s -> %s" % (
        s0.get("xruns"), s1.get("xruns")))
    rep.check("soak: heap stable", worst_heap > s0.get("heap", 0) - 16384, "heap %s -> lowest %s" % (
        s0.get("heap"), worst_heap))


TESTS = [("views", t_views), ("pads", t_pads), ("transport", t_transport), ("scenes", t_scenes),
         ("bass_keys", t_bass_keys), ("bass_full", t_bass_full), ("maximize", t_maximize), ("settings", t_settings),
         ("help", t_help)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--out", default=os.path.join("host", "device_shots"))
    ap.add_argument("--only", default="")
    ap.add_argument("--soak", type=int, default=0)
    ap.add_argument("--reset", action="store_true", help="reset the Tab5 first (it starts in the launcher)")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    d = Device(args.port, os.path.join(args.out, "device_test.log"), args.reset)
    rep = Report()
    try:
        print("waiting for ORBIT (ping)...")
        ready = d.wait_ready(90 if args.reset else 20)
        if not rep.check("ORBIT ready", ready, ready or "no answer (is ORBIT running? the launcher may wait for BOOT)"):
            return 2
        time.sleep(1.5)
        s = d.state()
        print("  state: " + " ".join("%s=%s" % kv for kv in s.items()))
        d.shot(os.path.join(args.out, "start.png"))
        only = [x for x in args.only.split(",") if x]
        for name, fn in TESTS:
            if not only or name in only:
                fn(d, rep, args.out)
        if args.soak:
            t_soak(d, rep, args.out, args.soak)
        s = d.state()
        print("  state: " + " ".join("%s=%s" % kv for kv in s.items()))
    finally:
        d.close()
    return 0 if rep.summary() else 1


if __name__ == "__main__":
    sys.exit(main())
