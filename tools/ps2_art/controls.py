"""The Options Controls page laid out as the PlayStation 2 game's: the DualShock 2 (render.py's controls texture, in
place of the Xbox pad, xbox_keys.py) a little smaller in the middle, the labels in the PS2 page's order down both
sides and the callouts redrawn to its buttons. The page's own elements move: game/ps2_controls.cpp swaps each one's
rect for the one here while PlayStation 2 prompts are on. Reads the Xbox rects from the player's game (the page's
records in dynamicxbox.umd) and writes src/game/ps2_controls.h.

    python -I tools/ps2_art/controls.py [--assets DIR] [--header FILE]
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import render  # noqa: E402
import xboxdata  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))

# Magma names elements by crc32 of their name (game/magma.h); the page's are not recovered
PAGE = 0x91CC5199
AREA = 0x942D1AFE     # the pad, its callouts; placed at its own x, y on the page
PICTURE = 0x03AF654A  # the pad, in the area
# the callouts' pieces, in the area: one pixel wide or tall
SEGMENTS = [0x4AD9856A, 0x3DDEB5FC, 0xA4D7E446, 0xD3D0D4D0, 0x4DB44173, 0x44020D58, 0x33053DCE, 0x40EDBFA3,
            0x37EA8F35, 0xD9E4EE19, 0x47807BBA, 0x30874B2C, 0xA98E1A96, 0xDE892A00, 0x4E363791, 0x39310707,
            0x6BC0EC60, 0x1CC7DCF6, 0x85CE8D4C, 0xF2C9BDDA, 0x6CAD2879, 0x82A34955, 0xF5A479C3, 0x651B6452,
            0xA3BA205F, 0x1BAA18EF, 0x121C54C4, 0x72DBDD21, 0x05DCEDB7, 0x9CD5BC0D, 0xEBD28C9B, 0x75B61938,
            0x02B129AE, 0xAEE3DE8F, 0x9BB87814, 0xECBF4882, 0x7C005513, 0x0B076585]
RECT = 0x1A  # a record's rect after its id: left, right, top, bottom (an area's x, y)

# The DualShock 2 on the page: the picture is square, the controls texture, which has dualshock2.svg (viewBox 205 82
# 665 403) in its middle (render.py TEXTURES)
PIC = (212, 143, 216)  # left, top, side
SVG_X, SVG_Y, SVG_W, SVG_H = 205, 82, 665, 403
_, TEX_SIZE, TEX_PAD, _ = render.TEXTURES["controls"]
SCALE = PIC[2] * TEX_PAD / TEX_SIZE / SVG_W
PAD_LEFT = PIC[0] + (PIC[2] - SVG_W * SCALE) / 2
PAD_TOP = PIC[1] + (PIC[2] - SVG_H * SCALE) / 2


def at(x, y):
    """A point of dualshock2.svg on the page."""
    return round(PAD_LEFT + (x - SVG_X) * SCALE), round(PAD_TOP + (y - SVG_Y) * SCALE)


# Where the callouts end, by the SVG's shapes, as on the PS2 page: the shoulders' tops (L2, R2) and outer sides (L1,
# R1), Select and Start from above, the D-pad's arms (up from its left, right from above, left and down from below),
# the square from above, the triangle from its right, the circle and cross from below, the sticks below their caps
L2_TOP, R2_TOP = at(340, 88), at(740, 88)
L1_SIDE, R1_SIDE = at(280, 116), at(797, 116)
SELECT_TOP, START_TOP = at(479, 242), at(596, 242)
DPAD_UP_SIDE, DPAD_RIGHT_TOP = at(321, 214), at(372, 230)
DPAD_LEFT_BOTTOM, DPAD_DOWN_BOTTOM = at(305, 266), at(339, 303)
SQUARE_TOP, TRIANGLE_SIDE = at(689, 224), at(766, 197)
CIRCLE_BOTTOM, CROSS_BOTTOM = at(794, 274), at(741, 327)
LSTICK_BOTTOM, RSTICK_BOTTOM = at(436, 391), at(642, 391)

# The labels' texts (by their element on the Xbox page)
OPSAT, WHISTLE, KO, THERMAL = 0x1BC729EF, 0xFCD4F99D, 0xFB12C0F1, 0xFBB93D84
EMF, NIGHT, STICKY, MOVE = 0x8CBE0D12, 0x15B75CA8, 0x62B06C3E, 0xFC7F04E8
PAUSE, QUICK, USE, EQUIP = 0x6CC01979, 0x65DDA827, 0x621B914B, 0x65765552
JUMP, CROUCH, INTERACT, EEV = 0x127165C4, 0x8C15F067, 0x8B78347E, 0x12DA98B1
# their text in the game's OPTIONS section (pregamemenus.int): ControllerAction[n]
ACTIONS = {USE: 1, KO: 2, CROUCH: 3, JUMP: 4, EQUIP: 5, MOVE: 6, INTERACT: 7, OPSAT: 8, PAUSE: 9, NIGHT: 10,
           STICKY: 11, THERMAL: 12, EMF: 13, EEV: 14, QUICK: 15, WHISTLE: 16}
NAMES = {OPSAT: "OPSAT / PDA MENU", WHISTLE: "WHISTLE / COOP ACTION", KO: "KO ATTACK", THERMAL: "THERMAL VISION",
         EMF: "EMF", NIGHT: "NIGHT VISION", STICKY: "STICKY CAMERA", MOVE: "MOVEMENT / BACK TO WALL",
         PAUSE: "PAUSE MENU", QUICK: "QUICK INVENTORY", USE: "USE EQUIPPED / LETHAL ATTACK", EQUIP: "EQUIP ITEM",
         JUMP: "JUMP", CROUCH: "CROUCH / STAND", INTERACT: "INTERACT", EEV: "CAMERA / EEV"}

# The labels down each side as on the PS2 page, by the line their first row of text sits on: the element that draws
# it (the Xbox page's labels are right aligned on its left, left aligned on its right: whistle and pause change
# sides, so each is drawn by the other's element with its text), the text, where its callout ends and, for a
# callout that turns twice, how far out it goes before the first turn
LEFT_TEXT, RIGHT_TEXT = (50, 210), (431, 590)
GAP = 3  # between a label and its callout
ROWS = [103, 125, 163, 203, 242, 280, 318, 357]
LEFT = [(OPSAT, OPSAT, SELECT_TOP, None),
        (PAUSE, WHISTLE, L2_TOP, None),
        (KO, KO, L1_SIDE, "middle"),
        (THERMAL, THERMAL, DPAD_RIGHT_TOP, None),
        (EMF, EMF, DPAD_UP_SIDE, 9),
        (NIGHT, NIGHT, DPAD_LEFT_BOTTOM, None),
        (STICKY, STICKY, DPAD_DOWN_BOTTOM, None),
        (MOVE, MOVE, LSTICK_BOTTOM, None)]
RIGHT = [(WHISTLE, PAUSE, START_TOP, None),
         (QUICK, QUICK, R2_TOP, None),
         (USE, USE, R1_SIDE, "middle"),
         (EQUIP, EQUIP, SQUARE_TOP, None),
         (JUMP, JUMP, TRIANGLE_SIDE, 9),
         (CROUCH, CROUCH, CIRCLE_BOTTOM, None),
         (INTERACT, INTERACT, CROSS_BOTTOM, None),
         (EEV, EEV, RSTICK_BOTTOM, None)]
TEXT_ABOVE = 11  # a label's top above its first row's line
# The PS2 page's stick labels also name their click, after the game's own text in each of its languages: zoom is the
# game's word for the EEV's (EEV_AL_clic), switch hand is worded as its Switch Weapon; such a label has room for
# three rows of text
MORE = {MOVE: {"int": "Switch Hand", "fra": "Changer de main", "deu": "Hand Wechseln", "ita": "Cambia mano",
               "esp": "Cambiar de mano"},
        EEV: {"int": "Zoom", "fra": "Zoom", "deu": "Zoom", "ita": "Avvicina", "esp": "Zoom"}}
MORE_ROWS, ROW = 3, 16


def records(umd):
    """Each element's record by its id: the one nearest the page's (other pages reuse names)."""
    page = umd.find(struct.pack("<I", PAGE))
    if page < 0:
        sys.exit("no Controls page in dynamicxbox.umd")
    out = {}
    for id_ in [AREA, PICTURE] + SEGMENTS + list(NAMES):
        best, i = -1, umd.find(struct.pack("<I", id_))
        while i >= 0:
            if best < 0 or abs(i - page) < abs(best - page):
                best = i
            i = umd.find(struct.pack("<I", id_), i + 1)
        if best < 0:
            sys.exit(f"no element {id_:08X}")
        out[id_] = struct.unpack_from("<hhhh", umd, best + RECT)
    return out


def callout(start, y, end, turn):
    """The polyline from a label's side at its row to a button: across to the button's column and on to it, or out
    to a column first, then up or down to the button's height and across to it."""
    sx = start
    tx, ty = end
    if turn is None:
        return [(sx, y), (tx, y), (tx, ty)]
    col = (sx + tx) // 2 if turn == "middle" else sx + (turn if tx > sx else -turn)
    return [(sx, y), (col, y), (col, ty), (tx, ty)]


