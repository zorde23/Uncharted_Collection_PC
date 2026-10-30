# SPDX-License-Identifier: GPL-2.0-or-later
"""Keyboard/mouse (or generic Xbox-style pad) button icons for the games' text (launcher: Controls >
icons in the game).

The three games draw the buttons inside their texts with one bitmap font, data/fonts/ps4/symbol3
(art/general/textures/font/ps4/ in Uncharted 1): a 512x512 TGA with 32 glyphs, the same .fnt and
layout in all three. This draws a new atlas from the player's key bindings (input.ini names) and
writes a copy of the game's data.psarc with only that texture replaced; the port mounts the copy
over /app0/uXdata/data.psarc. Needs Pillow."""
import colorsys, struct, zlib
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

# Glyph id -> (game button, as launcher action or special name), from the atlas layout.
GLYPHS = {1: 'dpad', 12: 'dpad_lr', 13: 'dpad_ud', 18: 'up', 21: 'down',
          2: 'circle', 3: 'square', 4: 'triangle', 5: 'cross',
          6: 'r1', 8: 'l1', 7: 'r2', 9: 'l2', 40: 'r1', 42: 'l1', 41: 'r2', 43: 'l2',
          10: 'stick', 15: 'ls', 16: 'rs', 45: 'ls', 46: 'rs',
          24: 'l3', 25: 'r3', 47: 'l3', 48: 'r3',
          11: 'options', 44: 'options', 14: 'touchpad_left',
          29: 'touchpad_right', 49: 'touchpad_right'}
# Short labels for long key names (input.ini names), Portuguese and English.
SHORT = {'Space': 'Espaço', 'Left Ctrl': 'Ctrl', 'Right Ctrl': 'Ctrl D', 'Left Shift': 'Shift',
         'Right Shift': 'Shift D', 'Left Alt': 'Alt', 'Right Alt': 'Alt Gr', 'Return': 'Enter', 'Escape': 'Esc',
         'Backspace': 'Bksp', 'Wheel Up': 'Roda ↑', 'Wheel Down': 'Roda ↓', 'CapsLock': 'Caps',
         'PageUp': 'PgUp', 'PageDown': 'PgDn', 'Delete': 'Del', 'Up': '↑', 'Down': '↓', 'Left': '←', 'Right': '→'}
SHORT_EN = {**SHORT, 'Space': 'Space', 'Right Ctrl': 'R Ctrl', 'Right Shift': 'R Shift',
            'Wheel Up': 'Wheel ↑', 'Wheel Down': 'Wheel ↓'}
MOUSE = {'Mouse Left': 'left', 'Mouse Right': 'right', 'Mouse Middle': 'middle',
         'Wheel Up': 'middle', 'Wheel Down': 'middle'}
INK, CAP, EDGE, HI = (236, 228, 214, 255), (38, 34, 32, 255), (150, 140, 120, 255), (230, 120, 40, 255)


def parse_fnt(text):
    chars = {}
    for line in text.splitlines():
        if line.startswith('char '):
            kv = dict(p.split('=') for p in line.split()[1:])
            chars[int(kv['id'])] = tuple(int(kv[k]) for k in ('x', 'y', 'width', 'height'))
    return chars


def first_key(keys, action):
    return (keys.get(action) or '').split('|')[0]


# Every glyph is drawn as the same key: CAP_H tall, top-aligned in its rectangle (BMFont places all
# glyphs by their top edge), at least square, with one label size.
CAP_H, CAP_TOP, LABEL = 40, 3, 20


def cap_rect(box, inner_w):
    x, y, w, h = box
    cw = min(max(CAP_H, inner_w), w - 2)
    cx = x + (w - cw) // 2
    return (cx, y + CAP_TOP, cx + cw - 1, y + CAP_TOP + CAP_H - 1)


def frame(d, r):
    d.rounded_rectangle(r, radius=8, fill=CAP, outline=EDGE, width=2)


# Keycaps in the game's style: the cap is the game's own touchpad button (glyph 14 of the atlas, the
# journal button of the HUD), its ends kept and its clean middle repeated to the key's width; the hand
# in the middle of it is never used. Made on the player's PC from their game files.
KEY_MATERIAL = None  # RGBA; build_atlas sets it from the game's atlas


def trim(img):
    """`img` cut to its visible part."""
    box = img.getchannel('A').point(lambda v: 255 if v > 40 else 0).getbbox()
    return img.crop(box) if box else img


