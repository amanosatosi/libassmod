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
|Miku:\NHello|\N\N|reply plz?|\N\N|Yurf:\NYo|\N\N|sorry|
```

Each `|Name:\Nbody|` block starts a message from that speaker. A bare `|body|`
block inherits the previous speaker and resolved side, so `reply plz?` is
Miku's next message and `sorry` is Yurf's. The pipe delimiters and `:\N`
speaker/body boundary do not render. `\N`, whitespace, or no separator at all
can appear between completed blocks without creating empty messages. Names
accept UTF-8 and surrounding whitespace is trimmed for identity. `\msgm(name)`
places matching speakers on the right and other speakers on the left. With no
main speaker, all named messages use the left side.

The older `|\Nbody|` form also inherits the last speaker and side. A bare
block before any named message creates an anonymous left message. `\msgshowname0`
hides labels but preserves identity, inheritance, and side selection. Leading
override blocks, such as `|{\3c&H39C5BB&}Miku:\NHello|` or
`|{\bubbs3}Another message|`, apply before that message and carry forward
through ordinary sequential render state. They are excluded from the logical
speaker name; formatting in the body stays ordinary Mangetsu text.

Only an unescaped `:\N` divides a name from a body. A colon in a bare body,
such as `|wait: what?|`, is ordinary text. Within a mode 2 message block, the
exact source sequence `\:\` decodes to a literal colon. For example,
`|Miku\:\Alt:\NHello|` has speaker `Miku:Alt` and body `Hello`. This escape
is specific to mode 2 blocks.

The scanner keeps pipes inside balanced ASS overrides and furigana groups
such as `<今日|きょう>` inside a message. An existing `\|` escape also stays in
the body. An arbitrary unescaped body pipe outside those structures closes
the block. Incomplete blocks are ignored.

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
its grammar. Later mode tags do not switch it. Mode 2 reads pipe message
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

The first `\msgtitle(name)` wins in all three modes. An empty or absent title
removes the header. The panel follows normal `\an`, `\pos`, `\move`, style
margins, and outer clip placement. The header uses an automatically selected
black or white surface and opposite text color.

The convenient chat color channels are:

| Tag | Native chat surface |
| --- | --- |
| `\c` / `\1c`, `\1a` | Message body fill color, alpha |
| `\2c`, `\2a` | Speaker-name fill color, alpha |
| `\3c`, `\3a` | Bubble fill color, alpha |
| `\4c`, `\4a` | Outer panel fill color, alpha |

Use these short tags for ordinary chat styling. ASS alpha runs from `&H00&`
(opaque) to `&HFF&` (transparent); the usual numeric 0–255 alpha form also
works. For more detailed styling, every chat mode also accepts:

| Tag | Native chat surface |
| --- | --- |
| `\bubc`, `\buba` | Explicit bubble fill color, alpha |
| `\bubbc`, `\bubba`, `\bubbs` | Rounded bubble border color, alpha, size |
| `\bc`, `\ba`, `\bs` | Message text outline color, alpha, size |

`\bubc` and `\buba` are aliases for the same bubble fill state as `\3c` and
`\3a`; they do not replace the short tags. Assignments apply in source order,
so the last tag for each property wins. The bubble border and message text
outline are independent. `\bubbs0` removes the bubble border; negative sizes
clamp to zero. Border width follows ASS script coordinates and is scaled with
the event. Bubble padding accounts for its border so it stays inside the
panel and away from the message text. New properties inherit from one message
to the next, regardless of speaker; `\r` resets them to the active style.

```ass
{\chatmode2\msgtitle(Miku)\msgm(Miku)}
|{\c&HFFFFFF&\bc&H000000&\ba&H30&\bs2\bubc&H332244&\buba&H20&\bubbc&HFF55CC&\bubba&H10&\bubbs4}Miku:\NHello|
|same speaker again|
```

Static values of these tags work in all modes. They are parsed safely inside
`\t(...)`, using the existing tag interpolation, but the phone panel and
bubble layout are not independently animated as a composite geometry.

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