def segments(points):
    """A polyline's pieces as one pixel rects: left, right, top, bottom."""
    out = []
    for (x0, y0), (x1, y1) in zip(points, points[1:]):
        if (x0, y0) == (x1, y1):
            continue
        if y0 == y1:
            out.append((min(x0, x1), max(x0, x1) + 1, y0, y0 + 1))
        elif x0 == x1:
            out.append((x0, x0 + 1, min(y0, y1), max(y0, y1) + 1))
        else:
            sys.exit(f"callout piece {(x0, y0)} {(x1, y1)} is not straight")
    return out


def layout(xbox):
    """(id, what, Xbox rect, PS2 rect) for each element, in its parent's units, the labels that draw another's text:
    (element, text), the labels as drawn: (text, right aligned, box) and the callout pieces, on the page."""
    ax, ay = xbox[AREA][0], xbox[AREA][1]
    rows = []
    pieces = []
    swaps = []
    labels_drawn = []
    for side, labels, text in ((-1, LEFT, LEFT_TEXT), (1, RIGHT, RIGHT_TEXT)):
        start = text[1] + GAP if side < 0 else text[0] - GAP
        for (id_, shown, end, turn), y in zip(labels, ROWS):
            _, _, t, b = xbox[shown]  # as tall as its text's own
            top = y - TEXT_ABOVE
            height = max(b - t, MORE_ROWS * ROW + 2) if shown in MORE else b - t
            name = NAMES[shown] if shown == id_ else f"{NAMES[shown]} (by {NAMES[id_]})"
            if shown in MORE:
                name += f" / {MORE[shown]['int'].upper()}"
            rows.append((id_, name, xbox[id_], (text[0], text[1], top, top + height)))
            labels_drawn.append((shown, side < 0, (text[0], text[1], top, top + height)))
            if shown != id_:
                swaps.append((id_, shown))
            pieces += segments(callout(start, y, end, turn))
    if len(pieces) > len(SEGMENTS):
        sys.exit(f"{len(pieces)} callout pieces, the page has {len(SEGMENTS)}")
    # the callouts are in the area
    for i, id_ in enumerate(SEGMENTS):
        ps2 = (0, 0, 0, 0)
        if i < len(pieces):
            l, r, t, b = pieces[i]
            ps2 = (l - ax, r - ax, t - ay, b - ay)
        rows.append((id_, "callout", xbox[id_], ps2))
    x, y, side = PIC
    rows.append((PICTURE, "pad", xbox[PICTURE], (x - ax, x + side - ax, y - ay, y + side - ay)))
    return rows, swaps, labels_drawn, pieces


