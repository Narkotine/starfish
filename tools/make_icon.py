#!/usr/bin/env python3
"""StarFish's icon -- one geometry, every artifact (session 59).

    python tools/make_icon.py            # writes the files below
    python tools/make_icon.py --preview build/captures

Writes:
  src/resources/starfish.svg           the mark, scalable
  src/resources/starfish-<N>.png       16..256 px, the tray and the windows (Qt resource)
  src/starfish.ico                     the same sizes, the executable's icon

The mark is ours from end to end: a coral starfish, five round-tipped arms
turning slightly in one direction, a cream pattern on the back, a dark coral
rim so it reads on a light *and* a dark taskbar. No panel, no title bar, no
tiles: nothing of the "fence in a box" glyph the previous tray icon was, which
looked like another product's.

The silhouette is a polar curve, r(theta) = body + (tip - body) * lobe(theta),
whose lobes are rounded, with a twist that grows with the radius (the sweep).
The PNGs are rendered at 16x their size and reduced with Lanczos; under 32 px
the pattern is left out and the rim is kept at least one pixel wide, so the
five arms stay readable at 16 px.
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]

FILL_IN = (0xFF, 0xA0, 0x6E)   # centre, warm
FILL_OUT = (0xF0, 0x4E, 0x2A)  # tips, coral
RIM = (0x8E, 0x23, 0x10)
DOTS = (0xFF, 0xE6, 0xC8)

BODY, TIP, SHARP, TWIST = 0.20, 0.47, 1.9, 0.20


def outline(n: int = 720) -> list[tuple[float, float]]:
    """The silhouette in unit coordinates (0..1), first arm pointing up."""
    pts = []
    for i in range(n):
        a = 2 * math.pi * i / n
        lobe = abs(math.cos(2.5 * a)) ** SHARP          # 5 rounded lobes
        r = BODY + (TIP - BODY) * lobe
        twist = TWIST * ((r - BODY) / (TIP - BODY)) ** 2  # arms turn towards their tip
        ang = a - math.pi / 2 + twist
        pts.append((0.5 + r * math.cos(ang), 0.52 + r * math.sin(ang)))
    return pts


def dots() -> list[tuple[float, float, float]]:
    """The back's pattern: a ring in the middle, a row down each arm."""
    out = []
    for k in range(5):
        a = 2 * math.pi * k / 5
        out.append((0.5 + 0.075 * math.cos(a - math.pi / 2 + 0.6), 0.52 + 0.075 * math.sin(a - math.pi / 2 + 0.6), 0.016))
        for j, (d, rad) in enumerate([(0.20, 0.022), (0.28, 0.019), (0.355, 0.015)]):
            lobe_r = d
            twist = TWIST * ((lobe_r - BODY) / (TIP - BODY)) ** 2 if lobe_r > BODY else 0.0
            ang = a - math.pi / 2 + twist
            out.append((0.5 + d * math.cos(ang), 0.52 + d * math.sin(ang), rad))
    return out


def svg() -> str:
    path = " ".join(("M" if i == 0 else "L") + f"{x * 256:.2f},{y * 256:.2f}" for i, (x, y) in enumerate(outline(360))) + " Z"
    circles = "\n    ".join(f'<circle cx="{x * 256:.2f}" cy="{y * 256:.2f}" r="{r * 256:.2f}"/>' for x, y, r in dots())
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256">
  <title>StarFish</title>
  <defs>
    <radialGradient id="body" cx="50%" cy="52%" r="48%">
      <stop offset="0" stop-color="#{FILL_IN[0]:02X}{FILL_IN[1]:02X}{FILL_IN[2]:02X}"/>
      <stop offset="1" stop-color="#{FILL_OUT[0]:02X}{FILL_OUT[1]:02X}{FILL_OUT[2]:02X}"/>
    </radialGradient>
  </defs>
  <path d="{path}" fill="url(#body)" stroke="#{RIM[0]:02X}{RIM[1]:02X}{RIM[2]:02X}" stroke-width="7" stroke-linejoin="round"/>
  <g fill="#{DOTS[0]:02X}{DOTS[1]:02X}{DOTS[2]:02X}">
    {circles}
  </g>
