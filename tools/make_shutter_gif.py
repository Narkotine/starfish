#!/usr/bin/env python3
"""Generate StarFish's built-in Bouton shutter GIF (session 112).

    python tools/make_shutter_gif.py
    python tools/make_shutter_gif.py --preview build/captures/shutter_preview

Writes:
  src/resources/skins/shutter-demo.gif

An original six-blade camera iris, 28 frames, square, transparent outside
the disc and in the growing aperture. Frame 0 is closed; the last frame is
open. StarFish scrubs this GIF with the Bouton's open progress (it does
not play on a clock). No third-party artwork: geometry is drawn here.
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "src" / "resources" / "skins" / "shutter-demo.gif"

SIZE = 256
FRAMES = 28
BLADES = 6
# Graphite blades, a cool rim; transparent elsewhere.
BLADE = (0x2A, 0x30, 0x38, 255)
BLADE_HI = (0x6E, 0x78, 0x86, 255)
BLADE_EDGE = (0x14, 0x16, 0x1A, 255)
RIM = (0x1A, 0x1D, 0x22, 255)
RIM_HI = (0x8A, 0x93, 0xA0, 255)


def _rot(px: float, py: float, ang: float, ox: float, oy: float) -> tuple[float, float]:
    c, s = math.cos(ang), math.sin(ang)
    x, y = px - ox, py - oy
    return ox + x * c - y * s, oy + x * s + y * c


def blade_polygon(i: int, t: float, cx: float, cy: float, r: float) -> list[tuple[float, float]]:
    """One iris blade. t=0 covers the centre; t=1 parks along the rim."""
    pivot_ang = -math.pi / 2.0 + i * (2.0 * math.pi / BLADES)
    px = cx + r * math.cos(pivot_ang)
    py = cy + r * math.sin(pivot_ang)
    # Inward heading, plus a sweep that clears a 6-blade aperture.
    heading = pivot_ang + math.pi + t * (2.0 * math.pi / BLADES + 0.22)
    length = r * 1.28
    w_base = r * 0.20
    w_mid = r * 0.42
    w_tip = r * 0.07
    local = [
        (0.00 * length, 0.15 * w_base),
        (0.08 * length, w_base),
        (0.38 * length, w_mid),
        (0.82 * length, w_tip),
        (1.00 * length, 0.0),
        (0.80 * length, -0.35 * w_tip),
        (0.22 * length, -0.22 * w_base),
        (0.00 * length, -0.08 * w_base),
    ]
    pts = []
    for lx, ly in local:
        # local x is along heading, y is perpendicular
        gx = px + lx * math.cos(heading) - ly * math.sin(heading)
        gy = py + lx * math.sin(heading) + ly * math.cos(heading)
        pts.append((gx, gy))
    return pts


def render_frame(index: int, scale: int = 2) -> Image.Image:
    t = index / (FRAMES - 1)
    # Slight ease so the last third opens more obviously.
    t = t * t * (3.0 - 2.0 * t)
    s = SIZE * scale
    cx = cy = s / 2.0
    r = s * 0.48
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img, "RGBA")

    # Blades first (under the housing ring).
    for i in range(BLADES):
        poly = blade_polygon(i, t, cx, cy, r)
        draw.polygon(poly, fill=BLADE, outline=BLADE_EDGE)
        # Leading-edge highlight: first two vertices toward the tip, a thin stroke.
        if len(poly) >= 5:
            draw.line([poly[0], poly[1], poly[2]], fill=BLADE_HI, width=max(1, scale))

    # Clip everything to the disc: punch outside back to transparent.
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).ellipse(
        [cx - r, cy - r, cx + r, cy + r], fill=255
    )
    # Soften the disc edge one pixel (at final size) so it sits in the Bouton.
    if scale > 1:
        mask = mask.filter(ImageFilter.GaussianBlur(radius=scale * 0.35))
    cleared = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    cleared.paste(img, mask=mask)
    img = cleared
    draw = ImageDraw.Draw(img, "RGBA")

    # Housing ring (always present, even when fully open).
    ring_w = max(scale * 3, 3)
    inset = ring_w / 2.0
    bbox = [cx - r + inset, cy - r + inset, cx + r - inset, cy + r - inset]
    draw.ellipse(bbox, outline=RIM, width=ring_w)
    draw.ellipse(bbox, outline=RIM_HI, width=max(1, scale))

    if scale > 1:
        img = img.resize((SIZE, SIZE), Image.Resampling.LANCZOS)
    return img


def to_transparent_gif(frames: list[Image.Image]) -> list[Image.Image]:
    """Shared palette, index 0 reserved for transparency (never used for opaque).

    Adaptive quantization must not smear a chroma key into the aperture:
    Qt would then paint the opening as solid green.
    """
    opaque_samples: list[tuple[int, int, int]] = []
    for im in frames:
        for r, g, b, a in im.getdata():
            if a >= 128:
                opaque_samples.append((r, g, b))
    if opaque_samples:
        train = Image.new("RGB", (len(opaque_samples), 1))
        train.putdata(opaque_samples)
        trained = train.quantize(colors=255, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        raw_pal = trained.getpalette() or []
        colours = [(raw_pal[i], raw_pal[i + 1], raw_pal[i + 2]) for i in range(0, min(len(raw_pal), 765), 3)]
    else:
        colours = []
    colours = colours[:255]
    while len(colours) < 255:
        colours.append((0, 0, 0))
    pal_colours = [(0, 255, 0)] + colours  # index 0 reserved, never selected
    pal_bytes = b"".join(bytes(c) for c in pal_colours)

    # Cache unique RGB -> index so we do not scan 255 colours per pixel.
    lookup: dict[tuple[int, int, int], int] = {}

    def nearest(rgb: tuple[int, int, int]) -> int:
        found = lookup.get(rgb)
        if found is not None:
            return found
        r, g, b = rgb
        best, best_d = 1, 1 << 30
        for i, (cr, cg, cb) in enumerate(colours, start=1):
            d = (r - cr) * (r - cr) + (g - cg) * (g - cg) + (b - cb) * (b - cb)
            if d < best_d:
                best, best_d = i, d
                if d == 0:
                    break
        lookup[rgb] = best
        return best

    out: list[Image.Image] = []
    for im in frames:
        mapped = [0 if a < 128 else nearest((r, g, b)) for r, g, b, a in im.getdata()]
        p = Image.new("P", im.size)
        p.putpalette(pal_bytes)
        p.putdata(mapped)
        p.info["transparency"] = 0
        p.info["disposal"] = 2
        out.append(p)
    return out



def write_gif(path: Path, preview: Path | None) -> None:
    raw = [render_frame(i) for i in range(FRAMES)]
    pal = to_transparent_gif(raw)
    path.parent.mkdir(parents=True, exist_ok=True)
    pal[0].save(
        path,
        format="GIF",
        save_all=True,
        append_images=pal[1:],
        loop=0,
        duration=40,
        disposal=2,
        transparency=0,
        optimize=False,
    )
    print(f"wrote {path} ({path.stat().st_size} bytes, {FRAMES} frames, {SIZE}x{SIZE})")
    if preview:
        preview.mkdir(parents=True, exist_ok=True)
        cols = 7
        rows = (FRAMES + cols - 1) // cols
        sheet = Image.new("RGBA", (cols * SIZE, rows * SIZE), (0x40, 0x44, 0x4A, 255))
        # chequer so transparency reads
        for y in range(0, sheet.height, 8):
            for x in range(0, sheet.width, 8):
                if (x // 8 + y // 8) % 2 == 0:
                    ImageDraw.Draw(sheet).rectangle([x, y, x + 7, y + 7], fill=(0x58, 0x5C, 0x64, 255))
        for i, im in enumerate(raw):
            r, c = divmod(i, cols)
            sheet.paste(im, (c * SIZE, r * SIZE), im)
        sheet_path = preview / "shutter-demo-frames.png"
        sheet.save(sheet_path)
        print(f"preview {sheet_path}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output", type=Path, default=OUT)
    parser.add_argument("--preview", type=Path, default=None)
    args = parser.parse_args()
    write_gif(args.output, args.preview)


if __name__ == "__main__":
    main()
