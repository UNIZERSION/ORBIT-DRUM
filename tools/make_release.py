"""Assembles the ORBIT DRUM release for the Tab5 launcher.

- Checks the firmware image (ESP32-P4 chip id, chip revision range covering v1.3, app
  descriptor, image checksum, appended SHA-256, size limit of the launcher's app slot).
- Writes release/KARTA_SD/ = what goes onto the microSD card:
    ORBIT/ORBIT_DRUM_<ver>.bin       the app, installed with the launcher's SD tab
    ORBIT/TEST/ORBIT_SELFTEST_<ver>.bin  optional hardware self-test (display, touch, audio, SD)
    ORBIT/KITS, SAMPLES, USER, PROJECTS  sample library folders (808/909 STYLE kits included)
- Writes release/ORBIT_DRUM_<ver>_M5Burner.bin (0.4.9): the whole flash image from address 0x0 (bootloader at
  0x2000, partition table at 0x8000, ORBIT at 0x10000 as the only app), for M5Burner or esptool write_flash 0x0.
  ORBIT then runs on its own, without a launcher.
- Writes release/manifest.json with the SHA-256 of every file.

Usage: python -I tools/make_release.py
"""
import hashlib
import json
import shutil
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / 'firmware' / 'build' / 'orbit_drum.bin'
BOOTLOADER = ROOT / 'firmware' / 'build' / 'bootloader' / 'bootloader.bin'
PTABLE = ROOT / 'firmware' / 'build' / 'partition_table' / 'partition-table.bin'
SELFTEST = ROOT / 'firmware_selftest' / 'build' / 'orbit_selftest.bin'
SD_SRC = ROOT / 'sdcard' / 'ORBIT'
OUT = ROOT / 'release'
CARD = OUT / 'KARTA_SD'
MAX_BYTES = 7_733_248          # app0 of the launcher scheme on the Tab5 (UiFlow2, 7552 KB): ORBIT is the only app
CHIP_ESP32P4 = 0x0012
APP_DESC_MAGIC = 0xABCD5432
SKIP_PREFIXES = ('__test', 'ZZ_', 'zz_', '.gitkeep')   # (.gitkeep: keeps the empty folders in git)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest().upper()


def check_image(path):
    img = path.read_bytes()
    if len(img) < 64 or img[0] != 0xE9:
        raise SystemExit(f'{path.name}: not an ESP image')
    chip_id = struct.unpack_from('<H', img, 12)[0]
    min_rev, max_rev = struct.unpack_from('<HH', img, 15)
    if chip_id != CHIP_ESP32P4:
        raise SystemExit(f'{path.name}: chip id 0x{chip_id:04X} is not ESP32-P4')
    if not (min_rev <= 103 <= max_rev):
        raise SystemExit(f'{path.name}: revision range {min_rev}..{max_rev} does not include v1.3')
    # segments + XOR checksum
    count = img[1]
    off = 24
    csum = 0xEF
    for _ in range(count):
        _addr, length = struct.unpack_from('<II', img, off)
        seg = img[off + 8: off + 8 + length]
        x = 0
        for b in seg:
            x ^= b
        csum ^= x
        off += 8 + length
    pos = off + (15 - off % 16)
    if img[pos] != csum:
        raise SystemExit(f'{path.name}: image checksum mismatch')
    if img[23] == 1:
        if hashlib.sha256(img[:pos + 1]).digest() != img[pos + 1:pos + 33]:
            raise SystemExit(f'{path.name}: appended SHA-256 mismatch')
    magic = struct.unpack_from('<I', img, 32)[0]
    if magic != APP_DESC_MAGIC:
        raise SystemExit(f'{path.name}: no app descriptor')
    version = img[48:80].split(b'\0')[0].decode('ascii', 'replace')
    project = img[80:112].split(b'\0')[0].decode('ascii', 'replace')
    built = (img[112:128].split(b'\0')[0] + b' ' + img[128:144].split(b'\0')[0]).decode('ascii', 'replace')
    return {'file': path.name, 'bytes': len(img), 'project': project, 'version': version, 'built': built,
            'chip_rev_range': f'{min_rev}..{max_rev}', 'sha256': sha256(path)}


