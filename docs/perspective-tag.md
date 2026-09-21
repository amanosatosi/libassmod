# Mangetsu `\perspective` override tag

`\perspective` is a true projective corner pin for a single flat subtitle
plane. It is separate from curved-text layout (`\ct`) and from Mangetsu's
bilinear deformation (`\distort`).

## Syntax and coordinates

```ass
\perspective(x0,y0,x1,y1,x2,y2,x3,y3)
```

All eight finite numeric arguments are required. Whitespace around a number is
accepted, and Mangetsu's explicit relative form (`~+N` or `~-N`) is accepted.
An empty, missing, extra, non-numeric, or non-finite argument makes the whole
tag invalid without changing the current render state.

The fixed corner order is:

```text
P0 -------- P1
 |            |
 |            |
P3 -------- P2
```

The coordinates are script-space offsets from the event positioning anchor.
They are not absolute screen positions. Therefore changing `\pos`, or the
evaluated position of `\move`, translates the complete projected plane while
leaving all eight values unchanged. Normal PlayRes/LayoutRes and pixel-aspect
scaling is applied by the renderer.

Example:

```ass
{\an5\pos(960,540)\perspective(-320,-120,280,-80,340,150,-260,190)}SIGN
```

## Source-plane definition

For each distinct effective perspective value in an event, Mangetsu builds one
deterministic core source rectangle. It takes the axis-aligned bounds of the
actual fill-outline path and Bezier control points after shaping, line layout,
`\ct`, `\distort`, and ordinary local ASS geometry transforms, but before
projective corner pinning. All text and drawing glyphs with the same effective
perspective value participate, including internal spaces through their effect
on glyph placement. The four rectangle corners are then mapped to P0/P1/P2/P3.

Borders, shadows, edge blur, and Gaussian blur are excluded from this source
rectangle. Changing `\bord`, `\xbord`, `\ybord`, `\shad`, `\xshad`, `\yshad`,
`\be`, or `\blur` therefore does not change what the eight destination
coordinates mean. Border and shadow geometry still use the resulting projective
transform, and blur is applied by the existing post-rasterization filter.

The rule is evaluated from the current event geometry every render. It never
uses raster bounds or state from a previous frame.

## Projective behavior

Mangetsu solves a 3 by 3 homography from the source rectangle to the four
destination corners. For a source point `(x,y)`:

```text
X = h00*x + h01*y + h02
Y = h10*x + h11*y + h12
W = h20*x + h21*y + h22
x' = X/W
y' = Y/W
```

The homography is composed with the renderer's existing outline transform and
is evaluated by the geometry rasterizer. No full-frame image warp or additional
subtitle bitmap pass is used. Straight source segments remain straight,
parallel lines can converge, and proper planar foreshortening is possible.

## Pipeline and interactions

The effective order is:

1. shaping, wrapping, and line layout;
2. Mangetsu bilinear `\distort`, when present;
3. curved-text placement (`\ct`) and ordinary local ASS transforms such as
   `\frz`, `\frx`, `\fry`, `\fax`, `\fay`, `\fscx`, `\fscy`, and `\scale`;
4. the `\perspective` homography;
5. positioning-anchor translation from `\pos`/`\move`;
6. existing screen-space clipping, gradient sampling, and composition.

Thus `\distort(...)\perspective(...)` first performs the artistic bilinear
warp and then projects that result as a plane. Neither tag disables the other.
Curved text is laid out on its path before the resulting geometry is projected.

Attached gradients and image fills remain attached to the rasterized subtitle
geometry under their existing rules. Positioned gradients such as `\pgrd`
retain their existing script/screen-space sampling semantics. `\clip` and
`\iclip` remain final script/screen-space clips; their paths are not made local
to the projected plane.

`\org` and the standard rotations still establish ordinary ASS local geometry
before the corner pin. The explicit perspective quad remains relative to the
normal positioning anchor, not to `\org`.

## Animation

`\perspective` is supported inside `\t`. The renderer interpolates the eight
corner coordinates independently using normal `\t` timing and acceleration,
then solves a fresh homography for the current render time. Matrix entries are
never interpolated. Rendering a time directly and reaching it through earlier
frames therefore produce the same result.

## Invalid states

The projective transform is ignored for the current render when any of the
following applies:

- a coordinate is non-finite;
- the core source rectangle has zero width or height;
- the destination is collinear or otherwise unsolvable;
- the projective denominator is singular or crosses zero inside the source
  rectangle (including bow-tie/self-crossing corner orders).

The object then renders through the ordinary non-projective path for that
frame. Mangetsu never reuses a previous frame's last valid matrix. Strongly
skewed, concave-looking-at-a-distance, or off-screen quads are not rejected if
they remain a finite, solvable plane without a horizon crossing.

## `\distort` vs `\perspective`

`\distort` is a bilinear deformation over a compacted text/drawing unit. It can
produce non-projective curvature of formerly straight interior lines and is
useful for KFX and artistic warping.

`\perspective` is one true homography representing a projected flat plane.
Straight lines stay straight. It is intended for signs, screens, walls, books,
and tracked planar surfaces.

Neither is a compatibility alias or a replacement for the other.

## Known limitations

- Static and transformed values render fully, but visual editors should edit a
  static top-level tag rather than rewriting a nested `\t` target.
- The source plane follows actual renderer outline/control-point geometry. An
  external editor without renderer geometry access may show a close metric
  approximation for a newly created identity quad; existing tag handles remain
  exact because their coordinates are read directly.
- Gaussian blur remains the renderer's screen-space post-filter, matching the
  existing ASS 3D-transform behavior rather than becoming an anisotropic
  projective blur kernel.
