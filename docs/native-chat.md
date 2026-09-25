# Native chat UI

One ASS dialogue event can contain a complete phone conversation. The three
source grammars below feed one cached chat scene and the same native layout,
shaping, furigana, timing, clipping, and rendering path.

## Explicit messages — `\chatmode1`

```ass
{\chatmode1\msgtitle(Miku)\msgm(Miku)\msgshowname1}
{\msg(Miku)}Hello{\msg(Yurf)}Yo
```

`\msg(name)` starts a message. `\msg(name,left)` and `\msg(name,right)`
override its side. Otherwise, the `\msgm(name)` speaker appears on the right
and other speakers on the left. `\msgshowname0` hides speaker labels while
keeping their identities for side selection. Names are shown by default.
An ordinary `\N` inside a message is a line break in that bubble.

## Named lazy messages — `\chatmode2`

```ass
{\chatmode2\msgtitle(Miku)\msgm(Miku)\msgshowname1}
|Miku:\NHello|\N\N|Yurf:\NYo|
```

Each `|Name:\Nbody|` block is one message. The pipe delimiters and `:\N`
speaker/body boundary do not render. `\N`, whitespace, or no separator at all
can appear between completed blocks without creating empty messages. Names
accept UTF-8 and surrounding whitespace is trimmed for identity. `\msgm(name)`
places matching speakers on the right and other speakers on the left. With no
main speaker, all named messages use the left side.

`|\Nbody|` inherits the last resolved speaker and side. If there was no
previous message, it creates an anonymous left message. `\msgshowname0` hides
labels but preserves identity, inheritance, and side selection. Override
blocks before the name, such as `|{\c&HFFFFFF&}Miku:\NHello|`, apply through
normal render state; the tags are excluded from the logical speaker name.
Formatting in the body stays ordinary Mangetsu text.

The scanner keeps pipes inside balanced ASS overrides and furigana groups
such as `<今日|きょう>` inside a message. An existing `\|` escape also stays in
the body. An arbitrary unescaped body pipe outside those structures closes
the block; this mode adds no new escape syntax. Malformed blocks are ignored.

## Alignment shorthand — `\chatmode3`

```ass
{\chatmode3\msgtitle(Miku)}{\ta7}Hello\N{\ta9}Yo\N{|}Bruh
```

At the beginning of the payload or after `\N`, `\ta1`, `\ta4`, and `\ta7`
start a left message; `\ta3`, `\ta6`, and `\ta9` start a right message.
`\ta2`, `\ta5`, and `\ta8` are reserved. `{|}` starts another message on
the previous side. A `\N` without a following message boundary remains a
normal line break. Ordinary formatting continues across `{|}`. This mode has
no speaker identity; `\msgm` and `\msgshowname` have no named labels to affect.

The first valid `\chatmode1`, `\chatmode2`, or `\chatmode3` in an event chooses
its grammar. Later mode tags do not switch it. Mode 2 reads only named pipe
blocks; mode 3 reads only alignment shorthand. Outside chat mode, pipes and
`{|}` retain their ordinary subtitle behavior.

## Shared timing, color, and placement

`\msgstartcount(n)` selects how many messages are visible at event start.
`\msgtime(t1,t2,...)` supplies absolute millisecond offsets from event start
for the remaining messages, in source order. With no `\msgtime`, every
message is visible throughout the event. Missing timing values leave the
corresponding messages hidden. Negative values become zero, later descending
values become the preceding time, and equal times reveal together.
`\msganim(ms)` sets the slide duration, which defaults to 250 ms. Zero is
immediate.

The first `\msgtitle(name)` wins. An empty or absent title removes the
header. The panel follows normal `\an`, `\pos`, `\move`, style margins, and
outer clip placement. `\c` or `\1c` colors message text, `\2c` colors speaker
labels, `\3c` colors bubbles, and `\4c` colors the panel. The header uses
an automatically selected black or white surface and opposite text color.

The panel width is limited to 30–56% of the video content width, and bubbles
are limited to 75% of its inner width. Text wraps within that cap. The fixed
message viewport is limited to 68% of the content height; older bubbles
scroll through its top clip while the header stays in place. These ratios are
centralized in `chat_choose_metrics` for visual tuning.

See [chat-modes.ass](../samples/chat-modes.ass) for comparable conversations
in all three modes.

## Current limits

The syntax model is cached per renderer, and glyph outlines/bitmaps use the
existing libass caches. Text still passes through the ordinary shaping and
wrapping path on each rendered frame. A complete reusable shaped-layout cache
is not yet present. Global rotation, shear, and perspective affect glyphs
through the existing path but do not transform the panel geometry as one
composite object.
