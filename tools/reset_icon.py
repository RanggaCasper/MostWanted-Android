# Makes the touch controls' RESET art (app/src/main/assets/touch/reset_car.png) from Up's: its round body with the
# arrow taken out, a circular arrow drawn over it in white, as the other art's glyphs are. The art's own source set has
# no reset button.
# usage: python tools/reset_icon.py
import math
import os

from PIL import Image, ImageDraw

TOUCH = os.path.join(os.path.dirname(__file__), "..", "app", "src", "main", "assets", "touch")
SS = 4  # the glyph drawn this many times larger and reduced, for its edges


def main():
    im = Image.open(os.path.join(TOUCH, "up.png")).convert("RGBA")
    w, h = im.size
    cx, cy = w / 2, h / 2
    px = im.load()
    # the body's shading is by row: the glyph's pixels take their row's colour from beside it
    for y in range(h):
        dy = y - cy
        if abs(dy) >= 92:
            continue
        sx = int(cx - math.sqrt(96 * 96 - dy * dy))
        body = px[sx, y]
        for x in range(w):
            if (x - cx) ** 2 + dy * dy < 92 * 92:
                px[x, y] = body
    # a circular arrow, an open ring turning clockwise to its head at the top right
    big = Image.new("L", (w * SS, h * SS), 0)
    d = ImageDraw.Draw(big)
    r, t = 52 * SS, 17 * SS
    box = (cx * SS - r, cy * SS - r, cx * SS + r, cy * SS + r)
    d.arc(box, start=-30, end=250, fill=255, width=t)
    end = math.radians(-30)
    mid = r - t / 2
    ex, ey = cx * SS + mid * math.cos(end), cy * SS + mid * math.sin(end)
    tx, ty = -math.sin(end), math.cos(end)  # backwards along the ring at its end
    nx, ny = math.cos(end), math.sin(end)
    head, back = 32 * SS, 40 * SS
    d.polygon([(ex + nx * head, ey + ny * head), (ex - nx * head, ey - ny * head), (ex - tx * back, ey - ty * back)],
              fill=255)
    mask = big.resize((w, h), Image.LANCZOS)
    white = Image.new("RGBA", (w, h), (255, 255, 255, 255))
    im.paste(white, mask=mask)
    im.save(os.path.join(TOUCH, "reset_car.png"), optimize=True)
    print(f"reset_car: {w}x{h}")


if __name__ == "__main__":
    main()
