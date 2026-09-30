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

### `\ta1`–`\ta9`

`\ta` aligns final visual lines within the ordinary multiline text block.
Values 1/4/7 mean left, 2/5/8 mean center, and 3/6/9 mean right; the vertical
digit component is ignored. `\an` still controls the event's `\pos`/margin
anchor, origin, and vertical placement. For example:

```ass
{\an7\ta2\pos(400,200)}LONG FIRST LINE\Nshort
```

The block stays top-left anchored at `(400,200)`, while `short` is centered
under the longer line. `\ta` applies after hard/soft wrapping, including
automatic visual lines. Without it, the existing `\an`/`\tan` line-alignment
behavior is unchanged. The first valid integer 1–9 wins until `\r` or
`\rStyleName`; a reset restores that legacy fallback. Invalid values are
ignored, and `\ta` inside `\t` has no effect.

### `\ctan1`–`\ctan9`

`\ctan` anchors the completed curved text object relative to the path using
the full numpad. Without an explicit `\ctan`, it inherits `\an`:

```text
effective curved alignment = explicit \ctan, otherwise \an
```

For example, `\an2\ct(...)` attaches exactly like `\an2\ctan2\ct(...)`.

The first visual line has no special anchoring authority. Its baseline may be
zero while line spacing is calculated, but final placement uses the geometry
of every curved visual line:

```text
7 8 9   top of text on path
4 5 6   vertical middle on path
1 2 3   bottom of text on path
│ │ │
│ │ └─ path end
│ └─── path center
└───── path start
```

For a horizontal path, `\ctan8` centers the text along it and places the
completed object's top at the path; `\ctan5` places its middle there; `\ctan2`
places its bottom there. The same rule applies to one line or many. After all
visual lines have been shaped and curved, Mangetsu measures the resulting glyph
geometry in the tangent/normal frame at the selected path point. The selected
left/center/right and top/middle/bottom point of those bounds is attached to
the path start/center/end, respectively. On a diagonal path, top and bottom
therefore follow its normal rather than screen Y. For a Bézier path, the basis
is the tangent at the selected attachment point.

The widest shaped visual line defines the shared block width before curving.
The horizontal digit attaches that block to the path start, center, or end;
`\ta` (or `\an` when `\ta` is absent) aligns shorter lines inside it.
For example, `\an7\ta2\ctan5` keeps the event top-left anchored, centers
short lines in the text block, and centers that block on the curved path.

Explicit `\ctan` changes curved attachment only. `\an` still determines the
event's normal `\pos`/margin anchor. The first valid `\ctan` integer 1–9 wins
until a style reset; a reset restores inheritance from `\an`. Invalid values
are ignored and `\ctan` inside `\t` has no effect.

For a manual multiline comparison in a 1920×1080 script, put these on
separate dialogue events. The longer second line supplies the block width:

```ass
{\fs100\an5\pos(984,634)\ctan2\ct(m -500 0 b -300 -180 300 -180 500 0)}testing\Nsuper testing
{\fs100\an5\pos(984,634)\ctan5\ct(m -500 0 b -300 -180 300 -180 500 0)}testing\Nsuper testing
{\fs100\an5\pos(984,634)\ctan8\ct(m -500 0 b -300 -180 300 -180 500 0)}testing\Nsuper testing
```

The full block is above, centered on, and below the path respectively.
Substitute `1/3`, `4/6`, and `7/9` to compare the left and right attachments.

### `\ctx<number>`

Moves each shaped line and its attachment point forward or backward along its
path. The value is a script-unit path-distance offset and participates in
Mangetsu's ordinary numeric `\t` interpolation:

```ass
{\ct(m -300 0 l 300 0)\t(0,1000,\ctx100)}TEXT
```

### `\cty<number>`

Offsets the curved object along the local path normal, including its
attachment point. The normal is consistently derived from `N = (-dy, dx)` in
ASS's Y-down screen convention. Therefore, on a left-to-right horizontal path,
positive `\cty` moves text down and negative `\cty` moves it up. `\cty` is
also interpolatable in `\t`.

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
along the same path. The widest line defines the path-aligned block and `\ta`
aligns shorter lines within it. Line spacing is computed from successive
baselines; each later line is offset along the local path normal by its
calculated baseline advance, including font metrics, scaling, and line spacing.
On a horizontal left-to-right path, later lines appear below the first; on a
diagonal or curved path, the offset follows the changing normal rather than
screen Y.
The final attachment is calculated only after every visual line is curved.

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

Progressive `\kf` and `\K` karaoke use cumulative shaped advances along the
path. Each glyph's inline wipe is transformed with its cluster and the usual
event projection, so it follows the local tangent even on vertical or returning
paths. `\ctan`, `\ta`, complete-block alignment, `\ctx` and `\cty` move text
and paint together without changing syllable timing. The same wipe applies to
secondary outline paint and RGBA output. Only glyphs intersecting the frontier
need temporary masks; raster and composite caches do not depend on progress.

## Invalid input and current limits

- An invalid or unusable path falls back to ordinary flat rendering. No path
  geometry is reused from another event or frame.
- Path morphing such as `\t(...,\ct(...))` is intentionally unsupported in
  this first version. A transformed `\ct` is ignored; animate `\ctx` and
  `\cty` instead.
- Column layout, furigana layout, and scroll effects still fall back to
  normal rendering for the whole event; text is never deleted. Native
  furigana has the same fallback for one-line and multiline events.
- Existing karaoke timing and shaping data are preserved. Karaoke paint never
  splits or independently places members of a HarfBuzz cluster. Furigana and
  other layouts listed above retain their ordinary karaoke fallback.
- BorderStyle 4/5 boxes and decorations retain their existing event-level
  geometry model; this version does not construct a separate curved box or a
  continuously bent underline.
