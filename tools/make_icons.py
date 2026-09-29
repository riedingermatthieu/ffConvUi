#!/usr/bin/env python3
"""Generate the ffconv icon files from assets/icons/ffconv.svg
(and ffconv-small.svg for the sizes up to 24 px).

    python3 tools/make_icons.py [--preview build/icon_preview.png]

Writes (run from the project root, in the MSYS2 MINGW64 shell):
  assets/icons/png/ffconv-<size>.png   16 20 24 32 40 48 64 128 256
  assets/icons/ffconv.ico              every size above, PNG-compressed entries

Needs rsvg-convert (MSYS2: mingw-w64-x86_64-librsvg). Everything else is the
Python standard library. The generated files are committed, so building the
application does not need this script.
"""
import argparse
import os
import struct
import subprocess
import sys
import zlib

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
SVG = "assets/icons/ffconv.svg"
SVG_SMALL = "assets/icons/ffconv-small.svg"   # used up to SMALL_MAX px
SMALL_MAX = 24
PNG_DIR = "assets/icons/png"
ICO = "assets/icons/ffconv.ico"


def render(size):
    out = os.path.join(PNG_DIR, "ffconv-%d.png" % size)
    src = SVG_SMALL if size <= SMALL_MAX else SVG
    subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), src, "-o", out], check=True)
    return out


def write_ico(pngs):
    """ICO with PNG-compressed images (supported since Windows Vista)."""
    header = struct.pack("<HHH", 0, 1, len(pngs))
    entries, blobs = b"", b""
    offset = 6 + 16 * len(pngs)
    for size, path in pngs:
        data = open(path, "rb").read()
        dim = 0 if size >= 256 else size          # 0 means 256
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        blobs += data
        offset += len(data)
    with open(ICO, "wb") as f:
        f.write(header + entries + blobs)


# --- preview sheet (optional): the icon at several sizes, small ones also
# --- enlarged, on light and dark backgrounds ----------------------------------

def read_png(path):
    d = open(path, "rb").read()
    i, w, h, idat = 8, 0, 0, b""
    while i < len(d):
        n = struct.unpack(">I", d[i:i + 4])[0]
        t, c = d[i + 4:i + 8], d[i + 8:i + 8 + n]
        if t == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", c[:10])
            assert depth == 8 and ctype == 6, "expected 8-bit RGBA"
        elif t == b"IDAT":
            idat += c
        i += 12 + n
    raw, stride = zlib.decompress(idat), w * 4
    rows, prev, o = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[o]
        o += 1
        line = bytearray(raw[o:o + stride])
        o += stride
        for x in range(stride):
            a = line[x - 4] if x >= 4 else 0
            b = prev[x]
            c = prev[x - 4] if x >= 4 else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, rows


def write_rgb_png(path, w, h, rows):
    raw = b"".join(b"\0" + bytes(r) for r in rows)

    def chunk(t, c):
        return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def preview(path, pngs):
    by_size = dict(pngs)
    items = [(by_size[256], 1), (by_size[64], 1), (by_size[32], 1), (by_size[16], 1),
             (by_size[32], 4), (by_size[16], 8)]
    loaded = [(read_png(p), s) for p, s in items]
    width = 24 + sum(w * s + 24 for (w, h, _), s in loaded)
    band = 48 + max(h * s for (w, h, _), s in loaded)
    out = []
    for bg in ((245, 245, 245), (36, 36, 40)):
        canvas = [bytearray(bg * width) for _ in range(band)]
        x0 = 24
        for (w, h, px), s in loaded:
            for y in range(h * s):
                src, dst = px[y // s], canvas[24 + y]
                for x in range(w * s):
                    r, g, b, a = src[(x // s) * 4:(x // s) * 4 + 4]
                    o = (x0 + x) * 3
                    for k, v in enumerate((r, g, b)):
                        dst[o + k] = (v * a + dst[o + k] * (255 - a)) // 255
            x0 += w * s + 24
        out += canvas
    write_rgb_png(path, width, len(out), out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--preview", help="also write a preview sheet to this PNG")
    args = ap.parse_args()

    os.makedirs(PNG_DIR, exist_ok=True)
    pngs = [(s, render(s)) for s in SIZES]
    write_ico(pngs)
    print("wrote %d PNGs and %s" % (len(pngs), ICO))
    if args.preview:
        preview(args.preview, pngs)
        print("wrote", args.preview)
    return 0


if __name__ == "__main__":
    sys.exit(main())
