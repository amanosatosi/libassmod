# Font fallback and text decorations

An automatically selected fallback face supplies missing glyphs. It does not
replace the font selected by the ASS style or `\fn` for underline or strikeout
placement and thickness. Both decorations use that selected face's metrics,
scaled at the current ASS font size with its own cell height. A low underline
position is preserved, not normalized to a fallback font's usual position.

For example, with MUA Better selected:

```ass
{\u1}〈ရာဇဝင်ဖြစ်သွားသူ〉
```

The angle brackets can require fallback while the Myanmar text stays in MUA
Better. The underline keeps MUA Better's metric across those boundaries. This
does not flatten real formatting changes: explicit `\fn`, `\fs`, `\u`/`\s`, and
style resets still select the appropriate decoration source, size, and flags.

## Compatibility evidence

The classic [xy-VSFilter CText::CreatePath implementation](https://github.com/pinterf/xy-VSFilter/blob/master/src/subtitles/RTS.cpp)
selects a GDI font built from the ASS style and calls `TextOutW` with that font
still selected, including its per-character spacing path. It does not switch
the selected logical font when Windows supplies a missing glyph.

[Wine's GDI-compatible ExtTextOut implementation](https://github.com/wine-mirror/wine/blob/master/dlls/win32u/font.c)
also provides an inspectable implementation of the ownership rule: after glyph
output, it obtains outline metrics from the selected HDC font and constructs
both underline and strikeout geometry. This supports applying the same source
rule to `\s1`. Wine is corroborating implementation evidence, not a native
Windows/VSFilter pixel oracle; exact Windows-version rasterization and newer
VSFilter variants with alternate glyph engines have not been asserted here.

## Implementation and cache identity

`ASS_Font` is the logical selection, and `faces[0]` is its initial resolved face.
Glyph fallback appends physical faces to that object without replacing the
initial face. Outline construction passes the glyph face and decoration face
separately. Only decorated fallback outline cache misses require sizing the
additional face. Glyph loading, shaping, advances, ascenders/descenders, and
vertical glyph rotation continue using the physical glyph face.

The outline key already contains the owning `ASS_Font`, size, physical face and
glyph indices, bold/italic values, and decoration flags. Its existing font
reference pins all faces, including the immutable decoration source. Thus two
ASS font selections sharing the same physical fallback font cannot share a
decorated outline incorrectly. No extra key field, reference, or cache lifetime
is needed. Bitmap and border caches inherit the outline identity.

Custom-colored decorations use the same constructor through `DECO_ONLY`, so
they retain this ownership rule. `DECO_ROTATE` continues to use the glyph face
for its existing rotation and advance behavior. Missing or invalid decoration
tables retain the previous validation behavior; no fallback metric is invented.

## Regression coverage

`selected-font-decoration-fallback` uses the original fixtures in `test/fonts`;
it does not depend on MUA Better or installed fonts. It tests automatic fallback
in the middle and at both ends, positions and thicknesses, a complete-face
control, both shapers, explicit font/size changes, decoration toggles, resets,
and repeated cache reuse across two authoritative fonts and different sizes.
Public rendering checks measure colored decoration geometry relative to an
independent glyph baseline. Static-library builds additionally assert fixed
26.6 rectangle coordinates, glyph advances, unchanged glyph outlines, both
decorations, and every `DECO_ONLY`/`DECO_ROTATE` combination. Render checks retain
borders/shadows, deterministic `\rnd`, custom decorations, legacy `@` rotation,
native vertical layout, and reverse/repeated frame requests.