HEADER = """#pragma once
// The Options Controls page laid out as the PlayStation 2 game's, the DualShock 2 in the Xbox pad's place: each
// element's rect on the Xbox page and the one it takes with PlayStation 2 prompts (left, right, top, bottom, in its
// parent's units; a callout piece the PS2 page has no use for gets an empty one). Written by
// tools/ps2_art/controls.py from the Xbox release this executable is made from.
#include <cstdint>

namespace game {

struct Ps2Rect {
    uint32_t id;
    int16_t xbox[4];
    int16_t ps2[4];
};

inline constexpr uint32_t kControlsPage = 0x%08X;

inline constexpr Ps2Rect kPs2Controls[] = {
%s
};

// labels that change sides draw with the text of the one whose alignment they take: element, its text's element
struct Ps2Text {
    uint32_t id;
    uint32_t text;
};

inline constexpr Ps2Text kPs2ControlsTexts[] = {
%s
};

// labels that say more on the PS2 page: the text's element, the game's language, what follows the text
struct Ps2More {
    uint32_t text;
    const char* language;
    const char16_t* more;
};

inline constexpr Ps2More kPs2ControlsMore[] = {
%s
};

// The same layout as drawn into a picture (the controller screen a new game starts with), in page units: each label's
// text (OPTIONS ControllerAction[n] and what kPs2ControlsMore has for its element), its box and whether it is right
// aligned (the left side), the callouts' pieces and the pad's square
struct Ps2Label {
    int action;
    uint32_t text;
    bool right;
    int16_t box[4];
};

inline constexpr Ps2Label kPs2ControlsLabels[] = {
%s
};

inline constexpr int16_t kPs2ControlsLines[][4] = {
%s
};

inline constexpr int16_t kPs2ControlsPad[4] = {%s};

}  // namespace game
"""


