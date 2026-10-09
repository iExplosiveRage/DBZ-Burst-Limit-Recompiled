"""Writes src/burstlimit_button_icons.inc: the Xbox Series, PlayStation and Nintendo Switch button
icons the game's
button prompts are redrawn with (src/burstlimit_buttons.cpp), as the PNG files' own bytes, and the
controller pictures that replace the game's Xbox 360 pad (Control Settings, tutorial): an Xbox
Series controller and a DualSense made from the pack's *_Diagram_Simple line art, and a Joy-Con pair
on a grip drawn here, all in
the style of the game's picture (white body, black outline, grey cel-shaded rims, each pad's
own sticks), with the face buttons where the Xbox ones are (the game's highlights are placed there).
Needs Pillow and NumPy.

The icons are from Xelu's Free Controller Prompts (Nicolae "Xelu" Berbece, Those Awesome Guys),
public domain (CC0): https://thoseawesomeguys.com/prompts/ - the 256x256 versions in the pack's
"Prompts Export 256.zip" (the game redraws them as sharp in-memory replacement textures, up to
256x256 for the 32x32 prompt icons). The controller pictures are made at 4 times the game's
size (1856x1280) for the same reason.

    python scripts/make_button_icons.py <folder of the pack: PS5\\, Switch\\, Prompts Export 256.zip>
"""
import io
import os
import sys
import zipfile

import numpy as np
from PIL import Image, ImageDraw

# The icons in "Prompts Export 256.zip" (numbered files; the same drawings as PS5\\PS5_Cross.png
# etc., at 256x256).
ZIP_256 = "Prompts Export 256.zip"
ICONS = [
    ("kPs5Cross", "Playstation/Playstation0024.png"),
    ("kPs5Circle", "Playstation/Playstation0021.png"),
    ("kPs5Square", "Playstation/Playstation0023.png"),
    ("kPs5Triangle", "Playstation/Playstation0022.png"),
    ("kPs5L1", "Playstation/Playstation0036.png"),
    ("kPs5R1", "Playstation/Playstation0037.png"),
    ("kPs5L2", "Playstation/Playstation0034.png"),
    ("kPs5R2", "Playstation/Playstation0035.png"),
    ("kSwitchA", "Switch/Switch0043.png"),
    ("kSwitchB", "Switch/Switch0044.png"),
    ("kSwitchX", "Switch/Switch0046.png"),
    ("kSwitchY", "Switch/Switch0045.png"),
    ("kSwitchL", "Switch/Switch0062.png"),
    ("kSwitchR", "Switch/Switch0063.png"),
    ("kSwitchZL", "Switch/Switch0060.png"),
    ("kSwitchZR", "Switch/Switch0061.png"),
    ("kXboxA", "Xbox/Xbox0001.png"),
    ("kXboxB", "Xbox/Xbox0002.png"),
    ("kXboxX", "Xbox/Xbox0004.png"),
    ("kXboxY", "Xbox/Xbox0003.png"),
    ("kXboxLB", "Xbox/Xbox0016.png"),
    ("kXboxRB", "Xbox/Xbox0017.png"),
    ("kXboxLT", "Xbox/Xbox0014.png"),
    ("kXboxRT", "Xbox/Xbox0015.png"),
]
ICON_FILE = dict(ICONS)


def icon_bytes(pack, name):
    with zipfile.ZipFile(os.path.join(pack, ZIP_256)) as archive:
        return archive.read(ICON_FILE[name])


# ---------------------------------------------------------------- controller pictures

W, H = 464, 320
OUT = 4  # the pictures are made this many times the game's size
S = 8  # supersampling (of the game's size; 2x2 samples a texel of the output)

# The Xbox picture's face buttons (centers) and their size.
FACE = {"Y": (379.0, 133.5), "X": (341.0, 168.5), "B": (416.5, 168.5), "A": (379.0, 203.0)}
FACE_SIZE = 37.0

BODY = 230
LIGHT = 241
RIM1 = 178
RIM2 = 128
LINE = 48


def mask_shift(mask, dx, dy):
    out = np.zeros_like(mask)
    h, w = mask.shape
    ys = slice(max(0, dy), min(h, h + dy))
    yd = slice(max(0, -dy), min(h, h - dy))
    xs = slice(max(0, dx), min(w, w + dx))
    xd = slice(max(0, -dx), min(w, w - dx))
    out[ys, xs] = mask[yd, xd]
    return out


