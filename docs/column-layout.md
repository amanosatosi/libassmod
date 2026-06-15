# Experimental ASS Column Layout

This fork supports an experimental, opt-in semantic column layout mode for
ASS-like subtitles. Normal ASS rendering is unchanged unless `\column1` is
active in the current dialogue event.

## Syntax

Column mode is controlled with override tags:

```ass
{\column1}A|B|C{\column0}
```

- `\column1` enables column mode.
- `\column0` disables column mode.
- Column mode is off by default.
- While column mode is active, `|` separates columns.
- While column mode is active, `||` creates an empty column.
- Outside column mode, `|` remains literal text.
- `|` inside override blocks is tag text and is not a divider.
- `|` inside drawing/vector mode is drawing text and is not a divider.

Rows are separated by normal ASS hard line breaks:

```ass
{\column1}HP|504\NMP|80{\column0}
```

Column measurement is event-local. Separate dialogue events do not share table
widths.

## Column Alignment

Column mode also recognizes:

```ass
\align<N>
```

where `N` is 1 through 9. The horizontal alignment mapping is:

```text
1, 4, 7 = left
2, 5, 8 = center
3, 6, 9 = right
```

`\align<N>` only matters while column mode is active. It applies to the current
column and persists for that same column index across later rows in the same
event. The default is left alignment, equivalent to `\align4`.

Outside column mode, `\align<N>` is harmless and does not affect normal ASS
event alignment.

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

## Initial Limitations

- No RTL-aware column layout is implemented.
- `\column2` is not implemented.
- Box/background behavior is not specialized for tables yet.
- Column state is temporary and event-local.
