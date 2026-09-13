# Experimental Furigana Extension

This fork supports an experimental native furigana syntax in normal event text:

```ass
<base|furi>
```

Furigana parsing is enabled by default. A text sequence is treated as furigana
only if it is an angle-bracket group containing an unescaped pipe. Angle-bracket
text without a pipe, such as `<cool>` or `<dramatic>`, remains literal text.

Malformed groups are rendered literally. Balanced override blocks are accepted
inside a group for karaoke timing, but only karaoke tags in the reading side
are interpreted. Other override tags should still be placed around the group.

## Karaoke timing

Karaoke timing may be placed in the reading side:

```ass
<病|{\k30}や{\k26}ま{\k10}い>
```

Each visible timed reading segment maps to a corresponding region of the
complete shaped base. The regions follow the relative visible widths of the
shaped reading segments, so they are not forced into equal divisions. `\kf`
and its `\K` alias use the same interval for the progressive reading fill and
the matching base region. Lowercase `\ko` retains its existing meaning.

An empty timing is a wait. It advances the shared event karaoke clock but does
not create a base region:

```ass
<同|{\k30}お{\k20}{\k50}な>
```

Karaoke tags in the base side are invalid and ignored for karaoke purposes;
they neither create a segment nor alter timing after the group:

```ass
<{\k50}病|やまい>
```

When the reading has no internal karaoke timing, ordinary outer karaoke is the
fallback and activates the base and reading together:

```ass
{\k50}<love|ai>
```

All timing belongs to one event-level timeline. A segment started in a reading
therefore remains active after the closing `>` until another karaoke tag starts
a segment:

```ass
<掴|{\k40}つ{\k60}か>ん{\k70}だ
```

Here the 60-centisecond segment owns `か`, the matching second base region, and
`ん`. Normal text without furigana continues to use the existing libass/VSFilter
karaoke path unchanged.

## Tags

```ass
\furi0
\furi1
\furis<N>
\furisx<N>
\furisy<N>
\furifsp<N>
\furipos(x,y)
\furiap1
\furiap0
\furistyle<N>
```

`\furi0` disables parsing of `<base|furi>` groups from that point in the event.
`\furi1` re-enables it.

Sizing values are percentages of the base text size. `\furis<N>` sets both
furigana axes. `\furisx<N>` and `\furisy<N>` set the horizontal and vertical
axes independently. The defaults are `\furis50`, `\furisx50`, and `\furisy50`.

`\furifsp<N>` mirrors ASS `\fsp`, but applies only to furigana text. The default
is 0.

Automatic vertical placement is enabled by default. `\furiap1` enables it and
`\furiap0` disables it. Automatic placement adds a small gap proportional to
the base text size (currently 4% of the base font size) between the base run's
typographic ascent and the visible bottom of its furigana. Disabling it uses
the previous zero-added-gap placement.

`\furipos(<x>,<y>)` selects manual placement and controls the furigana offset
from centered placement. Positive y moves furigana upward; negative y moves it
downward. An explicit `\furipos` has higher priority than `\furiap`, so the
automatic gap is not added when `\furipos` applies. This priority is independent
of tag order: `\furiap1\furipos(0,3)` and `\furipos(0,3)\furiap1` produce the
same manual placement. A parameterless `\furipos` clears the manual offset and
returns subsequent groups to the active automatic-placement setting.

`\furistyle<N>` controls horizontal group layout. The default is
`\furistyle0`. `\furistyle0` and `\furistyle1` both use Aegisub-style group
spacing: the group advance is the larger of the shaped base width and furi
width (including visible furi overhang), and both base and furi are centered
by their rendered glyph bounds inside that reservation. This retains the
font's shaped advances, side bearings, and inter-glyph spacing while keeping a
short base glyph visually under the middle of a longer ruby. After
shaping, overlapping adjacent ruby groups add space before the right-hand base
group, then use normal line alignment. `\furistyle2` uses manga-style
X-fit: furi wider than its base is horizontally shrunk to the base width, while
shorter furi keeps its normal width. The base advance is kept unchanged.

## Examples

```ass
{\furi1}<明日|あした>また会う
{\furi1\furis45\furifsp1\furipos(0,3)}<明日|あした>また会う
{\furi0}<A|B>
{\furi1\furistyle0}<水鏡|みずかがみ><心誘う|こころいざなう>
{\furi1\furistyle1}<水鏡|みずかがみ><心誘う|こころいざなう>
{\furi1\furistyle2}<水鏡|みずかがみ><心誘う|こころいざなう>
```

Furigana is shaped and rendered as sidecar glyphs tied to the base glyph range.
The base text remains the primary text for horizontal line layout and wrapping.
Ruby is placed against the base run's typographic ascent rather than the
visible top of a particular glyph, so short, descender-only, and tall glyphs
all keep the same ruby height. The automatic gap adds separation above that
typographic-ascent attachment point; it does not replace the font's own
ascent-to-visible-glyph whitespace.
After visual lines are resolved, furigana overhang above or below its base text
is included in that line's vertical metrics. Multiple furigana groups on the
same line use the maximum above and below extent, not the sum. Furigana may
overhang its base text horizontally and is included in rendered event bounds,
while Aegisub-style groups can increase the line advance to fit their shaped
ruby or to prevent adjacent ruby from overlapping.
