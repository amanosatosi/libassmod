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
black or white surface and opposite text color by default, based on the outer
panel color. `\msgtitlec&HBBGGRR&` sets the header/contact-name text color;
`\msgtitlegbc&HBBGGRR&` sets its background color. Both work in all three modes.

The convenient chat color channels are:

| Tag | Native chat surface |
| --- | --- |
| `\c` / `\1c`, `\1a` | Message body fill color, alpha |
| `\2c`, `\2a` | Speaker-name fill color, alpha; explicit `\2c` also sets header-name color |
| `\3c`, `\3a` | Bubble fill color, alpha |
| `\4c`, `\4a` | Outer panel fill color, alpha |

Use these short tags for ordinary chat styling. ASS alpha runs from `&H00&`
(opaque) to `&HFF&` (transparent); the usual numeric 0–255 alpha form also
works. For more detailed styling, every chat mode also accepts:

| Tag | Native chat surface |
| --- | --- |
| `\msgtitlec` | Explicit header/contact-name text color |
| `\msgtitlegbc` | Explicit header/contact-name background color |
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

`\2c` and `\msgtitlec` update the same header-name color state in source order.
The later assignment wins for the header, while only `\2c` changes message
speaker-name colors. For example:

```ass
{\chatmode2\2c&HFFFFFF&\msgtitlec&H39C5BB&\msgtitle(Miku)}|Miku:\NHello|
```

This uses white message speaker names and `&H39C5BB&` for the header name.
Reversing those two color tags makes both names white. A background override
can be added independently:

```ass
{\chatmode2\msgtitle(Miku)\msgtitlec&H39C5BB&\msgtitlegbc&H223344&}|Miku:\NHello|
```

Header colors are shared by the whole phone UI. Their final sequential state
applies to the header, including assignments inside message override blocks.
Use the leading override block for a fixed header color. Style-default
SecondaryColour and `\2a` alone do not override the automatic header text;
only an explicit `\2c` or `\msgtitlec` does. Without `\msgtitlegbc`, the
automatic header background remains unchanged. Setting only `\msgtitlegbc`
preserves the existing panel-based automatic text color. These header color
controls are opaque RGB colors and do not change message color or alpha
channels. Bare `\msgtitlec` / `\msgtitlegbc` restore their automatic fallbacks;
`\r` clears both header overrides. An explicit bare `\2c` restores the style's
speaker color and also uses that color for the header.

```ass
{\chatmode2\msgtitle(Miku)\msgm(Miku)}
|{\c&HFFFFFF&\bc&H000000&\ba&H30&\bs2\bubc&H332244&\buba&H20&\bubbc&HFF55CC&\bubba&H10&\bubbs4}Miku:\NHello|
|same speaker again|
```

Static values of these tags work in all modes. They are parsed safely inside
`\t(...)`, using the existing tag interpolation, but the phone panel and
bubble layout are not independently animated as a composite geometry.

## Left/right appearance presets

`\msgleft(...)` and `\msgright(...)` define reusable visual defaults for the
resolved left or right side. They never create messages. All three grammars
use their existing side rules, including explicit `\msg(name,left/right)`,
`\msgm(name)`, and mode 3's `\ta1/4/7` versus `\ta3/6/9`. Bare mode 2 blocks
and mode 3 `{|}` continuations keep the previous side.

The tuple has **exactly 12 positional fields**, in this order. This order is
fixed for tools that generate ASS:

| Position | Field | Existing chat control |
| --- | --- | --- |
| 1 | textColor | `\c` / `\1c` |
| 2 | textAlpha | `\1a` |
| 3 | nameColor | `\2c` (message names only) |
| 4 | nameAlpha | `\2a` |
| 5 | bubbleColor | `\3c` / `\bubc` |
| 6 | bubbleAlpha | `\3a` / `\buba` |
| 7 | bubbleBorderColor | `\bubbc` |
| 8 | bubbleBorderAlpha | `\bubba` |
| 9 | bubbleBorderSize | `\bubbs` |
| 10 | textOutlineColor | `\bc` |
| 11 | textOutlineAlpha | `\ba` |
| 12 | textOutlineSize | `\bs` |

