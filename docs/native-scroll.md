# Native animated multiline scrolling

Mangetsu scrolls selected content upward or downward inside one ordinary ASS
event. Text is shaped, wrapped, positioned, and measured normally; scrolling translates its
complete visual result before clipping and composition. No text rewriting,
synthetic events, generated `\move`, or host preprocessing is needed.

| Tag | Meaning | Initial value |
| --- | --- | --- |
| `\scroll(direction,time[\|duration],signed_lines,...)` | Start a context with incremental timed cues | Fixed content |
| `\scrollt<N>` | Duration in milliseconds for subsequently defined cues | 300 |
| `\scrollsl<N>` | Logical viewport showing N measured rows | No extra viewport |
| `\scroll0` | Make subsequent content fixed | — |

Times are nonnegative integer milliseconds from event start. Counts are signed
integers, durations are nonnegative integers, and values must fit signed 32-bit
integers. Surrounding numeric whitespace is accepted. These controls are
top-level overrides; `\t(...)` does not animate them.

## Direction and signed movement

The first argument selects the base direction: `ue` means upward, `shita` means
downward, and `sita` is an exact alias of `shita`. Names are lowercase. Positive
counts move in that direction; negative counts reverse it. `3` and `+3` are
equivalent. Zero is a valid cue with no movement; it does not disable scrolling.

```ass
\scroll(ue,3000,+3,5000,-1,7000,+2)
\scroll(shita,3000,+3,5000,-1)
\scroll(sita,3000|500,+4)
```

The first definition moves up three rows, down one row, then up two rows, for
net advancement of **four rows upward**. The second moves down three rows then
up one row, for **two rows downward**. The third moves down four rows over 500 ms.
These totals assume enough rows are available; bounds are described below.

For compatibility, the original `\scroll(time,lines,...)` syntax remains
supported with implicit `ue`. New scripts should specify the direction.

## Cues and durations

The cue list can be as long as needed. Counts are incremental, never coordinates
or absolute row positions:

```ass
{\scrollt300\scroll(ue,3000,6,5000,6)}...
```

- At 3000 ms, begin scrolling upward by six rows over 300 ms.
- At 5000 ms, begin scrolling by another six rows over 300 ms.
- Final advancement: **12 rows**.

At 2999 and 3000 ms displacement is zero. At 3150 ms the first contribution is
half complete; at 3300 ms it is complete. At 5150 ms displacement is the first
six rows plus half of the distance through the next six rows. Intermediate
positions retain the renderer's normal subpixel precision, without whole-row
rounding or easing.

`\scrollt500` sets 500 ms for subsequent ordinary cues. A later `\scrollt` does
not change an existing definition. `\scrollt0` means instantaneous motion.
`\r` restores duration 300, disables subsequent participation, and clears the
show-line default. Text emitted before the reset retains its own context.

Every time field independently accepts `time|animation_time`:

```ass
\scroll(ue,3000|700,6)
```

Begin at 3000 ms, move upward six rows, animate for 700 ms, and ignore `\scrollt`
for this cue only. Mixed cues retain independent durations:

```ass
{\scrollt300\scroll(ue,3000,6,5000|800,3,8000,2)}...
```

The durations are 300, 800, and 300 ms; final advancement is eleven rows.
For start S, duration D, measured distance P, and event-relative time T, each
contribution is zero before S, P immediately at S if D is zero, or otherwise
`P * clamp((T - S) / D, 0, 1)`. All contributions add independently, including
overlapping animations in opposite directions. A new cue never cancels, snaps,
or serializes an unfinished cue. For example:

```ass
\scroll(ue,3000|1000,+4,3500|500,-1)
```

At 3750 ms the position is 75% of the distance through the first four rows,
minus 50% of the distance back through the fourth row. Downward definitions use
the same timing with opposite displacement signs.

Prefer chronological cue ordering. Authored order determines which successive
rows each cue traverses, even if its timestamps are out of order. Evaluation
does not sort or mutate the list.

## Actual row geometry

Rows include explicit `\N` breaks and automatic wrapping. Override blocks add
no rows. Furigana such as `<世界|せかい>` stays with its base row and adds no
separate scroll row, while its reserved layout space contributes to metrics.

Distances use final ascent, descent, and inter-row spacing. Fonts, fallback,
`\fs`, `\fscy`, and mixed heights therefore affect the measured distance.
`\scroll(ue,3000,2,5000,3)` traverses the first two rows, then the next three;
it does not repeatedly measure from the first row. A logical row cursor starts
at zero and stays between zero and the number of participating rows. Every cue
updates that cursor in authored order, clamping requests beyond either end.
Direction sets the physical translation sign, not which rows are measured.
Negative movement at the initial cursor contributes zero; positive movement
past the last row clamps to the last row. A later reversal can return from that
boundary. No rows outside the context are accessed.

