# Experimental Furigana Extension

This fork supports an experimental native furigana syntax in normal event text:

```ass
<base|furi>
<base|furi|gyaku-furi>
<base||gyaku-furi>
```

Furigana parsing is enabled by default. A text sequence is treated as furigana
only if it is an angle-bracket group containing exactly **one or two valid,
unescaped field separators** (`|`) outside ASS override blocks. Angle brackets
alone do not activate furigana. `<test>` and `<{\c&H3535C5&}test>` remain ordinary
ASS text: the angle brackets are visible, and the color override executes
normally. Pipes inside `{...}` are not field separators.

The base must contain text. Field 2 is ordinary furigana; optional field 3 is
gyaku-furi, using the same font, shaping, size/scales, colors/alpha,
outline/shadow, transforms and inherited Mangetsu properties, on the **opposite
side** of the base. Field 2 may be empty when field 3 contains text. An empty
third field is allowed (`<base|furi|>` is equivalent to `<base|furi>`). At least
one annotation must contain text.

Inside an angle-bracket candidate, `\|\` produces a literal pipe **before field
splitting**. `<ABC\|\DEF|reading>` has base `ABC|DEF`, and `<ABC\|\DEF>` has no
field separators and renders as ordinary `<ABC|DEF>`. This three-character
escape is scoped to angle-bracket candidates; the existing Mangetsu escapes
`\<`, `\>`, `\|` and `\\` remain supported.

Malformed groups, including three or more real separators, remain ordinary ASS
text, with normal override parsing. Balanced override blocks are accepted
inside valid ruby for karaoke timing, but only karaoke tags in the reading are
interpreted. Place other overrides around the whole group to inherit them on
the base and both annotations. A group cannot contain a hard line break; place
line breaks between groups.

## Karaoke timing

Karaoke timing may be placed in the reading side:

```ass
<病|{\k30}や{\k26}ま{\k10}い>
```

Each visible timed reading segment maps to a corresponding region of the
complete shaped base. The regions follow the relative visible widths of the
shaped reading segments, so they are not forced into equal divisions. Region
ownership follows the shaped visual order after bidi reordering, rather than
karaoke tag source order. `\kf`
and its `\K` alias use the same interval for the progressive reading fill and
the matching base region. Their sweep follows the reading segment's bidi
direction, including on the corresponding base region. Lowercase `\ko`
retains its existing meaning. Uppercase `\kO` is Mangetsu reveal karaoke:
each reading segment and matching base region are hidden before their shared
start and appear fully at that start. It has no progressive sweep.

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

Both annotations use that same timeline. When both fields contain readings,
field 2 owns the internal-karaoke mapping onto the base; field 3 renders its
own timed segments without overwriting that mapping. For `<base||gyaku-furi>`,
field 3 owns the mapping instead. Outer karaoke remains inherited by both.

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
\furiplaceauto0
\furiplaceauto1
\furichangepos0
\furichangepos1
\furistyle<N>
```

`\furi0` disables parsing of `<base|furi>` groups from that point in the event.
`\furi1` re-enables it.

Sizing values are percentages of the base text size. `\furis<N>` sets both
furigana axes. `\furisx<N>` and `\furisy<N>` set the horizontal and vertical
axes independently. The defaults are `\furis50`, `\furisx50`, and `\furisy50`.

`\furifsp<N>` mirrors ASS `\fsp`, but applies only to furigana text. The default
is 0.

The existing `\furiap1` / `\furiap0` controls remain backward compatible. They
control the additional automatic vertical gap, enabled by default (4% of the
base font size). `\furiap0` removes that additional gap. Both settings retain a
minimum visual clearance, based on the actual shaped base/annotation bounds
and their sizes, even when a font's ascender or descender metrics are too tight.
This older shorthand controls the gap; it is **not an alias** for the new
two-line outward-placement tag.

`\furipos(<x>,<y>)` selects manual placement and controls the furigana offset
from centered placement. Positive y moves furigana upward; negative y moves it
downward. An explicit `\furipos` has higher priority than `\furiap`, so the
automatic gap is not added when `\furipos` applies. This priority is independent
of tag order: `\furiap1\furipos(0,3)` and `\furipos(0,3)\furiap1` produce the
same manual placement. A parameterless `\furipos` clears the manual offset and
returns subsequent groups to the active automatic-placement setting.

