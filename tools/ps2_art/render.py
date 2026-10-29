"""Renders the PlayStation 2 prompt art, data/ps2/art/*.svg, into the PNGs the game loads: icons into data/ps2/icons
at 128 x 128, pictures (an SVG whose root has data-px="W H") into data/ps2 at that size, and the textures drawn from
a picture (TEXTURES) into data/ps2/icons. Headless Microsoft Edge draws them, so the PNGs are committed and a build
needs no browser.

    python -I tools/ps2_art/render.py [NAME...]
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import re

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.normpath(os.path.join(HERE, "..", "..", "data", "ps2"))
ICON_PX = 128
# textures drawn from a picture: name -> (the picture's SVG, the texture's square size, the picture's width in its
# middle, the colour of a soft outline round it, which shows a black pad on a dark page) (controls: the Options
# Controls page's pad, xbox_keys.py PICTURES)
TEXTURES = {"controls": ("dualshock2", 512, 496, "#a0a0a0")}
EDGE = [os.path.expandvars(p) for p in (r"%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe",
                                         r"%ProgramFiles%\Microsoft\Edge\Application\msedge.exe")]


def edge():
    for p in EDGE:
        if os.path.isfile(p):
            return p
    sys.exit("Microsoft Edge not found")


PAGE = """<!doctype html><html><head><meta charset="utf-8">
<style>html, body {{ margin: 0; background: transparent; }}
svg {{ display: block; width: {w}px; height: {h}px; margin: {top}px {left}px; {extra} }}</style>
</head><body>{svg}</body></html>"""


def render(browser, svg, out, w, h, profile, page=None, outline=None):
    """The SVG at w x h, filling a page that size or in the middle of a square page this size, with a soft outline of
    this colour."""
    if os.path.exists(out):
        os.remove(out)
    pw, ph = (page, page) if page else (w, h)
    extra = f"filter: drop-shadow(0 0 1.5px {outline}) drop-shadow(0 0 1px {outline});" if outline else ""
    # an SVG file alone screenshots blank: it goes inline in a page of its size
    html = os.path.join(profile, "page.html")
    with open(html, "w", encoding="utf-8") as f:
        f.write(PAGE.format(w=w, h=h, top=(ph - h) // 2, left=(pw - w) // 2, extra=extra,
                            svg=open(svg, encoding="utf-8").read()))
    url = "file:///" + html.replace("\\", "/")
    subprocess.run([browser, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                    "--default-background-color=00000000", f"--window-size={pw},{ph}", f"--user-data-dir={profile}",
                    f"--screenshot={out}", url], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # Edge hands the page to a child process and may return before the file is written
    deadline = time.time() + 30
    last = -1
    while time.time() < deadline:
        size = os.path.getsize(out) if os.path.exists(out) else -1
        if size > 0 and size == last:
            return
        last = size
        time.sleep(0.3)
    sys.exit(f"{svg}: no screenshot")


def main():
    browser = edge()
    art = os.path.join(DATA, "art")
    names = sys.argv[1:] or sorted(n[:-4] for n in os.listdir(art) if n.endswith(".svg")) + sorted(TEXTURES)
    profile = tempfile.mkdtemp(prefix="ps2_art_")
    try:
        for name in names:
            if name in TEXTURES:
                picture, size, w, outline = TEXTURES[name]
                svg = os.path.join(art, picture + ".svg")
                vw, vh = map(float, re.search(r'viewBox="[\d.]+ [\d.]+ ([\d.]+) ([\d.]+)"',
                                              open(svg, encoding="utf-8").read(400)).groups())
                out = os.path.join(DATA, "icons", name + ".png")
                render(browser, svg, out, w, round(w * vh / vw), profile, size, outline)
                print(f"{name}: {picture} in {size}x{size} -> {os.path.relpath(out, DATA)}")
                continue
            svg = os.path.join(art, name + ".svg")
            m = re.search(r'data-px="(\d+) (\d+)"', open(svg, encoding="utf-8").read(400))
            if m:
                w, h = int(m.group(1)), int(m.group(2))
                out = os.path.join(DATA, name + ".png")
            else:
                w = h = ICON_PX
                out = os.path.join(DATA, "icons", name + ".png")
            render(browser, svg, out, w, h, profile)
            print(f"{name}: {w}x{h} -> {os.path.relpath(out, DATA)}")
    finally:
        shutil.rmtree(profile, ignore_errors=True)


if __name__ == "__main__":
    main()
