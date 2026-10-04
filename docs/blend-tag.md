# VSFilterMod-compatible `\blend`

## Existing Mangetsu/Toshi-ban path

Mangetsu's custom RGBA renderer produces an ordered list of premultiplied
RGBA8888 component tiles.  It uses that path for `\img`, gradients, color
fades, and other pixels which cannot be represented by the legacy one-color
`ASS_Image` API.  Events are sorted by layer before their tile lists are
concatenated, while fill, border, shadow, box, drawing, multi-border, and
texture tiles retain renderer order within each event.

Aegisub Toshi-ban's Mangetsu provider asks for `ass_render_frame_auto()` and
currently composites those tiles, in order, into the video preview's BGRA8
buffer.  The destination at each tile therefore already contains the video
and every earlier subtitle tile.  This is the correct place for `\blend`:
Mangetsu supplies the blend metadata and exact straight source RGB, and a
small optional compositor operates directly on that existing Aegisub buffer.
The legacy `ASS_Image` ABI and the premultiplied `ASS_ImageRGBA` layout remain
unchanged.

## Syntax

The documented modes are:

| Tag | Mode |
| --- | --- |
| `\blend0` | normal |
| `\blend1` | overlay |
| `\blend2` | add |
| `\blend3` | substract (VSFilterMod spelling) |
| `\blend4` | multiply |
| `\blend5` | screen |
| `\blend6` | difference |

The exact case-sensitive named forms accepted by VSFilterMod are `over`,
`add`, `sub`, `mult`, `scr`, and `diff`, for example `\blend(mult)` and
`\blend(scr)`.  For source compatibility it also accepts the reference
renderer's undocumented modes `\blend7`/`\blend(rsub)` (source minus
destination) and `\blend8`/`\blend(isub)` (source minus inverted destination).
Longer names such as `overlay` or `multiply` are not aliases.

An empty or malformed form resets to the original normal mode.  Numeric modes
outside 0 through 8 are retained by the parser but composite as normal, as in
VSFilterMod.  Both `\r` and `\rStyleName` reset the mode to normal.  A blend
tag inside `\t()` is applied immediately and discretely; it is not
interpolated and is not gated by the transform time.

```ass
{\blend4\1c&H80C0FF&}Multiply against the current preview
{\blend(scr)\1img(example.png)\p1\bord0\shad0}m 0 0 l 200 0 200 100 0 100
```

## Destination-aware rendering

Non-normal modes require the RGB pixels already under the current subtitle.
Toshi-ban uses `ass_composite_images_bgra()` to composite Mangetsu's ordered
RGBA tiles in place against its current BGRA8 preview frame.  The operation is
therefore video, then prior subtitle content, then the current tile.

Generic users of `ass_render_frame()` and `ass_render_frame_rgba()` keep their
existing contracts.  The latter still returns ordinary premultiplied RGBA
tiles; if a host does not call the destination-aware compositor, a
non-normal tile safely displays using normal alpha composition rather than
pretending it has access to a video backdrop.

## Reference arithmetic

All operations use ordinary encoded 8-bit channels.  With straight source
`s`, current destination `d`, and `div255(x) = (x + 1 + ((x + 1) >> 8)) >> 8`:

- overlay: `d < 128 ? 2*div255(s*d) : 255-2*div255((255-s)*(255-d))`
- add: `min(s+d, 255)`
- substract (`\blend3` / `\blend(sub)`): `max(s-d, 0)`
- multiply: `div255(s*d)`
- screen: `255-div255((255-s)*(255-d))`
- difference: `abs(s-d)`

Mode 3 follows `blendMixColor()` in the VSFilterMod
[reference revision 5e18b49](https://github.com/computerfan/VSFilterMod/blob/5e18b49cccdc82dc8329ddaa632a3cd32dada737/src/subtitles/Rasterizer.cpp#L916-L921):
`color` is the subtitle source and `dst` is the current destination.  Both
the scalar and SSE2 pixel mixers use that clamped source-minus-destination
result.  Later VSFilterMod revisions can differ in operand order.  For
example, source RGB `(219,151,34)` over destination `(11,9,11)` produces
`(208,142,23)` before coverage weighting.

The glyph/image mask, ASS alpha, fades, clipping, and texture alpha first
produce the tile coverage `a`.  RGB is then written with VSFilterMod's SSE2
weighting: `(d*(256-a) + blend(s,d)*(a+1)) >> 8`.  Destination alpha is not
used by the video-preview contract.