</svg>
'''


def render(size: int) -> Image.Image:
    k = 16
    s = size * k
    pts = [(x * s, y * s) for x, y in outline()]
    # The rim: 7/256 of the mark, never under one pixel of the final size.
    rim = max(7 / 256 * s, 1.0 * k if size <= 24 else 0.0)
    mask_out = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask_out).polygon(pts, fill=255)
    rim_draw = ImageDraw.Draw(mask_out)
    rim_draw.line(pts + [pts[0]], fill=255, width=int(rim), joint="curve")
    mask_in = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask_in).polygon(pts, fill=255)
    ImageDraw.Draw(mask_in).line(pts + [pts[0]], fill=0, width=int(rim), joint="curve")

    # Radial gradient for the body.
    grad = Image.new("RGB", (s, s))
    gp = grad.load()
    step = max(1, s // 256)
    cx, cy, rmax = 0.5 * s, 0.52 * s, 0.48 * s
    for y in range(0, s, step):
        for x in range(0, s, step):
            t = min(1.0, math.hypot(x - cx, y - cy) / rmax)
            c = tuple(int(FILL_IN[i] + (FILL_OUT[i] - FILL_IN[i]) * t) for i in range(3))
            for yy in range(y, min(y + step, s)):
                for xx in range(x, min(x + step, s)):
                    gp[xx, yy] = c

    img = Image.new("RGBA", (s, s), RIM + (0,))
    img.putalpha(mask_out)
    body = grad.convert("RGBA")
    body.putalpha(mask_in)
    img = Image.alpha_composite(img, body)
    if size >= 32:
        d = ImageDraw.Draw(img)
        for x, y, r in dots():
            d.ellipse([(x - r) * s, (y - r) * s, (x + r) * s, (y + r) * s], fill=DOTS + (255,))
    return img.resize((size, size), Image.LANCZOS)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", type=Path)
    args = ap.parse_args()
    res = ROOT / "src" / "resources"
    res.mkdir(parents=True, exist_ok=True)
    (res / "starfish.svg").write_text(svg(), encoding="utf-8", newline="\n")
    images = {size: render(size) for size in SIZES}
    for size in [16, 20, 24, 32, 48, 64, 128, 256]:
        images[size].save(res / f"starfish-{size}.png")
    images[256].save(ROOT / "src" / "starfish.ico", sizes=[(s, s) for s in SIZES],
                     append_images=[images[s] for s in SIZES if s != 256])
    if args.preview:
        args.preview.mkdir(parents=True, exist_ok=True)
        # The sheet: every size at 1x on a light and a dark strip, then 256 px.
        w = sum(SIZES) + 12 * len(SIZES) + 12
        sheet = Image.new("RGBA", (max(w, 560), 300 + 2 * 280), (255, 255, 255, 255))
        dark = Image.new("RGBA", (sheet.width, 280), (0x20, 0x20, 0x20, 255))
        light = Image.new("RGBA", (sheet.width, 280), (0xF3, 0xF3, 0xF3, 255))
        sheet.paste(light, (0, 0))
        sheet.paste(dark, (0, 280))
        for row, top in ((0, 10), (1, 290)):
            x = 12
            for size in SIZES:
                sheet.alpha_composite(images[size], (x, top))
                x += size + 12
        sheet.alpha_composite(images[256], (12, 300 + 280 - 10))
        big = images[16].resize((128, 128), Image.NEAREST)
        sheet.alpha_composite(big, (300, 300 + 280 + 40))
        sheet.save(args.preview / "icon_sheet.png")
    print("ok")


if __name__ == "__main__":
    main()
