---
title: RGBA Rendering Guide
---

# RGBA Rendering Guide

Vector gradients (`\vc` / `\1vc`..`\4vc` for four corner colors,
`\va` / `\1va`..`\4va` for corner alpha) and Mangetsu true gradients
(`\1grd`..`\5grd`, `\1gra`..`\5gra`, `\1bgrd`..`\10bgrd`,
`\1bga`..`\10bga`) and fixed-frame color gradients (`\pgrd`, `\1pgrd`..`\5pgrd`, `\1bpgrd`..`\10bpgrd`)
rely on per-pixel color or alpha. They cannot be
reproduced with the legacy `ASS_Image` output. `ASS_Image` nodes are one-byte
alpha masks with a single uniform RGBA color; they do not encode the
interpolation that gradient tags describe.

Use the RGBA rendering API whenever a subtitle contains these gradient tags or
your own feature detection says a frame needs RGBA. Examples like
`\1vc(&H00FFFF&, &HFFFF00&, &HFF00FF&, &H000000&)` draw a four-corner
bilinear fill, while `\1grd(0,&H000000&,&HFFFFFF&)` draws a Mangetsu linear
true color gradient with attached segment bounds and
`\1gra(0,&H00&,&HFF&)` draws a matching opaque-to-transparent true alpha
gradient. `\pgrd(100,100,500,300,0,&H000000&,&HFFFFFF&)` instead samples a
gradient only inside that fixed script-coordinate rectangle.

## API overview

### Structures

```c
typedef struct {
    int w, h;             // overlay bitmap size
    int stride;           // bytes per row (>= w*4)
    uint8_t *rgba;        // premultiplied RGBA8888 (R,G,B,A) rows
    int dst_x, dst_y;     // placement inside the frame
    int type;             // IMAGE_TYPE_CHARACTER/OUTLINE/SHADOW
    ASS_ImageRGBA *next;
} ASS_ImageRGBA;
```

```c
typedef struct {
    ASS_Image *imgs;             // legacy bitmap output (optional)
    ASS_ImageRGBA *imgs_rgba;    // RGBA output list
    int use_rgba;                // 1 if rgba list should be composited
} ASS_RenderResult;
```

Macros:

- `#define LIBASSMOD_FEATURE_RGBA 1`
- `#define LIBASSMOD_FEATURE_TAG_IMAGE 1`

Use these to probe for RGBA/image-fill API availability in host code.

### Key functions

- `ASS_ImageRGBA *ass_render_frame_rgba(ASS_Renderer *priv, ASS_Track *track, long long now, int *detect_change);`
  - Renders the frame directly into RGBA nodes. `ass_frame_needs_rgba(renderer)` will also be set when any event required RGBA.
- `void ass_free_images_rgba(ASS_ImageRGBA *img);`
  - Free the linked list returned by `ass_render_frame_rgba`.
- `ASS_RenderResult ass_render_frame_auto(...)` (or check `ass_frame_needs_rgba`) is the convenience wrapper added in libassmod to fetch both `ASS_Image` and `ASS_ImageRGBA` without losing the legacy behavior.
- `int ass_set_tag_image_rgba(...)` / `void ass_clear_tag_images(...)`
  - Register or clear host-decoded image buffers used by `\img` tags.

## Pixel format & blending

`ASS_ImageRGBA::rgba` is premultiplied RGBA8888: each pixel is `[R, G, B, A]`
where `R`, `G`, `B` are already scaled by alpha
(`src_rgb = raw_rgb * alpha / 255`). `stride` is the byte pitch per row; the
allocated buffer is at least `stride * h`.

Blend each tile in display order. The CPU formula for blending a premultiplied
source onto a destination pixel is:

```c
dst_rgb = src_rgb + dst_rgb * (1 - src_a / 255.0);
dst_a   = src_a + dst_a * (1 - src_a / 255.0);
```

In integer form (0..255):

```c
dst_chan = src_chan + ((dst_chan * (255 - src_a)) / 255);
dst_alpha = src_a + ((dst_alpha * (255 - src_a)) / 255);
```

