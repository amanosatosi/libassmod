# Experimental Furigana Extension

This fork supports an experimental native furigana syntax in normal event text:

```ass
<base|furi>
```

Furigana parsing is enabled by default. A text sequence is treated as furigana
only if it is an angle-bracket group containing an unescaped pipe. Angle-bracket
text without a pipe, such as `<cool>` or `<dramatic>`, remains literal text.

Malformed groups are rendered literally. Override tags inside a furigana group
are not supported in this first implementation; use override tags around the
group instead.

## Tags

```ass
\furi0
\furi1
\furis<N>
\furisx<N>
\furisy<N>
\furifsp<N>
\furipos(left|center|right,x,y)
```

`\furi0` disables parsing of `<base|furi>` groups from that point in the event.
`\furi1` re-enables it.

Sizing values are percentages of the base text size. `\furis<N>` sets both
furigana axes. `\furisx<N>` and `\furisy<N>` set the horizontal and vertical
axes independently. The defaults are `\furis50`, `\furisx50`, and `\furisy50`.

`\furifsp<N>` mirrors ASS `\fsp`, but applies only to furigana text. The default
is 0.

`\furipos(<align>,<x>,<y>)` controls furigana placement. Positive y moves
furigana upward; negative y moves it downward. The default is
`\furipos(center,0,0)`.

## Examples

```ass
{\furi1}<明日|あした>また会う
{\furi1\furis45\furifsp1\furipos(center,0,3)}<明日|あした>また会う
{\furi0}<A|B>
```

Furigana is shaped and rendered as sidecar glyphs tied to the base glyph range.
The base text remains the primary text for horizontal line layout and wrapping.
After visual lines are resolved, furigana overhang above or below its base text
is included in that line's vertical metrics. Multiple furigana groups on the
same line use the maximum above and below extent, not the sum. Furigana may
overhang its base text horizontally and is included in rendered event bounds,
but it does not increase the base advance width.
