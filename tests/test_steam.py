#!/usr/bin/env python3
"""tools/steam/steam_shortcut.py against a made-up Steam folder: an existing shortcut with fields
the tool doesn't know survives byte for byte, adding is idempotent, art lands under the id the
shortcut carries, and remove takes only its own entry.     python3 tests/test_steam.py"""
import os, struct, sys, tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "steam"))
import steam_shortcut as ss  # noqa: E402

fails = 0


def check(c, what):
    global fails
    if not c:
        print("FAIL", what); fails += 1


def main():
    tmp = tempfile.mkdtemp(prefix="dccore_steam_")
    cfg = os.path.join(tmp, "Steam", "userdata", "12345", "config")
    os.makedirs(cfg)
    vdf = os.path.join(cfg, "shortcuts.vdf")
    # someone else's shortcut, with a float, a uint64 and a field we have never heard of
    other = [(ss.T_INT, b"appid", struct.pack("<I", 0x80001234)), (ss.T_STR, b"AppName", b"Other Game"),
             (ss.T_STR, b"Exe", b'"/opt/other/run"'), (ss.T_FLOAT, b"Zoom", struct.pack("<f", 1.5)),
             (ss.T_U64, b"Big", struct.pack("<Q", 2 ** 40)), (ss.T_STR, b"SomethingNew", b"keep me"),
             (ss.T_MAP, b"tags", [(ss.T_STR, b"0", b"Favorites")])]
    original = ss.vdf_dump([(ss.T_MAP, b"shortcuts", [(ss.T_MAP, b"0", other)])])
    check(original.endswith(b"\x08\x08"), "a shortcuts.vdf ends with two end markers, as Steam writes it")
    open(vdf, "wb").write(original)
    check(ss.vdf_dump(ss.vdf_load(original)) == original, "load/dump round trip")

    exe = os.path.join(tmp, "Rush 2049", "rush2049.exe")
    os.makedirs(os.path.dirname(exe)); open(exe, "wb").write(b"")
    png = os.path.join(tmp, "p.png"); open(png, "wb").write(b"\x89PNG portrait")
    hero = os.path.join(tmp, "h.jpg"); open(hero, "wb").write(b"hero")
    base = ["--steam", os.path.join(tmp, "Steam"), "--force"]
    add = ["add", "--exe", exe, "--name", "San Francisco Rush 2049", "--portrait", png, "--hero", hero,
           "--tag", "Dreamcast"] + base
    check(ss.main(add) == 0, "add")
    check(ss.main(add) == 0, "add again")

    entries = ss.load(vdf)[0][2]
    check(len(entries) == 2, "two shortcuts after adding twice (got %d)" % len(entries))
    check(entries[0][2] == other, "the other shortcut is unchanged")
    check([k for _, k, _ in entries] == [b"0", b"1"], "entries numbered 0, 1")
    e = entries[1][2]
    exe_field = '"%s"' % os.path.abspath(exe)
    sid = struct.unpack("<I", ss.get(e, "appid"))[0]
    check(sid == ss.shortcut_id(exe_field, "San Francisco Rush 2049") and sid & 0x80000000, "appid")
    check(ss.get(e, "Exe") == exe_field.encode() and ss.get(e, "StartDir") == ('"%s"' % os.path.dirname(os.path.abspath(exe))).encode(),
          "exe and start dir are quoted paths")
    check([v for _, _, v in ss.get(e, "tags")] == [b"Dreamcast"], "tag once")
    grid = os.path.join(cfg, "grid")
    check(open(os.path.join(grid, "%dp.png" % sid), "rb").read() == b"\x89PNG portrait", "portrait art")
    check(os.path.exists(os.path.join(grid, "%d_hero.jpg" % sid)), "hero art")

    check(ss.main(["remove", "--exe", exe, "--name", "San Francisco Rush 2049"] + base) == 0, "remove")
    entries = ss.load(vdf)[0][2]
    check(len(entries) == 1 and entries[0][2] == other, "only the other shortcut left")
    check(open(vdf, "rb").read() == original, "file back to the original bytes")

    print("steam: %s" % ("FAILED" if fails else "all ok"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
