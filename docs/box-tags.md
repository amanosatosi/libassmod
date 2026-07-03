# Box Override Tags

This fork supports override tags for BorderStyle=4-style rectangular event
boxes. Existing styles with `BorderStyle=4` keep working without any override
tags.

Line-level BorderStyle overrides such as `\bs3`, `\bs4`, and `\bs5` are
documented in `docs/borderstyle-tags.md`.

## Box Mode

```ass
\box1
\box0
```

- `\box1` enables BorderStyle=4-style rectangular box rendering for the
  current override state.
- `\box0` disables it for the current override state.
- `\r` resets box mode to the current style default.
- `\4c` remains the box color.
- `\4a` and the existing shadow alpha behavior remain the box alpha behavior.

## Extra Padding

```ass
\boxp<N>
\boxpx<N>
\boxpy<N>
```

- `\boxp<N>` adds `N` pixels of extra padding to all sides.
- `\boxpx<N>` adds `N` pixels of extra horizontal padding to both left and
  right.
- `\boxpy<N>` adds `N` pixels of extra vertical padding to both top and bottom.
- Padding only affects the final rectangle when box mode is active.
- The box remains one clean rectangle; padding is not applied per glyph or per
  fragment.
- `\r` resets extra padding to zero.

Negative values are clamped to zero. Invalid or bare padding tags are ignored.
