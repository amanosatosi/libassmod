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

## Box Borders

BorderStyle=4 box mode also supports native box-border layers:

```ass
\Nbbs<N>
\Nbbc&HBBGGRR&
\Nbba&HAA&
```

`N` is a box-border layer number from 1 through 10. These tags affect only the
BorderStyle=4 rectangular box. They do not change normal glyph/vector outline
layers used by `\bord`, `\Nbs`, or `\bs5`.

Layer 1 has unnumbered aliases:

```ass
\bbs<N>        == \1bbs<N>
\bbc&HBBGGRR& == \1bbc&HBBGGRR&
\bba&HAA&     == \1bba&HAA&
```

Box borders are drawn around the final padded box rectangle and grow outward,
so they do not shrink the fill area or consume text padding. Multiple enabled
layers are rendered as non-overlapping outward rings, with larger layers behind
smaller layers.

If a layer has no size or a zero size, it is not drawn. Negative sizes are
clamped to zero. If a layer has no explicit `\Nbbc`, its RGB color falls back
to the current box color (`\4c`). If it has no explicit `\Nbba`, its alpha
falls back to the current box alpha (`\4a`/BackColour alpha).

Examples:

```ass
{\bs4\boxp12\bbs4\bbc&H000000&}Text
{\bs4\boxp12\1bbs3\1bbc&HFFFFFF&\2bbs8\2bbc&H000000&}Text
```
