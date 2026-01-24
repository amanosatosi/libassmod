# Gradient Math Notes

This note describes the math behind vector gradients in libass at a practical
level. It is meant as a quick reference when debugging or reviewing changes.
The implementation lives in `libass/gradient.c` and is applied during RGBA
rendering in `libass/ass_render.c`.

## Inputs and corner order

Vector gradients come from the `\1vc`..`\4vc` and `\1va`..`\4va` tags. The four
corner values are ordered as:

- 0: top-left
- 1: top-right
- 2: bottom-left
- 3: bottom-right

The same ordering is used for color and alpha.

## Coordinate mapping

Each rendered bitmap has a logical gradient rectangle in pixel space. For a
pixel at `(x, y)` inside that rectangle, libass maps it to normalized
coordinates `(u, v)`:

```
u = (x - rect_x) / max(1, rect_w - 1)
v = (y - rect_y) / max(1, rect_h - 1)
```

`u` and `v` are clamped to `[0, 1]`. This makes the gradient cover the entire
rectangle even for small sizes.

## Bilinear interpolation

Given corner values `c0..c3` and normalized coordinates `(u, v)`:

```
value = c0 * (1 - u) * (1 - v)
      + c1 * u * (1 - v)
      + c2 * (1 - u) * v
      + c3 * u * v
```

This is the same for RGB and alpha channels.

## Fixed-point path

The runtime uses 16.16 fixed-point for `u` and `v`:

- `u_fp` and `v_fp` are in `[0, 65536]`.
- Intermediate products accumulate in 64-bit integers.
- The final result is shifted right by 32 to bring it back to 0..255 (or
  0..65535 depending on the channel).

This keeps the interpolation stable and avoids floating-point jitter.

## Alpha and coverage

The gradient alpha is combined with the glyph coverage (mask) per pixel.
RGBA output is premultiplied:

- `A_final = A_gradient * coverage`
- `RGB_final = RGB_gradient * A_final`

This ensures correct blending with the destination.

## Line-level anchoring

Gradient rectangles are computed per line and per layer (fill, outline,
shadow). If a layer does not provide its own rectangle, it falls back to the
line's character rectangle so all layers stay visually aligned.

## Notes

- If `rect_w` or `rect_h` is 1, the corresponding normalized coordinate is 0.
- Values are clamped; out-of-range inputs do not extrapolate.
- No gamma correction is applied; interpolation is in linear channel space.
