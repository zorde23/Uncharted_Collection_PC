# SPDX-License-Identifier: GPL-2.0-or-later
"""Keyboard/mouse button pictures for the games' HUD (launcher: Controls > keyboard icons, part B).

The prompts drawn over the world (the triangle over a ledge, the shoulder buttons, the sticks, the
d-pad, the journal button) are textures in a pak: UC2 build/main/pak22/ps4/hud.pak, UC3
pak23/ps4/hud.pak, UC1 pak19/ps4/particle.pak. Each prompt is a colour texture and a separate
transparency texture (the "-trans" one). The texture data sits uncompressed at the end of the pak,
one block per texture, in the GPU's 1D-thin tiled layout, so a copy of the pak with those blocks
re-drawn (same format, same size) is all the game needs: the port mounts the copy over the original,
as with data.psarc in part A. Needs Pillow; key_icons.py (part A) for the key names and colours."""
import re, struct
from PIL import Image, ImageDraw, ImageFilter, ImageFont
import key_icons as K

# ---- pak -------------------------------------------------------------------------------------
# DXGI formats used by these textures: BC1 (71), BC1 sRGB (72), BC4 (80). The rest are left alone.
BC1, BC1_SRGB, BC4 = 71, 72, 80


def textures(data):
    """The pak's textures: name, format, size, mip count and where their data is in the file."""
    # Header: +0x08 page count, +0x20 size of the texture data, +0x2c one (offset, size, 0) per page
    # and one more; the texture data starts right after that last block and runs to the end.
    pages = struct.unpack_from('<I', data, 8)[0]
    off, size, _ = struct.unpack_from('<III', data, 0x2c + 12 * pages)
    base = off + size
    assert base + struct.unpack_from('<I', data, 0x20)[0] == len(data)
    out = []
    for m in re.finditer(rb'VRAM_DESC\x00', data):
        p = data.rfind(b'd:/dev', 0, m.start())
        off, size, _, _, fmt, _, mips, w, h = struct.unpack_from('<QIIIIIIII', data, p - 0x38)
        name = data[p:m.start() - 1].split(b'texture2/')[-1].decode()
        short = name.rsplit('/', 2)[-2].rsplit('.', 1)[0]
        out.append(dict(name=name, short=short, fmt=fmt, w=w, h=h, mips=mips, pos=base + off, size=size))
    return out


def _thin(x, y):
    return ((x & 1) | (y & 1) << 1 | (x >> 1 & 1) << 2 | (y >> 1 & 1) << 3 | (x >> 2 & 1) << 4 | (y >> 2 & 1) << 5)