def write_header(path, rows, swaps, labels, pieces):
    lines = [f"    {{0x{id_:08X}, {{{', '.join(map(str, x))}}}, {{{', '.join(map(str, p))}}}}},  // {name}"
             for id_, name, x, p in rows]
    texts = [f"    {{0x{id_:08X}, 0x{shown:08X}}},  // {NAMES[shown]}" for id_, shown in swaps]
    # in capitals, as the game has its labels' text
    more = [f"    {{0x{id_:08X}, \"{lang}\", u\" / {words.upper()}\"}},  // {NAMES[id_]}"
            for id_, by_lang in MORE.items() for lang, words in by_lang.items()]
    drawn = [f"    {{{ACTIONS[shown]}, 0x{shown:08X}, {'true' if right else 'false'}, {{{', '.join(map(str, box))}}}}},"
             f"  // {NAMES[shown]}" for shown, right, box in labels]
    pieces = [f"    {{{', '.join(map(str, p))}}}," for p in pieces]
    x, y, side = PIC
    with open(path, "w", newline="\n") as f:
        f.write(HEADER % (PAGE, "\n".join(lines), "\n".join(texts), "\n".join(more), "\n".join(drawn),
                          "\n".join(pieces), f"{x}, {x + side}, {y}, {y + side}"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--assets", default=os.path.join(HERE, "..", "..", "assets"))
    ap.add_argument("--header", help="write src/game/ps2_controls.h")
    args = ap.parse_args()
    game = xboxdata.find_game(os.path.normpath(args.assets))
    xbox = records(game.read("Default\\dynamicxbox.umd"))
    rows, swaps, labels, pieces = layout(xbox)
    for id_, name, x, p in rows:
        print(f"{id_:08X} {name:30s} {x} -> {p}")
    if args.header:
        write_header(args.header, rows, swaps, labels, pieces)


if __name__ == "__main__":
    main()
