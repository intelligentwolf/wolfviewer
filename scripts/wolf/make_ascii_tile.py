#!/usr/bin/env python3
"""[WOLF ASCII 2026-10-24] The Photo Effects tile for ASCII Art: wolfPhotoFilterF.glsl mode 16 re-done step for step over
the "Off" tile's harbour picture (skins/default/textures/wolf_photo/off.png), as the other tiles show their filter run
over it. Writes ascii.png and ascii_sel.png (the _sel tiles add a 3 px (80,160,255) border, as comic_sel.png does).
Glyphs come from make_ascii_glyphs.py (same font, size, threshold, ramp)."""
import math, os, sys
from PIL import Image, ImageDraw, ImageFont
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

HERE = os.path.dirname(os.path.abspath(__file__))
TILES = os.path.join(HERE, "..", "..", "indra", "newview", "skins", "default", "textures", "wolf_photo")
FONT = "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf"
RAMP = " .:-=+*#%@"
W, H = 8, 12

def glyphs():
    font = ImageFont.truetype(FONT, 13)
    out = {}
    for ch in RAMP:
        im = Image.new("L", (W, H), 0)
        d = ImageDraw.Draw(im)
        l, t, r, b = d.textbbox((0, 0), ch, font=font)
        # drawn in the cell's top-left 7x11 so a full-width glyph ('+', '=') never touches its neighbour: the right
        # column and bottom row stay empty, the gap a terminal font leaves between characters
        d.text(((W - 1 - (r - l)) / 2 - l, (H - 1 - (b - t)) / 2 - t), ch, fill=255, font=font)
        im.paste(0, (W - 1, 0, W, H)); im.paste(0, (0, H - 1, W, H))
        g = [[1 if im.getpixel((x, y)) >= 90 else 0 for x in range(W)] for y in range(H)]
        out[ch] = (sum(map(sum, g)), g)
    return [out[c][1] for c in sorted(RAMP, key=lambda c: out[c][0])]   # least ink first, as WP_ASCII

def main():
    # rendered at the tile's own size, one glyph pixel a tile pixel, so the tile shows real, readable characters
    # (25 across) - the real effect draws about 240 across a 1080p screen
    tile = Image.open(os.path.join(TILES, "off.png")).convert("RGB")
    src = tile
    w, h = src.size
    px = src.load()
    lin = lambda v: v / 255.0
    def tap(x, y):   # wpTap: clamp to the frame
        x = min(max(int(x), 0), w - 1); y = min(max(int(y), 0), h - 1)
        return [lin(c) for c in px[x, y]]
    lum = lambda c: 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]
    G = glyphs()
    s = 1.0   # one screen pixel a glyph pixel, as on a 1080-line frame (the shader's s rounded)
    gpx = max(1.0, math.floor(s + 0.5))
    cw, chh = 8 * gpx, 12 * gpx
    out = Image.new("RGB", (w, h))
    o = out.load()
    for y in range(h):
        py = h - 1 - y                                   # shader pixel rows count up from the bottom
        for x in range(w):
            cx, cy = math.floor(x / cw), math.floor(py / chh)
            ux, uy = (cx + 0.5) * cw, (cy + 0.5) * chh
            qx, qy = cw * 0.25, chh * 0.25
            taps = [tap(ux + dx, h - 1 - (uy + dy)) for dx, dy in ((0, 0), (qx, qy), (-qx, qy), (qx, -qy), (-qx, -qy))]
            cc = [sum(t[i] for t in taps) / 5.0 for i in range(3)]
            l = lum(cc)
            g = int(min(max(math.floor(math.sqrt(l) * 10.0), 0), 9))
            gx = int(min(max(math.floor((x - cx * cw) / gpx), 0), 7))
            gy = int(min(max(math.floor((py - cy * chh) / gpx), 0), 11))
            on = G[g][11 - gy][gx]
            sat = [min(max((l + (c - l) * 1.3) * 1.25 + 0.06, 0.0), 1.0) for c in cc]
            bg = (0.015, 0.02, 0.015)
            col = sat if on else bg
            o[x, y] = tuple(int(round(v * 255)) for v in col)
    out.save(os.path.join(TILES, "ascii.png"))
    sel = out.copy()
    d = ImageDraw.Draw(sel)
    for i in range(3):
        d.rectangle([i, i, w - 1 - i, h - 1 - i], outline=(80, 160, 255))
    sel.save(os.path.join(TILES, "ascii_sel.png"))
    print("wrote", os.path.join(TILES, "ascii.png"), "and ascii_sel.png", (w, h))

main()