def game_key(w, h, lit=False, material=None):
    """A keycap w x h from the game's button (`material`, else KEY_MATERIAL), or None without one.
    `lit` turns it orange (the key to press now, in the HUD's turning-stick prompts)."""
    mat = material or KEY_MATERIAL
    if mat is None:
        return None
    w, h = max(2, int(round(w))), max(2, int(round(h)))
    mw = max(4, round(mat.width * h / mat.height))
    m = mat.resize((mw, h), Image.LANCZOS)
    b = min(int(mw * 0.25), w // 2)
    out = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    out.paste(m.crop((0, 0, b, h)), (0, 0))
    out.paste(m.crop((mw - b, 0, mw, h)), (w - b, 0))
    strip = m.crop((b, 0, b + max(1, int(mw * 0.08)), h))  # clean part left of the hand
    for x in range(b, w - b, strip.width):
        out.paste(strip.crop((0, 0, min(strip.width, w - b - x), h)), (x, 0))
    if lit:
        lum = out.convert('L').point(lambda v: min(255, v * 2 + 40))
        tint = [lum.point(lambda v, c=c: v * c // 255) for c in HI[:3]]
        out = Image.merge('RGBA', (*tint, out.getchannel('A')))
    return out


def key_cluster(img, box, names, font_path):
    """The four movement keys as on the keyboard (up above left, down, right), each a small game
    keycap with its letter; `names` in the order up, left, down, right."""
    x, y, w, h = box
    k = min((w - 2) / 3.15, (h - 2) / 2.1)
    gap = k * 0.075
    gx, gy = x + (w - (3 * k + 2 * gap)) / 2, y + (h - (2 * k + gap)) / 2
    f = ImageFont.truetype(font_path, int(k * 0.62))
    d = ImageDraw.Draw(img)
    for label, (i, j) in zip(names, ((1, 0), (0, 1), (1, 1), (2, 1))):
        bx, by = gx + i * (k + gap), gy + j * (k + gap)
        img.alpha_composite(game_key(k, k), (int(round(bx)), int(round(by))))
        l, t, r, b = d.textbbox((0, 0), label, font=f)
        tx, ty = bx + k / 2 - (r - l) / 2 - l, by + k / 2 - (b - t) / 2 - t
        d.text((tx + 1, ty + 1), label, font=f, fill=(0, 0, 0, 255))  # the game's dark shadow
        d.text((tx, ty), label, font=f, fill=INK)


def keycap(img, box, label, font_path):
    """A key with the label at LABEL size; a label wider than the key can get is squeezed sideways,
    so every label keeps the same height."""
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(font_path, LABEL)
    l, t, r_, b = d.textbbox((0, 0), label, font=f)
    _, ct, _, cb = d.textbbox((0, 0), 'H', font=f)
    tw, room = r_ - l, box[2] - 2 - 10
    r = cap_rect(box, min(tw, room) + 10)
    cap = game_key(r[2] - r[0] + 1, r[3] - r[1] + 1)
    if cap:
        img.alpha_composite(cap, (r[0], r[1]))
    else:
        frame(d, r)
    text = Image.new('RGBA', (tw, b - t), (0, 0, 0, 0))
    ImageDraw.Draw(text).text((-l, -t), label, font=f, fill=INK)
    if tw > room:
        text = text.resize((room, text.height), Image.LANCZOS)
    if cap:  # a dark shadow, as the game's lettering
        shadow = Image.new('RGBA', text.size, (0, 0, 0, 0))
        shadow.putalpha(text.getchannel('A'))
        img.alpha_composite(shadow, (int((r[0] + r[2] + 1 - text.width) / 2) + 1, int((r[1] + r[3] + 1 - (cb - ct)) / 2 - ct + t) + 1))
    # Centered on the cap height of the font, so all labels sit on the same line.
    img.alpha_composite(text, (int((r[0] + r[2] + 1 - text.width) / 2), int((r[1] + r[3] + 1 - (cb - ct)) / 2 - ct + t)))


def mouse(img, box, which):
    """A mouse with no key around it, as tall as the keys, the used button in orange."""
    d = ImageDraw.Draw(img)
    x, y, w, h = box
    mw, mh = 28, CAP_H
    ox, oy = x + (w - mw) // 2, y + CAP_TOP
    d.rounded_rectangle((ox, oy, ox + mw - 1, oy + mh - 1), radius=mw // 2, fill=CAP, outline=EDGE, width=2)
    split = oy + mh * 0.45
    if which == 'left':
        d.pieslice((ox + 2, oy + 2, ox + mw - 3, oy + mw - 3), 180, 270, fill=HI)
        d.rectangle((ox + 2, oy + mw / 2, ox + mw / 2, split), fill=HI)
    elif which == 'right':
        d.pieslice((ox + 2, oy + 2, ox + mw - 3, oy + mw - 3), 270, 360, fill=HI)
        d.rectangle((ox + mw / 2, oy + mw / 2, ox + mw - 3, split), fill=HI)
    d.line((ox + mw / 2, oy, ox + mw / 2, split), fill=EDGE, width=2)
    d.line((ox + 2, split, ox + mw - 3, split), fill=EDGE, width=2)
    if which == 'middle':
        d.rounded_rectangle((ox + mw / 2 - 4, oy + 6, ox + mw / 2 + 4, oy + 16), radius=4, fill=HI)


# ---- Xbox-style controller ("xbox") -----------------------------------------------------------
# Made on the player's PC from the game's own button art: face buttons keep the game's dark disc and
# glow (hue changed) with A/B/X/Y painted where the symbol was, shoulders and triggers keep their shape
# with LB/RB and LT/RT in the game's lettering colour, Options/Share become Menu/View. No Microsoft
# artwork and no game art in the zip. The d-pad and the sticks are the same on both pads, so they stay
# the game's own. If a glyph does not look as expected, a plain drawing of our own is used instead.
XBOX_FACE = {'cross': ('A', (110, 190, 70)), 'circle': ('B', (225, 70, 60)),
             'square': ('X', (70, 135, 230)), 'triangle': ('Y', (235, 195, 50))}
XBOX_LABEL = {'l1': 'LB', 'r1': 'RB', 'l2': 'LT', 'r2': 'RT', 'l3': 'LS', 'r3': 'RS',
              'options': 'MENU', 'touchpad_left': 'VIEW', 'touchpad_right': 'VIEW'}
XBOX_GLYPHS = set(XBOX_FACE) | set(XBOX_LABEL)
DARK, GLOW = (34, 31, 29, 255), (226, 214, 190, 255)
XBOX_HUE = {'cross': 0.30, 'circle': 0.0, 'square': 0.60, 'triangle': 0.14}
# Where the old label is looked for (fractions of the glyph): the stick tops and Options/Share.
XBOX_REGION = {'l3': (0.3, 0.2, 0.7, 0.8), 'r3': (0.3, 0.2, 0.7, 0.8),
               'options': (0, 0, 1, 0.45), 'touchpad_right': (0, 0, 1, 0.45)}


def _hsv(img):
    return img.convert('RGB').convert('HSV')


def rehue(img, hue):
    """Pixels with colour keep their saturation and brightness and take `hue`."""
    a = img.getchannel('A')
    h, s, v = _hsv(img).split()
    h = h.point(lambda _: int(hue * 255))
    out = Image.merge('HSV', (h, s, v)).convert('RGB')
    out.putalpha(a)
    return out


def inpaint(img, mask, radius, source=None):
    """Pixels under `mask` (L, 255 = replace) filled from their surroundings (normalized blur);
    only `source` pixels feed the fill. Pixels far from any source use a wider blur."""
    rgb = img.convert('RGB')
    keep = ImageChops.invert(mask)
    if source is not None:
        keep = ImageChops.multiply(keep, source)
    chans = [ImageChops.multiply(c, keep) for c in rgb.split()]
    out = rgb.copy(); po = out.load(); pm = mask.load()
    w, h = out.size
    todo = [(x, y) for y in range(h) for x in range(w) if pm[x, y] > 127]
    r = radius
    while todo and r < max(w, h) * 2:
        blur = lambda im: im.filter(ImageFilter.GaussianBlur(r))
        num = [blur(c).load() for c in chans]
        den = blur(keep).load()
        left = []
        for x, y in todo:
            d = den[x, y]
            if d > 12:
                po[x, y] = tuple(min(255, n[x, y] * 255 // d) for n in num)
            else:
                left.append((x, y))
        todo, r = left, r * 2
    out.putalpha(img.getchannel('A'))
    return out


def _bbox(pred, img, region=None):
    w, h = img.size
    x0, y0, x1, y1 = region or (0, 0, w, h)
    px = img.load()
    xs, ys = [], []
    for y in range(y0, y1):
        for x in range(x0, x1):
            if pred(px[x, y]):
                xs.append(x); ys.append(y)
    return (min(xs), min(ys), max(xs) + 1, max(ys) + 1) if xs else None


def _sat(p):
    r, g, b = p[:3]
    mx, mn = max(r, g, b), min(r, g, b)
    return 0 if mx == 0 else (mx - mn) / mx


def _font(path, size):
    return ImageFont.truetype(path, max(6, int(size)))


def _letter_layer(size, label, font_path, box, colour, squash=1.0, fill_frac=0.62):
    """RGBA layer with `label` centered in `box`, height fill_frac of the box height."""
    x0, y0, x1, y1 = box
    bw, bh = x1 - x0, y1 - y0
    target_h = bh * fill_frac
    f = _font(font_path, target_h / squash * 1.35)
    d = ImageDraw.Draw(Image.new('L', (1, 1)))
    l, t, r, b = d.textbbox((0, 0), label, font=f)
    _, ct, _, cb = d.textbbox((0, 0), 'H', font=f)
    scale = target_h / ((cb - ct) * squash)
    f = _font(font_path, f.size * scale)
    l, t, r, b = d.textbbox((0, 0), label, font=f)
    _, ct, _, cb = d.textbbox((0, 0), 'H', font=f)
    m = Image.new('L', (r - l + 4, b - t + 4), 0)
    ImageDraw.Draw(m).text((2 - l, 2 - t), label, font=f, fill=255)
    if m.width > bw * 0.9:
        m = m.resize((int(bw * 0.9), m.height), Image.LANCZOS)
    m = m.resize((m.width, max(1, int(m.height * squash))), Image.LANCZOS)
    layer = Image.new('L', size, 0)
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    top = cy - (cb - ct) * squash / 2 - (ct - t + 2) * squash
    layer.paste(m, (int(cx - m.width / 2), int(top)))
    return layer


def restyle_face(src, act, font_path, zone=None, squash=None):
    """The game's own face button (any of the four) as A/B/X/Y: the symbol painted out from the dark
    disc around it, the hue of the glow changed, the letter drawn in its place with a glow. `zone`
    (the top of a 3D button, seen at an angle) is where the symbol is; flat buttons find their disc."""
    img = src.convert('RGBA')
    w, h = img.size
    if zone:  # the top of a 3D button, seen at an angle: its place is given
        ins = zone
    else:  # flat button: the dark disc, symbol inside
        dark = _bbox(lambda p: p[3] > 200 and max(p[:3]) < 70, img)
        if not dark:
            return None
        dw, dh = dark[2] - dark[0], dark[3] - dark[1]
        ins = (dark[0] + dw * 0.13, dark[1] + dh * 0.13, dark[2] - dw * 0.13, dark[3] - dh * 0.13)
    rw, rh = ins[2] - ins[0], ins[3] - ins[1]
    squash = squash or rh / rw
    zmask = Image.new('L', img.size, 0)
    ImageDraw.Draw(zmask).ellipse(ins, fill=255)
    hsv = _hsv(img)
    sym = zmask  # the whole inside is repainted from the dark rim: no trace of the old symbol
    darkpix = Image.eval(hsv.split()[2], lambda v: 255 if v < 80 else 0)
    plain = Image.eval(hsv.split()[1], lambda s: 255 if s < 90 else 0)
    out = inpaint(img, sym, max(2, rw * (0.10 if zone else 0.18)), source=ImageChops.multiply(darkpix, plain))
    out = rehue(out, XBOX_HUE[act])
    colour = tuple(int(c * 255) for c in colorsys.hsv_to_rgb(XBOX_HUE[act], 0.62, 1.0))
    let = _letter_layer(img.size, XBOX_FACE[act][0], font_path, ins, colour, squash=squash, fill_frac=0.56 if not zone else 0.62)
    glow = let.filter(ImageFilter.GaussianBlur(max(1, rw * 0.04)))
    out.paste(Image.new('RGB', img.size, tuple(c // 2 for c in colour)), (0, 0), glow)
    out.paste(Image.new('RGB', img.size, colour), (0, 0), let)
    out.putalpha(img.getchannel('A'))
    return out


def _alpha_after(img, mask, radius, *layers):
    """Alpha with the old text painted out (from its surroundings) and the new layers added."""
    a = img.getchannel('A')
    a2 = inpaint(Image.merge('RGBA', (a, a, a, Image.new('L', a.size, 255))), mask, radius).getchannel('R')
    for l in layers:
        a2 = ImageChops.lighter(a2, l)
    return a2


def restyle_label(src, label, font_path, region=None, colour=None):
    """The game's own button with its text painted out and `label` written in the same place and colour.
    `region` (fractions of the image) limits where the text is looked for."""
    img = src.convert('RGBA')
    w, h = img.size
    fx0, fy0, fx1, fy1 = region or (0.12, 0.12, 0.88, 0.88)
    zone_box = (int(w * fx0), int(h * fy0), int(w * fx1), int(h * fy1))
    def is_text(p):
        return p[3] > 100 and min(p[:3]) > 150 and _sat(p) < 0.35
    tb = _bbox(is_text, img, zone_box)
    if not tb:  # Uncharted 1: dark letters cut into the grey cap
        return _restyle_engraved(img, label, font_path, zone_box)
    px = img.load()
    vals = [px[x, y][:3] for y in range(tb[1], tb[3]) for x in range(tb[0], tb[2]) if is_text(px[x, y])]
    colour = colour or tuple(sum(v[i] for v in vals) // len(vals) for i in range(3))
    mask = Image.new('L', img.size, 0)
    mp = mask.load()
    for y in range(tb[1], tb[3]):
        for x in range(tb[0], tb[2]):
            p = px[x, y]
            if p[3] > 60 and max(p[:3]) > 90:
                mp[x, y] = 255
    mask = mask.filter(ImageFilter.MaxFilter(5))
    zone = Image.new('L', img.size, 0)
    ImageDraw.Draw(zone).rectangle(zone_box, fill=255)
    mask = ImageChops.multiply(mask, zone)
    out = inpaint(img, mask, max(2, (tb[3] - tb[1]) * 0.35))
    th = tb[3] - tb[1]
    cx = (tb[0] + tb[2]) / 2
    room = (zone_box[2] - zone_box[0])
    box = (cx - room / 2, tb[1], cx + room / 2, tb[3])
    let = _letter_layer(img.size, label, font_path, box, colour, fill_frac=1.0)
    shadow = let.filter(ImageFilter.GaussianBlur(max(1, th * 0.08)))
    out.paste(Image.new('RGB', img.size, (0, 0, 0)), (max(1, int(th * 0.05)), max(1, int(th * 0.05))), shadow)
    out.paste(Image.new('RGB', img.size, colour), (0, 0), let)
    out.putalpha(_alpha_after(img, mask, max(2, th * 0.35), let, shadow))
    return out


def _restyle_engraved(img, label, font_path, zone_box):
    """restyle_label for a cap with dark engraved letters (a light edge under them): the cap is
    the mid-grey part, the letters the dark pixels inside it."""
    v = _hsv(img).split()[2]
    low = img.convert('RGB').convert('HSV').split()[1].point(lambda s: 255 if s < 90 else 0)
    cap = ImageChops.multiply(v.point(lambda x: 255 if 45 <= x <= 160 else 0), low)
    whole = cap.filter(ImageFilter.MaxFilter(11)).filter(ImageFilter.MinFilter(11))  # cap with its letters
    inner = whole.filter(ImageFilter.MinFilter(9)).filter(ImageFilter.MinFilter(9)).filter(ImageFilter.MinFilter(7))  # away from the dark outline
    zone = Image.new('L', img.size, 0)
    ImageDraw.Draw(zone).rectangle(zone_box, fill=255)
    inner = ImageChops.multiply(inner, zone)
    dark = ImageChops.multiply(inner, v.point(lambda x: 255 if x < 45 else 0))
    tb = dark.getbbox()
    if not tb or tb[3] - tb[1] < img.height * 0.08:
        return None
    edge = ImageChops.multiply(inner, v.point(lambda x: 255 if x > 110 else 0))
    mask = ImageChops.multiply(ImageChops.lighter(dark, edge).filter(ImageFilter.MaxFilter(5)), inner)
    th = tb[3] - tb[1]
    out = inpaint(img, mask, max(2, th * 0.3), source=cap)
    cx = (tb[0] + tb[2]) / 2
    room = zone_box[2] - zone_box[0]
    let = _letter_layer(img.size, label, font_path, (cx - room / 2, tb[1], cx + room / 2, tb[3]), (0, 0, 0), fill_frac=1.0)
    off = max(1, int(th * 0.04))
    out.paste(Image.new('RGB', img.size, (140, 138, 128)), (off, off), let)  # the light edge
    out.paste(Image.new('RGB', img.size, (14, 13, 12)), (0, 0), let)
    out.putalpha(img.getchannel('A'))
    return out


def _centered(d, rect, label, font_path, size, fill):
    f = ImageFont.truetype(font_path, size)
    l, t, r_, b = d.textbbox((0, 0), label, font=f)
    while r_ - l > rect[2] - rect[0] - 6 and size > 8:
        size -= 1
        f = ImageFont.truetype(font_path, size)
        l, t, r_, b = d.textbbox((0, 0), label, font=f)
    _, ct, _, cb = d.textbbox((0, 0), 'H', font=f)
    d.text(((rect[0] + rect[2] + 1 - (r_ - l)) / 2 - l, (rect[1] + rect[3] + 1 - (cb - ct)) / 2 - ct), label, font=f, fill=fill)


def xbox_glyph(img, box, button, font_path, source=None):
    """One controller glyph in `box`, restyled from the game's glyph there (or at `source`, a box of
    the same atlas, centred in `box`)."""
    x, y, w, h = box
    sx, sy, sw, sh = source or box
    old = img.crop((sx, sy, sx + sw, sy + sh))
    try:
        if button in XBOX_FACE:
            new = restyle_face(old, button, font_path)
        else:
            src_button = 'touchpad_right' if button == 'touchpad_left' else button
            new = restyle_label(old, XBOX_LABEL[button], font_path, XBOX_REGION.get(src_button))
    except Exception:
        new = None
    if new is None:
        return _xbox_plain(img, box, button, font_path)
    ImageDraw.Draw(img).rectangle((x, y, x + w - 1, y + h - 1), fill=(0, 0, 0, 0))
    if new.width > w or new.height > h:
        new.thumbnail((w, h), Image.LANCZOS)
    img.paste(new, (x + (w - new.width) // 2, y + (h - new.height) // 2))


def _xbox_plain(img, box, button, font_path):
    """One controller glyph drawn in `box` (cleared first): a plain drawing of our own."""
    d = ImageDraw.Draw(img)
    x, y, w, h = box
    d.rectangle((x, y, x + w - 1, y + h - 1), fill=(0, 0, 0, 0))
    if button in XBOX_FACE:
        letter, colour = XBOX_FACE[button]
        side = min(w, h) - 8
        cx, cy = x + w / 2, y + h / 2
        r = (cx - side / 2, cy - side / 2, cx + side / 2, cy + side / 2)
        d.ellipse(r, fill=DARK, outline=colour + (255,), width=3)
        _centered(d, r, letter, font_path, int(side * 0.55), colour + (255,))
        return
    label = XBOX_LABEL[button]
    if button in ('l3', 'r3'):  # stick pressed in: a round stick top
        side = min(w, h) - 6
        cx, cy = x + w / 2, y + h / 2
        r = (cx - side / 2, cy - side / 2, cx + side / 2, cy + side / 2)
        d.ellipse(r, fill=DARK, outline=GLOW, width=2)
        _centered(d, r, label, font_path, int(side * 0.38), GLOW)
        return
    if button in ('options', 'touchpad_left', 'touchpad_right'):  # small round button, name above
        r = (x + 4, y + 2, x + w - 5, y + h - 3)
        d.rounded_rectangle(r, radius=10, fill=DARK, outline=GLOW, width=2)
        _centered(d, r, label, font_path, 18, GLOW)
        return
    # Bumpers (wide, flat) and triggers (taller, rounded top).
    if button in ('l2', 'r2'):
        r = (x + 6, y + 4, x + w - 7, y + h - 5)
        d.rounded_rectangle(r, radius=14, fill=DARK, outline=GLOW, width=2)
    else:
        r = (x + 3, y + 6, x + w - 4, y + h - 7)
        d.rounded_rectangle(r, radius=10, fill=DARK, outline=GLOW, width=2)
    _centered(d, r, label, font_path, 22, GLOW)


def draw_glyph(img, box, button, keys, font_path, short=SHORT):
    if button in ('dpad', 'dpad_lr', 'dpad_ud'):
        acts = {'dpad': ('up', 'down', 'left', 'right'), 'dpad_lr': ('left', 'right'), 'dpad_ud': ('up', 'down')}[button]
        names = [short.get(first_key(keys, a), first_key(keys, a)) for a in acts]
        if button == 'dpad' and sorted(names) == ['1', '2', '3', '4']:
            names = ['1-4']
        return keycap(img, box, '/'.join(n for n in names if n) or '?', font_path)
    if button == 'ls':
        names = [first_key(keys, a) for a in ('ls_up', 'ls_left', 'ls_down', 'ls_right')]
        if all(len(n) == 1 for n in names) and KEY_MATERIAL is not None:
            return key_cluster(img, box, names, font_path)
        names = ''.join(names)
        return keycap(img, box, names if len(names) == 4 else 'Mover', font_path)
    if button in ('rs', 'stick'):
        return mouse(img, box, None)
    key = first_key(keys, button)
    if key in MOUSE:
        return mouse(img, box, MOUSE[key])
    keycap(img, box, short.get(key, key) or '?', font_path)


def build_atlas(fnt_text, keys, font_path, size=512, short=SHORT, style='keys', original=None):
    """style 'keys': every glyph drawn from the key bindings; 'xbox': the game's atlas (`original`)
    with the PlayStation-only glyphs redrawn as a generic Xbox-style pad."""
    chars = parse_fnt(fnt_text)
    if style == 'xbox':
        img = original.convert('RGBA').copy()
        for gid, button in GLYPHS.items():
            if gid in chars and button in XBOX_GLYPHS:
                # The touchpad picture becomes the View button, made from the Share glyph.
                source = chars.get(29) if button == 'touchpad_left' else None
                xbox_glyph(img, chars[gid], button, font_path, source)
        return img
    global KEY_MATERIAL
    KEY_MATERIAL = None
    if original is not None and 14 in chars:  # the touchpad button
        x, y, w, h = chars[14]
        KEY_MATERIAL = trim(original.convert('RGBA').crop((x, y, x + w, y + h)))
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    for gid, button in GLYPHS.items():
        if gid in chars:
            draw_glyph(img, chars[gid], button, keys, font_path, short)
    return img


def tga_bytes(img, like):
    """An uncompressed TGA with the header layout of the original (`like`: its bytes)."""
    bpp, desc = like[16], like[17]
    w, h = img.size
    rows = range(h) if desc & 0x20 else range(h - 1, -1, -1)
    px = img.convert('RGBA').load()
    out = bytearray(like[:18])
    for yy in rows:
        for xx in range(w):
            r, g, b, a = px[xx, yy]
            out += bytes((b, g, r, a) if bpp == 32 else (b, g, r))
    return bytes(out) + like[18 + w * h * (bpp // 8):]


def alpha_tga(img, like):
    a = img.getchannel('A')
    return tga_bytes(Image.merge('RGBA', (a, a, a, a)), like)


# ---- psarc ------------------------------------------------------------------------------------
def read_psarc(path):
    data = open(path, 'rb').read()
    magic, ver, comp, toclen, esz, n, bs, flags = struct.unpack('>4sI4sIIIII', data[:32])
    assert magic == b'PSAR' and esz == 30
    toc = data[32:toclen]
    width = {65536: 2, 16777216: 3}.get(bs, 4)
    zt = toc[n * esz:]
    zs = [int.from_bytes(zt[i:i + width], 'big') for i in range(0, len(zt) - width + 1, width)]
    entries = []
    for i in range(n):
        e = toc[i * esz:(i + 1) * esz]
        entries.append({'md5': e[:16], 'block': struct.unpack('>I', e[16:20])[0],
                        'size': int.from_bytes(e[20:25], 'big'), 'offset': int.from_bytes(e[25:30], 'big')})
    return {'data': data, 'head': (ver, comp, flags, bs, width), 'entries': entries, 'zs': zs}


def read_entry(arc, e):
    bs, out, b, off = arc['head'][3], bytearray(), e['block'], e['offset']
    while len(out) < e['size']:
        z = arc['zs'][b]
        raw = arc['data'][off:off + (z or bs)]
        try:
            out += zlib.decompress(raw) if z else raw
        except zlib.error:
            out += raw  # stored block that is shorter than the block size
        off += z or bs
        b += 1
    return bytes(out[:e['size']])


def names_of(arc):
    return read_entry(arc, arc['entries'][0]).decode('utf8').split('\n')


def write_psarc(arc, replace, path):
    """Copies the archive with the entries named in `replace` ({name: bytes}) replaced; a new entry
    may change size as long as it keeps its block count. Everything else stays byte for byte: the new
    blocks go at the end of the file and only the entry's size, offset and block sizes in the table
    change (same block count, so the table keeps its length)."""
    ver, comp, flags, bs, width = arc['head']
    names = names_of(arc)
    data = bytearray(arc['data'])
    toclen = struct.unpack('>I', data[12:16])[0]
    n = len(arc['entries'])
    for i, name in enumerate(names, 1):
        if name not in replace:
            continue
        e, payload = arc['entries'][i], replace[name]
        assert -(-len(payload) // bs) == -(-e['size'] // bs), (name, len(payload), e['size'])
        offset = len(data)
        for j, pos in enumerate(range(0, len(payload), bs)):
            chunk = payload[pos:pos + bs]
            z = zlib.compress(chunk, 9)
            stored, field = (z, len(z)) if len(z) < len(chunk) else (chunk, 0 if len(chunk) == bs else len(chunk))
            assert field < (1 << (8 * width))
            data += stored
            zpos = 32 + 30 * n + width * (e['block'] + j)
            data[zpos:zpos + width] = field.to_bytes(width, 'big')
        tpos = 32 + 30 * (i)
        data[tpos + 20:tpos + 25] = len(payload).to_bytes(5, 'big')
        data[tpos + 25:tpos + 30] = offset.to_bytes(5, 'big')
    assert toclen == 32 + 30 * n + width * len(arc['zs'])
    open(path, 'wb').write(bytes(data))


def make_icon_psarc(src, dst, keys, font_path, preview=None, english=False, style='keys'):
    arc = read_psarc(src)
    names = names_of(arc)
    idx = {n: i + 1 for i, n in enumerate(names)}
    fnt = next(n for n in names if n.endswith('/symbol3.fnt'))
    tga = fnt[:-4] + '_00.tga'
    like = read_entry(arc, arc['entries'][idx[tga]])
    alpha = fnt.rsplit('/', 1)[0] + '/symbol_alpha.tga'
    import io
    original = Image.open(io.BytesIO(like)).convert('RGBA')  # the game's glyphs (Xbox; the keycap)
    if like[16] == 24 and alpha in idx:
        original.putalpha(Image.open(io.BytesIO(read_entry(arc, arc['entries'][idx[alpha]]))).convert('L'))
    atlas = build_atlas(read_entry(arc, arc['entries'][idx[fnt]]).decode('latin1'), keys, font_path,
                        short=SHORT_EN if english else SHORT, style=style, original=original)
    replace = {tga: tga_bytes(atlas, like)}
    if like[16] == 24 and alpha in idx:  # Uncharted 3: RGB, transparency in symbol_alpha
        replace[alpha] = alpha_tga(atlas, read_entry(arc, arc['entries'][idx[alpha]]))
    write_psarc(arc, replace, dst)
    if preview:
        bg = Image.new('RGBA', atlas.size, (70, 70, 70, 255))
        bg.alpha_composite(atlas)
        bg.convert('RGB').save(preview)
    return list(replace)


# ---- Uncharted 1 texts: the one prompt where the left stick glyph means "click it" ----------------
# UC1 draws the left stick (|A|) both for "move" (WASD) and in "Click left |A| to switch shoulders",
# which is L3. With keyboard icons that line gets |L3| instead, in every language (same string id).
# The text files (text1/ps4/N.common in core.psarc) are a count, (id, offset) pairs, then the strings.
L3_TEXTS = {0x3a8f5837}


def patch_common(d, ids=L3_TEXTS, old=b'|A|', new=b'|L3|'):
    n = struct.unpack('>I', d[:4])[0]
    base = 4 + n * 8
    ents = [list(struct.unpack('>II', d[4 + i * 8:12 + i * 8])) for i in range(n)]
    body = bytearray(d[base:])
    for e in ents:
        if e[0] not in ids:
            continue
        end = body.index(b'\0', e[1])
        at = body.find(old, e[1], end)
        if at < 0:
            continue
        body[at:at + len(old)] = new
        for f in ents:
            if f[1] > at:
                f[1] += len(new) - len(old)
    return struct.pack('>I', n) + b''.join(struct.pack('>II', *e) for e in ents) + bytes(body)


def make_text_psarc(src, dst, keys, font_path, preview=None, english=False, style='keys'):
    """core.psarc of Uncharted 1 with the L3 prompt fixed (keyboard icons only)."""
    arc = read_psarc(src)
    names = names_of(arc)
    replace = {}
    for i, name in enumerate(names, 1):
        if name.startswith('text1/') and name.endswith('.common'):
            old = read_entry(arc, arc['entries'][i])
            new = patch_common(old)
            if new != old:
                replace[name] = new
    write_psarc(arc, replace, dst)
    return list(replace)

