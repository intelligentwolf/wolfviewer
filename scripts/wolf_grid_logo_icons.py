# scripts/wolf_grid_logo_icons.py INDRA_NEWVIEW_DIR : [GRID LOGO 2026-11-12] every WolfViewer logo slot from the round-5
# logo set (/home/paulc/Wolf-Territories-Grid-logo, Paul 2026-11-12). Needs Pillow; run with indra/newview as the argument.
# Small sizes (<= 48) use the set's own favicon crops (README: the head goes muddy under ~36 px); bigger ones are
# downscaled from app-icon-1024 / the 2048 badge. Never upscales.
import sys, os
from PIL import Image
SRC = "/home/paulc/Wolf-Territories-Grid-logo"
DST = sys.argv[1]  # indra/newview
LANCZOS = Image.LANCZOS

app = Image.open(f"{SRC}/3-icons/app-icon-1024.png").convert("RGBA")
fav = {s: Image.open(f"{SRC}/3-icons/favicon-{s}x{s}.png").convert("RGBA") for s in (16, 32, 48)}

def icon(size):
    if size in fav:
        return fav[size].copy()
    assert size <= 1024
    return app.resize((size, size), LANCZOS)

channels = ["beta-os", "development-os", "nightly-os", "private-os", "project-os", "release-os", "test-os"]
ico_sizes = [(16, 16), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]

def save_ico(path):
    # Pillow writes each requested size from the largest image; give it the exact small crops via append_images
    imgs = [icon(s[0]) for s in ico_sizes]
    imgs[-1].save(path, format="ICO", sizes=ico_sizes, append_images=imgs[:-1], bitmap_format="png")

def save_icns(path):
    app.save(path, format="ICNS", append_images=[icon(s) for s in (16, 32, 64, 128, 256, 512)])

for ch in channels:
    d = os.path.join(DST, "icons", ch)
    for s in (16, 32, 48, 128, 256, 512):
        icon(s).save(os.path.join(d, f"firestorm_{s}.png"), optimize=True)
    # Linux SDL window icon (CMakeLists.txt:2356 -> res-sdl/firestorm_icon.BMP, SDL_LoadBMP: 24-bit, no alpha, was on black)
    bg = Image.new("RGBA", (256, 256), (0, 0, 0, 255)); bg.alpha_composite(icon(256))
    bg.convert("RGB").save(os.path.join(d, "firestorm_256.bmp"), format="BMP")
    save_ico(os.path.join(d, "firestorm_icon.ico"))
    save_icns(os.path.join(d, "firestorm_icon.icns"))

save_ico(os.path.join(DST, "installers/windows/firestorm_icon_os.ico"))
save_icns(os.path.join(DST, "firestormos_icon.icns"))
# Linux package icon (viewer_manifest.py:2205 res/firestorm_icon.png -> wolfviewer_icon.png, 512x512 hicolor) - was the Firestorm phoenix
icon(512).save(os.path.join(DST, "res/firestorm_icon.png"), optimize=True)

# login panel logo: 444x444 RGBA with transparent margin (panel_fs_nui_login.xml:52-58 shows it at 125x125)
badge = Image.open(f"{SRC}/1-logo/wolf-territories-grid-logo-on-dark-2048.png").convert("RGBA")
box = 432
w, h = badge.size; k = box / max(w, h)
b = badge.resize((round(w * k), round(h * k)), LANCZOS)
canvas = Image.new("RGBA", (444, 444), (0, 0, 0, 0))
canvas.alpha_composite(b, ((444 - b.width) // 2, (444 - b.height) // 2))
canvas.save(os.path.join(DST, "skins/default/textures/windows/login_fs_logo.png"), optimize=True)

# splash badge: RGB premultiplied over black + a grey alpha mask, both JPEG (wolfsplash.cpp decodes JPEG only)
SPL = 330  # px tall at the largest display scale we draw (3x of a 110 logical px badge)
k = SPL / h
sb = badge.resize((round(w * k), SPL), LANCZOS)
black = Image.new("RGBA", sb.size, (0, 0, 0, 255)); black.alpha_composite(sb)
tex = os.path.join(DST, "skins/default/textures/wolfgame")
black.convert("RGB").save(os.path.join(tex, "wolfsplash_badge.jpg"), quality=94, subsampling=0)
sb.getchannel("A").save(os.path.join(tex, "wolfsplash_badge_alpha.jpg"), quality=95, subsampling=0)

# macOS disk image: volume icon + background (300x180, was "Phoenix Firestorm Project, Inc." / "FIRESTORM VIEWER")
for ch in ("release", "beta", "nightly", "private"):
    dd = os.path.join(DST, f"installers/darwin/firestormos-{ch}-dmg")
    app.save(os.path.join(dd, "_VolumeIcon.icns"), format="ICNS", append_images=[icon(s) for s in (16, 32, 128, 256, 512)])
# DMG window background, 300x180 like the one it replaces ("Phoenix Firestorm Project, Inc." / "FIRESTORM VIEWER"):
# white (Finder draws the icon labels in black), the on-light badge top left, the names in Bricolage Grotesque 800
# (the logo set's own banner face, scripts/BricolageGrotesque-VF.ttf, SIL OFL; axes: optical size, weight, width)
from PIL import ImageDraw, ImageFont
lb = Image.open(f"{SRC}/1-logo/wolf-territories-grid-logo-on-light-512.png").convert("RGBA")
font = f"{SRC}/scripts/BricolageGrotesque-VF.ttf"
bgd = Image.new("RGBA", (300, 180), (255, 255, 255, 255))
bh = 44; lbs = lb.resize((round(lb.width * bh / lb.height), bh), LANCZOS)
bgd.alpha_composite(lbs, (6, 4))
dr = ImageDraw.Draw(bgd)
f1 = ImageFont.truetype(font, 14); f1.set_variation_by_axes([14, 800, 100])
f2 = ImageFont.truetype(font, 13); f2.set_variation_by_axes([13, 800, 100])
dr.text((60, 9), "Wolf Territories Grid", font=f1, fill=(15, 23, 42))
dr.text((300 - 8 - dr.textlength("WOLFVIEWER", font=f2), 160), "WOLFVIEWER", font=f2, fill=(15, 23, 42))
for ch in ("release", "beta", "nightly", "private"):
    bgd.convert("RGB").save(os.path.join(DST, f"installers/darwin/firestormos-{ch}-dmg/background.png"), optimize=True)
print("done")
