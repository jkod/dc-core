#!/usr/bin/env python3
"""A game's VMU icon (the 32x32, 16-colour picture its saves show in the Dreamcast's file manager) as
the player's app icon. Plain Python, no Pillow: the setup step runs it on the player's machine.

    vmuicon.py ICON OUTDIR [--flip] [--frame N] [--offset N]

ICON holds a 16-colour ARGB4444 palette (32 bytes) followed by 32x32 4-bit frames (512 bytes each),
the layout of a VMS save header from offset 0x60 and of most games' icon files on the disc (Re-Volt's
VMU/REVOLT.ICN; --flip for bottom-up rows). --offset skips a header (0x60 in a .VMS save).

Writes to OUTDIR:
    icon.ico        Windows: the executable's icon (32, 64, 128 and 256 pixels, PNG entries)
    icon.jpg        Switch: the .nro's homebrew-menu icon (256x256)
    icon.png        256x256, for anything else
    icon_rgba.c     the 32x32 picture as RGBA bytes (dccore_icon_rgba), the window / Dock icon at run time
Enlarged by whole multiples without smoothing, so the pixel art stays sharp.
"""
import argparse, math, os, struct, sys, zlib


def decode(data, flip=False, frame=0):
    """32x32 list of (r, g, b, a)"""
    if len(data) < 32 + 512 * (frame + 1):
        raise ValueError("icon data too short: %d bytes" % len(data))
    pal = []
    for k in range(16):
        c = struct.unpack_from("<H", data, k * 2)[0]
        pal.append((((c >> 8) & 15) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17, ((c >> 12) & 15) * 17))
    bm = data[32 + 512 * frame:32 + 512 * (frame + 1)]
    rows = []
    for y in range(32):
        row = []
        for x in range(32):
            b = bm[y * 16 + x // 2]
            row.append(pal[(b >> 4) if x % 2 == 0 else (b & 15)])
        rows.append(row)
    if flip:
        rows.reverse()
    return rows


def scale(px, k):
    return [[p for p in row for _ in range(k)] for row in px for _ in range(k)]


def png(px):
    h, w = len(px), len(px[0])
    raw = b"".join(b"\0" + bytes(c for p in row for c in p) for row in px)
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def ico(images):
    """images: list of (size, png bytes)"""
    out = struct.pack("<HHH", 0, 1, len(images))
    off = 6 + 16 * len(images)
    body = b""
    for size, data in images:
        s = size if size < 256 else 0
        out += struct.pack("<BBBBHHII", s, s, 0, 0, 1, 32, len(data), off + len(body))
        body += data
    return out + body


# ------------------------------------------------------------------ baseline JPEG (Switch icon)
ZIGZAG = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
          28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
          54, 47, 55, 62, 63]
Q_LUM = [16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51,
         87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101,
         72, 92, 95, 98, 112, 100, 103, 99]
Q_CHR = [17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99,
         99, 99, 99] + [99] * 32
# Annex K tables: (code length counts 1..16, values)
DC_LUM = ([0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0], list(range(12)))
DC_CHR = ([0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0], list(range(12)))
AC_LUM = ([0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D], bytes.fromhex(
    "01020300041105122131410613516107227114328191a1082342b1c11552d1f02433627282090a161718191a25262728292a3435363738"
    "393a434445464748494a535455565758595a636465666768696a737475767778797a838485868788898a92939495969798999aa2a3a4a5"
    "a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9fa"))
AC_CHR = ([0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77], bytes.fromhex(
    "000102031104052131061241510761711322328108144291a1b1c109233352f0156272d10a162434e125f11718191a262728292a35363738"
    "393a434445464748494a535455565758595a636465666768696a737475767778797a82838485868788898a92939495969798999aa2a3a4"
    "a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae2e3e4e5e6e7e8e9eaf2f3f4f5f6f7f8f9fa"))


def huff_codes(table):
    counts, vals = table
    codes, code, k = {}, 0, 0
    for length in range(1, 17):
        for _ in range(counts[length - 1]):
            codes[vals[k]] = (code, length)
            code += 1
            k += 1
        code <<= 1
    return codes


class Bits:
    def __init__(self):
        self.out, self.acc, self.n = bytearray(), 0, 0

    def put(self, v, n):
        self.acc = (self.acc << n) | (v & ((1 << n) - 1))
        self.n += n
        while self.n >= 8:
            self.n -= 8
            b = (self.acc >> self.n) & 0xFF
            self.out.append(b)
            if b == 0xFF:
                self.out.append(0)

    def flush(self):
        if self.n:
            self.put((1 << (8 - self.n)) - 1, 8 - self.n)


def _cat(v):
    v = abs(v)
    n = 0
    while v:
        n += 1
        v >>= 1
    return n


def _bitsof(v, n):
    return v if v >= 0 else v + (1 << n) - 1


COS = [[math.cos((2 * x + 1) * u * math.pi / 16) * (math.sqrt(0.5) if u == 0 else 1) for x in range(8)] for u in range(8)]


