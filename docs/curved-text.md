# Mangetsu curved text (`\ct`)

Mangetsu can lay normally shaped subtitle lines along an ASS vector path:

```ass
{\an5\pos(960,540)\ct(m -400 0 b -250 -180 250 -180 400 0)}CURVED TEXT
```

`\ct` is baseline layout, not bitmap warping. libass still performs parsing,
bidi resolution, font fallback, HarfBuzz shaping, kerning, ligature formation,
and OpenType mark positioning first. Curved layout then moves each complete
shaping cluster to one point on the path and rotates the cluster to the local
tangent. Every glyph and offset inside that cluster uses the same rigid local
frame.

This distinction is essential for complex scripts. For example:

```ass
{\an5\pos(960,540)\ct(m -360 0 b -220 -130 220 -130 360 0)}မြန်မာစာ သင်္ချာ ကြိုဆိုပါတယ်
```

A Myanmar base, medial, vowel, and tone may be separate output glyphs but one
HarfBuzz cluster. Mangetsu does not place those glyphs independently and does
not reconstruct their mark positions. Their HarfBuzz/libass positions remain
authoritative. Ligatures and joining-script clusters receive the same
treatment.

## Tags

### `\ct(path)`

The minimum path grammar is the normal ASS drawing grammar:

```text
m x y
l x y [x y ...]
b c1x c1y c2x c2y x y
```

The shared ASS drawing parser is used, so its spline commands are accepted
when they produce one usable continuous contour. The baseline is interpreted
as open; the drawing parser's implicit closing edge is not part of the text
path. Multiple contours, empty paths, non-finite coordinates, zero-length
paths, malformed paths that produce no usable contour, and paths over the
defensive size limit are rejected.

The first valid, non-transformed `\ct` definition in an event wins. Invalid
definitions do not prevent a later valid definition from being selected.
The path is event-wide and survives `\r` style resets.

### `\ctan1`, `\ctan2`, `\ctan3`

- `\ctan1`: align the shaped line to the start of the path.
- `\ctan2`: center the shaped line on the path.
- `\ctan3`: align the shaped line to the end of the path.

Each final visual line aligns independently. Without `\ctan`, Mangetsu derives
the value from the horizontal component of
`\an`: left (`\an1/4/7`) means start, center (`\an2/5/8`) means center, and
right (`\an3/6/9`) means end. `\ctan` affects only path alignment. Values other
than the strict integer enum 1 through 3 restore the `\an`-derived default.

### `\ctx<number>`

Moves each shaped line forward or backward along its path. The value
is a script-unit path-distance offset and participates in Mangetsu's ordinary
numeric `\t` interpolation:

```ass
{\ct(m -300 0 l 300 0)\t(0,1000,\ctx100)}TEXT
```

### `\cty<number>`

Offsets the baseline along the local path normal. The normal is consistently
derived from `N = (-dy, dx)` in ASS's Y-down screen convention. Therefore, on
a left-to-right horizontal path, positive `\cty` moves text down and negative
`\cty` moves it up. `\cty` is also interpolatable in `\t`.

## Coordinates and placement

Path coordinates are local script coordinates relative to the subtitle's ASS
positioning anchor. Moving `\pos` or `\move` moves the path and its text as one
object:

```ass
{\an5\pos(960,540)\ct(m -300 0 l 300 0)}TEXT
```

Here local path point `(0,0)` is at `(960,540)`. PlayRes and renderer scaling
use the same X/Y conversions as other local subtitle geometry. Mangetsu's
top-level `\scale` scales the path, `\ctx`, and `\cty` with the rest of the
local object; legacy `\fscx` and `\fscy` continue to affect glyph geometry and
shaped advances without redefining script-space path coordinates.

Path spacing uses cumulative shaped cluster advances. Cubic curves are
adaptively flattened into a polyline and sampled through a cumulative
arc-length table; Bézier parameter `t` is never treated as distance. A path
shorter than the shaped line is deterministic: layout extrapolates past its
ends using the first or last tangent instead of collapsing clusters at an
endpoint.

Hard breaks (`\N`), `\n` when WrapStyle treats it as a break, and automatic
wrapping all produce separate visual lines. Every line starts its own distance
along the same path and uses its own shaped width for start, center, or end
alignment. The first line follows the authored baseline. Each later line is
offset along the local path normal by the renderer's calculated baseline
advance, including font metrics, scaling, and line spacing. On a horizontal
left-to-right path, later lines appear below the first; on a diagonal or
curved path, the offset follows the changing normal rather than screen Y.

A horizontal left-to-right path has zero local rotation. For other tangents,
the screen-coordinate tangent angle is converted to libass's existing rotation
sign convention. Normal `\frz`, `\frx`, `\fry`, `\org`, and positioning still
apply to the complete curved result.

## Pipeline and effects

The implementation uses the existing `GlyphInfo` cluster root and its `next`
chain created from HarfBuzz cluster indices:

```text
parse -> bidi -> HarfBuzz shaping -> flat layout and effects
      -> one rigid transform per shaped cluster
      -> existing projection, border, shadow, blur, and raster pipeline
```

The cached ASS drawing outline is shared with the normal drawing parser. Its
screen-scaled arc-length polyline is built once per rendered event, not once
per glyph. Borders, shadows, blur, glyph scale, spacing, and ordinary rotation
use the transformed glyph matrices. Existing `\distort` outline work is
performed in the flat local layout before the resulting shaped clusters are
placed on the path; `\ct` itself never bends or raster-warps glyph interiors.

When `\ct` is absent, no curved-path code changes glyph positions or matrices.

## Invalid input and current limits

- An invalid or unusable path falls back to ordinary flat rendering. No path
  geometry is reused from another event or frame.
- Path morphing such as `\t(...,\ct(...))` is intentionally unsupported in
  this first version. A transformed `\ct` is ignored; animate `\ctx` and
  `\cty` instead.
- Column layout, furigana layout, and scroll effects still fall back to
  normal rendering for the whole event; text is never deleted. Native
  furigana has the same fallback for one-line and multiline events.
- Existing karaoke timing and shaping data are preserved. Karaoke never
  causes this feature to split a HarfBuzz cluster, but advanced sweep/reveal
  paint remains based on the existing renderer's karaoke model and is not a
  new curved-distance karaoke engine.
- BorderStyle 4/5 boxes and decorations retain their existing event-level
  geometry model; this version does not construct a separate curved box or a
  continuously bent underline.
