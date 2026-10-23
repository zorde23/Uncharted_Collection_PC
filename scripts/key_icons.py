# SPDX-License-Identifier: GPL-2.0-or-later
"""Keyboard/mouse button icons for the games' text (launcher: Controls > keyboard icons).

The three games draw the buttons inside their texts with one bitmap font, data/fonts/ps4/symbol3
(art/general/textures/font/ps4/ in Uncharted 1): a 512x512 TGA with 32 glyphs, the same .fnt and
layout in all three. This draws a new atlas from the player's key bindings (input.ini names) and
writes a copy of the game's data.psarc with only that texture replaced; the port mounts the copy
over /app0/uXdata/data.psarc. Needs Pillow."""
import struct, zlib
from PIL import Image, ImageDraw, ImageFont

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


def keycap(img, box, label, font_path):
    """A key with the label at LABEL size; a label wider than the key can get is squeezed sideways,
    so every label keeps the same height."""
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(font_path, LABEL)
    l, t, r_, b = d.textbbox((0, 0), label, font=f)
    _, ct, _, cb = d.textbbox((0, 0), 'H', font=f)
    tw, room = r_ - l, box[2] - 2 - 10
    r = cap_rect(box, min(tw, room) + 10)
    frame(d, r)
    text = Image.new('RGBA', (tw, b - t), (0, 0, 0, 0))
    ImageDraw.Draw(text).text((-l, -t), label, font=f, fill=INK)
    if tw > room:
        text = text.resize((room, text.height), Image.LANCZOS)
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


def draw_glyph(img, box, button, keys, font_path, short=SHORT):
    if button in ('dpad', 'dpad_lr', 'dpad_ud'):
        acts = {'dpad': ('up', 'down', 'left', 'right'), 'dpad_lr': ('left', 'right'), 'dpad_ud': ('up', 'down')}[button]
        names = [short.get(first_key(keys, a), first_key(keys, a)) for a in acts]
        if button == 'dpad' and sorted(names) == ['1', '2', '3', '4']:
            names = ['1-4']
        return keycap(img, box, '/'.join(n for n in names if n) or '?', font_path)
    if button == 'ls':
        names = ''.join(first_key(keys, a) for a in ('ls_up', 'ls_left', 'ls_down', 'ls_right'))
        return keycap(img, box, names if len(names) == 4 else 'Mover', font_path)
    if button in ('rs', 'stick'):
        return mouse(img, box, None)
    key = first_key(keys, button)
    if key in MOUSE:
        return mouse(img, box, MOUSE[key])
    keycap(img, box, short.get(key, key) or '?', font_path)


def build_atlas(fnt_text, keys, font_path, size=512, short=SHORT):
    chars = parse_fnt(fnt_text)
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
    """Copies the archive with the entries named in `replace` ({name: bytes of the SAME size}) replaced.
    Everything else stays byte for byte: the new blocks go at the end of the file and only the
    entry's offset and its block sizes in the table change (same block count, so the table keeps
    its length)."""
    ver, comp, flags, bs, width = arc['head']
    names = names_of(arc)
    data = bytearray(arc['data'])
    toclen = struct.unpack('>I', data[12:16])[0]
    n = len(arc['entries'])
    for i, name in enumerate(names, 1):
        if name not in replace:
            continue
        e, payload = arc['entries'][i], replace[name]
        assert len(payload) == e['size'], (name, len(payload), e['size'])
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
        data[tpos + 25:tpos + 30] = offset.to_bytes(5, 'big')
    assert toclen == 32 + 30 * n + width * len(arc['zs'])
    open(path, 'wb').write(bytes(data))


def make_icon_psarc(src, dst, keys, font_path, preview=None, english=False):
    arc = read_psarc(src)
    names = names_of(arc)
    idx = {n: i + 1 for i, n in enumerate(names)}
    fnt = next(n for n in names if n.endswith('/symbol3.fnt'))
    tga = fnt[:-4] + '_00.tga'
    atlas = build_atlas(read_entry(arc, arc['entries'][idx[fnt]]).decode('latin1'), keys, font_path,
                        short=SHORT_EN if english else SHORT)
    like = read_entry(arc, arc['entries'][idx[tga]])
    replace = {tga: tga_bytes(atlas, like)}
    alpha = fnt.rsplit('/', 1)[0] + '/symbol_alpha.tga'
    if like[16] == 24 and alpha in idx:  # Uncharted 3: RGB, transparency in symbol_alpha
        replace[alpha] = alpha_tga(atlas, read_entry(arc, arc['entries'][idx[alpha]]))
    write_psarc(arc, replace, dst)
    if preview:
        bg = Image.new('RGBA', atlas.size, (70, 70, 70, 255))
        bg.alpha_composite(atlas)
        bg.convert('RGB').save(preview)
    return list(replace)
