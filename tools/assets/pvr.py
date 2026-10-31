#!/usr/bin/env python3
"""dc-core PowerVR2 texture decoder (.PVR / raw PVRT chunks) -> RGBA / PNG.

    pvr.py in.pvr out.png [--pal palette.pvp] [--all-mips]
    pvr.py scan file.bin outdir/      # find every PVRT chunk inside a container and dump PNGs

Supports ARGB1555, RGB565, ARGB4444, YUV422, 4/8-bit palettised; twiddled, VQ, small VQ,
rectangular, stride and mipmapped variants. Pure Python + zlib, no dependencies.
"""
import struct, sys, zlib, os

# pixel formats
ARGB1555, RGB565, ARGB4444, YUV422, BUMP, PAL4, PAL8 = range(7)
# data formats
TW, TW_MM, VQ, VQ_MM, PAL4_TW, PAL4_TW_MM, PAL8_TW, PAL8_TW_MM, RECT, RECT_MM, STRIDE, STRIDE_MM, \
    RECT_TW, BMP, BMP_MM, SVQ, SVQ_MM, TW_MM_ALT = range(1, 19)


def untwiddle_table(n):
    t = []
    for i in range(n):
        v = 0
        for b in range(12):
            if i & (1 << b):
                v |= 1 << (2 * b)
        t.append(v)
    return t


_TW = untwiddle_table(1024)


def tw(x, y):
    # Dreamcast twiddle: y bits in even positions, x bits in odd positions
    return _TW[y] | (_TW[x] << 1)


