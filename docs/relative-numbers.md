# Mangetsu relative numeric values

Eligible override parameters accept a delta from their current state,
both in ordinary override blocks and inside existing `\t()` transforms.
Arithmetic uses the units written in ASS scripts.

## Syntax

The rule is per parameter. A parameter whose absolute domain is non-negative
accepts a bare sign as relative syntax:

| Spelling | Meaning |
| --- | --- |
| `N` | absolute value |
| `+N` | current value plus N |
| `-N` | current value minus N |
| `~+N` | explicit relative increase |
| `~-N` | explicit relative decrease |

For signed numeric parameters, bare signs remain absolute. Relative values require `~`:

| Spelling | Meaning |
| --- | --- |
| `N`, `+N` | absolute positive value |
| `-N` | absolute negative value |
| `~+N` | current value plus N |
| `~-N` | current value minus N |

`~` is accepted everywhere relative syntax is supported, so editors can always
generate explicit relative operands. A sign and a complete finite number must
follow it: use `~+10` or `~-10`, not `~10` or `+-10`. Fractional numbers and
supported Unicode decimal digits work as usual.

Numeric relative values use `~+N` to increase and `~-N` to decrease the numeric
value. Directional position coordinates use the same spelling but describe
movement: X `~+N` means right, X `~-N` means left, Y `~+N` means up, and Y `~-N`
means down. Bare signs on these coordinates remain literal absolute signs.

## Current state, Styles, and resets

The active Style initializes Style-backed properties. Other properties use
their normal renderer defaults. Each subsequent tag sees the state left by
earlier tags; it does not repeatedly reload the Style.

With Style Fontsize = 40:

```ass
{\fs-10}Text             ; 30
{\fs+10}Text             ; 50
{\fs~-10}Text            ; 30
{\fs50\fs-10}Text        ; 40
{\fs50\fs~+20}Text       ; 70
{\fs50\r\fs+10}Text      ; 50
```

`\rOtherStyle` uses the selected Style's reset values. State continues between
ordinary override blocks according to normal ASS tag order. Every event and
frame starts from freshly initialized state; deltas never accumulate from a
previous rendered frame.

Results go through each tag's existing conversion, interpolation, and safety
rules. For example, a nonpositive font-size result falls back to the effective
Style font size; negative border results clamp to zero. `\rnd` stores signed
VSFilterMod amplitudes and disables deformation on nonpositive axes; jitter
retains its existing magnitude/range rules. No relative-only clamp is added.

## Percentages are additive percentage points

**A relative `+50` means 50 percentage points, not a 50% increase.**

```ass
{\fscx50\t(0,500,\fscx+50)}Text  ; 50 -> 100, not 75
{\fscy150\fscy-50}Text           ; 100
{\scale75\scale+25}Text          ; 100
```

The internal representation of 50% as 0.5 does not change these units.

### Axis scale, soft scale, and object scale

| Tag | Role | Default |
| --- | --- | --- |
| `\fscx`, `\fscy` | direct per-axis glyph scales | Style ScaleX / ScaleY |
| `\fsc` | separate uniform soft multiplier over glyph scales | 100% |
| `\scale` | broader Mangetsu local-object geometry scale | 100% |

Given axis scales 80/120:

```ass
\fsc50    ; effective glyph scales 40/60
\fsc150   ; effective glyph scales 120/180
\fsc+50   ; from default factor 100 to 150: effective 120/180
\fsc-25   ; from default factor 100 to 75: effective 60/90, not 55/95
```

`\fsc` retains its own factor. It does not overwrite the logical axis values:

```ass
\fscx60\fscy140\fsc150   ; effective 90/210
\fsc150\fscx60\fscy140   ; also effective 90/210
\fsc150\fsc200           ; factor 200, not 300
\fsc150\fsc+50           ; factor 200
\fsc150\fsc-50           ; factor 100
```

