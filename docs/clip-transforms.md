# Mangetsu clip position and size transforms

Mangetsu adds two state tags for moving and uniformly resizing an existing ASS
clip without rewriting its coordinates:

- `\clippos(x,y)` — translate the active clip in script coordinates.
- `\clipsN` — uniformly scale the active clip to `N` percent of its original
  size.

They are intended primarily for motion tracking and animated masks. The clip
shape remains authored with ordinary `\clip` / `\iclip`; these tags change
only its translation and size.

## Supported clip forms

Both tags use the same geometry path for all four ASS clip forms:

```ass
\clip(x1,y1,x2,y2)
\iclip(x1,y1,x2,y2)
\clip(m ...)
\iclip(m ...)
```

Existing scaled vector clips also work:

```ass
\clip(2,m ...)
\iclip(4,m ...)
```

The numeric argument in `\clip(2,m ...)` is the existing ASS vector drawing
scale. It is resolved first. `\clips` is a separate Mangetsu transform applied
to the resulting geometry in script space.

## `\clippos(x,y)`

`\clippos` stores an X/Y translation offset for the active clip:

```ass
{\clip(100,80,300,240)\clippos(20,-10)}
```

This is equivalent in geometry to:

```ass
{\clip(120,70,320,230)}
```

The values are offsets, not replacement clip coordinates. Their default is
`(0,0)`.

Both axes are signed, so ordinary signs remain absolute:

```ass
\clippos(-100,50)      ; absolute offset (-100, 50)
\clippos(+20,-10)      ; absolute offset (20, -10)
```

Relative movement therefore uses Mangetsu's explicit signed-relative syntax:

- X `~+N` moves right; X `~-N` moves left.
- Y `~+N` moves up; Y `~-N` moves down.

These are author-facing directional signs, as for relative `\pos`. Absolute
offsets retain ordinary ASS screen coordinates, with positive Y downward.

```ass
\clippos(20,10)\clippos(~+20,~-10)
```

The resulting offset is `(40,20)`: the second tag moves right 20 and down 10.
This differs from relative rectangular `\clip` / `\iclip` corner operands,
which retain raw coordinate addition on both axes. A malformed tuple is rejected atomically:
one invalid component does not partially update the other axis.

## `\clipsN`

`\clips` is a single uniform percentage scale:

```ass
\clips100    ; original size
\clips125    ; 125%
\clips80     ; 80%
\clips0      ; collapse the clip geometry to its center
```

The default is `100`. Negative resolved values are rejected.

Unlike most non-negative Mangetsu scalar properties, `\clips` deliberately
uses signed-value syntax because a leading sign may be an absolute value.
Therefore relative scale changes require `~`:

```ass
\clips120\clips~+10   ; 130
\clips120\clips~-20   ; 100
\clips120\clips+80    ; absolute 80, not 200
```

As elsewhere in Mangetsu, relative percentages are additive percentage points.

Scaling is centered on the clip geometry's bounding-box center. Rectangular
clips use the rectangle center. Vector clips use the bounds of the painted
geometry after the normal vector drawing scale is resolved. Bézier extrema are
included in those bounds; move-only points which paint no geometry do not move
the scaling center.

For example:

```ass
{\clip(100,80,300,240)\clips125}
```

is geometrically equivalent to:

```ass
{\clip(75,60,325,260)}
```

## Combining movement and size

Translation is applied after the centered scale:

```ass
{\clip(100,80,300,240)\clips125\clippos(20,-10)}
```

which is equivalent to:

```ass
{\clip(95,50,345,250)}
```

This keeps motion-tracking data independent from the source clip geometry: the
tracker can emit only X/Y movement and uniform zoom even when the underlying
vector contains many points.

## Animation with `\t`

Both properties use normal numeric `\t` interpolation:

```ass
{\clip(m 100 80 l 300 80 300 240 100 240)
 \clippos(0,0)\clips100
 \t(0,1000,\clippos(120,-40)\clips140)}
```

At 500 ms in a linear transform, the offset is `(60,-20)` and the scale is
`120`.

Relative targets work inside transforms as well:

```ass
{\clip(100,80,300,240)
 \clippos(20,10)
 \t(0,1000,\clippos(~+40,~-20))}
```

The normal Mangetsu transform timing and acceleration rules apply.
The target offset is `(60,30)` (right 40, down 20 from `(20,10)`); at 500 ms
the offset is `(40,20)`. Using `~+20` for the relative Y target instead would
animate upward toward `(60,-10)`.

## Reset behavior

A style reset restores the transform state:

```text
clippos = (0,0)
clips   = 100
```

This applies to both `\r` and named resets such as `\rOtherStyle`.

The active clip geometry itself is not removed by that reset. For example,
after:

```ass
{\clip(100,80,300,240)\clippos(40,-20)\clips150\r}
```

the original `\clip(100,80,300,240)` remains active with no translation and
100% size.

## Tag order and replacement geometry

`\clippos` and `\clips` are clip state, so they may appear before or after
the shape declaration:

```ass
{\clippos(20,-10)\clips125\clip(100,80,300,240)}
{\clip(100,80,300,240)\clippos(20,-10)\clips125}
```

These render equivalently.

To make this reliable, an event containing either clip-transform tag uses one
replaceable active clip shape. A later `\clip` or `\iclip` declaration
replaces the earlier shape while keeping the current `\clippos` / `\clips`
state. Replacement may cross rectangle/vector and normal/inverse forms.

Events which do not use `\clippos` or `\clips` retain ordinary ASS clip
semantics, including the existing first-vector-wins/composition behavior.
Text that merely contains the tag names outside an override tag does not enable
replacement semantics.

## Normal and inverse clips

The geometry transform is identical for `\clip` and `\iclip`. Inversion is
applied by the normal clipping stage after the geometry has been transformed.

At `\clips0`, the geometry collapses to its center; the final visible result
therefore still follows the normal vs inverse meaning of the active clip.

## Rendering and compatibility notes

- `\clippos(0,0)\clips100` is a true no-op.
- Source clip coordinates and cached outlines are not destructively rewritten;
  each frame starts from the original geometry, so seeking and repeated frames
  do not accumulate transforms.
- Rectangular and vector clips share the same script-space scale/translation
  model. Vector rasterization may differ by boundary antialiasing/quantization
  from a manually rewritten rectangle even when their ideal geometry matches.
- `\movevc` remains separate. Existing scripts without the new tags retain
  their previous behavior.

## Regression coverage

The Meson `clip-transform-regression` test covers rectangle/vector and
normal/inverse clips, existing vector drawing scales, positive and negative
translation, uniform enlargement/reduction, zero scale, combined transforms,
`\t` animation, explicit relative operands, resets, tag ordering, replacement
geometry, geometric Bézier bounds, malformed syntax, and ordinary ASS control
cases.