def dilate(mask, radius):
    """Grows a mask by about `radius` in every direction (an octagon: one-texel
    steps alternating a cross and a square)."""
    out = mask.copy()
    for step in range(radius):
        grown = out.copy()
        grown[1:, :] |= out[:-1, :]
        grown[:-1, :] |= out[1:, :]
        grown[:, 1:] |= out[:, :-1]
        grown[:, :-1] |= out[:, 1:]
        if step % 2:
            grown[1:, 1:] |= out[:-1, :-1]
            grown[1:, :-1] |= out[:-1, 1:]
            grown[:-1, 1:] |= out[1:, :-1]
            grown[:-1, :-1] |= out[1:, 1:]
        out = grown
    return out


def erode(mask, radius):
    return ~dilate(~mask, radius)


def shade_body(rgb, body, scale, light_up=True):
    """Cel shading like the 360 picture: grey bands along the lower edge."""
    rgb[body] = BODY
    band1 = body & ~mask_shift(body, 0, -int(13 * scale))
    band2 = body & ~mask_shift(body, 0, -int(5 * scale))
    rgb[band1] = RIM1
    rgb[band2] = RIM2
    if light_up:
        top = body & ~mask_shift(body, 0, int(5 * scale))
        rgb[top & ~band1] = LIGHT


def outline(rgb, alpha, body, scale, width=4.0):
    ring = dilate(body, max(1, int(width * scale))) & ~body
    rgb[ring] = 0
    alpha[ring] = 255
    alpha[body] = 255


def downsample(rgb, alpha, size):
    """Premultiplied box filter to `size`."""
    h, w = alpha.shape
    a = alpha.astype(np.float32) / 255.0
    pre = rgb.astype(np.float32) * a[..., None]
    fy, fx = h // size[1], w // size[0]
    pre = pre[:size[1] * fy, :size[0] * fx].reshape(size[1], fy, size[0], fx, 3).mean((1, 3))
    a = a[:size[1] * fy, :size[0] * fx].reshape(size[1], fy, size[0], fx).mean((1, 3))
    col = np.where(a[..., None] > 0, pre / np.maximum(a[..., None], 1e-6), 0)
    out = np.dstack([np.clip(col, 0, 255), np.clip(a * 255, 0, 255)]).astype(np.uint8)
    return Image.fromarray(out, "RGBA")


def disc(mask_shape, cx, cy, r):
    h, w = mask_shape
    y, x = np.ogrid[:h, :w]
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def ring(mask_shape, cx, cy, r_outer, r_inner):
    return disc(mask_shape, cx, cy, r_outer) & ~disc(mask_shape, cx, cy, r_inner)


def draw_stick(rgb, alpha, cx, cy, r, scale, style="xbox"):
    """A thumbstick at supersampled (cx, cy), radius r (target px * scale), in the style of
    the controller: "xbox" (the 360's cap with its four dots), or "playstation" / "switch"
    (concentric rings around a concave top, like the DualSense and Joy-Con caps)."""
    shape = alpha.shape
    well = disc(shape, cx, cy, r * 1.32)
    rgb[well & (alpha > 0)] = LIGHT  # the lighter well around it
    rim = disc(shape, cx, cy, r + 2.5 * scale)
    rgb[rim] = 0
    alpha[rim] = 255
    base = disc(shape, cx, cy, r)
    rgb[base] = 62
    lower = base & ~disc(shape, cx, cy - 0.16 * r, r)
    rgb[lower] = 40
    glint = disc(shape, cx - r * 0.38, cy - r * 0.5, r * 0.14)
    if style in ("playstation", "switch"):
        # Concentric rings (DualSense / Joy-Con / Pro Controller caps): the outer rim, a thin
        # lighter ring, and the concave top, lit at its lower edge.
        oy = cy - 0.06 * r
        inner_r = r * (0.62 if style == "playstation" else 0.56)
        rgb[ring(shape, cx, oy, r * 0.9, r * 0.84)] = 96
        rgb[ring(shape, cx, oy, inner_r + 1.6 * scale, inner_r)] = 26
        top = disc(shape, cx, oy, inner_r)
        rgb[top] = 50
        rgb[top & ~disc(shape, cx, oy - 0.12 * r, inner_r)] = 70
        rgb[ring(shape, cx, oy, inner_r * 0.5, inner_r * 0.5 - 1.2 * scale)] = 60
        rgb[glint & base & ~top] = 120
    else:
        cap = disc(shape, cx, cy - 0.08 * r, r * 0.74)
        rgb[cap] = 82
        inner = disc(shape, cx, cy - 0.08 * r, r * 0.6)
        rgb[inner] = 74
        for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
            dot = disc(shape, cx + dx * r * 0.42, cy - 0.08 * r + dy * r * 0.42, 1.2 * scale)
            rgb[dot] = 120
        rgb[glint & base & ~cap] = 120


