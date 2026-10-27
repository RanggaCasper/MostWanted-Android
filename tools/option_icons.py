# Draws the options menu's icons for the recomp's sections (src/game/options_menu.cpp) and writes them as
# src/game/option_icons.inc. They are drawn as the game's own options icons are (OPTIONS_ICON_AUDIO: two gears with
# the subject in front of them): white, the details cut out, which the menu tints as it does the game's.
# usage: python tools/option_icons.py [preview.png]
import math
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter

SIZE = 64   # the texture, as the game's icons
SCALE = 8   # drawn this much bigger, then reduced
GAP = 2.2   # between the subject and the gears behind it, in texture pixels
S = SIZE * SCALE
OUT = os.path.join(os.path.dirname(__file__), "..", "src", "game", "option_icons.inc")


def canvas():
    img = Image.new("L", (S, S), 0)
    return img, ImageDraw.Draw(img)


def p(*xy):  # texture pixels to the canvas
    return [v * SCALE for v in xy]


def rrect(d, box, r, fill=255):
    d.rounded_rectangle(p(*box), radius=r * SCALE, fill=fill)


def gear(d, cx, cy, r, teeth=7, hole=0.4):
    # a wheel with stubby round teeth and a hole in the middle, as the game's gears
    body = r * 0.78
    d.ellipse(p(cx - body, cy - body, cx + body, cy + body), fill=255)
    for i in range(teeth):
        a = (i / teeth) * 2 * math.pi
        tx, ty, tr = cx + r * 0.8 * math.cos(a), cy + r * 0.8 * math.sin(a), r * 0.24
        d.ellipse(p(tx - tr, ty - tr, tx + tr, ty + tr), fill=255)
    d.ellipse(p(cx - r * hole, cy - r * hole, cx + r * hole, cy + r * hole), fill=0)


def gears():
    img, d = canvas()
    gear(d, 27, 13, 10.5, teeth=8)
    gear(d, 11.5, 27, 9.5, teeth=8)
    return img


def monitor():
    # a screen with a road running to the horizon under a sun, on its stand
    img, d = canvas()
    rrect(d, (19, 19, 62, 50), 3.5)
    d.polygon(p(36, 50, 46, 50, 49, 57, 33, 57), fill=255)
    rrect(d, (27, 56, 55, 61), 2)
    rrect(d, (23, 23, 58, 46), 1.5, fill=0)
    d.polygon(p(28, 46, 39.3, 30, 41.7, 30, 53, 46), fill=255)  # the road
    for y0, y1 in ((32, 34.5), (37, 40), (42.5, 46)):  # its centre line, nearer is wider
        w0, w1 = 0.25 + (y0 - 30) * 0.04, 0.25 + (y1 - 30) * 0.04
        d.polygon(p(40.5 - w0, y0, 40.5 + w0, y0, 40.5 + w1, y1, 40.5 - w1, y1), fill=0)
    d.ellipse(p(47, 25.5, 54, 32.5), fill=255)  # the sun
    return img


def phone():
    # a phone on its side, the touch controls on its screen: a dpad and four buttons
    img, d = canvas()
    rrect(d, (17, 23, 63, 54), 6)
    rrect(d, (23, 27, 58, 50), 2, fill=255)
    rrect(d, (23, 27, 58, 50), 2, fill=0)
    cx, cy = 31, 38.5
    d.rectangle(p(cx - 5.5, cy - 1.8, cx + 5.5, cy + 1.8), fill=255)  # the dpad
    d.rectangle(p(cx - 1.8, cy - 5.5, cx + 1.8, cy + 5.5), fill=255)
    bx, by = 50, 38.5
    for dx, dy in ((0, -4.2), (4.2, 0), (0, 4.2), (-4.2, 0)):  # the buttons
        d.ellipse(p(bx + dx - 2.1, by + dy - 2.1, bx + dx + 2.1, by + dy + 2.1), fill=255)
    d.ellipse(p(59.6, 36.8, 61.6, 40.2), fill=0)  # the home button
    return img


