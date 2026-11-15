# FreeType 2.15.0 HVF leak on macOS

The [Autotools leak job](https://github.com/amanosatosi/libassmod/actions/runs/43751451511/job/132926087435)
loads Homebrew FreeType 2.15.0. The 2,230 allocations in `generic.ass` come
from 223 `TT_Get_MM_Var` roots underneath `hvf_init_axis_coordinates`.

CoreText supplies a font file and PostScript name. `ass_face_open` closes
its probe and rejected collection faces. The provider closes the face it
opens to read metadata. Rendering faces go into `ASS_Font::faces` and
`ass_font_clear` closes them before renderer teardown destroys the library.
Mangetsu never receives an `FT_MM_Var` from these calls.

FreeType's HVF driver frees its temporary descriptor with `FT_Done_MM_Var`.
`TT_Get_MM_Var` also constructs a separate face-owned `GX_Blend` and its
variation tables. The TrueType driver frees them with `tt_done_blend`.
In FreeType 2.15.0, the HVF driver releases its own buffers and calls the
SFNT finalizer, which does not release `GX_Blend`. The matching fix belongs
in the HVF face finalizer, where it can run before the SFNT tables and
stream are released. This is a dependency resource leak with a valid
diagnostic from `leaks`.

- [FreeType 2.15.0 HVF initialization and finalization](https://github.com/freetype/freetype/blob/VER-2-15-0/src/hvf/hvfobjs.c)
- [FreeType 2.15.0 SFNT finalization](https://github.com/freetype/freetype/blob/VER-2-15-0/src/sfnt/sfobjs.c#L1433)
- [FreeType 2.15.0 variation allocation and teardown](https://github.com/freetype/freetype/blob/VER-2-15-0/src/truetype/ttgxvar.c)

Recheck with the same `check-art-fuzz` job after FreeType includes the
cleanup. Retain the HVF driver so system fonts such as PingFang remain
available, and keep `leaks --atExit` enabled.
