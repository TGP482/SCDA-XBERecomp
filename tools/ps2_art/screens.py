"""The PlayStation 2 controller screens. The Xbox ones (Default\\DataXb\\screens\\<lang>\\controller*.tga) are the
player's, so they are never shipped: the recomp redraws them at runtime from data/ps2/screens.toml, a recipe of
coordinates this script writes after measuring the player's own screens. The Xbox pad is filled in from its
surroundings, the DualShock 2 goes in its place, the callouts are drawn again to the buttons of the PS2 layout and
the green A of "PRESS A TO CONTINUE" becomes a cross (src/app/ps2_screens.cpp).

    python -I tools/ps2_art/screens.py [--assets DIR]
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xboxdata  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.normpath(os.path.join(HERE, "..", "..", "data", "ps2"))
LANGUAGES = ("int", "fra", "deu", "ita", "esp")

LINE = (118, 130, 105)  # the screens' callout colour
# where the DualShock 2 goes on both pages: dualshock2.png, its SVG's viewBox (205 82 665 403) at 220 x 133
PAD = (209, 163)
SCALE = 220 / 665


def at(x, y):
    """A point of dualshock2.svg on the screen."""
    return round(PAD[0] + (x - 205) * SCALE), round(PAD[1] + (y - 82) * SCALE)


# Where the callouts end, by the SVG's shapes. The shoulders: L2 and R2 are the bands on top, L1 and R1 the tabs
# under them, reached on their slopes beside the bands. The face buttons are reached from their right, the D-pad's
# arms from outside the pad, the sticks at their wells' edges.
L1_OUTER = at(286, 120)   # the L tab's outer slope, from above
L1_INNER = at(398, 120)   # its inner slope, from above
R1_OUTER = at(791, 120)
R1_INNER = at(687, 120)
L2_TOP = at(340, 88)      # the L2 band's top
R2_TOP = at(740, 89)
SELECT_TOP = at(479, 239)
DPAD_UP_LEFT = at(305, 194)      # the up arm's left side
DPAD_LEFT_BOTTOM = at(278, 285)  # the left arm's underside
DPAD_DOWN_BOTTOM = at(337, 338)
DPAD_RIGHT_END = at(426, 254)    # past the right arm's end
TRIANGLE_RIGHT = at(770, 197)
CIRCLE_RIGHT = at(823, 250)
CROSS_RIGHT = at(770, 302)
SQUARE_LEFT = at(669, 254)
L3_LEFT = at(365, 352)           # the left stick's well, its left edge
R3_BOTTOM = at(648, 422)         # the right stick's well, its bottom edge
# the Xbox pad's outline on both pages, filled in from what is round it
XBOX_PAD = [(234, 166), (248, 155), (274, 148), (366, 148), (392, 155), (406, 166), (418, 205), (428, 262),
            (430, 292), (419, 311), (388, 311), (373, 294), (364, 270), (358, 269), (283, 269), (276, 270),
            (268, 294), (253, 311), (222, 311), (212, 292), (213, 262), (216, 230), (222, 206), (229, 186)]


# Measuring the Xbox screens

def load(game, lang, page):
    """A screen as RGBA, top row first. The game's are 8-bit with a 24-bit palette, bottom row first."""
    data = game.read(f"Default\\DataXb\\screens\\{lang}\\{page}.tga")
    cmfirst, cmlen = struct.unpack_from("<HH", data, 3)
    w, h = struct.unpack_from("<HH", data, 12)
    pal = 18 + data[0]
    at = pal + cmlen * 3
    px = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            i = pal + (data[at + y * w + x] - cmfirst) * 3
            o = ((h - 1 - y) * w + x) * 4
            px[o:o + 4] = bytes((data[i + 2], data[i + 1], data[i], 255))
    return w, h, bytes(px)


