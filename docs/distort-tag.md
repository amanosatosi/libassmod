# Mangetsu `\distort` override tag

Mangetsu implements VSFilterMod’s six-argument `\distort` override tag and an extended eight-argument form that exposes the top-left corner and can warp a multiline text unit through one shared rectangle. Both forms use the same bilinear outline warp, not projective perspective.

Use [`\wtan1`–`\wtan9`](warp-text-anchor.md) to place the complete warped
multiline text block by its final outline bounds.

## User-facing behavior

- **Legacy syntax (6 args):** `\distort(u1,v1,u2,v2,u3,v3)` — the VSFilterMod-compatible three-point form. P0 is fixed at `(0,0)` and legacy line/run behavior is preserved.
- **Extended syntax (8 args):** `\distort(u1,v1,u2,v2,u3,v3,u0,v0)` — the Mangetsu four-point extension. P0 comes from the final two arguments. Eligible same-style text can span visual lines as one distortion unit, even when P0 is explicitly `(0,0)`.
- **Corner pins:** parameters are doubles (no clamping), in normalized bounding-box space. Signed and fractional values use the same parsing rules for every corner:
  - P0 `(u0,v0)`: top-left; the final two arguments, when present
  - P1 `(u1,v1)`: top-right
  - P2 `(u2,v2)`: bottom-right
  - P3 `(u3,v3)`: bottom-left
- **Relative coordinates:** use explicit `~+N` or `~-N`, for example
  `\distort(~+0.1,0,1,1,0,1,~+0.2,~-0.1)`. Bare signs remain absolute.
  See [relative numeric values](relative-numbers.md).
- **Defaults / enable:** The tag is disabled until first used. Defaults are identity: P0 `(0,0)`, P1 `(1,0)`, P2 `(1,1)`, P3 `(0,1)`. `\r` resets to disabled, legacy mode and the default corners. An ordinary six-argument tag after an eight-argument tag restores P0 to `(0,0)` and returns to line-local mode.
- **Animation:** Fully animatable with `\t`; each component, including `u0,v0`, interpolates independently toward its target with the same timing and acceleration. Six-argument transform targets interpolate P0 toward `(0,0)`. The form is discrete: each valid transform target selects its six- or eight-slot grouping immediately, including at zero progress; the boolean mode is not interpolated. Override parsing restarts from defaults each frame, so seeking does not retain a previous frame's mode or bbox.
- **Scope:** Six arguments warp each compactable same-style run on each visual line, as before. Eight arguments let an eligible unit cross hard `\N` breaks and automatic visual-line breaks. Internal spaces/NBSP stay inside both forms. Effective style changes, effective `\distort` parameter or form changes, required shaping/render-state boundaries, and vector-drawing chunks still split units. Every glyph emitted for a shaped cluster receives the same unit warp, and all layers (fill, border, shadow) stay aligned.
- **Examples:**
  - Identity / on-switch: `{\distort(1,0,1,1,0,1)}Text` (looks unchanged, enables distortion)
  - Extreme shear: `{\distort(1.6,-0.2,1.6,1.2,0,1)}Text`
  - Negative pin: `{\distort(-0.3,0.0,1,1.3,0,1)}Text`
  - Animated: `{\t(0,1000,\distort(1,0,1.4,1,-0.4,1))}Text`
  - Move only P0: `{\distort(1,0,1,1,0,1,0.2,0.1)}Text`
  - Shared multiline rectangle: `{\an5\distort(1,-0.15,1.2,1.15,-0.1,1.1,0.1,0.05)}FIRST LINE\NMUCH LONGER SECOND LINE\NTHIRD`
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
only the parser uses the historical syntax order. A separate `distort_extended`
flag records the eight-slot form; it is not inferred from the numerical P0.
Grouping compares the form and all four corners. Extended grouping uses the
effective style boundaries recorded before line trimming adds shaping breaks.
BorderStyle=4 boxes use the same mapping before their normal event transform.
Legacy boxes retain their padded fill domain. Extended boxes use their canonical
glyph's shared source bbox for the fill and all box-border rectangles, so box
padding follows the same deformation as the participating text.

### Placement in the pipeline

1. Units are detected in `apply_distortion`: consecutive glyphs in the same effective style run with identical distortion state and form. Internal whitespace does not split a unit. Six-slot units stop at visual-line boundaries. Eight-slot text units can cross those boundaries, while retaining style/parameter and drawing-run boundaries. The source bbox is accumulated once over actual transformed outline path points across all participating lines, then reused for every outline in the unit. Whitespace and hard-break markers add no bbox points; whitespace contributes positioning through following glyphs.
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
- `\r` clears the enabled and extended flags and resets corners to identity.
- Caching: distorted glyphs bypass bitmap/composite cache reuse to avoid stale geometry; they render fresh per unit.

### Differences vs upstream libass

- This tag is VSFilterMod-specific; upstream libass does not support it.
- The six-argument form retains VSFilterMod’s line-local bilinear warp over compacted same-style text runs (including corners in normalized path-point bbox space and identical warping of fill/outline/shadow). Known VSFilterMod quirks, such as allowing out-of-range pins and animating each component separately, are preserved. The appended P0 pair and multiline-capable domain are Mangetsu extensions selected only by the eight-slot form.

### Regression tests

`distortion-warp` checks the legacy formula bit for bit, all four corners, and
interior P0 weights. `distortion-render` compares pixel masks for identity and
nontrivial legacy forms, P0 movement and numeric syntax, resets, malformed
counts, fill/border/shadow and BS4 geometry, text-run grouping across internal
whitespace and style boundaries, shared two-/three-line bboxes, explicit zero-P0
mode detection, legacy line-local output, warped-block anchors, and animated
six/eight-argument transitions (including acceleration and out-of-order seeking).
