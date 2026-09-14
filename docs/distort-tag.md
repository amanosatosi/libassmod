# Mangetsu `\distort` override tag

Mangetsu implements VSFilterMod’s six-argument `\distort` override tag and an extended eight-argument form that also exposes the top-left corner. Both forms use the existing bilinear outline warp.

## User-facing behavior

- **Legacy syntax:** `\distort(u1,v1,u2,v2,u3,v3)` — P0 is `(0,0)`.
- **Extended syntax:** `\distort(u1,v1,u2,v2,u3,v3,u0,v0)`.
- **Corner pins:** parameters are doubles (no clamping), in normalized bounding-box space. Signed and fractional values use the same parsing rules for every corner:
  - P0 `(u0,v0)`: top-left; the final two arguments, when present
  - P1 `(u1,v1)`: top-right
  - P2 `(u2,v2)`: bottom-right
  - P3 `(u3,v3)`: bottom-left
- **Relative coordinates:** use explicit `~+N` or `~-N`, for example
  `\distort(~+0.1,0,1,1,0,1,~+0.2,~-0.1)`. Bare signs remain absolute.
  See [relative numeric values](relative-numbers.md).
- **Defaults / enable:** The tag is disabled until first used. Defaults are identity: P0 `(0,0)`, P1 `(1,0)`, P2 `(1,1)`, P3 `(0,1)`. `\r` resets to disabled and the default corners. A six-argument tag after an eight-argument tag restores P0 to `(0,0)`.
- **Animation:** Fully animatable with `\t`; each component, including `u0,v0`, interpolates independently toward its target with the same timing and acceleration. Six-argument transform targets interpolate P0 toward `(0,0)`. All transitions between six- and eight-argument states are supported.
- **Scope:** Applied per compactable same-style text run on each visual line. Internal spaces/NBSP stay inside the run; hard line breaks, effective style changes, `\distort` parameter changes, and vector-drawing chunks split units. Every glyph emitted for a shaped cluster receives the same warp, and all layers (fill, border, shadow) stay aligned.
- **Examples:**
  - Identity / on-switch: `{\distort(1,0,1,1,0,1)}Text` (looks unchanged, enables distortion)
  - Extreme shear: `{\distort(1.6,-0.2,1.6,1.2,0,1)}Text`
  - Negative pin: `{\distort(-0.3,0.0,1,1.3,0,1)}Text`
  - Animated: `{\t(0,1000,\distort(1,0,1.4,1,-0.4,1))}Text`
  - Move only P0: `{\distort(1,0,1,1,0,1,0.2,0.1)}Text`
  - Animate only P0: `{\t(0,1000,\distort(1,0,1,1,0,1,0.2,0.1))}Text`
  - Animate P0 back to zero: `{\distort(1,0,1,1,0,1,0.2,0.1)\t(0,1000,\distort(1,0,1,1,0,1))}Text`

### Corner order and compatibility

The original Mangetsu syntax exposed P1/P2/P3 while P0 was fixed at `(0,0)`.
P0 was later added at the end to preserve compatibility with existing scripts.
The original six arguments keep their order, meaning, and interpolation.

```text
P0 -------- P1
 |           |
 |           |
P3 -------- P2
```

```text
P0 = final two arguments in the extended syntax (otherwise (0,0))
P1 = arguments 1–2
P2 = arguments 3–4
P3 = arguments 5–6
```

For example, `\distort(1,0,1,1,0,1,0.2,0.1)` sets P0 to `(0.2,0.1)`
while P1, P2, and P3 remain `(1,0)`, `(1,1)`, and `(0,1)`.

## Developer notes

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
if P0 != (0,0):
    dx += (1-u)*(1-v)*P0.x
    dy += (1-u)*(1-v)*P0.y
x' = minx + dx * w
y' = miny + dy * h
```

This is the four-corner bilinear mapping. The original expression and operation
order are retained, and the P0 contribution is skipped when P0 is `(0,0)` to
preserve legacy floating-point results. The mapping is applied to every outline
point, including Bezier control points. It remains a bilinear warp, not a new
projective homography. Existing later projection stages are unchanged.

`ASS_DistortParams` carries P0/P1/P2/P3 together through render and glyph state;
only the parser uses the historical syntax order. Distortion-run grouping
compares all four corners in addition to normal style-run boundaries.
BorderStyle=4 boxes use the same mapping before their existing projective
transform.

### Placement in the pipeline

1. Units are detected in `apply_distortion`: consecutive glyphs in the same effective style run with identical distortion state. Internal whitespace does not split a unit; hard line breaks and drawing-run boundaries do. The unit bounding box is taken only over actual transformed outline path points. Whitespace contributes positioning through the following glyph positions but adds no bbox points.
2. Warp is applied immediately after layout/reorder/line alignment, before baseline shear/rotation and before any glyph transform (shear/scale/3D) inside `get_bitmap_glyph`.
3. Borders and shadows reuse the warped outline, so all layers stay aligned. Every glyph linked through a shaped cluster’s `GlyphInfo::next` chain receives the same unit warp.

### Edge cases

- Degenerate boxes (`w==0` or `h==0`) are skipped.
- Empty outlines, including whitespace, contribute no bbox points but may remain inside an otherwise valid distortion unit.
- Parameters are doubles; negative and >1 values are accepted.
- Empty coordinate slots retain their corresponding current values. The
  six-slot form still implies P0 `(0,0)`, even with empty slots.
- Malformed counts keep the previous fallback behavior: fewer than six slots
  or a nonempty seventh slot leave the state unchanged. The historically
  accepted empty seventh slot (a trailing comma) still behaves as the six-slot
  form. Eight slots are now valid; additional slots remain invalid.
- `\r` clears the enabled flag and resets corners to identity.
- Caching: distorted glyphs bypass bitmap/composite cache reuse to avoid stale geometry; they render fresh per unit.

### Differences vs upstream libass

- This tag is VSFilterMod-specific; upstream libass does not support it.
- The six-argument form retains VSFilterMod’s bilinear warp over compacted same-style text runs (including corners in normalized path-point bbox space and identical warping of fill/outline/shadow). Known VSFilterMod quirks, such as allowing out-of-range pins and animating each component separately, are preserved. The optional P0 pair is a Mangetsu extension.

### Regression tests

`distortion-warp` checks the legacy formula bit for bit, all four corners, and
interior P0 weights. `distortion-render` compares pixel masks for identity and
nontrivial legacy forms, P0 movement and numeric syntax, resets, malformed
counts, fill/border/shadow and BS4 geometry, text-run grouping across internal
whitespace and style boundaries, and animated six/eight-argument transitions
(including acceleration and seeking).