An argumentless `\fsc` restores its factor to 100%; `\r` also resets it.
Soft scaling affects glyphs/drawings and the spacing derived from glyph scale.
It does not multiply border thickness, shadow offsets, blur, or fixed BS4
padding and box-border widths. A BS4 box still follows the scaled text bounds.

`\scale` remains independent and retains its existing local-geometry coverage:
borders, shadows, blur, drawing offsets, local spacing, jitter, and box geometry.
See [motion and object scale](motion-scale.md).

```ass
{\fsc50\t(0,500,\fsc+50)}Text      ; soft factor 50 -> 100
{\scale50\t(0,500,\scale+50)}Text  ; object factor 50 -> 100
```

With axis scales 80/120, the first animation produces effective glyph scales
40/60 -> 80/120. The factor itself is interpolated.

## Signed numeric values

```ass
\frz-50                    ; absolute -50 degrees
\frz~+50                   ; add 50 degrees
\frz~-50                   ; subtract 50 degrees
\fax-0.2                   ; absolute -0.2 shear
\fax~+0.2                  ; add 0.2 shear
\fsp-2                     ; absolute -2 spacing
\fsp~-2                    ; subtract 2 from current spacing
```

## Directional position coordinates

Relative position syntax is author-facing directional shorthand, not raw
screen-coordinate arithmetic. On the X axis, positive moves right and negative
moves left. On the Y axis, positive moves up and negative moves down. Absolute
coordinates retain normal ASS screen-coordinate semantics.

```ass
\pos(-100,500)                       ; literal absolute coordinates
\pos(500,500)\pos(~+100,~+100)        ; right 100 / up 100 -> (600,400)
\pos(500,500)\pos(~-100,~-100)        ; left 100 / down 100 -> (400,600)
\pos(500,500)\pos(~+200,~-100)        ; right 200 / down 100 -> (700,600)
\pos(500,500)\pos(~+20,400)          ; (520,400): absolute Y stays literal
\pos(500,500)\pos(520,~-10)          ; (520,510): absolute X stays literal
```

Only relative Y positional operands are converted to upward-positive movement;
absolute Y values continue increasing downward. The same directional rule
applies to every X/Y point of `\move`, `\moves3`, `\moves4`, and `\mover`,
both translation endpoints of `\movevc`, and the offsets of `\clippos`.
For example, the coordinate resolver maps offsets `(~+20,~-10,~+40,~+10)`
from stored endpoints `(400,300,600,400)` to `(420,310,640,390)`.
This does not change when a motion tag is accepted: an ordinary later `\move`
still loses to the existing first position/motion. `\mover` angles and radii
remain signed numeric values; `~+N` increases them and `~-N` decreases them.
Timing fields are unchanged and do not accept relative operands.

Without an earlier explicit position or motion, relative `\pos` uses automatic
placement: the active Style/event margins, alignment (including `\an`), and
layout metrics determine the base. It is resolved after layout, not assumed to
be `(0,0)`. Mixed operands are supported: `\pos(~+20,400)` offsets X and sets
absolute Y=400. Repeated relative positions modify the current offsets.

An ordinary relative `\pos` translates an earlier position or motion, including
already parsed animated position targets. It applies the offset once to the
current animation; mixed absolute axes replace that axis of the preceding
motion. Ordinary absolute `\pos` retains its existing first-position-wins rule,
including after an automatic relative position.

Relative `\pos` retains the existing hard-override policy of `\pos` for player
margin and selective Style preferences. Its automatic base uses script Style
and event margins under that policy. Like explicit positioning, it disables
collision displacement. `\r` retains its normal line-level position semantics.

```ass
{\pos(500,500)\t(0,500,\pos(~+200,400))}Text
{\pos(500,500)\t(0,500,\pos(~+200,~-100))}Text
{\pos(500,500)\t(0,500,\pos(~+200,~+100))}Text
```

The first and third examples animate toward `(700,400)`. The second animates
toward `(700,600)` because `~-100` means down 100. The sign conversion does not
change transform timing, acceleration, or the point where a relative target's
reference position is resolved.

