# Mangetsu native vertical text

Native vertical text uses Mangetsu's own layout cursor. It shapes text with the
ordinary font fallback and HarfBuzz pipeline, measures positioned fill outlines,
and then places vertical units and columns. Font and HarfBuzz vertical advance
metrics do not decide unit spacing. Borders, shadows, blur, gradients, paint
effects, and final transforms run after layout.

## Tags

| Tag | Meaning |
| --- | --- |
| `\vert0` | Horizontal ASS layout (default). |
| `\vert1` | Mangetsu native vertical layout. |
| `\vtype0` | Classify each layout unit by script (default). |
| `\vtype1` | Override unit spacing/orientation with CJK cells. |
| `\vtype2` | Override unit spacing with Latin optical policy. |
| `\vtype3` | Override unit spacing with Myanmar optical policy. |
| `\vdir0` | Choose one column direction from the event's strong scripts (default). |
| `\vdir1` | New columns advance left. |
| `\vdir2` | New columns advance right. |
| `\vspN` | Add `N` script pixels between vertical units. |
| `\vcolspN` | Add `N` script pixels between columns. |

These are event layout controls: put them before the first text character.
Changes after text has begun and changes inside `\t` are ignored. `\r` style
resets do not change the event's vertical layout selection. `\vert0` preserves
ordinary ASS behavior and legacy `\fn@FontName` rendering.

Native vertical text has one shared flow. In `\vtype0`, each shaped cluster or
Myanmar syllable selects its own CJK, Latin, or Myanmar layout policy. A CJK
character does not disable Myanmar syllable grouping elsewhere in the event.
Explicit `\vtype1`–`3` override the unit spacing policy, while shaped clusters
and Myanmar combining sequences remain intact. Font fallback cannot change the
classification. `\vdir0` independently chooses a single direction for the
block: CJK-dominant text goes left, and other text goes right. `\vdir1` and
`\vdir2` override that direction for the whole block.

| Unit type | Layout unit | Spacing |
| --- | --- | --- |
| CJK | Complete shaped cluster | Approximately full-em cell |
| Latin | Complete shaped cluster | Fill ink edges plus gap and minimum cell |
| Myanmar | Grammatical syllable containing complete shaped clusters | Fill ink edges plus gap and minimum cell |

CJK uses `vert`/`vkna` substitutions for eligible upright characters and
punctuation. Han, Kana, Hangul, and vertical punctuation remain upright.
Sideways characters receive a local glyph orientation before the author's
`\frz` transform. Latin optical text stacks upright clusters. Myanmar syllables
remain horizontally shaped internally; medials, vowels, kinzi, and stacked
consonants are never individually rotated or advanced down the column.

`\N` finishes the current column. Automatic wrapping compares vertical units
with the usable frame height and starts another column at a legal break when
possible. `WrapStyle=2` suppresses automatic wrapping, as in horizontal ASS.
Existing `\an1`–`\an9`, margins, `\pos`, and `\move` anchor the final block
bbox in ordinary ASS coordinates, including events without `\pos`. `\ta1`–`9`
separately align shorter columns vertically and differently sized units
horizontally inside each column. Without `\ta`, internal alignment inherits
`\an`. Progressive `\kf`/`\K` karaoke wipes from top to bottom in native
vertical mode.

## Examples

```ass
{\an5\vert1\vtype1}日本語です。\N「縦書き」
{\an5\vert1}中文垂直排版。
{\an5\vert1}한국어세로쓰기
{\an5\vert1\vtype2\vsp2}minimum
{\an5\vert1\vtype3}မြန်မာနိုင်ငံ
{\an5\ta5\vert1}မြန်မာ日本ngar harနိုင်ငံ\N次列
```

In `TVアニメ` or `日本2026年`, Kana/Han use CJK cells while Latin letters and
digits use optical units. The chosen column direction stays fixed for the
whole event. Myanmar syllables remain grouped even when Japanese is present.
`\vdir2` overrides a CJK block's leftward default.

Proper native vertical furigana placement is outside this first version.
Furigana markup in a native vertical event is rendered as literal text;
horizontal Mangetsu furigana remains unchanged. Curved text paths are likewise
horizontal layout controls and are ignored in native vertical mode.
