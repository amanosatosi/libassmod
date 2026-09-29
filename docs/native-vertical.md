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
| `\vtype0` | Select one profile from the event's strong scripts (default). |
| `\vtype1` | CJK full-em cell profile. |
| `\vtype2` | Latin optical fill-bounds profile. |
| `\vtype3` | Myanmar syllable profile. |
| `\vdir0` | Use the profile's default column direction (default). |
| `\vdir1` | New columns advance left. |
| `\vdir2` | New columns advance right. |
| `\vspN` | Add `N` script pixels between vertical units. |
| `\vcolspN` | Add `N` script pixels between columns. |

These are event layout controls: put them before the first text character.
Changes after text has begun and changes inside `\t` are ignored. `\r` style
resets do not change the event's vertical layout selection. `\vert0` preserves
ordinary ASS behavior and legacy `\fn@FontName` rendering.

Automatic profile selection checks the whole event, with CJK strong scripts
(Han, Hiragana, Katakana, Hangul) taking priority over Myanmar, then Latin.
The selected profile and default direction remain fixed across font fallback
and mixed text. An explicit `\vtype` always wins.

| Profile | Layout unit | Spacing | Default new column |
| --- | --- | --- | --- |
| CJK | Complete shaped cluster | Approximately full-em cell | Left |
| Latin | Complete shaped cluster | Fill height plus gap, with a minimum | Right |
| Myanmar | Linguistic/layout syllable containing complete shaped clusters | Fill height plus gap, with a minimum | Right |

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
bbox in ordinary ASS coordinates. Progressive `\kf`/`\K` karaoke wipes from
top to bottom in native vertical mode.

## Examples

```ass
{\an5\vert1\vtype1}日本語です。\N「縦書き」
{\an5\vert1}中文垂直排版。
{\an5\vert1}한국어세로쓰기
{\an5\vert1\vtype2\vsp2}minimum
{\an5\vert1\vtype3}မြန်မာနိုင်ငံ
```

In a CJK event such as `TVアニメ` or `日本2026年`, the embedded Latin characters
do not switch the spacing profile or reverse the column direction. `\vdir2`
overrides the CJK leftward default.

Proper native vertical furigana placement is outside this first version.
Furigana markup in a native vertical event is rendered as literal text;
horizontal Mangetsu furigana remains unchanged. Curved text paths are likewise
horizontal layout controls and are ignored in native vertical mode.