Reversals retrace the actual previously traversed row advances. For mixed-height
rows, `+3,-1,+2` measures rows 1–3, reverses row 3, then measures rows 3–4.
Each cue's signed distance is the difference between prefix sums at its clamped
starting and ending cursors, multiplied by the base direction. The cursor is
resolved before timing evaluation; overlapping animations add these independent
signed distances, without rounding the animated position to a row.
The last row advances through its measured extent, with spacing if another
layout row follows. Nonpositive advances caused by negative spacing clamp to
zero, so every logical row advance remains nonnegative.

## Logical viewport and fixed footer

`\scrollsl5` spans the first five measured rows of a context at their normal
event position. The height includes actual row metrics and spacing between
those rows. It remains stationary relative to the event while content moves.
Upward content exits through its top edge; downward content exits through its
bottom edge. Reversing a cue changes the motion without moving the viewport.
If fewer rows exist it spans the available rows. A later valid `\scrollsl`
updates the current context and the default for subsequent contexts.

```ass
{\an7\scrollsl5\scrollt300\scroll(ue,2000,+1,3000,+1,4000|700,-1,5500,+2)}Message 1\NMessage 2\NMessage 3\NMessage 4\NMessage 5\NMessage 6\NMessage 7\NMessage 8\N{\scroll0}Typing...
```

Messages 1–8 scroll; `Typing...` stays at its normal position with fixed fill,
outline, shadow, and background. The five-row viewport constrains only the
scrolling section. `\scroll0` is a state transition, not a zero-valued cue.
The first two cues move upward over 300 ms each; the third temporarily moves
downward over 700 ms; the fourth resumes upward movement over 300 ms.
Another valid `\scroll(...)` begins an independent context for subsequent text.
Participation is stored on runs; scrolling and fixed runs may share a row.
The new context may select a different direction:

```ass
{\scroll(ue,1000,1)}Section A\N{\scroll0}Fixed\N{\scroll(shita,2000,1)}Section B
```

Normal `\an`, `\pos`, `\move`, `\org`, scaling, and rotations retain their
positioning/geometry meaning. Scrolling applies vertical frame-space translation
after their normal geometry is resolved. The logical viewport has horizontal
frame-space edges based on row metrics, even for rotated text. Borders and blur
extending beyond those metrics are clipped at its edges. Scrolling events use
normal author placement without automatic collision shifting.

## Explicit clips

Normal ASS clipping is a pixel-controlled alternative; `\scrollsl` is optional:

```ass
{\clip(200,200,800,600)\scroll(ue,2000,1,3000,1,4000,1)}...
```

If both forms are present, the logical viewport intersects the author's clip.
Rectangular and vector `\clip` and `\iclip` keep their meaning. The author's
clip still applies to fixed text; only the additional scroll viewport is
restricted to participating content. Vector clips provide arbitrary shapes.
The same composition applies to `shita`, `sita`, and signed reversals.

## Rendering and caching

Fill, secondary fill, borders, multi-borders, shadows, opaque glyph boxes,
decorations, images, patterns, gradients, alpha, and ruby move together.
Attached gradient segments separate at participation boundaries. Positioned
Mangetsu gradients on participating runs translate with those runs.
BorderStyle=4 backgrounds split at contiguous participation boundaries so the
footer background stays fixed. Existing box paint and padding remain event-level
settings. Scripts without scrolling retain their original single event box.

Validated definitions retain resolved cue durations in a renderer-owned cache.
Signed line counts map to distances once per measured geometry. Prefix sums keep
mapping O(rows + cues), including arbitrarily many reversals. Exact comparison
of row advances invalidates this mapping for resize, font changes, or animated metrics.
The existing layout is inspected each frame; scrolling adds no wrapping pass
and copies no glyph array. Evaluation sums cues once per context and translates
each participating glyph once. Active contexts pin definitions against eviction.

Malformed lists, unknown directions, unmatched cue arguments, invalid numbers,
and negative durations are ignored atomically, preserving preceding valid state.
The argument count after the optional direction must be even. Signed and zero
counts are valid; `\scrollsl` still requires a positive integer. Missing scalar
values and negative `\scrollt` values are ignored.
Allocation failure cannot publish a partially initialized context. Event expiry
ends rendering even if an animation remains unfinished.

This primitive targets ordinary multiline rows. Native chat scenes keep their
existing message-scrolling system. Native vertical layout currently exposes one
aggregate row, so its full block is one scrollable row rather than individual
vertical columns. No horizontal/diagonal scrolling, easing,
motion paths, new transform syntax, GUI, or automatic message detection is added.

CI runs `native-scroll-cues` and `native-scroll-render` with the existing
regressions. They cover both directions and the alias, signed mixed-height
reversals, boundaries, partial progress, overlapping opposite cues, out-of-order
cues, 1,024-cue lists, zero movement/duration, bounds, wrapping, mixed metrics,
resets, clipping, fixed content, effects in both directions, and event expiry.
Builds and execution belong to GitHub Actions.