def paste_icons(img, icons):
    """icons: {face letter: PIL RGBA icon} drawn at the Xbox positions."""
    for face, icon in icons.items():
        cx, cy = FACE[face]
        bbox = icon.getbbox()
        ic = icon.crop(bbox)
        s = FACE_SIZE / max(ic.size)
        size = (max(1, round(ic.size[0] * s * S)), max(1, round(ic.size[1] * s * S)))
        big = ic.resize(size, Image.LANCZOS)
        img.alpha_composite(big, (round(cx * S - size[0] / 2), round(cy * S - size[1] / 2)))


def icons_for(pack, files):
    return {k: Image.open(io.BytesIO(icon_bytes(pack, v))).convert("RGBA")
            for k, v in files.items()}


# ---------------------------------------------------------------------------- DualSense

def make_from_diagram(pack, diagram, transform, faces, sticks, parts, icons, stick_style):
    """A pad from one of the pack's *_Diagram_Simple line drawings: `transform` = (SX, SY, TX,
    TY) art -> target (x * SX + TX), `faces` the art's face buttons (x, y, radius) - erased, the
    icons go to the Xbox positions -, `sticks` the art's stick centers and radii (redrawn in
    `stick_style`, see draw_stick), `parts` (seed, gray) areas filled."""
    art = np.array(Image.open(os.path.join(pack, diagram)).convert("RGBA"))
    lines = art[..., 3] > 90
    SX, SY, TX, TY = transform
    closed = dilate(lines, 3)
    for cx, cy, r in faces:
        lines &= ~disc(lines.shape, cx, cy, r)
    # Regions by flood fill of the closed line drawing.
    canvas = Image.fromarray((closed * 255).astype(np.uint8)).copy()
    ImageDraw.floodfill(canvas, (0, 0), 100)
    body = np.array(canvas) != 100

    def region(seed):
        c = Image.fromarray((closed * 255).astype(np.uint8)).copy()
        ImageDraw.floodfill(c, seed, 77)
        return np.array(c) == 77

    # Supersampled target canvas: transform the art masks there.
    tw, th = W * S, H * S

    def to_target(mask):
        img = Image.fromarray((mask * 255).astype(np.uint8))
        a, e = 1.0 / (SX * S), 1.0 / (SY * S)
        out = img.transform((tw, th), Image.AFFINE, (a, 0, -TX / SX, 0, e, -TY / SY),
                            resample=Image.BILINEAR)
        return np.array(out) > 127

    # The sticks' own circles go, but not the body's outline around them.
    edge = dilate(body, 2) & ~erode(body, 6)
    for cx, cy, r in sticks:
        lines &= ~(disc(lines.shape, cx, cy, r) & ~edge)
    t_body = to_target(body)
    t_lines = to_target(dilate(lines, 3))
    rgb = np.zeros((th, tw), np.float32)
    alpha = np.zeros((th, tw), np.uint8)
    shade_body(rgb, t_body, S)
    for seed, color in parts:
        part = to_target(region(seed)) & t_body
        rgb[part] = color
        if color > 150:
            lower = part & ~mask_shift(part, 0, -int(3 * S))
            rgb[lower] = color - 40
    rgb[t_lines & t_body] = LINE
    outline(rgb, alpha, t_body, S)
    for cx, cy, _ in sticks:
        draw_stick(rgb, alpha, (cx * SX + TX) * S, (cy * SY + TY) * S, 26 * S, S, stick_style)
    rgb3 = np.dstack([rgb] * 3)
    img = Image.fromarray(np.dstack([rgb3.astype(np.uint8), alpha]), "RGBA")
    paste_icons(img, icons_for(pack, icons))
    a = np.array(img)
    return downsample(a[..., :3], a[..., 3], (W * OUT, H * OUT))


def make_ps5(pack):
    """A DualSense (PS5_Diagram_Simple): the face buttons near the Xbox ones, the triggers
    under the game's L2 / R2 highlights."""
    return make_from_diagram(
        pack, os.path.join("PS5", "PS5_Diagram_Simple.png"), (0.33, 0.36, -1.8, 7.4),
        faces=((1156, 356, 60), (1153, 533, 60), (1052, 449, 60), (1256, 447, 60)),
        sticks=((520, 640, 98), (940, 640, 98)),
        # Touchpad, triggers and shoulder buttons lighter grey, the D-pad dark, the Create /
        # Options buttons dark.
        parts=(((733, 333), 222), ((313, 93), 214), ((1093, 93), 214),
               ((307, 182), 206), ((1093, 182), 206),
               ((307, 378), 70), ((307, 525), 70), ((225, 450), 70), ((390, 450), 70),
               ((307, 450), 70), ((409, 309), 80), ((1047, 309), 80)),
        icons={"A": "kPs5Cross", "B": "kPs5Circle", "X": "kPs5Square", "Y": "kPs5Triangle"},
        stick_style="playstation")


