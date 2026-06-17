# Experimental ASS Column Layout

This fork supports an experimental, opt-in semantic column layout mode for ASS-like subtitles.

The goal is to make structured subtitle text easier to write, especially lists, status screens, UI-like displays, and translated layouts where manual spacing would be fragile.

Normal ASS rendering is unchanged unless `\column1` is active in the current dialogue event.

## Basic Syntax

Column mode is controlled with override tags:

```ass
{\column1}A|B|C{\column0}
```

* `\column1` enables column mode.
* `\column0` disables column mode.
* Column mode is off by default.
* While column mode is active, `|` separates columns.
* While column mode is active, `||` creates an empty column.
* Outside column mode, `|` remains literal text.
* `|` inside override blocks is tag text and is not a divider.
* `|` inside drawing/vector mode is drawing text and is not a divider.

Rows are separated by normal ASS hard line breaks:

```ass
{\column1}HP|504\NMP|80{\column0}
```

Column measurement is event-local. Separate dialogue events do not share table widths.

## Example

```ass
{\column1}HP|{\align5}:|{\align6}504\NMP|:|80\NAttack|:|1200{\column0}
```

Conceptually:

```text
HP       :    504
MP       :     80
Attack   :  1200
```

The separator is treated as a real column. This means punctuation such as `:`, `・`, `。`, or Burmese `။` can be aligned consistently.

Example:

```ass
{\column1}HP|။|၅၀၄\NMP|။|၈၀{\column0}
```

## Empty Columns

Two adjacent dividers create an empty column:

```ass
{\column1}Name||Status\NSubaru||Alive\NEmilia||Ready{\column0}
```

This creates three columns:

```text
column 0 = name
column 1 = empty spacer
column 2 = status
```

The empty column is part of the table structure. It is not rendered as literal text.

## Column Alignment

Column mode recognizes:

```ass
\align<N>
```

where `N` is `1` through `9`.

The horizontal alignment mapping follows ASS-style numpad direction:

```text
1, 4, 7 = left
2, 5, 8 = center
3, 6, 9 = right
```

The vertical part of the number is ignored.

Examples:

```ass
\align1
\align4
\align7
```

all mean left alignment.

```ass
\align2
\align5
\align8
```

all mean center alignment.

```ass
\align3
\align6
\align9
```

all mean right alignment.

`\align<N>` only matters while column mode is active. It applies to the current column and persists for that same column index across later rows in the same event.

The default is left alignment, equivalent to `\align4`.

Outside column mode, `\align<N>` is harmless and does not affect normal ASS event alignment.

## Karaoke Tags in Column Mode

Karaoke timing tags are ignored while column mode is active.

The ignored tags are:

```ass
\k
\K
\kf
\ko
\kt
```

Inside column mode, these tags are parsed and consumed, but they do not affect column layout, timing, coloring, measurement, or placement.

Example:

```ass
{\column1}HP|{\k20}:|504\NMP|{\K30}:|80{\column0}
```

behaves like:

```ass
{\column1}HP|:|504\NMP|:|80{\column0}
```

The numeric arguments are not shown as text.

Outside column mode, karaoke tags keep their normal ASS behavior.

## Per-Column Defaults

Column mode supports first-cell per-column visual defaults.

This allows a column to define its default styling once, then reuse that styling in later rows.

Example:

```ass
{\column1}HP|{\align5\1c&H00FFFF&}:|{\align6\fnArial\b1\fs48\1c&HFFFFFF&}504\NMP|:|80\NAttack|:|1200{\column0}
```

In this example:

* column 0 keeps the normal style.
* column 1 uses the first styled separator cell as its default.
* column 2 uses the first styled value cell as its default.
* later rows inherit those defaults for the same column index.

Conceptually, later rows behave as if written like this:

```ass
MP|{\align5\1c&H00FFFF&}:|{\align6\fnArial\b1\fs48\1c&HFFFFFF&}80
Attack|{\align5\1c&H00FFFF&}:|{\align6\fnArial\b1\fs48\1c&HFFFFFF&}1200
```

This is useful for clean table-like text:

```ass
{\column1}Name|{\fs60\1c&H00FFFF&}Value\NA|123\NLong Label|456{\column0}
```

The later `123` and `456` cells inherit the column 1 default font size and color.

Column defaults affect both measurement and rendering. If a column default changes font size, font face, border size, or other visual metrics, the renderer measures later cells using that inherited default.

## First Default Wins

For the current implementation, only the first explicit default-setting occurrence for each column becomes that column’s stored default.

Later rows may still contain local override tags, but they do not replace the stored column default.

Example:

```ass
{\column1}A|{\1c&H00FFFF&}first\NB|{\1c&HFF00FF&}second\NC|third{\column0}
```

Current behavior:

* column 1 default becomes cyan from the first row.
* the second row may locally render as magenta.
* the second row does not replace the stored column default.
* the third row inherits the original cyan default.

Future versions may allow later rows to update column defaults, but this version uses first default wins.

## Supported Per-Column Default Tags

The following visual tags may be stored as per-column defaults in column mode.

Font and style tags:

```ass
\fn
\fs
\b
\i
\u
\s
```

Color tags:

```ass
\1c
\2c
\3c
\4c
\5c
```

Alpha tags:

```ass
\1a
\2a
\3a
\4a
\5a
\alpha
```

Border, shadow, and blur tags:

```ass
\bord
\xbord
\ybord
\shad
\xshad
\yshad
\blur
\be
```

Column alignment tag:

```ass
\align<N>
```

## Tags Not Used as Column Defaults

Timing, animation, positioning, clipping, drawing, and reset tags are not stored as column defaults.

Examples:

```ass
\k
\K
\kf
\ko
\kt
\t
\pos
\move
\org
\clip
\iclip
\p
\r
```

Karaoke tags are ignored in column mode as described above.

## Scope and Lifetime

Column layout state is temporary and event-local.

Column defaults do not leak:

* outside `\column0`
* into later normal subtitle text
* into other dialogue events
* into reused renderer state

`\column0` ends the current column block and clears column layout state.

## Reset Behavior

`\r` keeps its normal ASS meaning for the current text state.

Inside column mode, `\r` does not replace the stored per-column defaults in this version. The stored defaults remain first-default-wins until the column block ends.

When column mode ends with `\column0`, all column defaults are cleared.

## Initial Limitations

* No RTL-aware column layout is implemented.
* `\column2` is not implemented.
* Column measurement is event-local only.
* Separate dialogue events do not share column widths.
* Later rows do not update stored column defaults yet.
* Box/background behavior is not specialized for tables yet.
* Column state is temporary and event-local.
