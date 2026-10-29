"""The texture pack keys of the game's button icons (Magma's Button_* textures) and of the Options Controls page's pad,
so the PlayStation 2 art can stand in for them. Reads the player's Xbox game in assets (disc image or extracted files)
and prints src/gpu/ps2_icons.h's table.

    python -I tools/ps2_art/xbox_keys.py [--assets DIR] [--header FILE]
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xboxdata  # noqa: E402

# Unreal's ETextureFormat to the Xbox D3DFORMAT the game creates the texture with (gpu/texture.cpp's table)
FORMATS = {
    3: (0x0C, "DXT1", 1),  # TEXF_DXT1
    5: (0x06, "A8R8G8B8", 0),  # TEXF_RGBA8, swizzled
    7: (0x0E, "DXT3", 3),
    8: (0x0F, "DXT5", 5),
}

# Each Xbox icon (prompts.cpp kIcons, by texture name) and the PlayStation 2 icon in its place, the PS2 game's layout:
# fire on R1, alt fire on L1, whistle on L2, quick inventory on R2. The other languages name the sticks and triggers
# by their own left (G, I, S) and right (D).
ICONS = {
    "button_a": "cross", "button_b": "circle", "button_x": "square", "button_y": "triangle",
    "button_lt": "l1", "button_gt": "l1", "button_it": "l1", "button_st": "l1",
    "button_rt": "r1", "button_dt": "r1",
    "button_black": "l2", "button_white": "r2",
    "button_back": "select", "button_start": "start",
    "button_du": "dpad_up", "button_dd": "dpad_down", "button_dl": "dpad_left", "button_dr": "dpad_right",
    "button_al": "lstick", "button_ag": "lstick", "button_ai": "lstick", "button_as": "lstick",
    "button_al_up": "lstick_up", "button_ag_up": "lstick_up", "button_ai_up": "lstick_up", "button_as_up": "lstick_up",
    "button_al_clic": "l3", "button_ag_clic": "l3", "button_ai_clic": "l3", "button_as_clic": "l3",
    "button_ar": "rstick", "button_ad": "rstick",
    "button_ar_clic": "r3", "button_ad_clic": "r3",
}
# The Options Controls page's pad (menu.lin's Controller, 256 x 256 DXT5) and the DualShock 2 drawn in its place
# (render.py TEXTURES); game/ps2_controls.cpp moves the page's callouts to it
PICTURES = {"controller": ("controls", 256, 256, 0x0F)}

# gpu::Hash (src/gpu/shaders.cpp)
K0, K1, M64 = 0x9E3779B97F4A7C15, 0xE7037ED1A0B428DB, (1 << 64) - 1


def _mix(a, b):
    r = a * b
    return (r & M64) ^ (r >> 64)


def gpu_hash(data, h=1469598103934665603):
    seed = h ^ K0
    n = len(data)
    i = 0
    while i + 16 <= n:
        x, y = struct.unpack_from("<QQ", data, i)
        seed = _mix(x ^ K1, y ^ seed)
        i += 16
    r = n - i
    if r:
        if r >= 8:
            (x,) = struct.unpack_from("<Q", data, i)
            (y,) = struct.unpack_from("<Q", data, n - 8)
        else:
            x = int.from_bytes(data[i:n], "little")
            y = 0
        seed = _mix(x ^ K1, y ^ seed ^ r)
    return _mix(seed ^ K0, n ^ K1)


def pack_key(fmt, w, h, level0):
    """texturedumper::Key: level 0's bytes seeded with format, width and height."""
    key = gpu_hash(level0, gpu_hash(struct.pack("<III", fmt, w, h)))
    return key or 1


def swizzle(linear, w, h, bpp):
    """The packages keep 32-bit textures in rows; the game swizzles them as it makes the Xbox texture, x's bits first
    (gpu/texture.cpp SwizzleMasks)."""
    out = bytearray(len(linear))
    for y, x in ((y, x) for y in range(h) for x in range(w)):
        i, bit, lx, ly = 0, 0, w.bit_length() - 1, h.bit_length() - 1
        for b in range(max(lx, ly)):
            if b < lx:
                i |= ((x >> b) & 1) << bit
                bit += 1
            if b < ly:
                i |= ((y >> b) & 1) << bit
                bit += 1
        out[i * bpp:(i + 1) * bpp] = linear[(y * w + x) * bpp:(y * w + x + 1) * bpp]
    return bytes(out)


def parse_texture(pkg, data, start, end):
    """Format, size and level 0 of a Texture export: its tagged properties end at None, then the mips follow."""
    names = pkg.names
    want = {n: names.index(n) for n in ("None", "Format", "USize", "VSize") if n in names}
    if len(want) < 4:
        return None
    # the properties start a few bytes into the object; find the TexVerNumber or Format tag and walk from the
    # first tag the object has
    i = start
    props = {}
    while i < end:
        name, i2 = xboxdata.compact(data, i)
        if name == want["None"]:
            i = i2
            break
        if not 0 <= name < len(names):
            return None
        info = data[i2]
        i2 += 1
        kind, size_code = info & 0x0F, (info >> 4) & 7
        size = {0: 1, 1: 2, 2: 4, 3: 12, 4: 16}.get(size_code)
        if size_code == 5:
            size = data[i2]
            i2 += 1
        elif size_code == 6:
            size = struct.unpack_from("<H", data, i2)[0]
            i2 += 2
        elif size_code == 7:
            size = struct.unpack_from("<I", data, i2)[0]
            i2 += 4
        if kind == 10:  # struct: its name first
            _, i2 = xboxdata.compact(data, i2)
        if info & 0x80 and kind != 3:  # array index
            i2 += 1
        if kind == 3:  # bool: the value is the array bit
            value = bool(info & 0x80)
        else:
            raw = data[i2:i2 + size]
            value = raw[0] if size == 1 else struct.unpack("<i", raw)[0] if size == 4 else raw
            i2 += size
        props[names[name]] = value
        i = i2
    else:
        return None
    if not all(k in props for k in ("Format", "USize", "VSize")):
        return None
    mips, i = xboxdata.compact(data, i)
    if mips < 1:
        return None
    i += 4  # the lazy array's end position
    n, i = xboxdata.compact(data, i)
    return props["Format"], props["USize"], props["VSize"], data[i:i + n]


