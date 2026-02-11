# `\img` Host Integration (RGBA)

This fork supports:

- `\img(path[,xoffset,yoffset])` (alias of `\1img`)
- `\1img(path[,xoffset,yoffset])`
- `\2img(path[,xoffset,yoffset])`
- `\3img(path[,xoffset,yoffset])`
- `\4img(path[,xoffset,yoffset])`

These tags are rendered through the RGBA pipeline (`ASS_ImageRGBA`).

## Important behavior

- libassmod does not decode image files for `\img`.
- The host app must decode image files (or attachments) and register RGBA buffers.
- Accepted source types are limited to:
  - `ASS_TAG_IMAGE_FORMAT_PNG`
  - `ASS_TAG_IMAGE_FORMAT_JPEG`
  - `ASS_TAG_IMAGE_FORMAT_WEBP`
- Registered images larger than script resolution are rejected at render time:
  - `image.width <= PlayResX`
  - `image.height <= PlayResY`

## API

```c
typedef enum {
    ASS_TAG_IMAGE_FORMAT_PNG = 1,
    ASS_TAG_IMAGE_FORMAT_JPEG = 2,
    ASS_TAG_IMAGE_FORMAT_WEBP = 3,
} ASS_TagImageFormat;

int ass_set_tag_image_rgba(ASS_Renderer *priv, const char *path,
                           ASS_TagImageFormat format, int width, int height,
                           int stride, const uint8_t *rgba);

void ass_clear_tag_images(ASS_Renderer *priv);
```

`rgba` is expected to be straight RGBA8888 rows (`R,G,B,A`), with `stride >= width * 4`.
libassmod copies the buffer internally.

## Path matching

Lookup supports:

- exact path key
- subtitle-relative path key (relative to `track->name` directory)
- quoted path forms are normalized (e.g. `"C:\path\img.png"` and `C:\path\img.png` map to the same key)

Path separators are normalized (`\` and `/` are treated equivalently).

Practical recommendation:

1. Register the attachment/basename key (for muxed attachment names).
2. Register resolved absolute or subtitle-relative keys used in script tags.

## Suggested host workflow

1. Parse subtitle text for `\img`/`\1img`..`\4img` paths.
2. Resolve each path:
   - attachment name
   - absolute file path
   - subtitle-relative file path
3. Decode source image (png/jpeg/webp) to RGBA.
4. Call `ass_set_tag_image_rgba(renderer, key, format, w, h, stride, rgba)`.
5. Render with `ass_render_frame_rgba(...)` (or auto API and `ass_frame_needs_rgba`).
6. On track/renderer reload, call `ass_clear_tag_images(renderer)`.

## Integration patterns

### Subtitle editor preview (Aegisub-style)

1. Parse active script and collect `\img` paths.
2. Resolve from:
   - script attachment table first
   - script directory second
3. Decode to RGBA and register once per preview reload.
4. Render preview with `ass_render_frame_rgba`.

### Encoding/transcoding pipeline

1. Before frame rendering, scan the loaded track once for `\img` paths.
2. Resolve files from mux attachments or sidecar resources.
3. Decode to RGBA and register with `ass_set_tag_image_rgba`.
4. Render all frames normally; refresh registration only when track/resources change.

## Tag interaction notes (VSFilterMod-style intent)

- `\1img`..`\4img` enable image fill for the corresponding layer.
- `\img` is equivalent to `\1img`.
- `\c`/`\1c`/`\2c`/`\3c`/`\4c` disable image fill on that layer.
- `\1vc`/`\2vc`/`\3vc`/`\4vc` and `\1va`/`\2va`/`\3va`/`\4va` disable image fill on the affected layer(s).
- `\1a`..`\4a` and `\alpha` still affect effective opacity of image fill.

## Minimal registration example

```c
// decoded_rgba: width * height RGBA8888 buffer
int ok = ass_set_tag_image_rgba(renderer,
                                "textures/noise.webp",
                                ASS_TAG_IMAGE_FORMAT_WEBP,
                                width, height, width * 4,
                                decoded_rgba);
if (ok < 0) {
    // invalid format/size/args or allocation failure
}
```
