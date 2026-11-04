#!/usr/bin/env python3
"""Add a built port to Steam as a non-Steam game, with its art, on Windows, macOS and Linux (Steam
Deck included). Steam must be closed: it rewrites shortcuts.vdf when it exits.

    steam_shortcut.py add    --exe PATH --name NAME [--args A] [--start-dir D] [--icon IMG]
                             [--portrait IMG] [--wide IMG] [--hero IMG] [--logo IMG] [--tag T ...]
    steam_shortcut.py remove --exe PATH --name NAME
    steam_shortcut.py list

    common: [--steam DIR] (Steam's folder; found by default) [--user ID] (every Steam user by default)
            [--force] (write even if Steam seems to be running)

Art (PNG or JPG) goes to userdata/<user>/config/grid/ under the shortcut's id, as Steam names it:
--portrait 600x900 library capsule, --wide 920x430 capsule, --hero 1920x620 banner, --logo
(transparent PNG over the hero). Adding the same exe + name again updates that shortcut. Every
other shortcut and field in the file is kept as it was (a .bak copy is written first).
"""
import argparse, os, shutil, struct, subprocess, sys, zlib

T_MAP, T_STR, T_INT, T_FLOAT, T_PTR, T_WSTR, T_COLOR, T_U64, T_END = 0, 1, 2, 3, 4, 5, 6, 7, 8


# ---- binary VDF (shortcuts.vdf): a tree of (type, key, value), order and types kept ----
def vdf_load(data):
    pos = 0

    def cstr():
        nonlocal pos
        e = data.index(b"\x00", pos)
        s = data[pos:e]; pos = e + 1
        return s

    def node():
        nonlocal pos
        items = []
        while True:
            t = data[pos]; pos += 1
            if t == T_END:
                return items
            key = cstr()
            if t == T_MAP:
                items.append((t, key, node()))
            elif t in (T_STR, T_WSTR):
                items.append((t, key, cstr()))
            elif t in (T_INT, T_FLOAT, T_PTR, T_COLOR):
                items.append((t, key, data[pos:pos + 4])); pos += 4
            elif t == T_U64:
                items.append((t, key, data[pos:pos + 8])); pos += 8
            else:
                raise ValueError("shortcuts.vdf: unknown field type %d at byte %d" % (t, pos - 1))
    tree = node()
    return tree


def vdf_dump(items):
    out = bytearray()
    for t, key, v in items:
        out.append(t); out += key + b"\x00"
        if t == T_MAP:
            out += vdf_dump(v)
        elif t in (T_STR, T_WSTR):
            out += v + b"\x00"
        else:
            out += v
    out.append(T_END)
    return bytes(out)


def _key(key):
    return (key.encode("utf-8") if isinstance(key, str) else key).lower()


def get(items, key, default=None):
    for t, k, v in items:
        if k.lower() == _key(key):
            return v
    return default


def put(items, t, key, value):
    for i, (tt, k, v) in enumerate(items):
        if k.lower() == _key(key):
            items[i] = (t, k, value); return
    items.append((t, key if isinstance(key, bytes) else key.encode("utf-8"), value))


def s(text):
    return text.encode("utf-8")


def i32(v):
    return struct.pack("<i", v)