def px16(v, fmt):
    if fmt == ARGB1555:
        a = 255 if v & 0x8000 else 0
        return ((v >> 10) & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, (v & 31) * 255 // 31, a
    if fmt == RGB565:
        return ((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31, 255
    if fmt == ARGB4444:
        return ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, ((v >> 12) & 15) * 17
    if fmt == BUMP:
        return (v >> 8) & 255, v & 255, 255, 255
    raise ValueError("pixel format %d" % fmt)


def yuv_pair(u0, u1):
    # u0 = Y0 U ; u1 = Y1 V  (each 16 bits: low byte chroma, high byte luma)
    U, Y0 = (u0 & 0xFF) - 128, u0 >> 8
    V, Y1 = (u1 & 0xFF) - 128, u1 >> 8
    def c(y):
        r = y + 1.375 * V; g = y - 0.34375 * U - 0.6875 * V; b = y + 1.71875 * U
        return tuple(max(0, min(255, int(k))) for k in (r, g, b)) + (255,)
    return c(Y0), c(Y1)


def vq_mip_bytes(w):
    """index bytes used by the levels smaller than w (1x1 is padded to one byte)."""
    off, s = 1, 2
    while s < w:
        off += (s // 2) * (s // 2); s *= 2
    return off


def decode(buf, pal=None, level0_only=True):
    """buf starts at 'PVRT'. Returns (w, h, rgba bytes)."""
    if buf[:4] != b"PVRT":
        raise ValueError("not a PVRT chunk")
    size = struct.unpack_from("<I", buf, 4)[0]
    pixfmt, datafmt = buf[8], buf[9]
    w, h = struct.unpack_from("<HH", buf, 12)
    d = buf[16:8 + size]
    out = bytearray(w * h * 4)

    def put(x, y, c):
        o = (y * w + x) * 4; out[o:o + 4] = bytes(c)

    def pal_color(i):
        if pal and i < len(pal): return pal[i]
        g = (i * 255) // (15 if pixfmt == PAL4 else 255); return (g, g, g, 255)

    mm = datafmt in (TW_MM, VQ_MM, PAL4_TW_MM, PAL8_TW_MM, RECT_MM, STRIDE_MM, BMP_MM, SVQ_MM, TW_MM_ALT)
    # mip levels are stored smallest first; the full-size level is the last w*h*bpp bytes
    lvl0_bytes = w * h // 2 if pixfmt == PAL4 else w * h if pixfmt == PAL8 else w * h * 2
    base = max(0, len(d) - lvl0_bytes) if mm else 0

    if datafmt in (VQ, VQ_MM, SVQ, SVQ_MM):
        lvl0 = (w // 2) * (h // 2)
        mipb = vq_mip_bytes(w) if mm else 0
        if datafmt in (SVQ, SVQ_MM):
            # small VQ: codebook size follows from the chunk length
            ncode = max(1, min(256, (len(d) - lvl0 - mipb) // 8))
        else:
            ncode = 256
        cb = [struct.unpack_from("<4H", d, i * 8) for i in range(ncode)]
        idx = d[ncode * 8:]
        base = len(idx) - lvl0 if mm else 0     # largest level is stored last
        for y in range(0, h, 2):
            for x in range(0, w, 2):
                k = idx[base + tw(x // 2, y // 2)]
                e = cb[k]  # order: (0,0) (0,1) (1,0) (1,1) in twiddle order
                if pixfmt == YUV422:
                    a, b = yuv_pair(e[0], e[2]); c_, d_ = yuv_pair(e[1], e[3])
                    put(x, y, a); put(x + 1, y, b); put(x, y + 1, c_); put(x + 1, y + 1, d_)
                else:
                    put(x, y, px16(e[0], pixfmt)); put(x, y + 1, px16(e[1], pixfmt))
                    put(x + 1, y, px16(e[2], pixfmt)); put(x + 1, y + 1, px16(e[3], pixfmt))
        return w, h, bytes(out)

    if pixfmt in (PAL4, PAL8) or datafmt in (PAL4_TW, PAL4_TW_MM, PAL8_TW, PAL8_TW_MM):
        for y in range(h):
            for x in range(w):
                t = tw(x, y)
                if pixfmt == PAL4:
                    b = d[base + t // 2]; i = (b >> 4) if t & 1 else (b & 15)
                else:
                    i = d[base + t]
                put(x, y, pal_color(i))
        return w, h, bytes(out)

    twiddled = datafmt in (TW, TW_MM, RECT_TW, TW_MM_ALT)
    if twiddled:
        m = min(w, h)
        for y in range(h):
            for x in range(w):
                # rectangular twiddled: square blocks of min(w,h) laid out linearly
                blk = (x // m) + (y // m)
                t = blk * m * m + tw(x % m, y % m)
                v = struct.unpack_from("<H", d, base + t * 2)[0]
                if pixfmt == YUV422:
                    put(x, y, (v >> 8,) * 3 + (255,))
                else:
                    put(x, y, px16(v, pixfmt))
        return w, h, bytes(out)

    # linear (rect / stride / bmp)
    for y in range(h):
        for x in range(0, w, 2 if pixfmt == YUV422 else 1):
            o = base + (y * w + x) * 2
            if pixfmt == YUV422:
                a, b = yuv_pair(*struct.unpack_from("<HH", d, o)); put(x, y, a); put(x + 1, y, b)
            else:
                put(x, y, px16(struct.unpack_from("<H", d, o)[0], pixfmt))
    return w, h, bytes(out)


def write_png(path, w, h, rgba):
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    def chunk(t, b): return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def read_pvp(path):
    b = open(path, "rb").read()
    p = b.find(b"PVPL")
    if p < 0: return None
    fmt = b[p + 8]; n = struct.unpack_from("<H", b, p + 14)[0]
    return [px16(struct.unpack_from("<H", b, p + 16 + i * 2)[0], fmt if fmt < 3 else ARGB1555) for i in range(n)]


def main():
    a = sys.argv[1:]
    if len(a) >= 3 and a[0] == "scan":
        data = open(a[1], "rb").read(); os.makedirs(a[2], exist_ok=True)
        p, n = 0, 0
        while True:
            p = data.find(b"PVRT", p)
            if p < 0: break
            try:
                w, h, rgba = decode(data[p:])
                write_png(os.path.join(a[2], "%08X_%dx%d.png" % (p, w, h)), w, h, rgba); n += 1
            except Exception as e:
                print("skip @%X: %s" % (p, e))
            p += 4
        print("%d textures" % n); return 0
    if len(a) < 2:
        print(__doc__); return 2
    data = open(a[0], "rb").read()
    pal = read_pvp(a[a.index("--pal") + 1]) if "--pal" in a else None
    p = data.find(b"PVRT")
    w, h, rgba = decode(data[p:], pal)
    write_png(a[1], w, h, rgba)
    print("%dx%d pixfmt=%d datafmt=%d" % (w, h, data[p + 8], data[p + 9]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