### Raw coordinates and other Y-bearing parameters

Rectangular `\clip` / `\iclip` corners are literal screen-space coordinates:
relative values use numeric addition on both axes. For example,
`\clip(100,80,300,240)\clip(~+20,~-10,~+20,~-10)` yields corners
`(120,70,320,230)`. To move the clip with directional signs instead, use
`\clippos(~+20,~-10)`, which moves it right 20 and down 10.

`\distort` contains normalized distortion pins, not screen-position movement;
both U and V retain numeric addition. Image-fill `\img` offsets are texture
sampling offsets and also retain numeric addition. Signed scalars such as
`\yshad`, `\fay`, `\fry`, and `\fsvp` keep numeric increase/decrease.
The curved-text tangent/normal offsets `\ctx` / `\cty` and vertical spacing
`\vcolsp` / `\vsp` likewise retain their existing numeric semantics.

`\furipos` already stores an upward-positive Y offset, which the renderer
subtracts when placing furigana. Its resolver therefore retains numeric
addition: relative `~+Y` increases that offset and visually moves ruby upward;
`~-Y` moves it downward. Applying the screen-position Y conversion there
would reverse the intended visual direction.

## Transforms and seeking

All four existing forms work: `\t(tags)`, `\t(accel,tags)`,
`\t(t1,t2,tags)`, and `\t(t1,t2,accel,tags)`.

```ass
{\fs40\t(0,500,\fs+20)}Text        ; 40 -> 60
{\fs40\t(0,1000,2,\fs~+20)}Text   ; same acceleration as absolute \fs60
{\frz30\t(0,500,\frz-90)}Text     ; absolute target -90
{\frz30\t(0,500,\frz~-90)}Text    ; relative target -60
```

Ordinary transformed properties resolve operands from the current state at
that point in the existing tag evaluation order, then use the normal transform
weight. Overlapping transforms remain ordered; a later relative operand sees
the value produced by earlier transforms at the requested timestamp.

Animated positions retain Mangetsu's existing simultaneous-motion semantics.
Each relative position target is resolved once at its own start boundary from
the position at that boundary, and stays fixed across subsequent rebasing.
All targets are reconstructed from tag data for each evaluation. Direct seeks,
sequential playback, and backwards frame requests give the same result.

## Supported parameters

| Classification | Tags / parameters |
| --- | --- |
| Non-negative (bare `+/-` or explicit `~+/-`) | `\fs`, `\fscx`, `\fscy`, `\fsc`, `\scale` |
| Non-negative | `\bord`, `\xbord`, `\ybord`, numbered `\Nbs`, `\Nbsx`, `\Nbsy`, `\bbs`, `\Nbbs` |
| Non-negative | `\shad`, `\blur`, `\xblur`, `\yblur`, `\be`, `\boxp`, `\boxpx`, `\boxpy`, `\colsp` |
| Non-negative | `\furis`, `\furisx`, `\furisy`, jitter extents and period |
| Signed (relative requires explicit `~+/-`) | `\rnd`, `\rndx`, `\rndy`, `\rndz` |
| Signed (explicit `~+/-` only) | `\fr`, `\frx`, `\fry`, `\frz`, `\frs`, `\fax`, `\fay`, `\z`, `\fsp`, `\fsvp`, `\fshp`, `\xshad`, `\yshad`, `\pbo` |
| Directional position (explicit `~+/-` only) | `\pos`, `\clippos`; X/Y coordinate fields of `\move`, `\movevc`, `\mover`, `\moves3`, `\moves4` |
| Signed numeric | rectangular `\clip` / `\iclip` corners, all `\distort` pins, `\furipos`, `\furifsp`, image-fill X/Y sampling offsets |
| Signed numeric | `\mover` angles/radii, `\ctx`, `\cty`, `\vcolsp`, `\vsp` |

