"""Put a retail frame and a port frame next to each other, labelled.

    python -m tools.retail.side_by_side retail.png port.png out.png [--title "Main menu, STORY"]
    python -m tools.retail.side_by_side retail.png port.png out.png --diff      # adds a difference strip

The two frames are scaled to the same height first. With --diff a third panel shows the
per-pixel difference, brightened four times, which is the quickest way to see what is off:
the menu's background moves, so only the 2D layer is expected to come out dark.
"""

import argparse

from PIL import Image, ImageChops, ImageDraw, ImageFont


def _font(size):
    for name in ("arial.ttf", "Arial.ttf", "DejaVuSans.ttf", "Helvetica.ttc"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def compose(retail_path, port_path, out_path, title="", diff=False, height=540):
    retail = Image.open(retail_path).convert("RGB")
    port = Image.open(port_path).convert("RGB")

    def fit(im):
        w = round(im.width * height / im.height)
        return im.resize((w, height), Image.LANCZOS) if im.height != height else im

    panels = [("RETAIL", fit(retail)), ("PORT", fit(port))]
    if diff:
        a, b = panels[0][1], panels[1][1]
        if a.size != b.size:
            b = b.resize(a.size, Image.LANCZOS)
        d = ImageChops.difference(a, b).point(lambda v: min(255, v * 4))
        panels.append(("DIFFERENCE x4", d))

    gap, band = 8, 34
    width = sum(p.width for _, p in panels) + gap * (len(panels) - 1)
    out = Image.new("RGB", (width, height + band), (24, 24, 24))
    draw = ImageDraw.Draw(out)
    font = _font(18)
    x = 0
    for label, im in panels:
        out.paste(im, (x, band))
        text = label if not title else "%s  -  %s" % (label, title)
        draw.text((x + 10, 7), text, fill=(235, 235, 235), font=font)
        x += im.width + gap
    out.save(out_path)
    return out_path


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("retail")
    ap.add_argument("port")
    ap.add_argument("out")
    ap.add_argument("--title", default="")
    ap.add_argument("--diff", action="store_true")
    ap.add_argument("--height", type=int, default=540)
    a = ap.parse_args(argv)
    print(compose(a.retail, a.port, a.out, a.title, a.diff, a.height))


if __name__ == "__main__":
    main()
