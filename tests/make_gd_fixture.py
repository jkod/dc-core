#!/usr/bin/env python3
"""Writes a synthetic GD-ROM image (no game data) laid out like a Redump GDI of a real disc:

    track 1  data   LBA 0       (low-density area)
    track 2  audio  LBA 750
    track 3  data   LBA 45000   IP.BIN, ISO9660 file system, BIG.BIN starts here ...
    track 4  audio  after track 3
    track 5  data   after track 4 + a 150-sector gap   ... and SMALL.BIN is stored here

    make_gd_fixture.py OUTDIR  ->  OUTDIR/disc.gdi + track files

Every sector's bytes are derived from its LBA, so a reader can be checked sector by sector.
With chdman installed, `chdman createcd -i disc.gdi -o disc.chd` gives the same disc as a CHD.
"""
import os, struct, sys

T1, T1N = 0, 300
T2, T2N = 750, 120
T3, T3N = 45000, 400
T4N = 90
T5N = 200
T4 = T3 + T3N
T5 = T4 + T4N + 150
BIG = (T3 + 100, 150 * 2048 - 77)     # BIG.BIN: 150 sectors in track 3
SMALL = (T5 + 20, 9 * 2048 + 5)       # SMALL.BIN: in track 5


def pattern(lba, n=2048):
    """bytes of sector `lba` (data) or one raw sector (audio): a simple function of the LBA"""
    return bytes(((lba * 7 + i * 13 + (i >> 8)) & 0xFF) for i in range(n))


def dirent(lba, size, is_dir, name):
    n = name.encode()
    ln = 33 + len(n) + (0 if len(n) & 1 else 1)
    b = bytearray(ln)
    b[0] = ln
    struct.pack_into("<I", b, 2, lba); struct.pack_into(">I", b, 6, lba)
    struct.pack_into("<I", b, 10, size); struct.pack_into(">I", b, 14, size)
    b[25] = 2 if is_dir else 0
    b[32] = len(n)
    b[33:33 + len(n)] = n
    return bytes(b)


def fs_sector(lba):
    """user data of a sector of the high-density area: IP.BIN, PVD, root directory, files"""
    rel = lba - T3
    if rel == 0:
        s = bytearray(2048)
        s[0:16] = b"SEGA SEGAKATANA "
        s[0x40:0x4A] = b"T-00000   "
        s[0x60:0x70] = b"1ST_READ.BIN    "
        return bytes(s)
    if rel == 16:
        s = bytearray(2048)
        s[0] = 1; s[1:6] = b"CD001"; s[6] = 1
        s[156:156 + 34] = dirent(T3 + 17, 2048, True, "\x00")
        return bytes(s)
    if rel == 18:
        s = bytearray(2048); s[0] = 255; s[1:6] = b"CD001"
        return bytes(s)
    if rel == 17:
        d = dirent(T3 + 17, 2048, True, "\x00") + dirent(T3 + 17, 2048, True, "\x01")
        d += dirent(BIG[0], BIG[1], False, "BIG.BIN;1") + dirent(SMALL[0], SMALL[1], False, "SMALL.BIN;1")
        return d + bytes(2048 - len(d))
    return pattern(lba)


def mode1_raw(data):
    """a 2352-byte MODE1 sector around 2048 bytes of user data (EDC/ECC left zero: readers skip them)"""
    return b"\x00" + b"\xFF" * 10 + b"\x00" + b"\x00\x00\x00\x01" + data + bytes(288)


def write(out):
    os.makedirs(out, exist_ok=True)
    tracks = [(1, T1, 4, T1N, "track01.bin"), (2, T2, 0, T2N, "track02.raw"), (3, T3, 4, T3N, "track03.bin"),
              (4, T4, 0, T4N, "track04.raw"), (5, T5, 4, T5N, "track05.bin")]
    for num, lba, typ, n, name in tracks:
        with open(os.path.join(out, name), "wb") as f:
            for k in range(n):
                if typ == 0:
                    f.write(pattern(lba + k, 2352))
                elif num == 1:
                    f.write(mode1_raw(pattern(lba + k)))
                else:
                    f.write(mode1_raw(fs_sector(lba + k)))
    with open(os.path.join(out, "disc.gdi"), "w") as f:
        f.write("%d\n" % len(tracks))
        for num, lba, typ, n, name in tracks:
            f.write("%d %d %d 2352 %s 0\n" % (num, lba, typ, name))


if __name__ == "__main__":
    write(sys.argv[1] if len(sys.argv) > 1 else "gd_fixture")