def mip_layout(t):
    """Per mip: (offset, width and height in 4x4 blocks, width and height in 8x8-block tiles)."""
    out, off, w, h = [], 0, t['w'], t['h']
    for _ in range(t['mips']):
        bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
        tw, th = (bw + 7) // 8, (bh + 7) // 8
        out.append((off, bw, bh, tw, th))
        off += tw * th * 64 * 8
        w, h = max(1, w // 2), max(1, h // 2)
    return out, off


def read_image(data, t, mip=0):
    """Mip `mip` as an upright image (RGBA for BC1, L for BC4)."""
    off, bw, bh, tw, _ = mip_layout(t)[0][mip]
    lin = bytearray(bw * bh * 8)
    for y in range(bh):
        for x in range(bw):
            a = t['pos'] + off + ((y >> 3) * tw + (x >> 3)) * 512 + _thin(x, y) * 8
            lin[(y * bw + x) * 8:(y * bw + x + 1) * 8] = data[a:a + 8]
    n = 4 if t['fmt'] == BC4 else 1
    im = Image.frombytes('L' if n == 4 else 'RGBA', (bw * 4, bh * 4), bytes(lin), 'bcn', n)
    w, h = max(1, t['w'] >> mip), max(1, t['h'] >> mip)
    return im.crop((0, 0, w, h)).transpose(Image.FLIP_TOP_BOTTOM)


# ---- BC encoders (small and simple: the drawings are flat colours) -----------------------------
def _bc1_block(px):
    lum = [r * 3 + g * 6 + b for r, g, b in px]
    hi, lo = px[lum.index(max(lum))], px[lum.index(min(lum))]
    c0 = (hi[0] >> 3) << 11 | (hi[1] >> 2) << 5 | hi[2] >> 3
    c1 = (lo[0] >> 3) << 11 | (lo[1] >> 2) << 5 | lo[2] >> 3
    if c0 == c1:
        return struct.pack('<HHI', c0, c1, 0)
    if c0 < c1:
        c0, c1 = c1, c0
    e = [((c >> 11) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31) for c in (c0, c1)]
    pal = [e[0], e[1], tuple((2 * a + b) // 3 for a, b in zip(*e)), tuple((a + 2 * b) // 3 for a, b in zip(*e))]
    bits = 0
    for i, p in enumerate(px):
        d = [(p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 + (p[2] - q[2]) ** 2 for q in pal]
        bits |= d.index(min(d)) << (2 * i)
    return struct.pack('<HHI', c0, c1, bits)


def _bc4_block(px):
    a0, a1 = max(px), min(px)
    if a0 == a1:
        return bytes((a0, a1, 0, 0, 0, 0, 0, 0))
    pal = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    bits = 0
    for i, p in enumerate(px):
        d = [abs(p - q) for q in pal]
        bits |= d.index(min(d)) << (3 * i)
    return bytes((a0, a1)) + bits.to_bytes(6, 'little')


def write_image(buf, t, img):
    """Encodes the upright image `img` (full size) and every mip of it into the pak copy `buf`."""
    bc4 = t['fmt'] == BC4
    img = img.convert('L' if bc4 else 'RGB')
    for mip, (off, bw, bh, tw, _) in enumerate(mip_layout(t)[0]):
        w, h = max(1, t['w'] >> mip), max(1, t['h'] >> mip)
        m = img if mip == 0 else img.resize((w, h), Image.LANCZOS)
        m = m.transpose(Image.FLIP_TOP_BOTTOM)
        if (w, h) != (bw * 4, bh * 4):
            pad = Image.new(m.mode, (bw * 4, bh * 4))
            pad.paste(m, (0, 0))
            for x in range(w, bw * 4):  # repeat the last column/row into the padding
                pad.paste(m.crop((w - 1, 0, w, h)), (x, 0))
            for y in range(h, bh * 4):
                pad.paste(pad.crop((0, h - 1, bw * 4, h)), (0, y))
            m = pad
        px = m.load()
        for y in range(bh):
            for x in range(bw):
                block = [px[x * 4 + i, y * 4 + j] for j in range(4) for i in range(4)]
                a = t['pos'] + off + ((y >> 3) * tw + (x >> 3)) * 512 + _thin(x, y) * 8
                buf[a:a + 8] = _bc4_block(block) if bc4 else _bc1_block(block)


# ---- drawings --------------------------------------------------------------------------------
INK, CAP, EDGE, HI = K.INK[:3], K.CAP[:3], K.EDGE[:3], K.HI[:3]


class Canvas:
    """A colour picture and its transparency, drawn together at the texture's size."""
    def __init__(self, w, h, ss=2):
        self.ss, self.w, self.h = ss, w, h
        self.rgb = Image.new('RGB', (w * ss, h * ss), CAP)
        self.a = Image.new('L', (w * ss, h * ss), 0)
        self.d, self.da = ImageDraw.Draw(self.rgb), ImageDraw.Draw(self.a)

    def s(self, box):
        return [v * self.ss for v in box]

    def done(self):
        return self.rgb.resize((self.w, self.h), Image.LANCZOS), self.a.resize((self.w, self.h), Image.LANCZOS)


SHORT = K.SHORT  # labels; make_hud_pak(english=True) uses K.SHORT_EN
STYLE = 'keys'  # make_hud_pak(style='xbox'): Xbox-style pad restyled from the game's own pictures


def label_for(keys, action):
    key = K.first_key(keys, action)
    return SHORT.get(key, key) or '?', key


def key(c, box, label, font, fill=CAP, depth=0, squash=1.0, hi=False):
    """A key (rounded square) in `box`; `depth` draws its side below it, `squash` < 1 looks at it
    from above at an angle (the 3D prompts)."""
    x0, y0, x1, y1 = c.s(box)
    dp = depth * c.ss
    r = int(min(x1 - x0, (y1 - y0 - dp)) * 0.22)
    lw = max(2, int((x1 - x0) * 0.035))
    if dp:
        c.d.rounded_rectangle((x0, y0 + dp, x1, y1), r, fill=tuple(v // 2 for v in EDGE))
        c.da.rounded_rectangle((x0, y0 + dp, x1, y1), r, fill=255)
    top = (x0, y0, x1, y1 - dp)
    c.d.rounded_rectangle(top, r, fill=HI if hi else fill, outline=EDGE, width=lw)
    c.da.rounded_rectangle(top, r, fill=255)
    if label:
        text(c, top, label, font, squash, CAP if hi else INK)


def text(c, box, label, font, squash=1.0, fill=INK):
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    size = int(h / squash * 0.5)
    f = ImageFont.truetype(font, size)
    l, t, r, b = c.d.textbbox((0, 0), label, font=f)
    _, ct, _, cb = c.d.textbbox((0, 0), 'H', font=f)
    tw, th = r - l, cb - ct
    room = w * 0.8
    im = Image.new('L', (r - l, b - t), 0)
    ImageDraw.Draw(im).text((-l, -t), label, font=f, fill=255)
    sx = min(1.0, room / tw)
    im = im.resize((max(1, int(im.width * sx)), max(1, int(im.height * squash))), Image.LANCZOS)
    px, py = int(x0 + (w - im.width) / 2), int(y0 + (h - th * squash) / 2 - (ct - t) * squash)
    c.rgb.paste(Image.new('RGB', im.size, fill), (px, py), im)


def mouse(c, box, which, arrow=None, ring=None):
    """A mouse in `box` (no key around it), the used button in orange; `arrow` adds a direction
    arrow beside it (left, right, up, down), `ring` a circular arrow at that angle step (0-3)."""
    x0, y0, x1, y1 = c.s(box)
    bw, bh = x1 - x0, y1 - y0
    mh = bh
    mw = min(bw, mh * 0.66)
    if arrow in ('left', 'right'):
        mw = min(mw, bw * 0.55)
    ox = x0 + (bw - mw) / 2 + (bw * 0.2 if arrow == 'left' else -bw * 0.2 if arrow == 'right' else 0)
    oy = y0
    lw = max(2, int(mw * 0.06))
    body = (ox, oy, ox + mw, oy + mh)
    c.d.rounded_rectangle(body, int(mw / 2), fill=CAP, outline=EDGE, width=lw)
    c.da.rounded_rectangle(body, int(mw / 2), fill=255)
    split = oy + mh * 0.45
    if which in ('left', 'right'):
        half = (ox + lw, oy + lw, ox + mw / 2, split) if which == 'left' else (ox + mw / 2, oy + lw, ox + mw - lw, split)
        mask = Image.new('L', c.rgb.size, 0)
        ImageDraw.Draw(mask).rounded_rectangle((ox + lw, oy + lw, ox + mw - lw, oy + mh - lw), int(mw / 2), fill=255)
        part = Image.new('L', c.rgb.size, 0)
        ImageDraw.Draw(part).rectangle(half, fill=255)
        from PIL import ImageChops
        c.rgb.paste(Image.new('RGB', c.rgb.size, HI), (0, 0), ImageChops.multiply(mask, part))
    c.d.line((ox + mw / 2, oy, ox + mw / 2, split), fill=EDGE, width=lw)
    c.d.line((ox + lw, split, ox + mw - lw, split), fill=EDGE, width=lw)
    wh = mw * 0.14
    c.d.rounded_rectangle((ox + mw / 2 - wh / 2, oy + mh * 0.13, ox + mw / 2 + wh / 2, oy + mh * 0.32), int(wh / 2),
                          fill=HI if which == 'middle' else INK)
    if arrow:
        a = mh * 0.22
        if arrow in ('left', 'right'):
            ax = ox - a * 1.5 if arrow == 'left' else ox + mw + a * 1.5
            ay = oy + mh / 2
            sgn = -1 if arrow == 'left' else 1
            pts = [(ax + sgn * a, ay), (ax - sgn * a * 0.2, ay - a), (ax - sgn * a * 0.2, ay + a)]
        else:
            ax, ay = ox + mw / 2, oy + mh / 2
            sgn = -1 if arrow == 'up' else 1
            pts = [(ax, ay + sgn * a), (ax - a, ay - sgn * a * 0.2), (ax + a, ay - sgn * a * 0.2)]
        c.d.polygon(pts, fill=HI, outline=EDGE)
        c.da.polygon(pts, fill=255)
    if ring is not None:
        cx, cy, rad = ox + mw / 2, oy + mh * 0.62, mw * 0.32
        start = -ring * 90  # the gap turns counter-clockwise, as the game's stick
        c.d.arc((cx - rad, cy - rad, cx + rad, cy + rad), start, start + 270, fill=HI, width=lw * 2)


def wasd(c, box, keys, font, lit=None, extra=None):
    """The four movement keys (two rows, like on the keyboard); `lit` is the one lit in orange."""
    acts = ('ls_up', 'ls_left', 'ls_down', 'ls_right')
    names = {a: label_for(keys, a)[0] for a in acts}
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    k = min(w / 3.3, h / 2.2)
    gx, gy = x0 + (w - k * 3.3) / 2, y0 + (h - k * 2.2) / 2
    pos = {'ls_up': (1, 0), 'ls_left': (0, 1), 'ls_down': (1, 1), 'ls_right': (2, 1)}
    for a in acts:
        i, j = pos[a]
        bx, by = gx + i * k * 1.15, gy + j * k * 1.2
        key(c, (bx, by, bx + k, by + k), names[a], font, hi=(a == lit))


# What each texture becomes. Every colour texture and its transparency are drawn from the same
# picture, so the two always match. Textures not named here stay as they are (the SIXAXIS motion
# prompts of UC1, the speaker, the glow halos).
FACE = {'x': 'cross', 'circle': 'circle', 'square': 'square', 'triangle': 'triangle'}
STICK_L = {'middle-analog': None, 'left-analog': 'ls_left', 'right-analog': 'ls_right',
           'toward-analog': 'ls_down', 'away-analog': 'ls_up'}
STICK_R = {'middle-analogRight': None, 'left-analogRight': 'left', 'right-analogRight': 'right',
           'toward-analogRight': 'down', 'away-analogRight': 'up'}
ROTATE_L = ['ls_left', 'ls_down', 'ls_right', 'ls_up']  # the game's stick turns counter-clockwise
TOUCHPAD = {'journal-select': (135, 64, 298, 162), 'map-select': (135, 64, 298, 162),
            'journal-add-page': (146, 116, 267, 188)}


def picture(short, w, h, keys, font, original):
    """(colour, transparency) for the texture `short`, or None to leave it. `original(name)` reads
    another texture of the pak (upright image) when a drawing keeps part of the old one."""
    base = re.sub(r'(?i)[-_](color|trans|trans-round)$', '', short)
    base = re.sub(r'(?i)-trans$', '', base)
    c = Canvas(w, h)

    def sc(box, ref=(256, 256)):  # boxes below are for the usual texture size; scale to this one
        return (box[0] * w / ref[0], box[1] * h / ref[1], box[2] * w / ref[0], box[3] * h / ref[1])
    if STYLE == 'xbox':
        return xbox_picture(c, base, short, sc, w, original, font)
    m = re.match(r'(?i)(x|circle|square|triangle)-button$', base)
    if m:  # flat face button, shown in the button-trans circle
        lab = label_for(keys, FACE[m.group(1).lower()])
        key_or_mouse(c, sc((58, 58, 198, 198)), keys, FACE[m.group(1).lower()], font)
        return c.done()
    if base.lower() == 'button':  # the circle every flat face button is cut with
        key(c, sc((58, 58, 198, 198)), None, font)
        return c.done()
    m = re.match(r'(?i)(?:(triangle|circle|square)-button|button)-(up|down)(?:-round)?$', base)
    if m:  # 3D face button over objects (UP raised, DOWN pressed), seen from above at an angle
        act = FACE[m.group(1).lower()] if m.group(1) else None
        down = m.group(2).lower() == 'down'
        box = sc((164, 222 if down else 198, 350, 322), (512, 512))
        lab = label_for(keys, act)[0] if act else None
        key(c, box, lab, font, depth=(10 if down else 26) * w / 512, squash=0.6)
        return c.done()
    m = re.match(r'(?i)([LR][12])-button$', base)
    if m:
        act = m.group(1).lower()
        key_or_mouse(c, sc((52, 70, 204, 186)), keys, act, font, shoulder=True)
        return c.done()
    m = re.match(r'(?i)button-TRANS-SHOULDER(1|2)(?:-SMALL|-BIG)?$', short)
    if m:  # masks shared by L1/R1 and by L2/R2
        if m.group(1) == '2' and all(K.first_key(keys, a) in K.MOUSE for a in ('l2', 'r2')):
            mouse(c, sc((52, 52, 204, 204)), None)
        else:
            key(c, sc((52, 70, 204, 186)), None, font)
        if re.search(r'(?i)-(SMALL|BIG)$', short):
            c.a = c.a.filter(ImageFilter.GaussianBlur(10 * c.ss))
        return c.done()
    if base in STICK_L or base == 'middle-2-analog':
        if base == 'middle-2-analog':  # stick pressed in: L3
            key_or_mouse(c, sc((84, 20, 172, 108), (256, 128)), keys, 'l3', font)
        else:
            wasd(c, sc((20, 8, 236, 120), (256, 128)), keys, font, lit=STICK_L[base])
        return c.done()
    if base in STICK_R or base == 'middle-2-analogRight':
        mouse(c, (w * 0.1, h * 0.05, w * 0.9, h * 0.95), 'middle' if base == 'middle-2-analogRight' else None,
              arrow=STICK_R.get(base))
        return c.done()
    m = re.match(r'(?i)Rotate-(Analog|Reverse)-FRAME([1-4])$', base)
    if m:  # turn the left stick around: the movement keys light up one after the other
        order = ROTATE_L if m.group(1).lower() == 'analog' else ROTATE_L[::-1]
        wasd(c, (0, int(h * 0.3), w, int(h * 0.8)), keys, font, lit=order[int(m.group(2)) - 1])
        return c.done()
    m = re.match(r'(?i)Rotate-(Analog|Reverse)Right-FRAME([1-4])$', base)
    if m:
        step = int(m.group(2)) - 1
        mouse(c, (w * 0.1, h * 0.1, w * 0.9, h * 0.9), None, ring=step if m.group(1).lower() == 'analog' else 3 - step)
        return c.done()
    if re.match(r'(?i)(bp-)?up-button(-up|-down)?$', base):  # d-pad with up lit, and its masks
        key_or_mouse(c, sc((48, 48, 208, 208)), keys, 'up', font)
        return c.done()
    if base.lower() == 'up-button-look':  # eyes on top (kept), d-pad below
        old_rgb, old_a = original('up-button-look-color'), original('up-button-look-trans')
        if old_rgb is None or old_a is None:
            return None
        top = int(h * 0.45)
        c.rgb.paste(old_rgb.convert('RGB').crop((0, 0, w, top)).resize((w * c.ss, top * c.ss)), (0, 0))
        c.a.paste(old_a.convert('L').crop((0, 0, w, top)).resize((w * c.ss, top * c.ss)), (0, 0))
        key_or_mouse(c, sc((48, 300, 208, 460), (256, 512)), keys, 'up', font)
        return c.done()
    for name, box in TOUCHPAD.items():  # the journal or map picture with a key on its button
        if base.lower() == name:
            old_rgb, old_a = original(name + '-color'), original(name + '-trans')
            if old_rgb is None or old_a is None:
                return None
            c.rgb.paste(old_rgb.convert('RGB').resize(c.rgb.size), (0, 0))
            c.a.paste(old_a.convert('L').resize(c.a.size), (0, 0))
            c.d.rectangle(c.s((box[0] - 4, box[1] - 4, box[2] + 4, box[3] + 4)), fill=(236, 230, 180))
            x0, y0, x1, y1 = box
            key(c, (x0, y0, x1, y1), label_for(keys, 'touchpad_left')[0], font)
            return c.done()
    return None


def xbox_picture(c, base, short, sc, w, original, font):
    """The Xbox-style pad, restyled from the game's own pictures: face buttons (flat and 3D),
    shoulders and triggers get A/B/X/Y and LB/RB/LT/RT, the touchpad pictures get View. The
    transparency textures stay the game's (same shapes); sticks and d-pad stay the game's own."""
    trans = re.search(r'(?i)trans', short)

    def done(img):
        if img is None:
            return None
        rgb = img.convert('RGB')
        return rgb, Image.new('L', rgb.size, 255)
    m = re.match(r'(?i)(x|circle|square|triangle)-button$', base)
    if m:
        return None if trans else done(K.restyle_face(original(short), FACE[m.group(1).lower()], font))
    m = re.match(r'(?i)(triangle|circle|square)-button-(up|down)(?:-round)?$', base)
    if m:  # 3D face button over objects (UP raised, DOWN pressed): the top seen at an angle
        if trans:
            return None
        k = w / 512
        zone = (190, 195, 324, 281) if m.group(2).lower() == 'up' else (184, 215, 330, 302)
        return done(K.restyle_face(original(short), FACE[m.group(1).lower()], font,
                                   zone=tuple(v * k for v in zone), squash=0.62))
    m = re.match(r'(?i)([LR][12])-button$', base)
    if m:
        return None if trans else done(K.restyle_label(original(short), K.XBOX_LABEL[m.group(1).lower()], font))
    for name, box in TOUCHPAD.items():
        if base.lower() == name:
            old_rgb, old_a = original(name + '-color'), original(name + '-trans')
            if old_rgb is None or old_a is None:
                return None
            c.rgb.paste(old_rgb.convert('RGB').resize(c.rgb.size), (0, 0))
            c.a.paste(old_a.convert('L').resize(c.a.size), (0, 0))
            c.d.rectangle(c.s((box[0] - 4, box[1] - 4, box[2] + 4, box[3] + 4)), fill=(236, 230, 180))
            key(c, box, 'View', font)
            return c.done()
    return None


def key_or_mouse(c, box, keys, action, font, shoulder=False):
    lab, raw = label_for(keys, action)
    if raw in K.MOUSE:
        x0, y0, x1, y1 = box
        side = y1 - y0
        if shoulder:  # same place as the L2/R2 mask's mouse
            s = c.w / 256
            box = (52 * s, 52 * s, 204 * s, 204 * s)
        mouse(c, box, K.MOUSE[raw])
    else:
        key(c, box, lab, font)


# ---- whole pak ---------------------------------------------------------------------------------
def make_hud_pak(src, dst, keys, font, preview=None, english=False, style='keys'):
    global SHORT, STYLE
    SHORT = K.SHORT_EN if english else K.SHORT
    STYLE = style
    data = open(src, 'rb').read()
    buf = bytearray(data)
    texs = [t for t in textures(data) if '/buttons/' in t['name'] and t['fmt'] in (BC1, BC1_SRGB, BC4)]
    by_short = {t['short'].lower(): t for t in texs}

    def original(short):
        t = by_short.get(short.lower())
        return read_image(data, t) if t else None

    done = []
    for t in texs:
        pic = picture(t['short'], t['w'], t['h'], keys, font, original)
        if pic is None:
            continue
        rgb, a = pic
        is_trans = 'trans' in t['short'].lower()
        write_image(buf, t, a if is_trans else rgb)
        done.append(t['short'])
    open(dst, 'wb').write(bytes(buf))
    return done
