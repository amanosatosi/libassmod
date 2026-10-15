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
horizontally inside each column. The first valid `\ta` in each logical line
delimited by `\N` applies to the entire column, including preceding units;
later tags and style resets do not replace it. Automatic wrapped columns
share that logical line's selection. A new explicit column without `\ta`
inherits the global `\tan`/`\an` alignment. The full numpad value still controls
both axes inside native columns; it never changes the event's block anchor.
Progressive `\kf`/`\K` karaoke wipes from top to bottom in native
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

## Native vertical furigana

```ass
{\vert1}<漢字|かんじ>
{\vert1}<漢字||かんじ>
{\vert1}<漢字|かんじ|カンジ>
```

Both base and readings flow from top to bottom, using native shaped clusters,
font fallback, vertical CJK substitutions and the same script-specific unit
policies as ordinary vertical text. Readings are not rotated horizontal images.
Normal ruby (field 2) is always on the physical **right** of its base;
gyaku-furigana (field 3) is always on the physical **left**. Gyaku does not
reverse character order. `\vdir1` advances columns leftward and `\vdir2`
rightward; neither direction nor `\furiplaceauto` changes reading sides.

Each reading is centered on its base's occupied Y span. Its X attachment uses
independently measured base and reading ink, borders and shadows, with a small
visual clearance. The two reading sides do not inherit each other's widths or
vertical metrics. Escapes, invalid-candidate rejection, empty reading sides,
`\furi0`/`\furi1` and style inheritance use the existing furigana parser.

`\furis`, `\furisx` and `\furisy` scale reading glyphs; `\furifsp` adds spacing
along Y. `\furistyle0` and `1` allow long readings to overhang their base's
natural height. `\furistyle2` fits an overlong reading along Y while preserving
its X size. Adjacent readings on the same side of a column are separated along
Y in base order; this moves annotations rather than base glyphs.

`\furipos(x,y)` retains screen axes: positive X moves right and positive Y
moves upward. Offsets apply after automatic attachment, constrained to keep
the reading on its declared side with minimum clearance. `\furiap` controls
the additional automatic gap; it cannot exchange sides. These controls are
captured independently for each group and reset by `\r`.

| Mode | Native vertical behavior |
| --- | --- |
| `\furichangepos0` (default) | Preserve the original base block anchor. The first column and each group's base Y advances remain fixed; later columns move outward when occupied annotation bands require clearance. |
| `\furichangepos1` | Reserve annotation bands between columns and use the complete occupied block for anchoring. |
| `\furichangepos2` | Separate complete occupied column bands, retaining the original base block anchor as in mode 0. |

Column reservation measures both reading sides, including borders and shadows,
before calculating a single outward shift for each subsequent column. Explicit
`\N` and automatically wrapped columns use the same pass. A base group stays
intact across automatic wrapping; a group taller than the usable frame remains
one overflowing column. No extra metrics or padding are added to vertical
events without annotations. Compact and strict placement distinguish base
anchoring from occupied rendering bounds: ruby contributes to collision and
background-box extents without shifting the base anchor at the outer edges.
Content can still extend outside the frame at an explicitly chosen position.

Internal `\k`, `\kf`/`\K`, `\ko` and reveal timing use the shared furigana
karaoke timeline. Reading segments project onto their associated base along Y;
progressive wipes run top to bottom, including independent two-sided readings
and empty timing segments. The ordinary glyph paint path supplies colors,
alpha, native border layers, shadows, images and gradients. `\pgrdalpha` samples
final-frame coordinates for reading pixels just as for base text; background
box exemptions are unchanged.

See [the annotated-column sample](../samples/native-vertical-furigana.ass).
Horizontal furigana keeps its existing placement, spacing and karaoke paths.
Curved text paths remain horizontal layout controls and are ignored in native
vertical mode.