def property_start(data, start, end, pkg):
    """The first tag of the object: the tag before TexVerNumber's (a Color struct, InternalTime) or TexVerNumber."""
    tex_ver = pkg.names.index("TexVerNumber") if "TexVerNumber" in pkg.names else -1
    for i in range(start, min(end, start + 64)):
        if data[i] == tex_ver and data[i + 1] == 0x22:
            return i
    return None


def icons(game):
    found = {}  # (name, key) -> entry
    lins = [n for n in game.names() if n.endswith(".lin")]
    for path in sorted(lins):
        data = xboxdata.unlin(game.read(path))
        for pkg in xboxdata.packages(data):
            base = pkg.tables_end - min((e["offset"] for e in pkg.exports if e["size"] > 0), default=0)
            for e in pkg.exports:
                if e["class"] != "Texture" or e["name"].lower() not in ICONS:
                    continue
                start = base + e["offset"]
                end = start + e["size"]
                at = property_start(data, start, end, pkg)
                tex = parse_texture(pkg, data, at, end) if at is not None else None
                if not tex:
                    print(f"  {path}: {e['name']} not parsed", file=sys.stderr)
                    continue
                ufmt, w, h, level0 = tex
                if (w, h) != (32, 32):  # the HUD's Button_A is the 256 x 128 ring round the stealth meter
                    continue
                if ufmt not in FORMATS:
                    print(f"  {path}: {e['name']} format {ufmt} unknown", file=sys.stderr)
                    continue
                fmt, fmt_name, dxt = FORMATS[ufmt]
                key = pack_key(fmt, w, h, level0 if dxt else swizzle(level0, w, h, 4))
                entry = found.setdefault((e["name"], key), {"name": e["name"], "key": key, "w": w, "h": h,
                                                           "format": fmt_name, "files": []})
                entry["files"].append(path)
    return sorted(found.values(), key=lambda e: (e["name"].lower(), e["key"]))


def pictures(game):
    """The PICTURES textures. A linear package keeps its objects in load order, so a big texture is not where its
    export's offset says: each Texture object of the package that exports the picture is read where its tags start
    (TexVerNumber, first), and the picture is the one of its size and format."""
    found = {}  # (name, key) -> entry
    for path in sorted(n for n in game.names() if n.endswith(".lin")):
        data = xboxdata.unlin(game.read(path))
        for pkg in xboxdata.packages(data):
            names = [e["name"].lower() for e in pkg.exports if e["class"] == "Texture" and e["name"].lower() in PICTURES]
            if not names or "TexVerNumber" not in pkg.names:
                continue
            tag = bytes([pkg.names.index("TexVerNumber"), 0x22])
            nxt = data.find(xboxdata.PACKAGE_MAGIC, pkg.at + 4)
            end = nxt if nxt > 0 else len(data)
            for name in names:
                _, w, h, fmt = PICTURES[name]
                at = data.find(tag, pkg.tables_end, end)
                while at >= 0:
                    tex = parse_texture(pkg, data, at, end)
                    if tex and tex[0] in FORMATS and FORMATS[tex[0]][0] == fmt and tex[1:3] == (w, h):
                        ufmt, _, _, level0 = tex
                        key = pack_key(fmt, w, h, level0)
                        entry = found.setdefault((name, key), {"name": name.capitalize(), "key": key, "w": w, "h": h,
                                                               "format": FORMATS[ufmt][1], "files": []})
                        entry["files"].append(path)
                        break
                    at = data.find(tag, at + 2, end)
    return list(found.values())


HEADER = """#pragma once
// The game's button icons (Magma's Button_* textures) and the Options Controls page's pad by their texture pack key,
// and the PlayStation 2 art drawn in their place, ps2\\icons\\<name>.png beside the exe. Written by
// tools/ps2_art/xbox_keys.py from the Xbox release this executable is made from.
#include <cstdint>

namespace gpu {

struct Ps2Icon {
    uint64_t key;
    const char* name;
};

inline constexpr Ps2Icon kPs2Icons[] = {
%s
};

}  // namespace gpu
"""


def write_header(path, found):
    names = {**ICONS, **{k: v[0] for k, v in PICTURES.items()}}
    rows = [f"    {{0x{e['key']:016x}ull, \"{names[e['name'].lower()]}\"}},  // {e['name']}" for e in found]
    with open(path, "w", newline="\n") as f:
        f.write(HEADER % "\n".join(rows))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--assets", default=os.path.join(here, "..", "..", "assets"))
    ap.add_argument("--header", help="write the key table, src/gpu/ps2_icons.h")
    args = ap.parse_args()
    game = xboxdata.find_game(os.path.normpath(args.assets))
    found = icons(game) + pictures(game)
    for e in found:
        print(f"{e['name']:16s} {e['w']:3d}x{e['h']:<3d} {e['format']:8s} {e['key']:016x}  {len(e['files'])} lins")
    if args.header:
        write_header(args.header, found)


if __name__ == "__main__":
    main()
