# VSFilterMod `\distort` override tag

This fork implements VSFilterMod’s `\distort(u1,v1,u2,v2,u3,v3)` override tag. It pins three corners of a glyph/group bounding box and warps the outline with a bilinear mapping, matching VSFilterMod semantics.

## User-facing behavior

- **Syntax:** `\distort(u1,v1,u2,v2,u3,v3)`
- **Corner pins:** parameters are doubles (no clamping). P0 (top‑left) is fixed at `(0,0)`; the parameters set the other three corners in normalized bounding-box space:
  - P1 `(u1,v1)`: top-right
  - P2 `(u2,v2)`: bottom-right
  - P3 `(u3,v3)`: bottom-left
- **Defaults / enable:** The tag is disabled until first used. Defaults are identity: `(1,0, 1,1, 0,1)`. `\r` resets to disabled and the default corners.
- **Animation:** Fully animatable with `\t`; each component interpolates independently toward its target.
- **Scope:** Applied per word-like unit (runs split at spaces/NBSP/newlines and when `\distort` parameters change). Vector drawings (`\p`) are warped per drawing chunk. All layers (fill, border, shadow) share the same warp.
- **Examples:**
  - Identity / on-switch: `{\distort(1,0,1,1,0,1)}Text` (looks unchanged, enables distortion)
  - Extreme shear: `{\distort(1.6,-0.2,1.6,1.2,0,1)}Text`
  - Negative pin: `{\distort(-0.3,0.0,1,1.3,0,1)}Text`
  - Animated: `{\t(0,1000,\distort(1,0,1.4,1,-0.4,1))}Text`

## Developer notes
visually, tag goes like this.
```
0 1
3 2
```
or
↗,↘,↙

### Math

Given an outline point `(x,y)` and the unit being warped:

```
w = maxx - minx
h = maxy - miny
if w == 0 || h == 0: skip
u = (x - minx) / w
v = (y - miny) / h
dx = u*P1.x + v*P3.x + u*v*(P2.x - P1.x - P3.x)
dy = u*P1.y + v*P3.y + u*v*(P2.y - P1.y - P3.y)
x' = minx + dx * w
y' = miny + dy * h
```

P0 is fixed at `(0,0)`; P1,P2,P3 come from the parameters above. The mapping is applied to every outline point, including Bezier control points.

### Placement in the pipeline

1. Units are detected in `apply_distortion`: consecutive glyphs with identical distortion state, split on whitespace/newline and on drawing-run boundaries. The unit’s bounding box is taken over outlines plus advance to match VSFilterMod’s per-word behavior.
2. Warp is applied immediately after layout/reorder/line alignment, before baseline shear/rotation and before any glyph transform (shear/scale/3D) inside `get_bitmap_glyph`.
3. Borders and shadows reuse the warped outline, so all layers stay aligned.

### Edge cases

- Degenerate boxes (`w==0` or `h==0`) are skipped.
- Empty/degenerate outlines are skipped from the unit bbox; distortion stays disabled for those glyphs.
- Parameters are doubles; negative and >1 values are accepted.
- `\r` clears the enabled flag and resets corners to identity.
- Caching: distorted glyphs bypass bitmap/composite cache reuse to avoid stale geometry; they render fresh per unit.

### Differences vs upstream libass

- This tag is VSFilterMod-specific; upstream libass does not support it.
- Behavior matches VSFilterMod’s per-word bilinear warp (including corners in normalized bbox space and identical warping of fill/outline/shadow). Known VSFilterMod quirks, such as allowing out-of-range pins and animating each component separately, are preserved.