def lum(px, w, h, x, y):
    if not (0 <= x < w and 0 <= y < h):
        return 0
    o = (y * w + x) * 4
    return (px[o] * 3 + px[o + 1] * 6 + px[o + 2]) / 10


def runs(px, w, h, minlen=12):
    """The callouts: thin light horizontal and vertical runs, as (kind, at, start, end)."""
    out = []
    L = [[lum(px, w, h, x, y) for x in range(w)] for y in range(h)]

    def get(x, y):
        return L[y][x] if 0 <= x < w and 0 <= y < h else 0

    def hline(x, y):
        v = get(x, y)
        return v > 85 and v - get(x, y - 2) > 25 and v - get(x, y + 2) > 25

    def vline(x, y):
        v = get(x, y)
        return v > 85 and v - get(x - 2, y) > 25 and v - get(x + 2, y) > 25

    for y in range(h):
        x = 0
        while x < w:
            if hline(x, y):
                s = x
                while x < w and hline(x, y):
                    x += 1
                if x - s >= minlen:
                    out.append(("H", y, s, x - 1))
            else:
                x += 1
    for x in range(w):
        y = 0
        while y < h:
            if vline(x, y):
                s = y
                while y < h and vline(x, y):
                    y += 1
                if y - s >= minlen:
                    out.append(("V", x, s, y - 1))
            else:
                y += 1
    return out


def find(found, kind, at_range, span_hint=None):
    """The run of that kind whose row (or column) is in at_range and, given a hint, covers that coordinate."""
    for k, at, s, e in found:
        if k == kind and at_range[0] <= at <= at_range[1] and (span_hint is None or s <= span_hint <= e):
            return at, s, e
    raise LookupError(f"no {kind} run at {at_range} over {span_hint}")