def full_image(path):
    """The flash image from 0x0 (as esptool merge_bin writes it, the gaps 0xFF): bootloader, table, app."""
    parts = [(0x2000, BOOTLOADER), (0x8000, PTABLE), (0x10000, APP)]
    for off, p in parts:
        if not p.exists():
            raise SystemExit(f'missing {p}: build the firmware first')
    boot = BOOTLOADER.read_bytes()
    if boot[0] != 0xE9 or 0x2000 + len(boot) > 0x8000:
        raise SystemExit('bootloader.bin: not an ESP image, or too big for 0x2000..0x8000')
    table = PTABLE.read_bytes()
    if table[:2] != b'\xaa\x50':
        raise SystemExit('partition-table.bin: no partition entries')
    # the table must put an app at 0x10000 big enough for ORBIT
    app_len = APP.stat().st_size
    ok = False
    for i in range(0, len(table), 32):
        e = table[i:i + 32]
        if e[:2] != b'\xaa\x50':
            break
        off, size = struct.unpack_from('<II', e, 4)
        ok = ok or (e[2] == 0 and off == 0x10000 and size >= app_len)
    if not ok:
        raise SystemExit('partition-table.bin: no app partition at 0x10000 for orbit_drum.bin')
    img = bytearray(b'\xff' * (0x10000 + app_len))
    for off, p in parts:
        b = p.read_bytes()
        img[off:off + len(b)] = b
    path.write_bytes(bytes(img))
    return {'file': path.name, 'bytes': len(img), 'flash_offset': '0x0', 'sha256': sha256(path)}


def main():
    if not APP.exists():
        raise SystemExit('build the firmware first: tools\\idf_build.ps1 -Project firmware')
    app = check_image(APP)
    if app['bytes'] > MAX_BYTES:
        raise SystemExit(f"orbit_drum.bin is {app['bytes']} bytes, launcher limit {MAX_BYTES}")
    ver = app['version'] or '0.0.0'
    if CARD.exists():
        shutil.rmtree(CARD)
    (CARD / 'ORBIT').mkdir(parents=True)
    for sub in ('KITS', 'SAMPLES', 'USER', 'PROJECTS'):
        src = SD_SRC / sub
        dst = CARD / 'ORBIT' / sub
        if src.exists():
            shutil.copytree(src, dst, ignore=lambda d, names: [n for n in names if n.startswith(SKIP_PREFIXES)])
        else:
            dst.mkdir(parents=True)
    shutil.copy2(APP, CARD / 'ORBIT' / f'ORBIT_DRUM_{ver}.bin')
    files = {'app': app}
    if SELFTEST.exists():
        st = check_image(SELFTEST)
        (CARD / 'ORBIT' / 'TEST').mkdir()
        shutil.copy2(SELFTEST, CARD / 'ORBIT' / 'TEST' / f'ORBIT_SELFTEST_{ver}.bin')
        files['selftest'] = st
    files['full_image'] = full_image(OUT / f'ORBIT_DRUM_{ver}_M5Burner.bin')
    manifest = {'project': 'ORBIT DRUM', 'version': ver, 'images': files, 'card': {}}
    for p in sorted(CARD.rglob('*')):
        if p.is_file():
            manifest['card'][p.relative_to(CARD).as_posix()] = {'bytes': p.stat().st_size, 'sha256': sha256(p)}
    (OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding='utf-8')
    print(f"app {app['project']} {ver}: {app['bytes']} B ({app['bytes'] / 1e6:.2f} MB), built {app['built']}")
    if 'selftest' in files:
        print(f"selftest: {files['selftest']['bytes']} B")
    print(f"card files: {len(manifest['card'])} -> {CARD}")
    fi = files['full_image']
    print(f"full flash image (M5Burner, write at 0x0): {fi['file']} {fi['bytes']} B")


if __name__ == '__main__':
    sys.exit(main())
