#!/usr/bin/env python3
"""Pure-Python reader for CD/GD-ROM CHD images (MAME's Compressed Hunks of Data, version 5), so the
setup steps can take a .chd without chdman or a compiled helper. Python's own zlib and lzma cover
the cdzl and cdlz codecs chdman uses for data; cdfl (FLAC) is used for audio, which the setup
never reads. cdzs (Zstandard) works when the `zstandard` module (or Python 3.14's
compression.zstd) is there.

    c = CHD("game.chd")
    c.tracks      -> [ChdTrack(num, lba, type, sector, sectors, frame), ...]  (as a .gdi lists them)
    c.frame(n)    -> 2352 bytes of sector n of the CHD (subcode left out)

The runtime reads CHDs through libchdr (runtime/src/disc.c); both lay tracks out the same way.
"""
import lzma, struct, zlib

CD_FRAME, CD_SECTOR, CD_SUB = 2448, 2352, 96
SYNC = b"\x00" + b"\xFF" * 10 + b"\x00"


class ChdError(Exception):
    pass


class ChdTrack:
    def __init__(self, num, lba, ttype, sector, sectors, frame):
        self.num, self.lba, self.type, self.sector, self.sectors, self.frame = num, lba, ttype, sector, sectors, frame


class _Bits:
    """MSB-first bit reader"""
    def __init__(self, data):
        self.data, self.pos = data, 0

    def read(self, n):
        v = 0
        for _ in range(n):
            byte = self.data[self.pos >> 3] if (self.pos >> 3) < len(self.data) else 0
            v = (v << 1) | ((byte >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v


def _huffman(bits, numcodes=16, maxbits=8):
    """the map's code-length table (RLE-coded) -> a decoder: canonical codes, as MAME assigns them"""
    nb = 5 if maxbits >= 16 else 4 if maxbits >= 8 else 3
    lens, i = [0] * numcodes, 0
    while i < numcodes:
        v = bits.read(nb)
        if v != 1:
            lens[i] = v; i += 1
        else:
            v = bits.read(nb)
            if v == 1:
                lens[i] = 1; i += 1
            else:
                rep = bits.read(nb) + 3
                for _ in range(rep):
                    if i < numcodes:
                        lens[i] = v; i += 1
    histo, start = [0] * 33, 0
    for l in lens:
        histo[l] += 1
    for cl in range(32, 0, -1):
        nxt = (start + histo[cl]) >> 1
        histo[cl] = start
        start = nxt
    table = {}
    for sym, l in enumerate(lens):
        if l:
            table[(l, histo[l])] = sym
            histo[l] += 1

    def decode():
        code = 0
        for l in range(1, maxbits + 1):
            code = (code << 1) | bits.read(1)
            if (l, code) in table:
                return table[(l, code)]
        raise ChdError("bad map code")
    return decode


def _lzma_dict(hunkbytes):
    """the dictionary size the encoder settled on (level 9, reduced to the hunk size), as libchdr does"""
    d = 1 << 26
    if d > hunkbytes:
        for i in range(11, 31):
            if hunkbytes <= (2 << i):
                return 2 << i
            if hunkbytes <= (3 << i):
                return 3 << i
    return d


def _zstd(data, size):
    try:
        from compression import zstd           # Python 3.14
        return zstd.decompress(data)[:size]
    except ImportError:
        pass
    try:
        import zstandard
        return zstandard.ZstdDecompressor().decompress(data, max_output_size=size)
    except ImportError:
        raise ChdError("this CHD uses Zstandard (cdzs): install the 'zstandard' module, or convert it with chdman")


class CHD:
    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(124)
        if h[:8] != b"MComprHD":
            raise ChdError("%s is not a CHD" % path)
        ver = struct.unpack(">I", h[12:16])[0]
        if ver != 5:
            raise ChdError("CHD version %d: only version 5 is read here (chdman copy -i old.chd -o new.chd)" % ver)
        self.codecs = [h[16 + 4 * i:20 + 4 * i] for i in range(4)]
        self.logical, self.mapoffset, self.metaoffset = struct.unpack(">QQQ", h[32:56])
        self.hunkbytes, self.unitbytes = struct.unpack(">II", h[56:64])
        if h[104:124] != bytes(20):
            raise ChdError("this CHD needs a parent CHD, which is not supported")
        self.hunks = (self.logical + self.hunkbytes - 1) // self.hunkbytes
        self.frames_per_hunk = self.hunkbytes // CD_FRAME
        self._read_map()
        self._cache = (None, None)
        self.gd = False
        self.tracks = self._tracks()

    # ---- the hunk map ----
    def _read_map(self):
        self.map = []                      # (type, offset, length) per hunk
        if self.codecs[0] == bytes(4):     # uncompressed CHD: plain offsets
            self.f.seek(self.mapoffset)
            raw = self.f.read(4 * self.hunks)
            for i in range(self.hunks):
                off = struct.unpack_from(">I", raw, 4 * i)[0] * self.hunkbytes
                self.map.append((4 if off else 99, off, self.hunkbytes))
            return
        self.f.seek(self.mapoffset)
        mh = self.f.read(16)
        mapbytes = struct.unpack(">I", mh[0:4])[0]
        firstoffs = int.from_bytes(mh[4:10], "big")
        lengthbits, selfbits, parentbits = mh[12], mh[13], mh[14]
        bits = _Bits(self.f.read(mapbytes))
        decode = _huffman(bits)
        types, rep, last = [], 0, 0
        for _ in range(self.hunks):
            if rep:
                types.append(last); rep -= 1; continue
            v = decode()
            if v == 7:                      # RLE small
                types.append(last); rep = 2 + decode()
            elif v == 8:                    # RLE large
                types.append(last); rep = 2 + 16 + (decode() << 4); rep += decode()
            else:
                types.append(v); last = v
        cur, last_self, last_parent = firstoffs, 0, 0
        for n, t in enumerate(types):
            off, ln = cur, 0
            if t <= 3:
                ln = bits.read(lengthbits); cur += ln; bits.read(16)
            elif t == 4:
                ln = self.hunkbytes; cur += ln; bits.read(16)
            elif t == 5:
                off = last_self = bits.read(selfbits)
            elif t == 6:
                off = last_parent = bits.read(parentbits)
            elif t in (9, 10):              # self, same / next as the last one
                if t == 10:
                    last_self += 1
                t, off = 5, last_self
            elif t == 11:
                t = 6; off = last_parent = (n * self.hunkbytes) // self.unitbytes
            elif t in (12, 13):
                if t == 13:
                    last_parent += self.hunkbytes // self.unitbytes
                t, off = 6, last_parent
            self.map.append((t, off, ln))

    # ---- hunks ----
    def hunk(self, n):
        if self._cache[0] == n:
            return self._cache[1]
        t, off, ln = self.map[n]
        if t == 5:
            data = self.hunk(off)
        elif t == 6:
            raise ChdError("hunk %d is in a parent CHD, which is not supported" % n)
        elif t == 99:
            data = bytes(self.hunkbytes)
        else:
            self.f.seek(off)
            src = self.f.read(ln)
            data = src if t == 4 else self._decompress(self.codecs[t], src)
        self._cache = (n, data)
        return data

    def _decompress(self, codec, src):
        size = self.hunkbytes
        if codec in (b"cdlz", b"cdzl", b"cdzs", b"cdfl"):
            if codec == b"cdfl":
                raise ChdError("a hunk compressed with FLAC (CD audio) was read: not supported here")
            frames = size // CD_FRAME
            cl_bytes = 2 if size < 65536 else 3
            ecc_bytes = (frames + 7) // 8
            hb = ecc_bytes + cl_bytes
            base_len = int.from_bytes(src[ecc_bytes:hb], "big")
            base = self._codec_base(codec, src[hb:hb + base_len], frames * CD_SECTOR)
            sub = zlib.decompress(src[hb + base_len:], -15, frames * CD_SUB) if hb + base_len < len(src) else bytes(frames * CD_SUB)
            out = bytearray(size)
            for k in range(frames):
                s = base[k * CD_SECTOR:(k + 1) * CD_SECTOR]
                if src[k >> 3] & (1 << (k & 7)):        # ECC was stripped: the sync pattern goes back
                    s = SYNC + s[12:]                    # (ECC itself is not needed for user data)
                out[k * CD_FRAME:k * CD_FRAME + CD_SECTOR] = s
                out[k * CD_FRAME + CD_SECTOR:(k + 1) * CD_FRAME] = sub[k * CD_SUB:(k + 1) * CD_SUB]
            return bytes(out)
        if codec == b"zlib":
            return zlib.decompress(src, -15, size)
        if codec == b"lzma":
            return self._lzma(src, size)
        raise ChdError("CHD codec %r is not supported" % codec)

    def _codec_base(self, codec, data, size):
        if codec == b"cdlz":
            return self._lzma(data, size)
        if codec == b"cdzs":
            return _zstd(data, size)
        return zlib.decompress(data, -15, size)

    def _lzma(self, data, size):
        filt = [{"id": lzma.FILTER_LZMA1, "dict_size": _lzma_dict(self.hunkbytes), "lc": 3, "lp": 0, "pb": 2}]
        d = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=filt)
        out = d.decompress(data, max_length=size)
        if len(out) < size:
            raise ChdError("LZMA hunk decoded to %d of %d bytes" % (len(out), size))
        return out

    def frame(self, n):
        """2352 bytes of CHD frame n"""
        h = self.hunk(n // self.frames_per_hunk)
        o = (n % self.frames_per_hunk) * CD_FRAME
        return h[o:o + CD_SECTOR]

    # ---- metadata: the track list ----
    def _metadata(self):
        out, off = [], self.metaoffset
        while off:
            self.f.seek(off)
            mh = self.f.read(16)
            tag = mh[0:4]
            ln = int.from_bytes(mh[5:8], "big")
            nxt = struct.unpack(">Q", mh[8:16])[0]
            out.append((tag, self.f.read(ln).rstrip(b"\x00").decode("latin-1")))
            off = nxt
        return out

    def _tracks(self):
        """Track LBAs as runtime/src/disc.c lays them out: a track starts where the frames before
        it end (chdman pads GD-ROM tracks out to the next one), the high-density area at 45000; a
        pregap of type V* is stored in the CHD before the track's data, others are only counted.
        Each track's frames start on a multiple of 4 in the CHD."""
        meta = self._metadata()
        for tag in (b"CHGD", b"CHT2", b"CHTR"):
            ents = [m for t, m in meta if t == tag]
            if ents:
                break
        else:
            raise ChdError("no CD/GD-ROM track list in this CHD")
        self.gd = tag == b"CHGD"
        tracks, lba, frame = [], 0, 0
        for m in ents:
            f = dict(kv.split(":", 1) for kv in m.split() if ":" in kv)
            num, frames = int(f["TRACK"]), int(f["FRAMES"])
            pad, pregap, postgap = int(f.get("PAD", 0)), int(f.get("PREGAP", 0)), int(f.get("POSTGAP", 0))
            if self.gd and num == 3 and lba < 45000:
                lba = 45000
            stored = pregap if f.get("PGTYPE", "").startswith("V") else 0
            typ = f["TYPE"]
            tracks.append(ChdTrack(num, lba + pregap, 0 if typ == "AUDIO" else 4,
                                   2048 if typ in ("MODE1", "MODE2_FORM1") else 2352,
                                   frames - stored - pad, frame + stored))
            lba += pregap - stored + frames + postgap
            frame += (frames + 3) & ~3
        return tracks
