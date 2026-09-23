# Warped text anchor (`\wtan`)

`\wtan1` through `\wtan9` choose the anchor of a `\distort` text block using
the usual ASS numpad grid:

```text
7 8 9   top
4 5 6   middle
1 2 3   bottom
L C R
```

The anchor is computed from the union of the final distorted glyph outlines
on **all visual lines**, plus visible furigana glyphs. For example, `\wtan8`
puts the top center of that whole block at `\pos`, while `\wtan2` puts its
bottom center there. Unequal line widths and glyphs warped beyond their
ordinary line boxes are included. Borders, shadows, and blur do not enlarge
this anchor, consistent with ordinary text alignment using text geometry.

`\wtan` changes only placement of distorted text. Without it, existing `\an`
and `\tan` behavior is unchanged. The first valid `\wtan` in an override
scope is used; `\r` resets it. Values outside 1–9 and `\wtan` inside `\t`
are ignored. The final placement translation happens after deformation, so
it is not itself warped.

The anchor uses outline control bounds after `\distort` and baseline layout.
The existing distortion pass does not deform ruby outlines; ruby is included
at its rendered position. Later 3D/perspective bitmap transforms are outside
these bounds, so combining those transforms with `\wtan` can produce a
different final pixel extent. Raster hinting can also shift edges slightly.

For manual comparison, paste these dialogue texts into a 1280×720 ASS script.
Give each line a different time interval and add a visible guide at
`(640,360)` if desired. The second line is deliberately widest:

```ass
{\an5\pos(640,360)\fs36\wtan8\distort(1,-0.2,1.25,1.2,-0.15,1)}A\NA MUCH LONGER SECOND LINE\NABC
{\an5\pos(640,360)\fs36\wtan5\distort(1,-0.2,1.25,1.2,-0.15,1)}A\NA MUCH LONGER SECOND LINE\NABC
{\an5\pos(640,360)\fs36\wtan2\distort(1,-0.2,1.25,1.2,-0.15,1)}A\NA MUCH LONGER SECOND LINE\NABC
{\an5\pos(640,360)\fs36\wtan7\distort(1,-0.2,1.25,1.2,-0.15,1)}A\NA MUCH LONGER SECOND LINE\NABC
{\an5\pos(640,360)\fs36\wtan9\distort(1,-0.2,1.25,1.2,-0.15,1)}A\NA MUCH LONGER SECOND LINE\NABC
{\an5\pos(640,360)\fs36\wtan1\distort(1,-0.2,1.25,1.2,-0.15,1)}A\NA MUCH LONGER SECOND LINE\NABC
{\an5\pos(640,360)\fs36\wtan3\distort(1,-0.2,1.25,1.2,-0.15,1)}A\NA MUCH LONGER SECOND LINE\NABC
```

For a stronger warp in which a later line can supply the top edge:

```ass
{\an5\pos(640,360)\fs36\wtan8\distort(1,0,1.2,1,-0.1,1)}A\N{\distort(1,-3,1.35,1.2,-0.1,1.1)}LONG SECOND LINE\N{\distort(1,0.1,1.35,1.7,-0.1,1.3)}ABC
```
