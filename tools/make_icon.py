#!/usr/bin/env python3
"""Generate src/starfish.ico -- the application icon embedded in starfish.exe.

Drawn rather than hand-painted, so the mark can be adjusted (arm shape, colours,
padding) and regenerated deterministically:

    python tools/make_icon.py                    # writes src/starfish.ico
    python tools/make_icon.py --variant panel     # the fence palette instead
    python tools/make_icon.py --shape star        # a plain star, for comparison
    python tools/make_icon.py --preview build/    # also write preview PNGs

The mark is a starfish: the name is the product, and a glyph of the thing named
reads at 16 px in Explorer and in the taskbar, where a miniature fence does not.
Two details keep it from reading as a rating star -- the arms are long and taper
to a rounded point rather than meeting in sharp notches, and they sweep together
in one rotational direction the way a real starfish's arms do.

Everything is rendered on a supersampled canvas (1024 px) and downsampled per
size with Lanczos, so one drawing serves 16 px and 256 px. Each arm is a spine
sampled along an arc, offset left and right by an interpolated half-width, with a
circle capping the tip; five arms and a central disc are unioned, then the mask is
blurred and re-thresholded, which rounds the concave joins instead of leaving
five hard wedge cuts.
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

S = 1024  # supersampled canvas; the source of every size below
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]

# The fence palette (src/ui/tray_icon.cpp), so the icon belongs to the product.
PANEL = (0x1F, 0x29, 0x37)
TITLE_BAR = (0x60, 0xA5, 0xFA)
TILE_TOP = (0x7D, 0xB4, 0xFC)
TILE_BOTTOM = (0x1D, 0x4E, 0xD8)
INK = (0xF8, 0xFA, 0xFC)

# Arm geometry, in fractions of the starfish's radius. The arms are the whole
# shape: long, thick where they leave the body, rounded at the tip, swept.
# `starfish` is tuned by measuring legibility, not by taste: the first version
# (a fatter body, 0.26) turned into a white blob at 24 px and below, because the
# arms had no room between them. A small body is what keeps five arms visible at
# 16 px -- the joins have to be deep, not merely rounded.
SHAPES = {
    "starfish": dict(body=0.13, start=0.04, tip=0.86, width=0.30, tip_width=0.13, cap=0.13, sweep=0.16),
    "bold": dict(body=0.19, start=0.04, tip=0.84, width=0.34, tip_width=0.17, cap=0.17, sweep=0.16),
    "star": dict(body=0.12, start=0.04, tip=0.86, width=0.26, tip_width=0.08, cap=0.06, sweep=0.0),
}


def starfish_mask(size: int, shape: dict, radius: float) -> Image.Image:
    """The starfish silhouette -- five swept arms and a body, white on black."""
    mask = Image.new("L", (size, size), 0)
    draw = ImageDraw.Draw(mask)
    cx = cy = size / 2.0
    r = radius * size
    steps = 48

    for arm in range(5):
        base_angle = math.radians(-90.0 + 72.0 * arm)  # first arm straight up
        left, right = [], []
        for step in range(steps + 1):
            t = step / steps
            # The sweep grows towards the tip, so the arm bends instead of kinking.
            angle = base_angle + shape["sweep"] * t**1.5
            distance = (shape["start"] + (shape["tip"] - shape["start"]) * t) * r
            half = (shape["width"] + (shape["tip_width"] - shape["width"]) * t) * r
            u = (math.cos(angle), math.sin(angle))
            v = (-u[1], u[0])
            px, py = cx + distance * u[0], cy + distance * u[1]
            left.append((px + half * v[0], py + half * v[1]))
            right.append((px - half * v[0], py - half * v[1]))
        draw.polygon(left + right[::-1], fill=255)

        # Cap the tip with a circle whose far edge lands exactly on `radius`.
        tipx = (left[-1][0] + right[-1][0]) / 2.0
        tipy = (left[-1][1] + right[-1][1]) / 2.0
        cap = shape["cap"] * r
        draw.ellipse([tipx - cap, tipy - cap, tipx + cap, tipy + cap], fill=255)

    body = shape["body"] * r
    draw.ellipse([cx - body, cy - body, cx + body, cy + body], fill=255)

    # Blur then re-threshold: the concave joins between arms become rounded.
    mask = mask.filter(ImageFilter.GaussianBlur(0.010 * size))
    return mask.point(lambda value: 255 if value >= 110 else 0)


def gradient(size: int, top: tuple, bottom: tuple) -> Image.Image:
    image = Image.new("RGB", (size, size))
    draw = ImageDraw.Draw(image)
    for y in range(size):
        t = y / max(size - 1, 1)
        draw.line([(0, y), (size, y)], fill=tuple(round(top[c] + (bottom[c] - top[c]) * t) for c in range(3)))
    return image


def rounded_mask(size: int, radius_fraction: float) -> Image.Image:
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, size - 1, size - 1], radius=round(radius_fraction * size), fill=255)
    return mask


def build(variant: str, shape: dict) -> tuple[Image.Image, Image.Image]:
    """Return (background RGBA, foreground mask) at the supersampled size."""
    if variant == "tile":
        bg = gradient(S, TILE_TOP, TILE_BOTTOM).convert("RGBA")
        # A soft sheen across the top, so the tile is not a flat swatch.
        sheen = Image.new("L", (S, S), 0)
        ImageDraw.Draw(sheen).ellipse([-0.35 * S, -0.75 * S, 1.35 * S, 0.42 * S], fill=48)
        bg = Image.composite(Image.new("RGBA", (S, S), (255, 255, 255, 255)), bg, sheen)
        bg.putalpha(rounded_mask(S, 0.22))
        return bg, starfish_mask(S, shape, radius=0.44)

    if variant == "panel":
        # The fence itself: the dark panel, a lighter band where a fence's title
        # bar sits, and the starfish in the accent colour.
        bg = Image.new("RGBA", (S, S), PANEL + (255,))
        band = Image.new("L", (S, S), 0)
        ImageDraw.Draw(band).rectangle([0, 0, S, round(0.34 * S)], fill=70)
        bg = Image.composite(
            Image.new("RGBA", (S, S), TITLE_BAR + (255,)), bg, band.filter(ImageFilter.GaussianBlur(0.02 * S))
        )
        bg.putalpha(rounded_mask(S, 0.20))
        return bg, starfish_mask(S, shape, radius=0.44)

    raise SystemExit(f"unknown variant: {variant}")


def render(variant: str, shape: dict, size: int) -> Image.Image:
    bg, mask = build(variant, shape)
    bg = bg.resize((size, size), Image.Resampling.LANCZOS)
    mask = mask.resize((size, size), Image.Resampling.LANCZOS)
    ink = INK if variant == "tile" else TITLE_BAR
    return Image.composite(Image.new("RGBA", (size, size), ink + (255,)), bg, mask)


def _frames(path: Path) -> list[tuple[int, int]]:
    """Read the ICO back so the printed frame list is measured, not assumed."""
    with Image.open(path) as ico:
        # Pillow exposes an ICO's directory as `.ico.sizes()`, not through the
        # multi-frame API: `n_frames` is None and `seek()` cannot see them.
        return sorted(ico.ico.sizes())


def preview(path: Path, variant: str, shape: dict) -> None:
    """A sheet with every size the ICO carries, on a dark and on a light band."""
    sheet = Image.new("RGBA", (1000, 340), (0x20, 0x20, 0x20, 255))
    # The taskbar and Explorer do not share a background, and an icon judged on
    # one of them is judged by half.
    ImageDraw.Draw(sheet).rectangle([0, 170, sheet.width, 340], fill=(0xF3, 0xF4, 0xF6, 255))
    for x, size in ((30, 256), (320, 128), (470, 64), (560, 48), (630, 32), (682, 24), (720, 16)):
        image = render(variant, shape, size)
        sheet.alpha_composite(image, (x, 170 + (160 - size) // 2))
        sheet.alpha_composite(image, (x, (170 - size) // 2))
    path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", default="tile", choices=["tile", "panel"])
    parser.add_argument("--shape", default="starfish", choices=sorted(SHAPES))
    parser.add_argument("--out", default="src/starfish.ico")
    parser.add_argument("--preview", default=None, help="directory for preview PNGs")
    args = parser.parse_args()

    shape = SHAPES[args.shape]
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    # Each frame is rendered from the supersample rather than sliced out of the
    # base image, so the 16 px frame is a reduction of the drawing itself.
    render(args.variant, shape, 256).save(
        out,
        format="ICO",
        sizes=[(size, size) for size in SIZES],
        append_images=[render(args.variant, shape, size) for size in SIZES if size != 256],
    )
    print(f"wrote {out} ({out.stat().st_size} bytes), frames: {_frames(out)}")

    if args.preview:
        directory = Path(args.preview)
        directory.mkdir(parents=True, exist_ok=True)
        for size in (16, 24, 32, 48, 64, 128, 256):
            render(args.variant, shape, size).save(directory / f"icon_{size}.png")
        preview(directory / f"preview_{args.variant}_{args.shape}.png", args.variant, shape)
        print(f"wrote previews to {directory}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
