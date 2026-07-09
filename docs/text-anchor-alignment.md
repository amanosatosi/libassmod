# Text Anchor Alignment Tag

This fork supports an ASS override tag for separating text layout alignment
from the object anchor:

```ass
\tan<N>
```

`N` uses the same numpad values as `\an`, from 1 through 9.

`\an` continues to select the object anchor used for positioning and transform
origin behavior. `\tan` selects the text alignment relative to that anchor.
When `\tan` is absent, text alignment defaults to the active `\an` value, so
existing ASS files render unchanged.

Example:

```ass
{\an3\tan7\pos(1000,700)}Hello
```

The object anchor is still the `\an3` bottom-right anchor at `(1000,700)`.
The text is laid out with `\tan7` top-left alignment relative to that anchor.

Malformed or out-of-range `\tan` values are ignored.
