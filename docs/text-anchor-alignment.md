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

\ta1–\ta9 select a logical line's internal alignment, independently of
\an's external block anchor. A valid selection is inherited across subsequent
\N lines within that dialogue event until another valid \ta changes it.
Before the first \ta, normal global \tan/\an defaults apply.

~~~ass
{\ta7}First line\NSecond line\NThird line
{\ta7}First line\NSecond line\N{\ta3}Third line\NFourth line
~~~

The first example left-aligns all three lines. The second uses 7,7,3,3.
The first valid explicit tag on a logical line still controls that entire line,
including preceding text. Later valid tags on the same line leave its first
choice intact but set the inheritance for following lines:

~~~ass
{\ta7}First{\ta3}\NSecond\NThird
~~~

This uses 7,3,3. Invalid tags and tags inside \t do not change either choice.
Bare/named \r do not erase logical alignment. Inheritance ends with the dialogue;
a separate event starts from its own normal defaults. Automatic wrapping and
lowercase \n retain their existing logical-line scope, including WrapStyle 2.
Native vertical columns and curved text use the same inherited selections.
See [line alignment](curved-text.md#ta1ta9) for layout details.
