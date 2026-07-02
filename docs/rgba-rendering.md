---
title: RGBA Rendering Guide
---

# RGBA Rendering Guide

Vector gradients (`\1vc`..`\4vc` for four corner colors,
`\1va`..`\4va` for corner alpha) and Mangetsu true gradients
(`\1grd`..`\5grd`, `\1bgrd`..`\10bgrd`) rely on per-pixel color or alpha.
They cannot be reproduced with the legacy `ASS_Image` output. `ASS_Image`
nodes are one-byte alpha masks with a single uniform RGBA color; they do not
encode the interpolation that gradient tags describe.

Use the RGBA rendering API whenever a subtitle contains these gradient tags or
your own feature detection says a frame needs RGBA. Examples like
`\1vc(&H00FFFF&, &HFFFF00&, &HFF00FF&, &H000000&)` draw a four-corner
bilinear fill, while `\1grd(0,&H000000&,&HFFFFFF&)` draws a Mangetsu linear
true gradient with attached segment bounds.

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
- Alternatively, inspect the subtitle text for `\1vc`..`\4vc`, `\1va`..`\4va`, `\1grd`..`\5grd`, or `\1bgrd`..`\10bgrd` before rendering and only use RGBA when present.
- If your app already calls `ass_render_frame`, use `ass_frame_needs_rgba(renderer)` or the new `ASS_RenderResult` wrapper to decide whether to render again with `ass_render_frame_rgba`.
- For a single-call path, use `ass_render_frame_compat()` and then `ass_render_result_free()` to free any RGBA list. This keeps legacy output intact while enabling gradients when needed.

## Gradient tags at a glance

- `\1vc(&HBBGGRR&, &HBBGGRR&, &HBBGGRR&, &HBBGGRR&)` - four corner colors for primary fill.
- `\1va(&HAA&, &HAA&, &HAA&, &HAA&)` - per-corner alpha overrides.
- `\1grd(angle,&HBBGGRR&,&HBBGGRR&)` through `\5grd(...)` - Mangetsu attached linear true gradients with percentage stops.
- `\1bgrd(...)` through `\10bgrd(...)` - Mangetsu true-gradient colors for native border layers. `\3grd(...)` is the layer-1 border alias.
- `\1vc`/`\1va` gradients are blended per line box (`\N` or wrapping resets the coordinates).
- Mangetsu true-gradient segments span font changes and `\N`; they are sampled over the final active segment bounds.
- Uniform color tags like `\c`, `\1c`/`\2c`/`...`, and `\Nbc` reset the matching true-gradient color source. Existing `\vc`/`\bvc` color gradients remain separate; whichever matching color-gradient tag appears later wins.

Use the RGBA API to preserve gradient interpolation.

## `\img` tags

`\img` / `\1img`..`\4img` are also RGBA-only features in this fork.
They require host-provided decoded image buffers (libassmod does not decode
image files on its own for these tags).

See:

`docs/img-tags-host-api.md`