def fdct(block):
    """8x8 (row-major) -> 64 coefficients, row-major"""
    if all(v == block[0] for v in block):          # flat (every block of an icon enlarged x8): DC only
        return [block[0] * 8.0] + [0.0] * 63
    tmp = [sum(COS[u][x] * block[y * 8 + x] for x in range(8)) / 2 for y in range(8) for u in range(8)]
    return [sum(COS[v][y] * tmp[y * 8 + u] for y in range(8)) / 2 for v in range(8) for u in range(8)]


def jpeg(px, quality=95):
    h, w = len(px), len(px[0])
    if w % 8 or h % 8:
        raise ValueError("size must be a multiple of 8")
    s = 5000 / quality if quality < 50 else 200 - quality * 2
    ql = [min(255, max(1, (q * s + 50) // 100)) for q in Q_LUM]
    qc = [min(255, max(1, (q * s + 50) // 100)) for q in Q_CHR]
    dcl, dcc, acl, acc = huff_codes(DC_LUM), huff_codes(DC_CHR), huff_codes(AC_LUM), huff_codes(AC_CHR)
    planes = [[], [], []]
    for row in px:
        for r, g, b, a in row:
            if a < 255:                              # against black, as the VMU shows it
                r, g, b = r * a // 255, g * a // 255, b * a // 255
            planes[0].append(0.299 * r + 0.587 * g + 0.114 * b - 128)
            planes[1].append(-0.168736 * r - 0.331264 * g + 0.5 * b)
            planes[2].append(0.5 * r - 0.418688 * g - 0.081312 * b)
    bits, prev = Bits(), [0, 0, 0]
    for by in range(0, h, 8):
        for bx in range(0, w, 8):
            for c in range(3):
                blk = [planes[c][(by + y) * w + bx + x] for y in range(8) for x in range(8)]
                co = fdct(blk)
                q = ql if c == 0 else qc
                zz = [int(round(co[ZIGZAG[i]] / q[ZIGZAG[i]])) for i in range(64)]
                dc_t, ac_t = (dcl, acl) if c == 0 else (dcc, acc)
                d = zz[0] - prev[c]
                prev[c] = zz[0]
                n = _cat(d)
                bits.put(*dc_t[n])
                if n:
                    bits.put(_bitsof(d, n), n)
                run = 0
                for i in range(1, 64):
                    v = zz[i]
                    if v == 0:
                        run += 1
                        continue
                    while run > 15:
                        bits.put(*ac_t[0xF0])
                        run -= 16
                    n = _cat(v)
                    bits.put(*ac_t[(run << 4) | n])
                    bits.put(_bitsof(v, n), n)
                    run = 0
                if run:
                    bits.put(*ac_t[0x00])
    bits.flush()

    def seg(marker, data):
        return struct.pack(">BBH", 0xFF, marker, len(data) + 2) + data
    out = b"\xFF\xD8" + seg(0xE0, b"JFIF\0\x01\x01\0\0\x01\0\x01\0\0")
    out += seg(0xDB, b"\0" + bytes(ql[ZIGZAG[i]] for i in range(64)) + b"\x01" + bytes(qc[ZIGZAG[i]] for i in range(64)))
    out += seg(0xC0, struct.pack(">BHHB", 8, h, w, 3) + b"\x01\x11\x00\x02\x11\x01\x03\x11\x01")
    for cls_id, (counts, vals) in ((0x00, DC_LUM), (0x10, AC_LUM), (0x01, DC_CHR), (0x11, AC_CHR)):
        out += seg(0xC4, bytes([cls_id]) + bytes(counts) + bytes(vals))
    out += seg(0xDA, b"\x03\x01\x00\x02\x11\x03\x11\x00\x3F\x00")
    return out + bytes(bits.out) + b"\xFF\xD9"


def write_all(px, outdir):
    os.makedirs(outdir, exist_ok=True)
    big = scale(px, 8)
    with open(os.path.join(outdir, "icon.ico"), "wb") as f:
        f.write(ico([(32 * k, png(scale(px, k))) for k in (1, 2, 4, 8)]))
    with open(os.path.join(outdir, "icon.png"), "wb") as f:
        f.write(png(big))
    with open(os.path.join(outdir, "icon.jpg"), "wb") as f:
        f.write(jpeg(big))
    rgba = bytes(c for row in px for p in row for c in p)
    with open(os.path.join(outdir, "icon_rgba.c"), "w") as f:
        f.write("/* generated by dc-core tools/icon/vmuicon.py from your disc */\n"
                "const unsigned char dccore_icon_rgba[32 * 32 * 4] = {\n")
        for i in range(0, len(rgba), 32):
            f.write("    " + ",".join(str(b) for b in rgba[i:i + 32]) + ",\n")
        f.write("};\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("icon")
    ap.add_argument("outdir")
    ap.add_argument("--flip", action="store_true", help="rows stored bottom-up")
    ap.add_argument("--frame", type=int, default=0, help="which animation frame (default 0)")
    ap.add_argument("--offset", type=lambda s: int(s, 0), default=0, help="bytes to skip first (0x60 in a .VMS save)")
    a = ap.parse_args()
    data = open(a.icon, "rb").read()[a.offset:]
    write_all(decode(data, a.flip, a.frame), a.outdir)
    print("icon: %s -> %s" % (a.icon, a.outdir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
