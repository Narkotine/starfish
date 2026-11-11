# Zone shapes (special fences) — design note
# Session 101 (T-095 / T-126 baby step)

## Goal

Classic fences stay a rounded rectangle with a title-bar roll-up. New
“organic” presentations (button, petal SVG later, …) must plug in without
rewriting `FenceContainer`. Physics (optional, never the default) will later
read each shape’s bounding geometry.

## Abstraction

`starfish::ui::zone_shape` (header-only helpers today; presenters can grow):

| Concern | Classic | Button (v1) | Later (petal, …) |
|---|---|---|---|
| Collapsed geometry | Title-bar strip | Circle ≈ 2× icon | SVG silhouette |
| Expanded geometry | Stored `rect` | Larger circle; stored `rect` stays centred on the collapsed anchor (display may shift to stay on-screen) | Soft stretch |
| Item layout | Icon grid | Concentric rings (0 = top, clockwise; extra rings if needed) | Free / theme |
| Open / close | Roll-up animation | Single-radius scale (always a circle; near an edge the shown centre shifts once to fit, then returns to the stored anchor) | Path morph |
| Hit-test | Rect / region | Circle (and icon cells) | Path |

Persistence: `FenceStyle.shape` — absent / `""` = classic (backward compatible);
`"button"` = Bouton. Unknown values are kept and treated as classic until a
newer build knows them (same pattern as widget types).

`rolledUp` still means “collapsed” for every shape. Hover-unroll and the roll
timer reuse the existing container paths; only `visibleRectOf` and painting
branch on `shape`.

## Physics (later)

A shape exposes `bounds(progress)` (and later a coarse polygon). The optional
physics toggle will push neighbours from those bounds during expand — not in v1.

## Creating a Bouton

Desktop right-click → **New button** (`starfish:create-button`), next to
Create a fence. Born collapsed at a free spot; hover or click expands.

## Non-goals for v1

Physics, SVG petals, free item positions inside classic fences (T-126),
changing classic roll-up.


## Expand / collapse (session 105)

The collapsed centre is the anchor and does not move except by a user drag
(unlocked). Hover or click expands a single radius (`width == height` every
frame, mask and paint are the inscribed circle). If the expanded circle would
leave the screen, its *shown* centre is computed once at expand start so the
circle fits, then collapse returns exactly to the anchor. Overlap-push and
classic roll-edge clamp never run on a Bouton during that animation. Leave
means the cursor is outside the expanded circle, plus a short delay; a new
expand/collapse does not start while one is already running.

A locked Bouton never moves: no title-bar drag, no border resize hit, no
leftover shift of the anchor after a near-edge expand.

## Hover drift (session 106)

The stored centre is a durable anchor (`buttonAnchor`), never re-derived from
a rebuilt square. `buttonSquareAt` uses `QRect::moveCenter` so it is the exact
inverse of `QRect::center()` (the old `x - d/2` reconstruction walked one
pixel toward the top-left per cycle when the diameter was even). Physical
diameters are snapped so `logical * dpr` maps back without clipping the disc
at 125 % / 150 %, and the frame is painted centred if a rounded pixel remains.
A Bouton moves only when deliberately dragged, and never when locked.


## Move mode (session 107)

A Bouton is not movable by default: the disc is not a title-bar grab, the
cursor stays an arrow, `WM_NCHITTEST` stays `HTCLIENT`, and icons inside the
expanded disc are clicked, launched and dragged like icons in a classic zone.
Moving is an explicit compact-only mode, opened from the Bouton's menu
("Move button" / "Déplacer le bouton"), with a move cursor and an accent
outline (same language as a widget's resize mode). It ends on drop, Esc, a
click elsewhere, or after 10 s idle. The entry is greyed when the Bouton is
locked. The stored anchor is updated only when a move-mode drag completes.
