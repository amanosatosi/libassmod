# Native animated multiline scrolling

Mangetsu scrolls selected content upward inside one ordinary ASS event. Text is
shaped, wrapped, positioned, and measured normally; scrolling translates its
complete visual result before clipping and composition. No text rewriting,
synthetic events, generated `\move`, or host preprocessing is needed.

| Tag | Meaning | Initial value |
| --- | --- | --- |
| `\scroll(time,lines,...)` | Start a context with incremental timed cues | Fixed content |
| `\scrollt<N>` | Duration in milliseconds for subsequently defined cues | 300 |
| `\scrollsl<N>` | Logical viewport showing N measured rows | No extra viewport |
| `\scroll0` | Make subsequent content fixed | — |

Times are nonnegative integer milliseconds from event start. Counts are positive
integers, durations are nonnegative integers, and values must fit signed 32-bit
integers. Surrounding numeric whitespace is accepted. These controls are
top-level overrides; `\t(...)` does not animate them.

## Cues and durations

The cue list can be as long as needed. Counts are incremental, never coordinates
or absolute row positions:

```ass
{\scrollt300\scroll(3000,6,5000,6)}...
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
\scroll(3000|700,6)
```

Begin at 3000 ms, move six rows, animate for 700 ms, and ignore `\scrollt` for
this cue only. Mixed cues retain independent durations:

```ass
{\scrollt300\scroll(3000,6,5000|800,3,8000,2)}...
```

The durations are 300, 800, and 300 ms; final advancement is eleven rows.
For start S, duration D, measured distance P, and event-relative time T, each
contribution is zero before S, P immediately at S if D is zero, or otherwise
`P * clamp((T - S) / D, 0, 1)`. All contributions add independently, including
overlapping animations. A new cue never cancels an unfinished cue.

Prefer chronological cue ordering. Authored order determines which successive
rows each cue consumes, even if its timestamps are out of order. Evaluation
does not sort or mutate the list.

## Actual row geometry

Rows include explicit `\N` breaks and automatic wrapping. Override blocks add
no rows. Furigana such as `<世界|せかい>` stays with its base row and adds no
separate scroll row, while its reserved layout space contributes to metrics.

Distances use final ascent, descent, and inter-row spacing. Fonts, fallback,
`\fs`, `\fscy`, and mixed heights therefore affect the measured distance.
`\scroll(3000,2,5000,3)` consumes the first two rows, then the next three;
it does not repeatedly measure from the first row. Requests beyond remaining
rows clamp safely. Once all rows have been consumed, later cues add no distance.
The last row advances through its measured extent, with spacing if another
layout row follows. Nonpositive advances caused by negative spacing clamp to
zero, preserving upward-only motion.

## Logical viewport and fixed footer

`\scrollsl5` spans the first five measured rows of a context at their normal
event position. The height includes actual row metrics and spacing between
those rows. It remains stationary relative to the event while content moves.
If fewer rows exist it spans the available rows. A later valid `\scrollsl`
updates the current context and the default for subsequent contexts.

```ass
{\an7\scrollsl5\scroll(2000,1,3000,1,4000,1,5000,1)}Message 1\NMessage 2\NMessage 3\NMessage 4\NMessage 5\NMessage 6\NMessage 7\NMessage 8\N{\scroll0}Typing...
```

Messages 1–8 scroll; `Typing...` stays at its normal position with fixed fill,
outline, shadow, and background. The five-row viewport constrains only the
scrolling section. `\scroll0` is a state transition, not a zero-valued cue.
Another valid `\scroll(...)` begins an independent context for subsequent text.
Participation is stored on runs; scrolling and fixed runs may share a row.

Normal `\an`, `\pos`, `\move`, `\org`, scaling, and rotations retain their
positioning/geometry meaning. Scrolling applies vertical frame-space translation
after their normal geometry is resolved. The logical viewport has horizontal
frame-space edges based on row metrics, even for rotated text. Borders and blur
extending beyond those metrics are clipped at its edges. Scrolling events use
normal author placement without automatic collision shifting.

## Explicit clips

Normal ASS clipping is a pixel-controlled alternative; `\scrollsl` is optional:

```ass
{\clip(200,200,800,600)\scroll(2000,1,3000,1,4000,1)}...
```

If both forms are present, the logical viewport intersects the author's clip.
Rectangular and vector `\clip` and `\iclip` keep their meaning. The author's
clip still applies to fixed text; only the additional scroll viewport is
restricted to participating content. Vector clips provide arbitrary shapes.

## Rendering and caching

Fill, secondary fill, borders, multi-borders, shadows, opaque glyph boxes,
decorations, images, patterns, gradients, alpha, and ruby move together.
Attached gradient segments separate at participation boundaries. Positioned
Mangetsu gradients on participating runs translate with those runs.
BorderStyle=4 backgrounds split at contiguous participation boundaries so the
footer background stays fixed. Existing box paint and padding remain event-level
settings. Scripts without scrolling retain their original single event box.

Validated definitions retain resolved cue durations in a renderer-owned cache.
Line counts map to distances once per measured geometry. Exact comparison of row
advances invalidates this mapping for resize, font changes, or animated metrics.
The existing layout is inspected each frame; scrolling adds no wrapping pass
and copies no glyph array. Evaluation sums cues once per context and translates
each participating glyph once. Active contexts pin definitions against eviction.

Malformed lists, odd argument counts, invalid numbers, negative durations, and
nonpositive counts are ignored atomically, preserving preceding valid state.
Allocation failure cannot publish a partially initialized context. Event expiry
ends rendering even if an animation remains unfinished.

This primitive targets ordinary multiline rows. Native chat scenes keep their
existing message-scrolling system. Native vertical layout currently exposes one
aggregate row, so its full block is one scrollable row rather than individual
vertical columns. No horizontal/diagonal scrolling, reverse motion, easing,
motion paths, new transform syntax, GUI, or automatic message detection is added.

CI runs `native-scroll-cues` and `native-scroll-render` with the existing
regressions. They cover boundaries, partial progress, overlapping and out-of-order
cues, 1,024-cue lists, wrapping, mixed metrics, resets, clipping, fixed content,
effects, and event expiry. Builds and execution belong to GitHub Actions.