def sliders():
    # a panel of three faders, set at different heights
    img, d = canvas()
    rrect(d, (19, 19, 62, 61), 4)
    for x, knob in ((29, 46), (40.5, 29), (52, 38)):
        rrect(d, (x - 1.4, 24, x + 1.4, 56), 1.4, fill=0)
        rrect(d, (x - 5.5, knob - 3.6, x + 5.5, knob + 3.6), 1.8, fill=0)
        rrect(d, (x - 3.8, knob - 2, x + 3.8, knob + 2), 1, fill=255)
    return img


def android():
    # the Android robot: a domed head with its antennae and eyes, its body and arms, for the menu that holds the rest
    img, d = canvas()
    d.pieslice(p(26, 17, 56, 45), 180, 360, fill=255)  # the head
    d.line(p(32, 20, 29, 14), fill=255, width=int(1.8 * SCALE))  # the antennae
    d.line(p(50, 20, 53, 14), fill=255, width=int(1.8 * SCALE))
    d.ellipse(p(33.5, 24.5, 37.5, 28.5), fill=0)  # the eyes
    d.ellipse(p(44.5, 24.5, 48.5, 28.5), fill=0)
    rrect(d, (26, 33, 56, 55), 3.5)  # the body
    rrect(d, (19, 33, 24, 50), 2.5)  # the arms
    rrect(d, (58, 33, 63, 50), 2.5)
    rrect(d, (31, 53, 36, 62), 2.5)  # the legs
    rrect(d, (46, 53, 51, 62), 2.5)
    return img


# in the sections' order: graphics, controls, general, and the Android Options menu that holds them
ICONS = [monitor, phone, sliders, android]


def compose(subject):
    # the gears behind, cut back from the subject's outline so a gap shows between them
    behind = gears()
    shape = subject.point(lambda v: 255 if v else 0)
    # the subject's outline, holes filled, so the gap follows the outside only
    solid = shape.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.MinFilter(5))
    halo = solid.filter(ImageFilter.MaxFilter(int(GAP * SCALE) * 2 + 1))
    behind = ImageChops.subtract(behind, halo)
    return ImageChops.lighter(behind, subject)


def finish(mask):
    alpha = mask.resize((SIZE, SIZE), Image.LANCZOS)
    white = Image.new("L", (SIZE, SIZE), 255)
    return Image.merge("RGBA", (white, white, white, alpha))


def main():
    icons = [finish(compose(f())) for f in ICONS]
    lines = ["// written by tools/option_icons.py, do not edit", f"constexpr int kIconCount = {len(icons)};",
             f"constexpr uint32_t kIconSize = {SIZE};",
             f"constexpr uint32_t kIcons[kIconCount][kIconSize * kIconSize] = {{"]
    for icon in icons:
        px = [(a << 24) | (r << 16) | (g << 8) | b for r, g, b, a in icon.getdata()]
        lines.append("    {")
        for i in range(0, len(px), 8):
            lines.append("        " + " ".join(f"0x{v:08X}," for v in px[i:i + 8]))
        lines.append("    },")
    lines.append("};")
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    if len(sys.argv) > 1:
        # as the menus show them: white in the title's, amber in the pause menu's
        big = SIZE * 4
        sheet = Image.new("RGBA", (big * len(icons), big * 2), (24, 24, 24, 255))
        for i, icon in enumerate(icons):
            up = icon.resize((big, big), Image.LANCZOS)
            sheet.alpha_composite(up, (i * big, 0))
            r, g, b, a = up.split()
            amber = Image.merge("RGBA", (r.point(lambda v: v * 250 // 255), g.point(lambda v: v * 180 // 255),
                                         b.point(lambda v: v * 40 // 255), a))
            sheet.alpha_composite(amber, (i * big, big))
        sheet.save(sys.argv[1])


if __name__ == "__main__":
    main()