# ---- Steam's folders ----
def steam_dirs():
    home = os.path.expanduser("~")
    c = []
    if sys.platform == "win32":
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
                c.append(winreg.QueryValueEx(k, "SteamPath")[0])
        except OSError:
            pass
        c += [os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Steam")]
    elif sys.platform == "darwin":
        c.append(os.path.join(home, "Library", "Application Support", "Steam"))
    else:
        c += [os.path.join(home, ".steam", "steam"), os.path.join(home, ".local", "share", "Steam"),
              os.path.join(home, ".var", "app", "com.valvesoftware.Steam", ".local", "share", "Steam")]
    seen = []
    for d in c:
        d = os.path.realpath(d)
        if os.path.isdir(os.path.join(d, "userdata")) and d not in seen:
            seen.append(d)
    return seen


def users(steam, only=None):
    ud = os.path.join(steam, "userdata")
    out = [u for u in sorted(os.listdir(ud)) if u.isdigit() and u != "0"]
    return [u for u in out if only is None or u == str(only)]


def steam_running():
    try:
        if sys.platform == "win32":
            r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq steam.exe"], capture_output=True, text=True)
            return "steam.exe" in r.stdout.lower()
        names = ["steam_osx"] if sys.platform == "darwin" else ["steam", "steamwebhelper"]
        return any(subprocess.run(["pgrep", "-x", n], capture_output=True).returncode == 0 for n in names)
    except OSError:
        return False


# ---- shortcuts ----
def shortcut_id(exe_field, name):
    """the id Steam uses for a non-Steam game: crc32 of exe + name with the top bit set (grid art
    is named by it); written to the shortcut's appid field so Steam and the art agree"""
    return (zlib.crc32(s(exe_field + name)) & 0xFFFFFFFF) | 0x80000000


def quoted(p):
    return '"%s"' % os.path.abspath(p)


def load(path):
    if not os.path.exists(path):
        return [(T_MAP, b"shortcuts", [])]
    with open(path, "rb") as f:
        data = f.read()
    tree = vdf_load(data)
    if not tree or tree[0][1].lower() != b"shortcuts":
        raise ValueError("%s does not look like Steam's shortcuts.vdf" % path)
    return tree


def save(path, tree):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(path):
        shutil.copy2(path, path + ".bak")
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(vdf_dump(tree))
    os.replace(tmp, path)


def find(entries, exe_field, name):
    for i, (t, k, e) in enumerate(entries):
        if t == T_MAP and (get(e, "Exe", b"") or b"").decode("utf-8", "replace") == exe_field \
                and (get(e, "AppName", b"") or b"").decode("utf-8", "replace") == name:
            return i
    return None


def renumber(entries):
    return [(t, str(i).encode(), e) for i, (t, k, e) in enumerate(entries)]


def add(vdf, args):
    tree = load(vdf)
    entries = tree[0][2]
    exe_field = quoted(args.exe)
    sid = shortcut_id(exe_field, args.name)
    i = find(entries, exe_field, args.name)
    e = entries[i][2] if i is not None else []
    put(e, T_INT, b"appid", struct.pack("<I", sid))
    put(e, T_STR, b"AppName", s(args.name))
    put(e, T_STR, b"Exe", s(exe_field))
    put(e, T_STR, b"StartDir", s(quoted(args.start_dir or os.path.dirname(os.path.abspath(args.exe)))))
    put(e, T_STR, b"icon", s(os.path.abspath(args.icon) if args.icon else ""))
    put(e, T_STR, b"ShortcutPath", get(e, "ShortcutPath", b""))
    put(e, T_STR, b"LaunchOptions", s(args.args or ""))
    for key, default in ((b"IsHidden", 0), (b"AllowDesktopConfig", 1), (b"AllowOverlay", 1), (b"OpenVR", 0),
                         (b"Devkit", 0)):
        if get(e, key) is None:
            put(e, T_INT, key, i32(default))
    if get(e, "DevkitGameID") is None:
        put(e, T_STR, b"DevkitGameID", b"")
    if get(e, "DevkitOverrideAppID") is None:
        put(e, T_INT, b"DevkitOverrideAppID", i32(0))
    if get(e, "LastPlayTime") is None:
        put(e, T_INT, b"LastPlayTime", i32(0))
    if get(e, "FlatpakAppID") is None:
        put(e, T_STR, b"FlatpakAppID", b"")
    tags = get(e, "tags")
    if tags is None:
        tags = []; put(e, T_MAP, b"tags", tags)
    have = {v for _, _, v in tags}
    for tag in args.tag or []:
        if s(tag) not in have:
            tags.append((T_STR, str(len(tags)).encode(), s(tag)))
    if i is None:
        entries.append((T_MAP, b"", e))
    tree[0] = (T_MAP, tree[0][1], renumber(entries))
    save(vdf, tree)
    return sid


def art(grid, sid, args):
    os.makedirs(grid, exist_ok=True)
    names = {"portrait": "%dp", "wide": "%d", "hero": "%d_hero", "logo": "%d_logo"}
    done = []
    for opt, pat in names.items():
        src = getattr(args, opt)
        if not src:
            continue
        ext = os.path.splitext(src)[1].lower() or ".png"
        for old in (".png", ".jpg", ".jpeg"):           # one file per slot: drop another format's copy
            p = os.path.join(grid, (pat % sid) + old)
            if old != ext and os.path.exists(p):
                os.remove(p)
        dst = os.path.join(grid, (pat % sid) + ext)
        shutil.copyfile(src, dst)
        done.append(dst)
    return done


def remove(vdf, args):
    tree = load(vdf)
    entries = tree[0][2]
    i = find(entries, quoted(args.exe), args.name)
    if i is None:
        return False
    del entries[i]
    tree[0] = (T_MAP, tree[0][1], renumber(entries))
    save(vdf, tree)
    return True


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["add", "remove", "list"])
    ap.add_argument("--exe"); ap.add_argument("--name"); ap.add_argument("--args")
    ap.add_argument("--start-dir"); ap.add_argument("--icon")
    for a in ("portrait", "wide", "hero", "logo"):
        ap.add_argument("--" + a)
    ap.add_argument("--tag", action="append")
    ap.add_argument("--steam"); ap.add_argument("--user"); ap.add_argument("--force", action="store_true")
    args = ap.parse_args(argv)
    if args.cmd in ("add", "remove") and not (args.exe and args.name):
        ap.error("--exe and --name are needed")
    if args.cmd == "add" and not os.path.isfile(args.exe):
        ap.error("%s: no such file" % args.exe)
    roots = [args.steam] if args.steam else steam_dirs()
    if not roots:
        print("Steam not found (give its folder with --steam)"); return 1
    if args.cmd != "list" and not args.force and steam_running():
        print("Steam is running: close it first (it rewrites shortcuts.vdf when it exits), or use --force"); return 1
    n = 0
    for root in roots:
        for u in users(root, args.user):
            cfg = os.path.join(root, "userdata", u, "config")
            vdf = os.path.join(cfg, "shortcuts.vdf")
            if args.cmd == "list":
                for _, _, e in load(vdf)[0][2]:
                    sid = struct.unpack("<I", get(e, "appid", b"\0\0\0\0"))[0]
                    print("user %s  %10d  %-30s %s" % (u, sid, (get(e, "AppName", b"") or b"").decode("utf-8", "replace"),
                                                     (get(e, "Exe", b"") or b"").decode("utf-8", "replace")))
                continue
            if args.cmd == "add":
                sid = add(vdf, args)
                files = art(os.path.join(cfg, "grid"), sid, args)
                print("user %s: %s added (id %d%s)" % (u, args.name, sid, ", %d images" % len(files) if files else ""))
            elif remove(vdf, args):
                print("user %s: %s removed" % (u, args.name))
            n += 1
    if args.cmd != "list" and not n:
        print("no Steam user found under %s" % ", ".join(roots)); return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