For OpenGL, use premultiplied blending:

```glsl
glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
```

## Sample render/composite flow

```c
ASS_ImageRGBA *rgba = ass_render_frame_rgba(renderer, track, timestamp, &detect);
if (!rgba) return;

uint8_t *frame = calloc(frame_h * frame_stride, 1); // premultiplied RGB framebuffer
for (ASS_ImageRGBA *node = rgba; node; node = node->next) {
    for (int y = 0; y < node->h; ++y) {
        uint8_t *row = node->rgba + y * node->stride;
        uint8_t *dst = frame + (node->dst_y + y) * frame_stride + node->dst_x * 4;
        for (int x = 0; x < node->w; ++x) {
            uint8_t sa = row[4 * x + 3];
            for (int c = 0; c < 3; ++c)
                dst[c] = row[4 * x + c] + ((dst[c] * (255 - sa)) / 255);
            dst[3] = sa + ((dst[3] * (255 - sa)) / 255);
            dst += 4;
        }
    }
}
ass_free_images_rgba(rgba);
```

## Legacy vs RGBA

- `ass_render_frame()` / `ASS_Image` produce alpha masks painted with one color per node. Gradients require per-pixel hue/alpha variation, so legacy output can only approximate gradients by splitting nodes with `\3c`/`\4c`. Use the RGBA API to see rendered gradient colors.
- Keep legacy behavior for draw masks. Only switch to RGBA when you need gradients, filters, or other per-pixel effects that require full color data.

## Auto-switch suggestions

- Always call `ass_render_frame_rgba` and composite the premultiplied tiles in order; the routine will still populate the legacy `ASS_Image` list, so you can keep both for compatibility.
- Alternatively, inspect the subtitle text for `\1vc`..`\4vc`, `\1va`..`\4va`, `\1grd`..`\5grd`, `\1gra`..`\5gra`, `\1bgrd`..`\10bgrd`, `\1bga`..`\10bga`, `\pgrd`, `\1pgrd`..`\5pgrd`, or `\1bpgrd`..`\10bpgrd` before rendering and only use RGBA when present.
- If your app already calls `ass_render_frame`, use `ass_frame_needs_rgba(renderer)` or the new `ASS_RenderResult` wrapper to decide whether to render again with `ass_render_frame_rgba`.
- For a single-call path, use `ass_render_frame_compat()` and then `ass_render_result_free()` to free any RGBA list. This keeps legacy output intact while enabling gradients when needed.

## Mangetsu color and alpha values

Mangetsu color-valued arguments continue to accept ASS BGR hexadecimal values
such as `&HFFFFFF&` and `&H000000&`. They also accept these case-insensitive
shortcuts: `$white`, `$siro`, and `$shiro` mean `&HFFFFFF&`; `$black` and
`$kuro` mean `&H000000&`. The `$` prefix is required for named colors.

Alpha-valued paint arguments use ASS hexadecimal unless prefixed with `$`.
This includes digit-only arguments: `\alpha50` means hexadecimal `0x50`
(decimal 80), and `\alpha10` means hexadecimal `0x10` (decimal 16).
The `$` prefix selects an explicit decimal byte from 0 through 255:

```ass
Hex alpha (ASS-compatible):
\alpha&H80&
\alpha80
\1aFF

Decimal alpha (Mangetsu extension):
\alpha$128
\1a$255
\3a$100
```

Both spellings use inverse opacity: 0 is completely opaque and 255 is
completely transparent. `$100` means alpha byte 100, never 100% transparency.
Hexadecimal uses ASCII digits and `A`–`F`; `$` decimal also accepts Unicode
[decimal digits](unicode-decimal-digits.md).

The `$` decimal rule applies consistently to scalar alpha, vector corners,
gradient stops, chat preset fields, and these paint arguments inside `\t`.
Hexadecimal parsing retains each feature's earlier compatibility contract:

