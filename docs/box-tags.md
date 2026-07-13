# Box Override Tags

This fork supports override tags for BorderStyle=4-style event boxes. Existing
styles with `BorderStyle=4` keep working without any override tags.

A BorderStyle=4 (BS4) box is one event-level background shape. It is built in
the event's local layout coordinate system and then follows the event's normal
geometry. A rotated or projected sign therefore has a rotated or projected box,
not a new axis-aligned screen-space rectangle around its final pixel bounds.

Line-level BorderStyle overrides such as `\bs3`, `\bs4`, and `\bs5` are
documented in `docs/borderstyle-tags.md`.

## Box Mode

```ass
\box1
\box0
```

- `\box1` enables BorderStyle=4-style rectangular box rendering for the
  current override state.
- `\box0` disables it for the current override state.
- `\r` resets box mode to the current style default.
- `\4c` remains the box color.
- `\4a` and the existing shadow alpha behavior remain the box alpha behavior.

## Extra Padding

```ass
\boxp<N>
\boxpx<N>
\boxpy<N>
```

- `\boxp<N>` adds `N` pixels of extra padding to all sides.
- `\boxpx<N>` adds `N` pixels of extra horizontal padding to both left and
  right.
- `\boxpy<N>` adds `N` pixels of extra vertical padding to both top and bottom.
- Padding affects the local box rectangle only when box mode is active.
- The box remains one clean rectangle; padding is not applied per glyph or per
  fragment.
- `\r` resets extra padding to zero.

Negative values are clamped to zero. Invalid or bare padding tags are ignored.

## Local Geometry, Transforms, and Clips

The renderer first finds the event's untransformed local layout bounds. This
preserves the existing one-box behavior for shaped runs, wrapping, explicit
`\N`, and supported drawings. The normal BS4 extent allowance and `\boxp`,
`\boxpx`, or `\boxpy` padding are applied to those local bounds before the
box is made into a closed rectangle.

That rectangle is sent through the same applicable event geometry as the
content. Positioning and motion keep the box attached to the event; rotation
(`Angle`, `\fr`, `\frz`) rotates it around the effective origin (`\org` when
present); `\frx` and `\fry` use the active 3D projection; and scaling
(`ScaleX`/`ScaleY`, `\fscx`, `\fscy`, `\fsc`), shear (`\fax`, `\fay`), and the
current Mangetsu `\distort` state transform the box as well. Animated values
in `\t` are evaluated for the current frame, so the box updates with an
animated transform or movement.

Consequently, padding is local geometry too: it rotates, scales, shears,
projects, and distorts with the box. It is not added by expanding an
axis-aligned rectangle after the event has been transformed.

The transformed box is rasterized and then masked by the normal active clip.
This applies to rectangular and inverse clips (`\clip`/`\iclip`) as well as
vector clips; the clip keeps its usual ASS coordinate semantics and is not
itself rotated merely because the box is rotated. The same clipping behavior
applies to every BS4 box-border layer.

### Canonical Event Geometry

BS4 is deliberately a single event-level object, while ordinary subtitle
content can carry geometry per glyph or drawing chunk. For a deterministic
box transform, the renderer uses the geometry state active on the first
visible glyph or drawing command (with the normal renderer fallback for an
event with no visible outline). The selected state includes the effective
style geometry, overrides before that content, current-time animation values,
event position, origin, projection, and distortion state.

Later geometry changes do not split the box. For example, this still has one
box that uses the first visible content's `\frz20` geometry:

```ass
{\bs4\frz20}AB{\frz-20}CD
```

This is intentional: a BS4 box cannot exactly follow several unrelated
per-glyph transforms while remaining one event-level shape. In particular,
the per-glyph randomized boundary effects `\rnd`, `\rndx`, `\rndy`, and
`\rndz` do not deform or subdivide the box. Similarly, separately parameterized
per-word distortion runs do not create separate boxes; the selected canonical
`\distort` state is applied once to the whole local box rectangle.

## Transformed Box Examples

All examples below retain one box behind the event content:

```ass
{\an5\pos(640,360)\bs4\boxp20\frz35}ROTATED SIGN
{\an5\pos(640,360)\org(400,300)\bs4\boxp20\frz35}CUSTOM ORIGIN
{\an5\pos(640,360)\bs4\boxp20\frx40\fry-25\frz10}PERSPECTIVE
{\bs4\boxpx30\boxpy10\fscx150\fscy75}SCALED PADDING
{\bs4\boxp16\fax0.30}SHEARED
{\an5\pos(640,360)\bs4\boxp20\distort(1,0,1.3,1,-0.15,1)}DISTORTED
```

The following clips mask the already transformed box, rather than changing it
into a screen-aligned rectangle:

```ass
{\an5\pos(640,360)\bs4\boxp20\frz35\clip(500,250,800,500)}RECTANGULAR CLIP
{\an5\pos(640,360)\bs4\boxp20\frz35\clip(m 500 250 l 800 250 800 500 500 500)}VECTOR CLIP
{\an5\pos(640,360)\bs4\boxp20\frz35\iclip(500,250,800,500)}INVERSE CLIP
```

## Box Borders

BorderStyle=4 box mode also supports native box-border layers:

```ass
\Nbbs<N>
\Nbbc&HBBGGRR&
\Nbba&HAA&
```

`N` is a box-border layer number from 1 through 10. These tags affect only the
BorderStyle=4 rectangular box. They do not change normal glyph/vector outline
layers used by `\bord`, `\Nbs`, or `\bs5`.

Layer 1 has unnumbered aliases:

```ass
\bbs<N>        == \1bbs<N>
\bbc&HBBGGRR& == \1bbc&HBBGGRR&
\bba&HAA&     == \1bba&HAA&
```

Box borders are generated as outward rings around the local padded box
rectangle, so they do not shrink the fill area or consume text padding. Each
ring then uses the same event transform, projection, distortion, rasterization,
and clipping as the fill. Every `\Nbbs` size is the thickness of that individual
layer, not an absolute outer extent. Geometry accumulates in layer order: for
example, `\bbs4\2bbs3\3bbs2` produces 4 px, 3 px, and 2 px non-overlapping
outward rings whose cumulative outer extent is 9 px. Later layers remain visible
even when their thickness is smaller than an earlier layer. This ordering and
sizing are preserved when the box is rotated, projected, or distorted.

If a layer has no size or a zero size, it is not drawn. Negative sizes are
clamped to zero. If a layer has no explicit `\Nbbc`, its RGB color falls back
to the current box color (`\4c`). If it has no explicit `\Nbba`, its alpha
falls back to the current box alpha (`\4a`/BackColour alpha).

Examples:

```ass
{\bs4\boxp12\bbs4\bbc&H000000&}Text
{\bs4\boxp12\1bbs4\1bbc&HFFFFFF&\2bbs3\2bbc&H000000&}Text
{\an5\pos(640,360)\bs4\boxp12\bbs4\bbc&HFFFFFF&\2bbs3\2bbc&H000000&\frz30}Layered sign
```