def make_xbox(pack):
    """An Xbox Series controller (XboxSeriesX_Diagram_Simple). Its face buttons sit higher
    and further out than the 360's, where the game's highlights are: the pad is fitted to
    the picture and the buttons go to the 360 positions (the art's are erased)."""
    return make_from_diagram(
        pack, os.path.join("Xbox Series", "XboxSeriesX_Diagram_Simple.png"),
        (0.315, 0.335, 15.1, 3.0),
        faces=((1092, 317, 58), (1185, 401, 58), (994, 409, 58), (1087, 493, 58)),
        sticks=((375, 438, 82), (914, 638, 82)),
        # Triggers, bumpers and the top band lighter grey; the D-pad dark in its lighter
        # ring; View / Menu dark.
        parts=(((353, 81), 214), ((1107, 81), 214), ((397, 131), 206), ((1062, 131), 206),
               ((730, 148), 222),
               ((549, 618), 70), ((550, 553), 70), ((550, 679), 70), ((482, 620), 70),
               ((617, 620), 70),
               ((484, 559), 150), ((614, 559), 150), ((489, 678), 150), ((610, 678), 150),
               ((629, 410), 80), ((831, 410), 80)),
        icons={"A": "kXboxA", "B": "kXboxB", "X": "kXboxX", "Y": "kXboxY"},
        stick_style="xbox")


# ---------------------------------------------------------------------------- Joy-Cons

def rounded(draw, box, radii, fill):
    """A box with its own radius per corner (tl, tr, br, bl), supersampled coordinates."""
    x0, y0, x1, y1 = box
    draw.rectangle((x0, y0, x1, y1), fill=fill)
    tl, tr, br, bl = radii
    for r, sx, sy in ((tl, x0, y0), (tr, x1 - tr, y0), (br, x1 - br, y1 - br), (bl, x0, y1 - bl)):
        if r <= 0:
            continue
        draw.rectangle((sx, sy, sx + r, sy + r), fill=0)
        cx = sx + r if sx == x0 else sx
        cy = sy + r if sy == y0 else sy
        draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=fill)
    # The ellipses reach past their corner squares only inside the box.


def shape_mask(fn):
    img = Image.new("L", (W * S, H * S), 0)
    fn(ImageDraw.Draw(img))
    return np.array(img) > 127


