# Reduces the touch controls' art (white on transparent PNGs, the "WITH BUTTON" set: the pad's letter in a badge over
# each face button and pedal) to the sizes the controls are drawn at and writes it into the APK's assets, where
# native/touch.cpp loads it (app/src/main/assets/touch/<name>.png).
# usage: python tools/touch_art.py <folder with "WITH BUTTON" and analog_stick.png>
import os
import sys

from PIL import Image, ImageFilter

OUT = os.path.join(os.path.dirname(__file__), "..", "app", "src", "main", "assets", "touch")

# the file each control's art comes from and its width in the APK: a round button's body 256 pixels across, more
# for the pedals, the sticks and the wheel, which are drawn bigger
ART = {
    "a": ("WITH BUTTON/A.png", 256),
    "b": ("WITH BUTTON/B.png", 256),
    "x": ("WITH BUTTON/X.png", 256),
    "y": ("WITH BUTTON/Y.png", 256),
    "lt": ("WITH BUTTON/L.png", 512),
    "rt": ("WITH BUTTON/R.png", 384),
    "black": ("WITH BUTTON/black.png", 256),
    "start": ("WITH BUTTON/start.png", 256),
    "back": ("WITH BUTTON/back.png", 256),
    "up": ("WITH BUTTON/up.png", 256),
    "down": ("WITH BUTTON/down.png", 256),
    "left": ("WITH BUTTON/left.png", 256),
    "right": ("WITH BUTTON/right.png", 256),
    "stick_ring": ("WITH BUTTON/analog_left.png", 384),
    "stick_knob": ("WITH BUTTON/analog_base.png", 192),
    "stick_shift": ("WITH BUTTON/analog_right.png", 192),
    "wheel": ("analog_stick.png", 384),
}


def reduce(src, width):
    im = Image.open(src).convert("RGBA")
    h = round(im.height * width / im.width)
    # reduced premultiplied, so the transparent pixels' colour does not darken the edges
    im = im.convert("RGBa").resize((width, h), Image.LANCZOS).convert("RGBA")
    # the clear pixels take their neighbours' colour: the controls are drawn with linear filtering
    r, g, b, a = im.split()
    rgb = Image.merge("RGB", (r, g, b))
    grown = rgb.filter(ImageFilter.MaxFilter(5))
    clear = a.point(lambda v: 255 if v == 0 else 0)
    rgb.paste(grown, mask=clear)
    return Image.merge("RGBA", (*rgb.split(), a))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__ or "usage: python tools/touch_art.py <art folder>")
    os.makedirs(OUT, exist_ok=True)
    for name, (src, width) in ART.items():
        im = reduce(os.path.join(sys.argv[1], src), width)
        im.save(os.path.join(OUT, name + ".png"), optimize=True)
        print(f"{name}: {im.width}x{im.height}")


if __name__ == "__main__":
    main()
