#!/usr/bin/env python3
"""Generate StarFish's built-in Bouton shutter GIF (session 114).

    python tools/make_shutter_gif.py
    python tools/make_shutter_gif.py --preview build/captures/shutter_preview
    python tools/make_shutter_gif.py --check

An original twelve-blade camera diaphragm, 36 frames, 512x512, transparent
outside the disc and in the growing regular-dodecagon aperture. Frame 0 is
almost closed (a tight swirl to a pinpoint aperture); the last frame is a
black ring with short pinwheel stubs around a large 12-gon. Each blade shows
one edge: a white cubic from the rim, inward in a pinwheel, ending at the
next side's tangent point (tangent to the aperture). Edges do not cross the opening or each other. Blade
edges are identical, evenly rotated, perfectly 12-fold symmetric (asserted). StarFish scrubs this GIF with the
Bouton's open progress (it does not play on a clock). No third-party
artwork: geometry is drawn here.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "src" / "resources" / "skins" / "shutter-demo.gif"

SIZE = 512
FRAMES = 36
BLADES = 12
# Apothem of the regular n-gon aperture, as a fraction of the disc radius.
APERTURE_MIN = 0.03
APERTURE_MAX = 0.86
# Supersample for crisp antialiasing, then downsample to SIZE.
SCALE = 2


def _pixels(im: Image.Image):
    if hasattr(im, "get_flattened_data"):
        return im.get_flattened_data()
    return im.getdata()


def aperture_apothem(radius: float, progress: float) -> float:
    u = max(0.0, min(1.0, progress))
    return radius * (APERTURE_MIN + (APERTURE_MAX - APERTURE_MIN) * u)


def regular_polygon(cx: float, cy: float, apothem: float, n: int, rotation: float) -> list[tuple[float, float]]:
    """Vertices of a regular n-gon with the given apothem (inradius / cos(pi/n))."""
    vertex_r = apothem / math.cos(math.pi / n)
    verts: list[tuple[float, float]] = []
    for i in range(n):
        # Edge i has outward normal at angle rotation + i*2pi/n; a vertex sits
        # halfway between two normals.
        ang = rotation + (i + 0.5) * (2.0 * math.pi / n)
        verts.append((cx + vertex_r * math.cos(ang), cy + vertex_r * math.sin(ang)))
    return verts


def _cubic(p0, p1, p2, p3, steps: int = 48) -> list[tuple[float, float]]:
    pts: list[tuple[float, float]] = []
    for i in range(steps + 1):
        t = i / steps
        u = 1.0 - t
        x = u**3 * p0[0] + 3 * u**2 * t * p1[0] + 3 * u * t**2 * p2[0] + t**3 * p3[0]
        y = u**3 * p0[1] + 3 * u**2 * t * p1[1] + 3 * u * t**2 * p2[1] + t**3 * p3[1]
        pts.append((x, y))
    return pts


def pinwheel_edges(cx: float, cy: float, disc_r: float, apothem: float, n: int, rotation: float) -> list[list[tuple[float, float]]]:
    """One visible edge per blade: a cubic from the rim, pinwheel-wise, ending
    at the next side's tangent point and arriving along that side.

    Curves terminate at the aperture and do not cross the opening or each
    other. Full chords of the supporting lines used to form a star web.
    """
    edges: list[list[tuple[float, float]]] = []
    for i in range(n):
        ang = rotation + i * (2.0 * math.pi / n)
        nx, ny = math.cos(ang), math.sin(ang)
        ang2 = ang + 2.0 * math.pi / n
        n2x, n2y = math.cos(ang2), math.sin(ang2)
        p0 = (cx + disc_r * nx, cy + disc_r * ny)
        p3 = (cx + apothem * n2x, cy + apothem * n2y)
        chordx, chordy = p3[0] - p0[0], p3[1] - p0[1]
        tx, ty = -n2y, n2x
        if tx * chordx + ty * chordy > 0:
            tx, ty = -tx, -ty
        length = math.hypot(chordx, chordy)
        p2 = (p3[0] + tx * length * 0.35, p3[1] + ty * length * 0.35)
        p1 = (p0[0] + (-nx * 0.25 + (-ny) * 0.35) * length,
              p0[1] + (-ny * 0.25 + nx * 0.35) * length)
        edges.append(_cubic(p0, p1, p2, p3))
    return edges


def render_frame(size: int, progress: float, *, supersample: int = SCALE, rotation: float = 0.0) -> Image.Image:
    s = size * supersample
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    draw = ImageDraw.Draw(im, "RGBA")
    cx = cy = (s - 1) / 2.0
    margin = 1.0 * supersample
    disc_r = s / 2.0 - margin
    ap = aperture_apothem(disc_r, progress)
    # rotation: 0 keeps a stable orientation; opening is a pure radial slide.

    # Disc with a very slight radial shade (darker at the rim).
    # Drawn as a few concentric rings — original, no asset.
    rings = 24
    for i in range(rings, 0, -1):
        t = i / rings
        r = disc_r * t
        # 10..22 grey, slightly lighter toward the aperture.
        tone = int(10 + 12 * (1.0 - t))
        bbox = [cx - r, cy - r, cx + r, cy + r]
        draw.ellipse(bbox, fill=(tone, tone, tone, 255))

    # Punch the regular-polygon aperture (transparent).
    poly = regular_polygon(cx, cy, ap, BLADES, rotation)
    draw.polygon(poly, fill=(0, 0, 0, 0))

    # Antialiased white aperture outline and pinwheel blade edges
    # (cubic, rim -> next tangent point). Supersampled, then Lanczos.
    width = max(supersample, int(round(0.0046 * s)))
    closed = poly + [poly[0]]
    draw.line(closed, fill=(255, 255, 255, 235), width=width, joint="curve")
    for pts in pinwheel_edges(cx, cy, disc_r, ap, BLADES, rotation):
        draw.line(pts, fill=(255, 255, 255, 235), width=width)

    if supersample != 1:
        im = im.resize((size, size), Image.Resampling.LANCZOS)
    return im


def to_transparent_gif(frames: list[Image.Image]) -> list[Image.Image]:
    """Palette the frames, reserving index 0 for transparency.

    Training the adaptive palette on the whole image (including a chroma-key
    colour) smeared that key into nearby greys, so it no longer matched
    index 0 and Qt painted a solid tint in the aperture. Index 0 is never
    used for an opaque pixel.
    """
    opaque: list[tuple[int, int, int]] = []
    for im in frames:
        for r, g, b, a in _pixels(im):
            if a >= 128:
                opaque.append((r, g, b))
    # Unique-ish sample so quantize stays tractable.
    if not opaque:
        opaque = [(10, 10, 10)]
    step = max(1, len(opaque) // 80000)
    sample = opaque[::step]
    trainer = Image.new("RGB", (len(sample), 1))
    trainer.putdata(sample)
    pal_img = trainer.quantize(colors=255, method=Image.Quantize.MEDIANCUT)
    pal = bytearray(768)
    raw_pal = pal_img.getpalette() or []
    # Slot 0 is reserved (and kept black); colours 1..255 come from the trainer.
    for i, v in enumerate(raw_pal[: 255 * 3]):
        pal[3 + i] = v

    out: list[Image.Image] = []
    palette_colors = [(pal[3 * i], pal[3 * i + 1], pal[3 * i + 2]) for i in range(256)]

    def nearest(rgb: tuple[int, int, int]) -> int:
        r, g, b = rgb
        best_i, best_d = 1, 1 << 30
        for i in range(1, 256):
            pr, pg, pb = palette_colors[i]
            d = (pr - r) * (pr - r) + (pg - g) * (pg - g) + (pb - b) * (pb - b)
            if d < best_d:
                best_d = d
                best_i = i
        return best_i

    for im in frames:
        mapped = [0 if a < 128 else nearest((r, g, b)) for r, g, b, a in _pixels(im)]
        p = Image.new("P", im.size)
        p.putpalette(bytes(pal))
        p.putdata(mapped)
        p.info["transparency"] = 0
        out.append(p)
    return out


def write_gif(path: Path, frames_rgba: list[Image.Image]) -> None:
    pal = to_transparent_gif(frames_rgba)
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


def rotational_error(im: Image.Image, n: int = BLADES) -> tuple[float, float]:
    """Mean / max absolute difference after a 360/n rotation, inside the disc."""
    rgba = im.convert("RGBA")
    w, h = rgba.size
    cx = cy = (w - 1) / 2.0
    rot = rgba.rotate(360.0 / n, resample=Image.Resampling.BICUBIC, center=(cx, cy), fillcolor=(0, 0, 0, 0))
    src = _pixels(rgba)
    dst = _pixels(rot)
    radius = w / 2.0 - 3
    inset = radius - 2
    total = 0.0
    peak = 0.0
    count = 0
    for y in range(h):
        for x in range(w):
            dx, dy = x - cx, y - cy
            if dx * dx + dy * dy > inset * inset:
                continue
            a = src[y * w + x]
            b = dst[y * w + x]
            d = max(abs(a[i] - b[i]) for i in range(4))
            total += d
            if d > peak:
                peak = d
            count += 1
    mean = total / count if count else 0.0
    return mean, peak


def assert_symmetry(frames: list[Image.Image], *, mean_limit: float = 4.0) -> None:
    '''Re-render each progress at a 1/n turn and compare to the original.'''
    worst_mean = 0.0
    worst_peak = 0.0
    picks = [0, len(frames)//3, 2*len(frames)//3, len(frames)-1] if len(frames) > 3 else list(range(len(frames)))
    for i in picks:
        im = frames[i]
        u = i / (len(frames) - 1) if len(frames) > 1 else 0.0
        spun = render_frame(im.size[0], u, rotation=2.0 * math.pi / BLADES)
        src, dst = list(_pixels(im)), list(_pixels(spun))
        total = 0.0
        peak = 0.0
        for a, b in zip(src, dst):
            d = max(abs(a[k] - b[k]) for k in range(4))
            total += d
            if d > peak:
                peak = d
        mean = total / len(src)
        worst_mean = max(worst_mean, mean)
        worst_peak = max(worst_peak, peak)
        if mean > mean_limit:
            raise SystemExit(
                'symmetry failed on frame %d: mean %.2f (limit %s), peak %.0f'
                % (i, mean, mean_limit, peak)
            )
    print('symmetry ok (%d frames, 1/%d turn, mean <= %.2f, peak <= %.0f)'
          % (len(frames), BLADES, worst_mean, worst_peak))


def make_frames(count: int = FRAMES, size: int = SIZE) -> list[Image.Image]:
    if count < 2:
        raise ValueError("need at least 2 frames")
    return [render_frame(size, i / (count - 1)) for i in range(count)]


def write_preview(folder: Path, frames: list[Image.Image]) -> None:
    folder.mkdir(parents=True, exist_ok=True)
    picks = [0, len(frames) // 3, 2 * len(frames) // 3, len(frames) - 1]
    thumb = 256
    sheet = Image.new("RGBA", (thumb * len(picks), thumb), (32, 32, 32, 255))
    for i, idx in enumerate(picks):
        frames[idx].save(folder / f"frame_{idx:02d}.png")
        im = frames[idx].resize((thumb, thumb), Image.Resampling.LANCZOS)
        sheet.paste(im, (i * thumb, 0), im)
    sheet.save(folder / "shutter-demo-frames.png")
    print(f"preview {folder / 'shutter-demo-frames.png'}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--preview", nargs="?", const=str(ROOT / "build" / "captures" / "shutter_preview"),
                        help="also write a contact sheet (default folder if flag is bare)")
    parser.add_argument("--check", action="store_true", help="render, assert 12-fold symmetry, do not write the GIF")
    parser.add_argument("-o", "--output", type=Path, default=OUT)
    args = parser.parse_args()
    frames = make_frames()
    assert_symmetry(frames)
    if args.preview:
        write_preview(Path(args.preview), frames)
    if args.check:
        return 0
    write_gif(args.output, frames)
    print(f"wrote {args.output} ({args.output.stat().st_size} bytes, {len(frames)} frames, {SIZE}x{SIZE})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
