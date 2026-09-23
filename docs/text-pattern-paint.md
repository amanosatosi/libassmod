# Text pattern paint

Mangetsu can paint text faces and outlines with a cycling palette or a
staggered polka-dot pattern. Both use the existing glyph and border masks;
neither changes the text geometry.

## Cycling

`\cyc(mode,color,...)` and `\1cyc` select the primary face, `\2cyc` and
`\scyc` the secondary face, `\3cyc` the ASS outline, and `\Nbcyc` extra
border N (for example `\2bcyc`). Colors use ASS `&HBBGGRR&` notation. A
palette can contain 1 to 32 colors. Each new tag restarts its layer's loop.

Mode 1 advances per HarfBuzz shaped cluster, so combining marks and Myanmar
syllable components joined by shaping share a color. Mode 2 advances for every
output glyph in the cluster. Spaces and line breaks do not advance either
loop. Ruby inherits the first associated base cluster's selected color for
each ruby span, without consuming another palette entry. When a ruby span
covers multiple base clusters, the first cluster is the fallback because the
current furigana model has no finer reading-to-base alignment.

Example: `{\cyc(1,&H0000FF&,&H00FF00&,&HFF0000&)}ABCDEF`.

Palette tags in `\t()` are currently ignored; static palette switching is
supported. Drawings, shadows, and boxes are outside this paint system.

## Polka dots

`\polc&HBBGGRR&` sets dot color, `\polsN` sets diameter, and `\polspN`
sets center spacing. The unprefixed tags address the primary face; `\1pol*`,
`\2pol*`, and `\3pol*` address the primary face, secondary face, and ASS
outline respectively. Extra borders use `\Nbpc`, `\Nbps`, and `\Nbsp`.

The underlying layer color, including a cycle color, remains the base paint.
Dots are sampled over the layer mask in alternating offset rows and move with
the glyph. Omitted spacing defaults to 2.5 times the diameter. Size zero or a
missing dot color leaves the layer undotted. Numeric and color polka tags use
the ordinary `\t()` interpolation path.

`\zpol` or `\zpol1` propagates the primary polka settings to text faces,
outlines, extra borders, and furigana; `\zpol0` stops propagation. Explicit
values on another layer override the corresponding primary values. Propagation
does not affect shadows, drawings, decorations, or background boxes.

Example: `{\c&HFFFFFF&\polc&HFF80C0&\pols8\polsp20}Polka`.
