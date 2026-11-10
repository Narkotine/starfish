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
| Expanded geometry | Stored `rect` | Larger circle (stored `rect`) | Soft stretch |
| Item layout | Icon grid | Concentric rings (0 = top, clockwise; extra rings if needed) | Free / theme |
| Open / close | Roll-up animation | Scale + ease (overshoot) | Path morph |
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
