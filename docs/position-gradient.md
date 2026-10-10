---
title: Fixed-frame color gradients
---

# Fixed-frame color gradients

Mangetsu positioned color gradients are RGBA-only linear fields fixed in the
subtitle frame. Unlike attached `\Ngrd` gradients, their coordinates do not
follow text bounds. `\pgrd` remains an alias for `\1pgrd`.

Native scrolling does not translate the gradient rectangle. When scrolling
text covers the same final-frame pixel at two timestamps, that pixel samples
the same gradient color. Movement, scaling, rotation, and distortion likewise
move only the subtitle mask through this stationary field.

```ass
\pgrd(x1,y1,x2,y2,angle,stops...)
\Npgrd(x1,y1,x2,y2,angle,stops...)
\Nbpgrd(x1,y1,x2,y2,angle,stops...)
```

| Tag | Color target |
| --- | --- |
| `\pgrd`, `\1pgrd` | Primary fill |
| `\2pgrd` | Secondary / waiting karaoke fill |
| `\3pgrd`, `\1bpgrd` | Ordinary outline / first numbered border (shared paint) |
| `\4pgrd` | Shadow; also the existing fourth-channel BS4 box fill |
| `\5pgrd` | Underline and strikeout through the existing fifth-color mechanism |
| `\Nbpgrd`, N=1..10 | Nth native glyph border, following `\Nbgrd` conventions |

Each target keeps its own rectangle, angle and stops. Setting paint does not
enable its geometry: use karaoke tags, `\bord`/`\Nbs`, `\shad`, or `\u`/`\s`
as appropriate. There are no positioned alpha fields, box-ring variants, or
separate positioned waiting-outline (`\3sgrd`) tags.

## Arguments and stops

The first four values are finite decimal ASS script coordinates. `angle` is a
finite decimal and appears before the first stop. Negative and off-frame
coordinates are valid. The stop section is exactly the existing `\1grd`
grammar: ASS BGR colors (`&HBBGGRR&`) or the shared named-color shortcuts
(`$white`/`$siro`/`$shiro` and `$black`/`$kuro`), an optional `position%, color`
pair between the first and final colors, sorted positions, duplicate positions,
and the existing maximum stop count. For example:

```ass
{\1c&HFFFFFF&\pgrd(100,200,700,500,45,&H0000FF&,50%,&H00FF00&,&HFF0000&)}Text
```

Malformed parentheses, missing/empty fields, non-finite values, trailing
numeric garbage, or malformed stops reject the entire tag without changing the
current color source. Empty parentheses reset only a currently active positioned
gradient on that target: `\2pgrd()` leaves primary, outline and shadow paint
intact; `\3pgrd()` and `\1bpgrd()` reset the same first-border source.
`\Nbpgrd()` resets only border N. The ordinary solid color remains intact.
If the current source is attached, vector, image or cycling paint, a positioned
reset leaves it untouched. Inline `0` is not a positioned reset syntax, and
resets inside `\t` remain ignored.

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
endpoint: it uses the target's active ordinary RGB (`\Nc`, `\Nbc`, style color,
or actor-colorcoded color). Fifth-channel fallback follows normal decoration
color inheritance. Alpha remains the target's normal active alpha, including
its existing alpha-gradient source, both inside and outside the rectangle.
Zero-size rectangles or invalid projected ranges are empty and therefore use
the target's ordinary color everywhere.

Consequently, `\pos`, `\move`, rotation, scaling, shearing, distortion,
perspective transforms, font changes, shaping, wrapping, and `\N` alter the
text pixels but never move, resize, rotate, or restart the gradient field.
Clipping only controls visibility and does not modify the field.

## Precedence and transforms

Color sources use latest-valid-tag-wins behavior on the same target. A later
`\Ngrd`/`\Nbgrd`, vector `\Nvc`/`\Nbvc`, image fill, or color cycle replaces
its positioned gradient; a later positioned gradient replaces conflicting
sources on that target. A later ordinary `\Nc`/`\Nbc` disables it, while an
earlier solid color remains the outside fallback. `\r` and `\rStyleName`
clear all gradients and restore normal style paint.

Existing shared paint conventions remain: `\3pgrd` and `\1bpgrd` alias the
first border just like `\3grd` and `\1bgrd`. Primary/secondary image fills share
an image source, so applying either primary or secondary positioned paint
clears those shared image fills, while retaining the other channel's independent
gradient. Shadow and fifth-color paint follow their existing supported source
families; positioned tags do not add new vector/image/cycle families.

Positioned-to-positioned `\t(...)` transforms interpolate all four rectangle
coordinates, the shortest-path Mangetsu angle, stop RGB values, and stop
positions. Different stop lists use the existing union-and-resample transform
rule. A solid source can animate into its positioned target using that color as the
synthesized source stop list. Direct attached/positioned transforms
(for example `\3grd` to `\3pgrd`, or `\2pgrd` to `\2grd`) are ignored safely
because their coordinate spaces are incompatible. This applies independently
to all five channels and ten borders, including outline aliases. Nested and
overlapping transforms retain the existing ASS `\t` ordering and nesting rules.

Use `ass_render_frame_rgba()` (or an automatic RGBA wrapper) whenever this tag
is present. `ass_frame_needs_rgba()` is set for accepted positioned gradients.

## Attached versus fixed-frame gradients

`\Ngrd` / `\Nbgrd` are attached to their target's rendered segment and use
its final bounds. `\Npgrd` / `\Nbpgrd` exist only in their fixed rectangle
in script coordinates. Subtitle pixels sample each field independently by
final frame position; outside pixels use that target's ordinary color.

```ass
{\bord6\shad8\1c&HFFFFFF&\3c&H000000&\4c&H202020&
 \1pgrd(100,100,800,600,0,&H0000FF&,&HFF0000&)
 \3pgrd(100,100,800,600,90,&H00FF00&,&HFFFFFF&)
 \4pgrd(100,100,800,600,45,&H000000&,&HFF00FF&)}Text
{\1bs4\2bs10\1bpgrd(100,100,800,600,0,&H0000FF&,&HFF0000&)
 \2bpgrd(100,100,800,600,90,&H00FF00&,&HFFFFFF&)}Two borders
{\2pgrd(100,100,800,600,45,&H0000FF&,&HFF0000&)\kf200}Karaoke
{\u1\5pgrd(100,100,800,600,0,&H0000FF&,&HFF0000&)}Underline
```

Normal text and ASS drawings use the same field. Opacity, fades, clipping,
karaoke coverage and blending keep their existing behavior. Use RGBA output;
legacy `ASS_Image` carries masks and solid fallback colors only.

See `samples/position-gradient.ass` for horizontal, vertical, diagonal,
arbitrary-angle, movement, rotation, fallback, and reset examples.
