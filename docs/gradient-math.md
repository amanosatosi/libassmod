---
title: Gradient Math
---

# Gradient Math

This note explains how libass computes vector gradients (`\1vc`..`\4vc` and
`\1va`..`\4va`) at render time. The implementation lives in `libass/gradient.c`
and is applied during RGBA rendering in `libass/ass_render.c`.

## Overview

Gradients are bilinear: each pixel samples the four corner values and blends
them by a 2D weight derived from `(u, v)` in the range `[0, 1]`.

Corner layout follows the tag order:

```
0: top-left     1: top-right
2: bottom-left  3: bottom-right
```

The same math is used for color channels and alpha values.

## Coordinate normalization

For each rendered bitmap, libass defines a logical rectangle in pixel space.
Every pixel in the bitmap is mapped to `(u, v)` using its position inside that
rectangle:

```
u = x / (W - 1)
v = y / (H - 1)
```

`W`/`H` are the logical width/height. If `W <= 1` or `H <= 1`, the coordinate
falls back to `0` (no division).

In code this happens in `render_bitmap_rgba()` in `libass/ass_render.c` where:

- `src_x`/`src_y` are the pixel coordinates inside the logical bitmap
  (relative to the logical origin).
- `full_w`/`full_h` are the logical dimensions.
- `u`/`v` are computed in fixed-point as `uf`/`vf` in 16.16 format.

## Bilinear sampling

Given corner values `c0..c3` and fixed-point `uf`/`vf` in `[0, 65536]`,
the weights are:

```
w0 = 1 - u
w1 = u
h0 = 1 - v
h1 = v

value = c0*w0*h0 + c1*w1*h0 + c2*w0*h1 + c3*w1*h1
```

The fixed-point path uses 16.16 weights and accumulates in 64-bit, then
truncates by shifting right 32 bits. This is implemented in
`ass_gradient_sample_color_fixed()` / `ass_gradient_sample_alpha_fixed()` in
`libass/gradient.c`.

## Fixed-frame Mangetsu primary gradients

`\pgrd` / `\1pgrd` use the Mangetsu linear-stop sampler, not the bilinear
sampler described above. Their rectangle is converted from ASS script
coordinates to final frame coordinates using the same positioned-coordinate
mapping as `\pos` and `\move`. The renderer normalizes the rectangle, projects
its four final-frame corners onto the existing Mangetsu angle direction, and
maps the interval between the minimum and maximum projections to `[0, 1]`.

Only final pixel centres inside the rectangle are sampled. Pixels outside are
not endpoint-clamped; they retain the ordinary active primary color. Full tag,
transform, and edge semantics are documented in `position-gradient.md`.

## Color vs alpha

Color and alpha use the same bilinear math, but are stored separately:

- Colors are stored in RGBA with per-corner color values.
- Alpha uses per-corner alpha values.

In `render_bitmap_rgba()`:

- The per-pixel coverage from the glyph mask is combined with the sampled
  alpha to produce premultiplied output.
- The sampled RGB is multiplied by the final alpha (`A`) so the result is
  premultiplied RGBA.

## Target bounds

Four-corner vector sampling uses each final combined bitmap's logical
dimensions and keeps the original source coordinates when clipping or karaoke
splits a tile. These VSFilterMod-compatible coordinates are unchanged.

Attached Mangetsu gradients use a union of rendered target bounds for runs
sharing a gradient segment and scrolling participation. Fill uses glyph bounds;
shadow uses its shadow bitmap; native outline layers use the occupied mask
bounds of their individual ring, including blur. The latter avoids extending
an inner ring's gradient over allocation padding shared with larger rings.
Fifth-channel decoration gradients are transferred to the separate decoration
bitmap before these bounds are computed.

BS4 has one captured event box (or one per scrolling participation region).
Its fill and each outward ring use their own transformed logical bounds.
All attached angles use final screen axes over those bounds. Positioned
primary gradients instead use the fixed script-frame rectangle documented
above. See [gradient targets and limits](gradient-support.md).

## Notes and limits

- If gradients are disabled for a layer, the base color/alpha is used instead.
- The fixed-point path clamps `uf`/`vf` into `[0, 65536]` to avoid overflow.
- The math is purely per-pixel; there is no gamma correction or perceptual
  blending.