def glyph(px, w, h):
    """The green A's disc: found by its strongly green letter near the bottom, then measured by every greenish pixel
    round it (the captions are tinted green too), with two pixels to spare."""
    def green(x, y, by, floor=0):
        o = (y * w + x) * 4
        return px[o + 1] > px[o] + by and px[o + 1] > px[o + 2] + by and px[o + 1] > floor

    letter = [x for y in range(400, h) for x in range(w) if green(x, y, 40)]
    cx = (min(letter) + max(letter) + 1) // 2
    disc = [(x, y) for y in range(400, h) for x in range(cx - 14, cx + 15) if green(x, y, 12, 45)]
    xs = [p[0] for p in disc]
    ys = [p[1] for p in disc]
    r = (max(max(xs) - min(xs), max(ys) - min(ys)) + 1) // 2 + 2
    return [(min(xs) + max(xs) + 1) // 2, (min(ys) + max(ys) + 1) // 2, r]


def erase_runs(found, zone):
    """Every callout piece inside the zone, as rectangles one pixel wide (the arrowheads at their ends too)."""
    x0, y0, x1, y1 = zone
    out = []
    for k, at, s, e in found:
        if k == "H" and y0 <= at <= y1:
            a, b = max(s, x0), min(e, x1)
            if a <= b:
                out.append([a, at, b, at])
        elif k == "V" and x0 <= at <= x1:
            a, b = max(s, y0), min(e, y1)
            if a <= b:
                out.append([at, a, at, b])
    return out


def layout_page(px, w, h):
    """controller2: what each control does, the callouts' rows found on this language's screen. Opsat crosses
    Secondary Fire's line to reach SELECT in the middle; Thermal Vision and Equip Item go round under the pad, between
    its sticks."""
    f = runs(px, w, h)
    zone = (214, 150, 430, 330)
    secondary_rule = find(f, "H", (118, 126), 240)[0]
    primary_rule = find(f, "H", (118, 126), 400)[0]
    secondary_drop = find(f, "V", (255, 266), 140)[0]
    primary_drop = find(f, "V", (371, 382), 140)[0]
    opsat = find(f, "H", (155, 168), 200)[0]
    emf = find(f, "H", (222, 232), 200)[0]
    night = find(f, "H", (252, 262), 200)[0]
    sticky = find(f, "H", (280, 292), 200)[0]
    thermal = find(f, "H", (310, 322), 200)[0]
    jump = find(f, "H", (178, 190), 440)[0]
    crouch = find(f, "H", (208, 220), 440)[0]
    interact = find(f, "H", (238, 250), 440)[0]
    equip = find(f, "H", (266, 294), 440)[0]
    under = 225  # the row under SELECT and START, over the sticks
    lines = [
        {"path": [[255, secondary_rule], [L1_INNER[0], secondary_rule], [L1_INNER[0], L1_INNER[1] - 2]],
         "gaps": [[L1_INNER[0], opsat]]},
        {"path": [[383, primary_rule], [R1_INNER[0], primary_rule], [R1_INNER[0], R1_INNER[1] - 2]]},
        {"path": [[214, opsat], [SELECT_TOP[0], opsat], [SELECT_TOP[0], SELECT_TOP[1] - 1]]},
        {"path": [[214, emf], [221, emf], [221, DPAD_UP_LEFT[1]], list(DPAD_UP_LEFT)]},
        {"path": [[214, night], [DPAD_LEFT_BOTTOM[0], night], list(DPAD_LEFT_BOTTOM)]},
        {"path": [[214, sticky], [DPAD_DOWN_BOTTOM[0], sticky], list(DPAD_DOWN_BOTTOM)]},
        {"path": [[214, thermal], [320, thermal], [320, under], [DPAD_RIGHT_END[0], under]]},
        {"path": [[430, jump], [420, jump], [420, TRIANGLE_RIGHT[1]], list(TRIANGLE_RIGHT)]},
        {"path": [[430, crouch], [425, crouch], [425, CIRCLE_RIGHT[1]], list(CIRCLE_RIGHT)]},
        {"path": [[430, interact], [405, interact], [405, CROSS_RIGHT[1]], list(CROSS_RIGHT)]},
        {"path": [[430, equip], [425, equip], [425, 282], [325, 282], [325, under], [SQUARE_LEFT[0] - 1, under]]},
    ]
    erase = erase_runs(f, zone)
    # the shoulders' old drops above the zone
    erase += [[secondary_drop, 123, secondary_drop, 150], [primary_drop, 123, primary_drop, 150]]
    return {"fill": XBOX_PAD, "erase": erase, "pad": list(PAD), "lines": lines}


def moves_page(px, w, h):
    """controller: six moves around the pad, each in a photo frame. The stick clicks' lines carry a click icon, the
    L3 and R3 icons go in their place. Whistle (L2) and quick inventory (R2) leave their frames on the right for the
    shoulders on top, over Lethal Attack's line."""
    f = runs(px, w, h)
    zone = (214, 150, 430, 340)
    ko_row = find(f, "H", (93, 101), 235)
    lethal_row = find(f, "H", (93, 101), 390)
    ko_drop = find(f, "V", (260, 270), 120)[0]
    lethal_drop = find(f, "V", (368, 378), 120)[0]
    switch = find(f, "H", (250, 260), 255)[0]
    eev = find(f, "H", (344, 354), 320)[0]
    eev_drop = find(f, "V", (342, 352), 300)
    whistle_row = find(f, "H", (222, 234), 420)[0]
    quick_row = find(f, "H", (328, 340), 420)[0]
    whistle_y, quick_y = 156, 160
    lines = [
        {"path": [[229, ko_row[0]], [L1_OUTER[0], ko_row[0]], [L1_OUTER[0], L1_OUTER[1] - 2]]},
        {"path": [[410, lethal_row[0]], [R1_OUTER[0], lethal_row[0]], [R1_OUTER[0], R1_OUTER[1] - 2]],
         "gaps": [[R1_OUTER[0], whistle_y], [R1_OUTER[0], quick_y]]},
        {"path": [[202, switch], [L3_LEFT[0] - 1, switch]]},
        {"path": [[202, eev], [R3_BOTTOM[0], eev], [R3_BOTTOM[0], R3_BOTTOM[1] + 1]]},
        {"path": [[438, 175], [434, 175], [434, whistle_y], [L2_TOP[0], whistle_y], [L2_TOP[0], L2_TOP[1] - 1]]},
        {"path": [[437, quick_row], [426, quick_row], [426, quick_y], [R2_TOP[0], quick_y], [R2_TOP[0], R2_TOP[1] - 1]]},
    ]
    erase = erase_runs(f, zone)
    erase += [
        # the old whistle stub outside the zone, between it and its frame; the Xbox stick click icons
        [431, whistle_row, 437, whistle_row], [216, 232, 248, 271], [266, 325, 299, 364],
        # the shoulders' old drops above the zone, and their rows past the new ones
        [ko_drop, 98, ko_drop, 150], [L1_OUTER[0] + 1, ko_row[0], ko_row[2], ko_row[0]],
        [lethal_drop, 98, lethal_drop, 150], [lethal_row[1], lethal_row[0], R1_OUTER[0] - 1, lethal_row[0]],
        # EEV's old rise to the Xbox right stick, down to its row
        [eev_drop[0], eev_drop[1], eev_drop[0], eev_drop[2]],
    ]
    icons = [["l3", 211, switch - 2, 22], ["r3", 282, eev - 3, 24]]
    return {"fill": XBOX_PAD, "erase": erase, "pad": list(PAD), "lines": lines, "icons": icons}


def recipe(game):
    out = {}
    for lang in LANGUAGES:
        pages = {}
        for page, design in (("controller", moves_page), ("controller2", layout_page), ("controller3", None),
                             ("controller4", None)):
            w, h, px = load(game, lang, page)
            p = design(px, w, h) if design else {}
            p["glyph"] = glyph(px, w, h)
            pages[page] = p
        out[lang] = pages
    return out


# The recipe as TOML (src/app/ps2_screens.cpp reads it with toml++)

def toml_value(v):
    if isinstance(v, str):
        return '"' + v + '"'
    if isinstance(v, list):
        return "[" + ", ".join(toml_value(x) for x in v) + "]"
    if isinstance(v, tuple):
        return toml_value(list(v))
    return str(v)


def write_toml(path, rec):
    out = ["# The PlayStation 2 controller screens, drawn over the player's Xbox ones by src/app/ps2_screens.cpp.",
           "# Written by tools/ps2_art/screens.py; coordinates on the 640 x 480 screens. fill: the Xbox pad's outline,",
           "# filled in from round it. erase: callout pieces, x0 y0 x1 y1. pad: where dualshock2.png goes. lines:",
           "# callouts, each a path ending in an arrowhead, with gaps where another line crosses over. glyph: the",
           "# green A's centre and radius, the cross icon goes there. icons: name, centre and size of icons on the lines.",
           "# controller1 is controller.",
           f"line_colour = {toml_value(list(LINE))}", ""]
    for lang, pages in rec.items():
        for page, p in pages.items():
            out.append(f"[{lang}.{page}]")
            for key in ("glyph", "pad", "icons", "fill", "erase"):
                if key in p:
                    out.append(f"{key} = {toml_value(p[key])}")
            if "lines" in p:
                out.append("lines = [")
                for ln in p["lines"]:
                    out.append(f"    {{path = {toml_value(ln['path'])}, gaps = {toml_value(ln.get('gaps', []))}}},")
                out.append("]")
            out.append("")
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--assets", default=os.path.join(HERE, "..", "..", "assets"))
    args = ap.parse_args()
    game = xboxdata.find_game(os.path.normpath(args.assets))
    write_toml(os.path.join(DATA, "screens.toml"), recipe(game))
    print("wrote", os.path.join(DATA, "screens.toml"))


if __name__ == "__main__":
    main()