| Alpha arguments | Hexadecimal contract |
| --- | --- |
| `\alpha`, `\1a`–`\4a`; chat `\buba`, `\bubba`, `\ba` and chat channel alpha | Legacy scalar parsing: accept wider values and tolerated trailing text. Interpolate the full parsed value before converting to a byte. |
| Decoration `\5a` | Complete hexadecimal argument, without a byte limit; use its low byte. |
| Native vector `\va`, `\1va`–`\4va`, `\Nbva` | Validate the used hexadecimal corners, convert to bytes before corner interpolation. Use the first four arguments and ignore extras. |
| Native border `\Nba`; box border `\bba`/`\Nbba`, `\bbva`/`\Nbbva`; gradient `\1gra`–`\5gra`, `\Nbga`, `\bbga`/`\Nbbga`; `\msgleft`/`\msgright` alpha fields | Complete hexadecimal arguments whose parsed values must fit 0–255. Box vectors accept at most four corners; invalid tuples reject the whole update. |

Legacy scalar alpha accepts these ASS spellings:

```ass
\alpha&H1FF&
\1a100
\1a1FF
{\1a0\t(0,1000,\1a1FF)}Two alpha cycles
```

`1FF` is hexadecimal 511. At 250, 500, and 750 milliseconds the example has
alpha bytes 127, 255, and 127 respectively. The renderer interpolates toward
511, then truncates the result to a byte, as required by
[upstream libass PR #637](https://github.com/libass/libass/pull/637).
Scalar spellings such as `FFG` and `&H80&junk` retain their legacy parsed prefix.
Signed input and large hexadecimal values use the existing upstream signed
32-bit saturation and animation conversion rules; they are not newly clamped
to a byte. Structured Mangetsu hexadecimal arguments retain their existing
unsigned 32-bit accumulation, with the strict byte check applied afterward.

In contrast, `\1gra(0,80,FF)` is valid, but `\1gra(0,100,FF)` is rejected:
its hexadecimal 256 stop exceeds the field's byte limit. Native vector
`\va(0,80,100,1FF)` converts its corners to 0, 128, 0, and 255. Decoration
alpha and vector corners keep their earlier conversion behavior rather than
using scalar full-value animation.

```ass
{\1a$100\2a80\3a$200\4aFF}Mixed alpha spellings
{\2bs8\2ba$128\1gra(0,$0,50%,80,$255)}Independent border and fill alpha
{\alpha$0\t(0,1000,\alpha$255)}Fade through a transform
```

All `$` decimal arguments remain strictly within 0–255, with no partial
parsing or hexadecimal fallback. `$256`, `$-1`, `$abc`, `$`, and `$12xyz`
are ignored without changing the current paint. Invalid used vector corners,
gradient stops, or preset fields reject the whole update. An omitted argument
retains the tag's existing reset behavior; invalid decimal input is not a reset.

**Migration:** older Mangetsu subtitles using unprefixed decimal alpha must
add `$`: change `\alpha128` to `\alpha$128` and a decimal alpha stop `128`
to `$128`. There is no compatibility heuristic for unprefixed values.

Standard ASS `\fade(a1,a2,a3,t1,t2,t3,t4)` retains decimal alpha parameters.
SSA `AlphaLevel` style fields also retain their standard decimal meaning.
Ordinary numeric arguments (angles, percentages, timings, coordinates) and
color arguments are unchanged. Extended `\fad` color parameters remain
colors, including their existing `+a` option.

## Gradient tags at a glance

- `\1vc(&HBBGGRR&, &HBBGGRR&, &HBBGGRR&, &HBBGGRR&)` - four corner colors for primary fill.
- `\1va(&HAA&, &HAA&, &HAA&, &HAA&)` - per-corner alpha overrides.
- `\1grd(angle,&HBBGGRR&,&HBBGGRR&)` through `\5grd(...)` - Mangetsu attached linear true gradients with percentage stops.
- `\pgrd(x1,y1,x2,y2,angle,&HBBGGRR&,... )`, `\1pgrd(...)`..`\5pgrd(...)` - independent primary, secondary, outline, shadow and decoration color fields bounded to fixed script-coordinate rectangles. Outside pixels use the target's ordinary color.
- `\1bpgrd(...)` through `\10bpgrd(...)` - positioned native-border colors; `\3pgrd(...)` aliases `\1bpgrd(...)`. Empty parentheses reset only the target's positioned source, preserving unrelated or attached paint. See [positioned gradients](position-gradient.md).
- `\1bgrd(...)` through `\10bgrd(...)` - Mangetsu true-gradient colors for native border layers. `\3grd(...)` is the layer-1 border alias.
- `\1gra(angle,&HAA&,&HAA&)` through `\5gra(...)` - Mangetsu attached linear true alpha gradients with percentage stops. ASS alpha is inverse opacity: `&H00&` is opaque and `&HFF&` is transparent.
- `\1bga(...)` through `\10bga(...)` - Mangetsu true-gradient alpha for native border layers. `\3gra(...)` is the layer-1 border-alpha alias.
- `\1vc`/`\1va` gradients are blended per line box (`\N` or wrapping resets the coordinates).
- Mangetsu true-gradient segments span font changes and `\N`; they are sampled over the final active segment bounds.
- Positioned primary gradients span font changes, wrapping, and `\N`, but their rectangle stays fixed in the subtitle frame and is sampled from final RGBA-tile destination coordinates.
- Uniform color tags like `\c`, `\1c`/`\2c`/`...`, and `\Nbc` reset the matching true-gradient color source. Existing `\vc`/`\bvc` color gradients remain separate; whichever matching color-gradient tag appears later wins.
- Uniform alpha tags like `\alpha`, `\1a`/`\2a`/`...`, and `\Nba` reset the matching true-gradient alpha source. Existing `\va`/`\bva` vector alpha gradients remain separate; whichever matching alpha-gradient tag appears later wins.

## Mangetsu true-gradient transforms

Mangetsu `\grd`, `\bgrd`, `\gra`, and `\bga` tags can be animated inside
normal ASS `\t(...)` forms:

```ass
{\1grd(0,&H000000&,&HFFFFFF&)\t(0,1000,\1grd(90,&H000000&,&HFFFFFF&))}Text
{\1gra(0,&H00&,&HFF&)\t(0,1000,\1gra(90,&H00&,&HFF&))}Text
```

The transform uses the same progress value, timing, and acceleration handling
as other transformable ASS tags. The gradient remains attached to the subtitle
object and keeps one segment across font changes and `\N`.

- Angles interpolate along the shortest path, so `350` to `10` passes through `0`.
- Stop lists with different positions are merged for the current frame; source and target values are sampled at the merged stops and then interpolated.
- Color gradients interpolate RGB components. Alpha gradients interpolate ASS alpha bytes numerically, without converting them to opacity.
- If the source is solid, a temporary source gradient is synthesized from the target stop positions with every stop using the current solid color or alpha value.
- `\3grd(...)`/`\1bgrd(...)` and `\3gra(...)`/`\1bga(...)` remain aliases inside `\t`.
- Animated gradient resets such as `\t(\1grd())`, `\t(\2bgrd0)`, `\t(\1gra())`, and `\t(\2bga0)` are ignored safely. Gradient-to-solid animation through `\t(\c...)` or `\t(\1a...)` is not implemented.

Use the RGBA API to preserve gradient interpolation.

All `\Npgrd` and `\Nbpgrd` tags support positioned-to-positioned `\t` transforms: rectangle
coordinates, angle, stops, and stop positions interpolate with the same rules
as `\1grd`. Attached-to-positioned and positioned-to-attached transforms are
ignored safely because their coordinate systems differ. See
`docs/position-gradient.md` for the full syntax and sampling rules.

## `\img` tags

`\img` / `\1img`..`\4img` are also RGBA-only features in this fork.
They require host-provided decoded image buffers (libassmod does not decode
image files on its own for these tags).

See:

`docs/img-tags-host-api.md`
