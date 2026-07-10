# Text Alignment Tag

This fork supports an ASS override tag for selecting the horizontal text
alignment independently from the object anchor:

```ass
\tan<N>
```

`N` uses the same numpad values as `\an`, from 1 through 9. Only the horizontal
part is used:

```text
7, 4, 1 = left
8, 5, 2 = center
9, 6, 3 = right
```

`\an` continues to select the object anchor and the vertical text alignment.
`\tan` replaces only the horizontal text alignment relative to that anchor.
When `\tan` is absent, text alignment defaults to the active `\an` value, so
existing ASS files render unchanged.

Example:

```ass
{\an3\tan7\pos(1000,700)}Hello
```

The object anchor is still the `\an3` bottom-right anchor at `(1000,700)`.
The text is laid out left-aligned, with the same bottom vertical alignment as
the `\an3` anchor.

Malformed or out-of-range `\tan` values are ignored.
