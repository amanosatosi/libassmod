These four tiny TrueType fixtures are original rectangle fonts, distributed
under the repository's ISC license. No data was copied from MUA Better or any
other font. Regenerate them with `python3 generate-decoration-fonts.py`; the
generator uses only the Python standard library and is not needed by CI.

| Family | UPEM | Cell height | Underline position/thickness | Strikeout position/thickness | Coverage |
| --- | ---: | ---: | --- | --- | --- |
| Deco Primary A | 2048 | 2048 | -900 / 100 | 600 / 128 | space, A, C |
| Deco Primary B | 1000 | 1000 | -150 / 240 | 400 / 200 | space, A, C |
| Deco Fallback | 1000 | 1500 | -100 / 400 | 40 / 300 | space, B, 〈, 〉 |
| Deco Complete | 2048 | 2048 | -900 / 100 | 600 / 128 | all of the above |

The different fallback cell height also detects scaling decoration metrics with
the glyph face's scale. Advances and rectangle ink are normalized to the cell
height, giving predictable probes for both simple and complex shaping.

The same generator also creates two original ruby clearance fixtures (ISC):
`furi-tight-ascent.ttf` has ascent/descent 250/750 but ink from 0 to 500;
`furi-tight-descent.ttf` has ascent/descent 900/100 but ink from -400 to 100.
Both use UPEM/cell height 1000 and cover space, W and M. The visible rectangle
therefore exceeds one nominal vertical metric. The furigana geometry tests run
with each family explicitly selected, covering both annotation sides.