def make_switch(pack):
    s = S
    rgb = np.zeros((H * s, W * s), np.float32)
    alpha = np.zeros((H * s, W * s), np.uint8)

    def box(x0, y0, x1, y1, radii, scale=s):
        return shape_mask(lambda d: rounded(d, (x0 * scale, y0 * scale, x1 * scale, y1 * scale),
                                            [r * scale for r in radii], 255))

    # ZL / ZR behind the Joy-Cons, L / R along their tops.
    zl = box(52, 24, 140, 70, (16, 10, 0, 0))
    zr = box(324, 24, 412, 70, (10, 16, 0, 0))
    left = box(22, 50, 152, 304, (62, 6, 6, 62))
    right = box(312, 50, 442, 304, (6, 62, 62, 6))
    grip = box(140, 66, 324, 248, (14, 14, 40, 40))
    for part, color in ((zl, 150), (zr, 150)):
        rgb[part] = color
        lower = part & ~mask_shift(part, 0, -3 * s)
        rgb[lower] = 110
        outline(rgb, alpha, part, s, 3.5)
    shade_body(rgb, grip, s, light_up=False)
    rgb[grip] = np.where(rgb[grip] == BODY, 150, np.where(rgb[grip] == RIM1, 118, 92))
    outline(rgb, alpha, grip, s, 3.5)
    for jc in (left, right):
        shade_body(rgb, jc, s)
        outline(rgb, alpha, jc, s, 4.0)
    # L / R: the top band of each Joy-Con.
    top_l = left & ~mask_shift(left, 0, 20 * s)
    top_r = right & ~mask_shift(right, 0, 20 * s)
    for band in (top_l, top_r):
        inner = erode(band, int(1.5 * s))
        rgb[band] = 0
        rgb[inner] = 196
    # The rails between the Joy-Cons and the grip.
    for x in (152, 312):
        rail = shape_mask(lambda d, x=x: d.rectangle(((x - 2) * s, 50 * s, (x + 2) * s, 300 * s), 255))
        rgb[rail & (alpha > 0)] = 40
    # Sticks, D-pad buttons, -, +, Home, Capture.
    draw_stick(rgb, alpha, 87 * s, 130 * s, 26 * s, s, "switch")
    draw_stick(rgb, alpha, 377 * s, 258 * s, 27 * s, s, "switch")

    def button(cx, cy, r, color, rim=2.0):
        m = disc(alpha.shape, cx * s, cy * s, (r + rim) * s)
        rgb[m] = 0
        alpha[m] = 255
        m = disc(alpha.shape, cx * s, cy * s, r * s)
        rgb[m] = color
        low = m & ~disc(alpha.shape, cx * s, (cy - r * 0.2) * s, r * s)
        rgb[low] = color - 28

    for dx, dy in ((0, -25), (0, 25), (-25, 0), (25, 0)):
        button(87 + dx, 214 + dy, 11.5, 78)
    button(132, 84, 8, 70)
    button(332, 84, 8, 70)
    button(334, 292, 7.5, 70)
    cap = box(102, 262, 116, 276, (3, 3, 3, 3))
    rgb[dilate(cap, 2 * s)] = 0
    rgb[cap] = 70
    rgb3 = np.dstack([rgb] * 3)
    img = Image.fromarray(np.dstack([rgb3.astype(np.uint8), alpha]), "RGBA")
    d = ImageDraw.Draw(img)
    # - and + and the D-pad arrows in light grey.
    for cx, cy, plus in ((132, 84, False), (332, 84, True)):
        d.rectangle(((cx - 4.5) * s, (cy - 1.2) * s, (cx + 4.5) * s, (cy + 1.2) * s), fill=(220, 220, 224, 255))
        if plus:
            d.rectangle(((cx - 1.2) * s, (cy - 4.5) * s, (cx + 1.2) * s, (cy + 4.5) * s), fill=(220, 220, 224, 255))
    for (dx, dy) in ((0, -25), (0, 25), (-25, 0), (25, 0)):
        cx, cy = 87 + dx, 214 + dy
        ux, uy = (0, -1) if dy < 0 else (0, 1) if dy > 0 else (-1, 0) if dx < 0 else (1, 0)
        px, py = -uy, ux
        tip = ((cx + ux * 5) * s, (cy + uy * 5) * s)
        b1 = ((cx - ux * 3 + px * 5) * s, (cy - uy * 3 + py * 5) * s)
        b2 = ((cx - ux * 3 - px * 5) * s, (cy - uy * 3 - py * 5) * s)
        d.polygon([tip, b1, b2], fill=(205, 205, 210, 255))
    d.ellipse(((334 - 4) * s, (292 - 4) * s, (334 + 4) * s, (292 + 4) * s), outline=(205, 205, 210, 255), width=s)
    paste_icons(img, icons_for(pack, {"A": "kSwitchB", "B": "kSwitchA", "X": "kSwitchY",
                                      "Y": "kSwitchX"}))
    a = np.array(img)
    return downsample(a[..., :3], a[..., 3], (W * OUT, H * OUT))


def main():
    pack = sys.argv[1]
    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src",
                            "burstlimit_button_icons.inc")
    lines = [
        "// Generated by scripts/make_button_icons.py - do not edit.",
        "// Xelu's Free Controller Prompts (Nicolae \"Xelu\" Berbece), public domain (CC0):",
        "// https://thoseawesomeguys.com/prompts/ - PNG files, 256x256 RGBA; the controller pictures",
        "// are made from the pack's PS5 diagram and drawn by the script.",
        "",
    ]
    for name, file in ICONS:
        data = icon_bytes(pack, name)
        lines.append(f"// {file}")
        lines.append(f"constexpr uint8_t {name}Png[{len(data)}] = {{")
        for i in range(0, len(data), 24):
            lines.append("    " + ",".join(f"0x{b:02X}" for b in data[i:i + 24]) + ",")
        lines.append("};")
    for name, make in (("kPs5PadPng", make_ps5), ("kSwitchPadPng", make_switch),
                       ("kXboxPadPng", make_xbox)):
        buffer = io.BytesIO()
        make(pack).save(buffer, format="PNG", optimize=True)
        data = buffer.getvalue()
        lines.append(f"// The controller picture ({W * OUT}x{H * OUT}), made by this script")
        lines.append(f"constexpr uint8_t {name}[{len(data)}] = {{")
        for i in range(0, len(data), 24):
            lines.append("    " + ",".join(f"0x{b:02X}" for b in data[i:i + 24]) + ",")
        lines.append("};")
    lines.append("")
    with open(out_path, "w", newline="\n") as f:
        f.write("\n".join(lines))
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