`\clips` is a special clip percentage state: it is non-negative after
resolution, but its parser intentionally uses signed-value syntax so bare
`+N` / `-N` remain absolute. Use `\clips~+N` / `\clips~-N` for relative
percentage-point changes. See [clip position and size transforms](clip-transforms.md).

Combined axis tags such as `\bord+2` add the delta to each current axis
independently. Motion-parameter deltas use their corresponding stored motion
fields. Existing first-use restrictions and transformability remain in force;
this feature does not add interpolation to step-valued tags such as box padding,
column spacing, drawing baseline offset, or `\movevc`.

Distortion keeps its historical P1/P2/P3/P0 argument order and both six- and
eight-slot forms. Coordinates are signed, so `-0.1` stays absolute and `~-0.1`
is relative. Six slots still imply P0 `(0,0)`, including when used as an animated
target. Empty slots and malformed-count behavior are unchanged.

## Exclusions and compatibility

Colors, alpha/hex data, names, strings, enums, toggles, encodings, font weights,
and jitter seeds are excluded. Timing/control arguments (`\t` timestamps and
acceleration, motion/fade times, karaoke clocks), drawing/vector-clip scale
exponents and path commands, and structured gradient definitions are also
excluded: they do not share this scalar current-property model. `\org` keeps
its existing first-use and implicit-origin behavior; this change does not add
relative origins or origin animation.

Unsigned absolute operands, signed absolute operands, ordinary absolute
transform evaluation, color parsing, and distortion order retain their existing
behavior, with these intentional syntax/semantic changes:

- Relative positional signs describe right/left and up/down movement as above.
  Scripts written for the earlier generalized numeric-Y behavior must reverse
  only their relative positional Y signs; absolute Y and other numeric Y values
  are unaffected.

- Bare signed operands on the listed non-negative parameters now mean deltas.
  In particular, `\fs+10` now adds 10 script units instead of using the previous
  proportional font-size rule.
- `\fsc` now stores a soft multiplier rather than assigning both axes. Existing
  scripts relying on the old assignment semantics should use `\fscxN\fscyN`.
  Compositions with non-100% Style scales consequently change as specified.

Absolute tokens retain their previous parser strictness. New relative operands
must be complete finite numbers. An invalid relative scalar supplies its current
value to the tag's existing interpolation/validation; strict tags reject invalid
operands. This does not bypass the tag's normal clamping or other side effects.

The public VSFilterMod parsers inspected in
[sorayuki's RTS.cpp](https://github.com/sorayuki/VSFilterMod/blob/master/src/subtitles/RTS.cpp)
and [computerfan's RTS.cpp](https://github.com/computerfan/VSFilterMod/blob/master/src/subtitles/RTS.cpp)
parse `\pos` and motion coordinates with `wcstod` and do not implement
Mangetsu's `~` relative syntax. These sources confirm literal absolute parsing,
but do not independently establish the relative-sign convention. Mangetsu's
explicit author-facing directional rule is specified here; existing absolute
ASS/VSFilter behavior and first-use restrictions remain in force.

## Regression coverage

The Meson `relative-numbers` test checks numeric state and rendered signatures:
Style/reset bases, signed compatibility, percentage points, soft/object scale
composition, four transform forms, acceleration, overlap targets, automatic
placement for nine alignments, frame/event margins, mixed coordinates, ordinary
offsets after animated motion, event isolation, malformed operands, Unicode
digits, and fresh/sequential/reversed seeking. Existing distortion and
motion/scale tests remain registered unchanged. Directional coverage includes
four directions at all nine alignments, direct motion control-point/timing
assertions, separation of `\mover` positions from angles/radii, `\movevc`
endpoints, and fixed absolute targets as seeking oracles. Numeric controls cover
raw clip corners, six/eight-slot distortion, scalar/percentage state, and image
sampling offsets. The furigana selftest checks relative ruby movement visually;
clip-transform regressions compare relative clip/`\movevc` movement with
independently rewritten rectangular/vector geometry.
Compilation and execution belong to GitHub Actions under this repository's
no-local-compilation policy.
