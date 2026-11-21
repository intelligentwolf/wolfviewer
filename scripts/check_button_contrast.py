#!/usr/bin/env python3
"""Button label contrast per skin and theme, normal / hover / pressed / selected, computed the way LLButton draws them.

Source of the drawing (indra/llui/llbutton.cpp LLButton::draw):
  - the state image (image_unselected / image_pressed / image_selected ...) is drawn tinted by image_color, alpha-blended;
  - on hover with no image_hover_* the SAME image is drawn again with drawSolid in white at alpha hover_glow_amount, blend
    BT_ADD_WITH_ALPHA (dst += src * src_alpha), i.e. each channel + glow * texel alpha;
  - the label is drawn in label_color (label_color_selected when toggled on); it does not change on hover.
Widget defaults: skins/default/xui/en/widgets/button.xml (image_unselected PushButton_Off, image_selected PushButton_Selected,
hover_glow_amount 0.25, image_color ButtonImageColor, label_color ButtonLabelColor), image_pressed PushButton_Press
(llbutton.cpp Params default is unset; button.xml leaves it to the code default, see PRESSED below).
Colours: a skin's colors.xml, then a theme's colors.xml over it, references followed, falling back to skins/default/colors.xml
(LLUIColorTable loads default first, then the skin, then the theme). Textures: textures.xml (default, skin, theme layered) maps the name to a file, looked up in the
theme's textures folder, then the skin's, then default's (lldir.cpp setSkinFolder search order).
The floater behind a button is FloaterFocusBackgroundColor over a backdrop; the world behind a translucent floater changes,
so the ratio is given for a black and for a mid-grey backdrop and the worse is the one that counts.
Contrast: WCAG 2.x relative luminance.
Usage: scripts/check_button_contrast.py [skins dir]   (prints one line a skin/theme/state; exit 1 when any label is under 4.5:1)
"""
import os, sys, re
import xml.etree.ElementTree as ET
from PIL import Image

SKINS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "indra", "newview", "skins")
GLOW = 0.25
BACKDROPS = {'black': (0.0, 0.0, 0.0), 'grey': (0.5, 0.5, 0.5)}


def load_colors(path, table):
    if not os.path.exists(path):
        return
    for el in ET.parse(path).getroot().iter('color'):
        n = el.get('name')
        val = el.get('value')
        if val is not None and not re.match(r'^[\s0-9.eE+-]+$', val):
            table[n] = ('r', val.strip())   # a value naming another colour (e.g. "SchemeLightest")
        elif val is not None:
            v = [float(x) for x in val.split()]
            if max(v) > 1.0:  # 0-255 form
                v = [x / 255.0 for x in v]
            if len(v) == 3:
                v.append(1.0)
            table[n] = ('v', tuple(v))
        elif el.get('reference') is not None:
            table[n] = ('r', el.get('reference'))


def color(table, name, depth=0):
    e = table.get(name)
    if e is None or depth > 20:
        return None
    if e[0] == 'v':
        return e[1]
    return color(table, e[1], depth + 1)


def layers(skin, theme):
    """The skin folders in the viewer's search order, most general first (lldir.cpp LLDir::setSkinFolder: default, the skin,
    then the theme)."""
    out = ['default']
    if skin != 'default':
        out.append(skin)
    if theme:
        out.append(os.path.join(skin, 'themes', theme))
    return out


def load_textures(skin, theme=''):
    m = {}
    for s in layers(skin, theme):
        p = os.path.join(SKINS, s, 'textures', 'textures.xml')
        if os.path.exists(p):
            for el in ET.parse(p).getroot().iter('texture'):
                if el.get('file_name'):
                    m[el.get('name')] = el.get('file_name')
    return m