```ass
{\chatmode2\msgm(Miku)\msgright(&HFFFFFF&,&H00&,&H39C5BB&,&H00&,&H332244&,&H20&,&HFF55CC&,&H10&,4,&H000000&,&H30&,2)}|Miku:\NHello||{\bubc&H0000FF&}Important||Normal purple bubble again|
```

Colors use ASS BGR notation; alpha uses ASS hexadecimal or numeric 0–255
notation. Sizes use the same script coordinates as the individual tags.
Negative sizes clamp to zero; sizes above 10000 clamp to 10000. Missing,
extra, empty, or invalid fields reject the entire tuple without changing the
previous preset. `\msgleft()` / `\msgright()` clear just that side's preset.

The precedence is existing style, actor/inherited defaults, side preset,
then explicit message tags. A preset uses the existing chat paint state;
individual tags still override one property. On a side with a preset, those
message overrides stay local: the next bubble starts from the preset again.
Left and right presets are independent. On a side with no preset, the
existing sequential inheritance of ordinary tags remains unchanged.

Presets can be declared in the event prefix or a message's leading override
blocks; leading declarations are collected before that message's explicit
tags. A declaration within body text applies to subsequent messages. Font,
panel, header, title, timing, and receipt state are outside these tuples.
In particular, a preset nameColor does **not** alias `\msgtitlec`; an explicit
ordinary `\2c` retains its existing header alias behavior.

The existing `mangetsu-colorcoding` actor metadata may supply side presets
and receipt defaults. Its actor lookup and style whitelist rules still apply;
an event preset overrides the actor preset. `\r` clears event presets and
receipt overrides, then restores applicable actor defaults. A reset inside
a message restores its baseline immediately; presets apply again at the next
message. These new configuration tags are discrete and ignored inside `\t`.

## Native sent/read marks

Receipts are opt-in: using `\readmark` or `\readtime` activates them from that
point onward, including when inherited from actor metadata. Scripts with no
receipt tags retain their old output. The initial receipt state is
`\readmark2` with zero delay:

| Tag | Final receipt state |
| --- | --- |
| `\readmark0` | Hidden |
| `\readmark1` | Sent: one check |
| `\readmark2` | Read: two checks |

In modes 1 and 2, marks appear on right-side messages from the `\msgm(name)`
speaker. A different speaker explicitly placed on the right has no receipt;
a self message explicitly placed on the left also has none. In mode 3, right
is outgoing and left is incoming. Incoming messages never draw a receipt,
but do not erase the inherited `\readmark` or `\readtime` state.

Checks are native metadata in the outgoing bubble's lower-right padding,
using its body text color and alpha. They follow the existing bubble viewport,
scrolling, animation position, and user clips. They do not add characters,
text advances, wrapping, or extra bubble dimensions.

`\readtime1245` sets a delay of 1245 milliseconds **relative to each message's
own appearance time**, without parentheses. Both receipt tags are stateful:
later applicable messages inherit them until changed or reset. Valid delays
are integers from 0 through 2147483647; malformed values are ignored.

```ass
{\chatmode2\msgm(Miku)\readmark2\readtime1245\msgstartcount(0)\msgtime(1000,3000)\msganim(250)}|Miku:\NFirst||Second|
```

The first message starts sent at event +1000 ms and becomes read at +2245 ms;
the second starts sent at +3000 ms and becomes read at +4245 ms. The clock
starts at the existing reveal time, including during the slide animation.
Messages visible at event start, or static messages without `\msgtime`, use
event start as their appearance time. Unscheduled hidden messages have no
receipt. The sent-to-read switch is immediate: no fade or interpolation.

`\readmark2` with no delay, or with `\readtime0`, shows read immediately.
`\readmark1` remains sent regardless of delay; `\readmark0` remains hidden.
Receipt tags in a message's overrides set that message's metadata and carry
forward. These tags are ignored outside native chat mode.

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
