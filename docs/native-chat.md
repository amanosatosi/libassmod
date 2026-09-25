# Native chat UI

One ASS dialogue event can contain a complete phone conversation. Mangetsu
shapes its text with the ordinary ASS and furigana pipeline, measures the
result, and draws the panel, header, and rounded message bubbles itself.

## Explicit messages

```ass
{\chatmode1\msgtitle(Miku)\msgm(Miku)\msgshowname1}
{\msg(Miku)}Hello\Nagain{\msg(Yurf,left)}Hi
```

`\msg(name)` starts a message. `\msg(name,left)` and `\msg(name,right)`
override the side. Otherwise, the `\msgm(name)` speaker appears on the right
and other speakers on the left. `\msgshowname0` hides speaker labels while
keeping their identities for side selection. Names are shown by default.
An ordinary `\N` inside a message is a line break in that bubble.

## Fast messages

```ass
{\chatmode2\msgtitle(Miku)}{\ta7}Hey\N{\ta9}What\N{|}Really?
```

At the beginning of the payload or after `\N`, `\ta1`, `\ta4`, and `\ta7`
start a left message; `\ta3`, `\ta6`, and `\ta9` start a right message.
`\ta2`, `\ta5`, and `\ta8` are reserved. `{|}` starts another message on
the previous side. A `\N` without a following message boundary remains a
normal line break. Ordinary formatting continues across `{|}`. Outside
`\chatmode2`, the marker has no chat meaning.

## Timing and placement

`\msgstartcount(n)` selects how many messages are visible at event start.
`\msgtime(t1,t2,...)` supplies absolute millisecond offsets from event start
for the remaining messages, in source order. With no `\msgtime`, every
message is visible throughout the event. Missing timing values leave the
corresponding messages hidden. Negative values become zero, later descending
values become the preceding time, and equal times reveal together.
`\msganim(ms)` sets the slide duration, which defaults to 250 ms. Zero is
immediate.

The first valid `\chatmode1` or `\chatmode2` chooses the event grammar.
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

See [chat-modes.ass](../samples/chat-modes.ass) for equivalent explicit and
fast conversations positioned side by side.

## Current limits

The syntax model is cached per renderer, and glyph outlines/bitmaps use the
existing libass caches. Text still passes through the ordinary shaping and
wrapping path on each rendered frame. A complete reusable shaped-layout cache
is not yet present. Global rotation, shear, and perspective affect glyphs
through the existing path but do not transform the panel geometry as one
composite object.
