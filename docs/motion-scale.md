# Mangetsu animated position and object scale

Mangetsu extends the normal ASS transform system with animated `\pos` and a
top-level, percentage-based `\scale` tag. These extensions are intended for
motion tracking and KFX while leaving ordinary ASS positioning and glyph-scale
semantics unchanged.

## Animated `\pos`

`\pos(x,y)` is accepted inside all normal `\t` forms:

```ass
{\pos(500,500)\t(\pos(200,500))}
{\pos(500,500)\t(0,500,\pos(200,500))}
{\pos(500,500)\t(0,500,2,\pos(200,500))}
```

The first example uses the event duration. The second moves linearly over the
specified interval. The third applies the transform's acceleration exponent.
The resolved subtitle position at the beginning of the motion is its anchor;
an explicit base `\pos` is recommended for generated motion.

`\move` remains fully supported for ASS compatibility. Mangetsu generators can
use transformed `\pos` for new motion without changing the meaning of existing
`\move` scripts.

### Overlapping transforms compete

Unlike ordinary transformed properties, overlapping transformed positions do
not use last-transform-wins behavior. Each active `\t(...,\pos())` is a motion
intent aimed at its own target, and their eased displacement contributions are
added.

```ass
{\pos(500,500)
 \t(0,500,\pos(200,500))
 \t(250,800,\pos(700,500))}
```

From 0 through 250 ms only the first transform moves the object. At 250 ms the
actual X position is 350. The active intents are then rebased at 350:

```text
first velocity  = (200 - 350) / 250 = -0.6 px/ms
second velocity = (700 - 350) / 550 =  0.6363636 px/ms
combined        =                       0.0363636 px/ms
```

The object therefore almost stalls while the intents fight. At 500 ms the
first intent expires at the actual combined position, approximately 359.0909,
instead of forcing the object through X=200. The remaining transform is rebased
there and reaches exactly X=700 at 800 ms:

```text
0 ms:   x = 500
250 ms: x = 350
500 ms: x = 359.0909
800 ms: x = 700
```

Every transform start or end is an active-set boundary. Mangetsu resolves the
actual position at that exact time, uses it as the next segment's anchor, and
recomputes every still-active transform from that anchor to its own target over
its remaining duration. Accelerated transforms use their acceleration easing
on each rebased segment. Evaluation is analytic from the event timestamp and
tag data; it does not accumulate frame-by-frame state, so 24 fps, 60 fps, and
out-of-order frame requests follow the same trajectory without boundary jumps.

Three or more overlapping position transforms follow the same rule:

```ass
{\pos(640,360)
 \t(0,500,\pos(400,360))
 \t(150,650,\pos(700,200))
 \t(350,900,\pos(900,500))}
```

## Global object `\scale`

`\scale` is a uniform percentage scale above the existing glyph-scale system:

```ass
\scale50    ; half size
\scale100   ; normal size
\scale200   ; double size
\scale125.5 ; floating-point values are accepted
```

The default is 100%. `\scale100` is render-equivalent to omitting the tag.
Negative values are clamped to zero, and malformed values are ignored.

`\fsc`, `\fscx`, and `\fscy` retain their existing ASS behavior underneath the
global scale. The factors multiply:

```ass
{\scale120\fscx80\fscy110}
```

This produces effective glyph factors X=1.20 x 0.80=0.96 and
Y=1.20 x 1.10=1.32. The important difference is that `\scale` also scales local
object geometry, while `\fscx` and `\fscy` do not redefine legacy outline and
shadow thickness semantics:

```ass
{\bord4\scale200}          ; 200% glyphs, effective 8px border
{\bord4\fscx200\fscy200}  ; 200% glyphs, effective 4px border
```

### Geometry affected by `\scale`

The resolved scale is applied from canonical tag values at render time to:

- text glyph and vector-drawing geometry;
- `\bord`, `\xbord`, and `\ybord`;
- native Mangetsu multi-border sizes such as `\2bs`, `\3bs`, and axis forms;
- BorderStyle=4 box padding and all numbered box-border layers;
- shadow offsets and Gaussian blur radii;
- character spacing, `\fsvp`/`\fshp` local spacing, and drawing baseline offset;
- furigana local placement gaps/offsets;
- Mangetsu jitter and randomized local deformation extents.

Values stored by the parser are not rewritten, so repeated frames never apply
the factor cumulatively.

### Coordinates not affected

`\scale` changes object size around the normal ASS alignment/origin. It does not
scale script/video-space coordinates or non-length values, including:

- `\pos`, `\move`, `\mover`, `\moves3`, and `\moves4` coordinates;
- `\org` coordinates;
- rectangular or vector `\clip` / `\iclip` coordinates;
- script/video resolution and margins;
- transform timing values;
- rotation angles.

Thus `{\an5\pos(500,300)\scale200}` remains anchored at exactly `(500,300)`.

### Animation

`\scale` uses normal property interpolation inside `\t`:

```ass
{\scale100\t(0,1000,\scale200)}
```

For a linear transform this resolves to 100% at 0 ms, 150% at 500 ms, and
200% at 1000 ms. Overlapping scale transforms retain the transform engine's
normal property semantics; only transformed `\pos` has competing-motion
composition.

## Motion-tracking and KFX example

Translation, uniform zoom, glyph deformation, and rotation can be expressed as
separate concerns:

```ass
{\an5\pos(500,500)\scale100\fscx105\fscy95\frz0
 \t(0,500,\pos(200,500)\scale150\frz12)
 \t(250,800,\pos(700,500)\scale70\frz-8)}Tracked object
```

This lets generators layer simple eased position intents while using `\scale`
for uniform object zoom, `\fscx`/`\fscy` only for glyph deformation, and
`\frz`/`\frx`/`\fry` for rotation.