def texel(skin, texmap, name, theme=''):
    f = texmap.get(name)
    if not f:
        return None
    for s in reversed(layers(skin, theme)):
        p = os.path.join(SKINS, s, 'textures', f)
        if os.path.exists(p):
            im = Image.open(p).convert('RGBA')
            w, h = im.size
            r, g, b, a = im.getpixel((w // 2, h // 2))   # the stretched middle: where the label sits
            return (r / 255.0, g / 255.0, b / 255.0, a / 255.0), p
    return None


def over(src, dst):
    a = src[3]
    return tuple(src[i] * a + dst[i] * (1 - a) for i in range(3))


def lum(c):
    def ch(x):
        return x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4
    r, g, b = (ch(max(0.0, min(1.0, x))) for x in c[:3])
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def ratio(a, b):
    la, lb = lum(a), lum(b)
    return (max(la, lb) + 0.05) / (min(la, lb) + 0.05)


def button_bg(t, tint, floater, backdrop, glow=0.0, focus=None):
    """The colour behind the label. focus: FocusColor when the button has keyboard focus - LLButton::draw first calls
    drawBorder(imagep, focus colour, 1-3 px), and LLUIImage::drawBorder (llrender/lluiimage.inl) is drawSolid of the WHOLE
    image stretched by the border width, so the focus colour fills the middle too, under the (translucent) button image."""
    c = over(floater, backdrop)
    if focus is not None:
        c = over((focus[0], focus[1], focus[2], focus[3] * t[3]), c)
    img = (t[0] * tint[0], t[1] * tint[1], t[2] * tint[2], t[3] * tint[3])
    c = over(img, c)
    if glow:
        c = tuple(min(1.0, x + glow * t[3]) for x in c)   # BT_ADD_WITH_ALPHA, white, alpha glow * texel alpha
    return c


def ring(t, floater, backdrop, focus):
    """The focus ring itself (the 1-3 px outside the image): the image edge's alpha x FocusColor over the floater, and the
    floater beside it."""
    f = over(floater, backdrop)
    return over((focus[0], focus[1], focus[2], focus[3] * t), f), f


def button_params(skin):
    """The <button> widget defaults as the viewer layers them (LLUICtrlFactory::loadWidgetTemplate ->
    LLXMLNode::getLayeredXMLNode: skins/default/xui/en/widgets/button.xml, then the skin's own, attribute by attribute)."""
    p = {}
    for s in ('default', skin):
        f = os.path.join(SKINS, s, 'xui', 'en', 'widgets', 'button.xml')
        if os.path.exists(f) and (s == 'default' or skin != 'default'):
            p.update(ET.parse(f).getroot().attrib)
    if os.environ.get('GLOW') is not None:   # what-if: python3 check_button_contrast.py with GLOW=0.15
        p['hover_glow_amount'] = os.environ['GLOW']
    return p


def hexc(c):
    return '#%02x%02x%02x' % tuple(int(round(max(0, min(1, x)) * 255)) for x in c[:3])


def themes(skin):
    out = [('', None)]
    td = os.path.join(SKINS, skin, 'themes')
    if os.path.isdir(td):
        for t in sorted(os.listdir(td)):
            if os.path.exists(os.path.join(td, t, 'colors.xml')):
                out.append((t, os.path.join(td, t, 'colors.xml')))
    return out


def evaluate(skin, theme, tpath, extra=None):
    """Every state of a <button> in one skin/theme: [(state, ratio, label hex, bg hex, backdrop)], plus the focus ring ratio.
    extra: colour overrides {name: (r, g, b, a)} for trying a fix before writing it."""
    texmap = load_textures(skin, theme)
    table = {}
    load_colors(os.path.join(SKINS, 'default', 'colors.xml'), table)
    if skin != 'default':
        load_colors(os.path.join(SKINS, skin, 'colors.xml'), table)
    if tpath:
        load_colors(tpath, table)
    for k, v in (extra or {}).items():
        table[k] = ('v', tuple(v))
    floater = color(table, 'FloaterFocusBackgroundColor')
    bp = button_params(skin)
    tint = color(table, bp.get('image_color', 'ButtonImageColor')) or (1, 1, 1, 1)
    label = color(table, bp.get('label_color', 'ButtonLabelColor'))
    label_sel = color(table, bp.get('label_color_selected', 'ButtonLabelSelectedColor'))
    off, sel = bp.get('image_unselected', 'PushButton_Off'), bp.get('image_selected', 'PushButton_Selected')
    focus = color(table, 'FocusColor')
    glow = float(bp.get('hover_glow_amount', GLOW))
    # pressed: no image_pressed in button.xml, so LLButton uses image_selected (llbutton.cpp "if
    # (!p.image_pressed.isProvided()) mImagePressed = mImageSelected"), with the unselected label colour
    states = [('normal', off, label, 0.0, False), ('hover', off, label, glow, False),
              ('focus', off, label, 0.0, True), ('focus+hover', off, label, glow, True),
              ('pressed', sel, label, 0.0, True),
              ('selected', sel, label_sel, 0.0, False), ('selected+hover', sel, label_sel, glow, False),
              ('selected+focus+hover', sel, label_sel, glow, True)]
    rows = []
    for st, tex, lab, gl, foc in states:
        tx = texel(skin, texmap, tex, theme)
        if tx is None or lab is None or floater is None or (foc and focus is None):
            rows.append((st, None, '', '', 'missing'))
            continue
        rs = []
        for bname, bd in BACKDROPS.items():
            bg = button_bg(tx[0], tint, floater, bd, gl, focus if foc else None)
            fg = over(lab, bg)
            rs.append((ratio(fg, bg), bname, bg, fg))
        r, bname, bg, fg = min(rs)
        rows.append((st, r, hexc(fg), hexc(bg), bname))
    ringr = None
    if focus is not None:
        edge = 0.5   # PushButton_Off's edge texels: alpha 128 (the image's outer row)
        ringr = min(ratio(*ring(edge, floater, bd, focus)) for bd in BACKDROPS.values())
    return rows, ringr, glow


def all_sets():
    for skin in sorted(d for d in os.listdir(SKINS) if os.path.exists(os.path.join(SKINS, d, 'colors.xml'))):
        for theme, tpath in themes(skin):
            yield skin, theme, tpath


def main():
    worst = 99.0
    bad = []
    for skin, theme, tpath in all_sets():
        rows, ringr, glow = evaluate(skin, theme, tpath)
        name = f'{skin}/{theme or "-"}'
        for st, r, fg, bg, bname in rows:
            if r is None:
                print(f'{name} {st}: missing')
                continue
            flag = '' if r >= 7 else (' AA' if r >= 4.5 else ' FAIL')
            print(f'{name} {st:21s} {r:5.2f}:1 (label {fg} on {bg}, {bname} backdrop){flag}')
            worst = min(worst, r)
            if r < 4.5:
                bad.append(f'{name} {st}')
        if ringr is not None:
            print(f'{name} focus ring vs window   {ringr:5.2f}:1 (glow {glow})')
    print('worst label %.2f:1' % worst)
    if bad:
        print('UNDER 4.5:1: ' + ', '.join(bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
