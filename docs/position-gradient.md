---
title: Fixed-frame primary gradients
---

# Fixed-frame primary gradients

`\pgrd` and `\1pgrd` are aliases for an RGBA-only Mangetsu linear gradient on
the normal primary fill. Unlike `\1grd`, they are fields fixed in the subtitle
frame, not effects attached to text bounds.

```ass
\pgrd(x1,y1,x2,y2,angle,stops...)
\1pgrd(x1,y1,x2,y2,angle,stops...)
```

Only the primary fill is supported. There are no positioned alpha, outline,
shadow, decoration, box, or native-border variants.

## Arguments and stops

The first four values are finite decimal ASS script coordinates. `angle` is a
finite decimal and appears before the first stop. Negative and off-frame
coordinates are valid. The stop section is exactly the existing `\1grd`
grammar: ASS BGR colors (`&HBBGGRR&`), an optional `position%, color` pair
between the first and final colors, sorted positions, duplicate positions, and
the existing maximum stop count. For example:

```ass
{\1c&HFFFFFF&\pgrd(100,200,700,500,45,&H0000FF&,50%,&H00FF00&,&HFF0000&)}Text
```

Malformed parentheses, missing/empty fields, non-finite values, trailing
numeric garbage, or malformed stops reject the entire tag without changing the
current color source. The explicit reset forms are `\pgrd()` and `\1pgrd()`.
They disable a currently active positioned primary gradient and leave the
ordinary primary color intact.

## Fixed-frame mapping

The renderer converts each rectangle endpoint with the same script-to-output
position mapping used by fixed ASS coordinates such as `\pos`, `\move`, and
rectangular `\clip`: PlayRes, output size, margins, storage resolution, and
pixel-aspect handling are all respected. The rectangle is then normalized to
`left=min(x1,x2)`, `right=max(x1,x2)`, `top=min(y1,y2)`, and
`bottom=max(y1,y2)` without clipping it to the frame.

Angle direction is identical to `\1grd`: `0` progresses left-to-right;
positive angles rotate in the renderer's screen coordinate system (toward
increasing Y), and decimals, negative values, and values outside 0 to 360 use the
same trigonometric behavior. The four rectangle corners are projected onto
that direction. Their minimum projection maps to the first stop and their
maximum maps to the final stop, so arbitrary diagonal angles cover the whole
rectangle.

For a final destination pixel, the renderer tests its centre (`dst_x + 0.5`,
`dst_y + 0.5`) inclusively against the converted rectangle. An inside pixel is
sampled from the projected stop list. An outside pixel is **not** clamped to an
endpoint: it uses the active ordinary primary RGB (`\1c`/`\c`, style color, or
actor-colorcoded primary color). Primary alpha remains the normal active alpha
both inside and outside the rectangle. Zero-size rectangles or invalid projected
ranges are empty and therefore use the ordinary primary color everywhere.

Consequently, `\pos`, `\move`, rotation, scaling, shearing, distortion,
perspective transforms, font changes, shaping, wrapping, and `\N` alter the
text pixels but never move, resize, rotate, or restart the gradient field.
Clipping only controls visibility and does not modify the field.

## Precedence and transforms

Primary fill color sources use latest-valid-tag-wins behavior. A later
`\1grd`, `\1vc`, or `\1img` replaces a positioned gradient; a later `\pgrd`
replaces each of those. A later ordinary `\c`/`\1c` disables it, while an
earlier ordinary color remains the outside fallback. `\r` and `\rStyleName`
also clear it and restore their normal style primary color.

Positioned-to-positioned `\t(...)` transforms interpolate all four rectangle
coordinates, the shortest-path Mangetsu angle, stop RGB values, and stop
positions. Different stop lists use the existing union-and-resample transform
rule. A solid primary source can animate into `\pgrd` using that color as the
synthesized source stop list. Direct attached/positioned transforms
(`\1grd` to `\pgrd`, or `\pgrd` to `\1grd`) are intentionally ignored safely because their coordinate
spaces are incompatible.

Use `ass_render_frame_rgba()` (or an automatic RGBA wrapper) whenever this tag
is present. `ass_frame_needs_rgba()` is set for accepted positioned gradients.

## Attached versus fixed-frame gradients

`\1grd` is attached to the rendered subtitle segment and is calculated from
that segment's final bounds. `\pgrd` / `\1pgrd` exist only in their fixed
rectangle in script coordinates. Subtitle pixels sample that field by final
frame position, and pixels outside it use the ordinary primary color.

See `samples/position-gradient.ass` for horizontal, vertical, diagonal,
arbitrary-angle, movement, rotation, fallback, and reset examples.