Small negative Y offsets keep the reading above the base while moving it
downward, subject to minimum clearance. A sufficiently negative Y offset
selects the lower side. Gyaku-furi always uses the opposite side, with the same
clearance system. Manual offsets never bypass the minimum visual clearance.

`\furiplaceauto0` is the default. `\furiplaceauto1` selects outward ordinary
furigana **only when the event has exactly two laid-out text lines**, including
lines produced by wrapping: field 2 goes above the visual top line and below
the visual bottom line. Field 3 always goes on the opposite side. One line and
three or more lines retain normal placement, including `\furipos`. In the
two-line case the automatic rule selects the side; manual X/Y offsets still
apply, constrained to that side by visual clearance. These controls are
captured per group and reset with `\r` like the other furigana properties.

`\furichangepos0` is the default and keeps base text placement authoritative.
Adding either annotation does not alter the base advances, line metrics,
alignment anchor, `\pos` placement, or the block bounds used for ordinary ASS
event collision placement. Ruby extends outward and its own geometry resolves
spacing; it can extend outside the video at an edge just like other positioned
content. For example, these put `漢字` at identical coordinates:

```ass
{\pos(640,500)}漢字
{\pos(640,500)\furichangepos0}<漢字|かんじ>
{\pos(640,500)\furichangepos0}<漢字|かんじ|KANJI>
```

`\furichangepos1` enables the older reserve-space behavior: upper and lower
annotation overhang may enlarge line/block bounds, move base lines, and add
spacing between base groups when annotations collide. It is an opt-in
compatibility path. It does not change annotation shaping, opposite-side
placement, minimum clearance or the two-line rule.

`\furistyle<N>` controls horizontal group layout. The default is
`\furistyle0`. `\furistyle0` and `\furistyle1` preserve the base text's normal
shaped advance. Furigana is centered over the base by rendered glyph bounds
and may freely overhang it horizontally; being wider than the base does not by
itself add main-line spacing. Ordinary non-furigana text does not participate
in ruby collision avoidance. If separately annotated furigana groups visually
approach too closely, Mangetsu measures their occupied bounds, including
overhang, strokes and shadows, separately on each side of each visual line.
It moves the annotations by the smallest total squared displacement from their
centered positions that maintains a small size-scaled horizontal gap. Base
advances and positions stay unchanged with `\furichangepos0`. With
`\furichangepos1`, the older base-spacing path is also available.
`\furistyle2` retains its explicitly requested manga-style X-fit: furigana
wider than its base is horizontally shrunk to the base width, while shorter
furigana keeps its normal width. The base advance is kept unchanged.

## Examples

```ass
{\furi1}<明日|あした>また会う
{\furi1\furis45\furifsp1\furipos(0,3)}<明日|あした>また会う
{\furi0}<A|B>
{\furi1\furistyle0}<水鏡|みずかがみ><心誘う|こころいざなう>
{\furi1\furistyle1}<水鏡|みずかがみ><心誘う|こころいざなう>
{\furi1\furistyle2}<水鏡|みずかがみ><心誘う|こころいざなう>
<漢字|かんじ|KANJI>
<漢字||KANJI>
<ABC\|\DEF|reading>
{\furiplaceauto1}<上段|じょうだん>\N<下段|げだん>
{\furichangepos1}<漢字|かんじ>
```

Furigana is shaped and rendered as sidecar glyphs tied to the base glyph range.
The base text remains the primary text for horizontal line layout and wrapping.
The existing typographic attachment height keeps short, descender-only and
tall base glyphs aligned where possible. Shaped outline/stroke/shadow bounds
extend that attachment when nominal metrics would violate visual clearance.
Base and annotation geometry remain separate. Annotation bounds participate in
rendering and effects; only groups with `\furichangepos1` contribute their
overhang to line/block reservation. Multiple groups on a line reserve the
maximum upper and lower overhang, rather than summing it.

Regression coverage is in `test/furi_selftest.c` (`furi-extension` in Meson),
with additional `furi-tight-ascent` and `furi-tight-descent` geometry runs using
original fixture fonts whose ink exceeds their nominal metrics. GitHub Actions
also runs these under AddressSanitizer/UndefinedBehaviorSanitizer in the
Mangetsu furigana regression workflow. Builds are performed in CI.
