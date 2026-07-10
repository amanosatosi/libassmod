/*
 * Renderer-level regression coverage for transformed BorderStyle=4 boxes.
 * Keep the glyph fill transparent so the captured masks describe only BS4
 * fill/ring geometry and are independent of font outlines.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ass.h"

enum { WIDTH = 640, HEIGHT = 360 };

typedef struct {
    uint8_t pixels[WIDTH * HEIGHT];
    uint64_t coverage;
    int images;
    int outlines;
    int min_x, min_y, max_x, max_y;
} Mask;

static void msg_cb(int level, const char *fmt, va_list va, void *data)
{
    (void) level;
    (void) fmt;
    (void) va;
    (void) data;
}

static ASS_Track *read_track(ASS_Library *lib, const char *text,
                             int border_style)
{
    char script[16384];
    int n = snprintf(
        script, sizeof(script),
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: 640\n"
        "PlayResY: 360\n"
        "ScaledBorderAndShadow: yes\n"
        "\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,48,&H00FFFFFF,&H00FFFFFF,&H00000000,&H000000FF,0,0,0,0,100,100,0,0,%d,0,0,2,10,10,10,1\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:02.00,Default,,0,0,0,,%s\n",
        border_style, text);
    if (n < 0 || n >= (int) sizeof(script))
        return NULL;
    return ass_read_memory(lib, script, strlen(script), NULL);
}

static void mask_reset(Mask *mask)
{
    memset(mask, 0, sizeof(*mask));
    mask->min_x = WIDTH;
    mask->min_y = HEIGHT;
}

static void mask_add(Mask *mask, int x, int y, uint8_t alpha)
{
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT || !alpha)
        return;
    uint8_t *dst = &mask->pixels[y * WIDTH + x];
    if (alpha > *dst)
        *dst = alpha;
    mask->coverage += alpha;
    if (x < mask->min_x) mask->min_x = x;
    if (y < mask->min_y) mask->min_y = y;
    if (x + 1 > mask->max_x) mask->max_x = x + 1;
    if (y + 1 > mask->max_y) mask->max_y = y + 1;
}

static bool render_legacy(ASS_Library *lib, ASS_Renderer *renderer,
                          const char *text, int border_style, long long now,
                          Mask *mask)
{
    ASS_Track *track = read_track(lib, text, border_style);
    if (!track)
        return false;

    int change = 0;
    ASS_Image *images = ass_render_frame(renderer, track, now, &change);
    (void) change;
    mask_reset(mask);
    for (ASS_Image *img = images; img; img = img->next) {
        uint8_t opacity = 255 - (uint8_t) img->color;
        mask->images++;
        if (img->type == IMAGE_TYPE_OUTLINE)
            mask->outlines++;
        for (int y = 0; y < img->h; y++) {
            const uint8_t *row = img->bitmap + y * img->stride;
            for (int x = 0; x < img->w; x++) {
                uint8_t alpha = (uint8_t) ((row[x] * opacity + 127) / 255);
                mask_add(mask, img->dst_x + x, img->dst_y + y, alpha);
            }
        }
    }
    ass_free_track(track);
    return true;
}

static bool render_rgba(ASS_Library *lib, ASS_Renderer *renderer,
                        const char *text, int border_style, long long now,
                        Mask *mask)
{
    ASS_Track *track = read_track(lib, text, border_style);
    if (!track)
        return false;

    int change = 0;
    ASS_ImageRGBA *images = ass_render_frame_rgba(renderer, track, now, &change);
    (void) change;
    mask_reset(mask);
    for (ASS_ImageRGBA *img = images; img; img = img->next) {
        mask->images++;
        if (img->type == IMAGE_TYPE_OUTLINE)
            mask->outlines++;
        for (int y = 0; y < img->h; y++) {
            const uint8_t *row = img->rgba + y * img->stride;
            for (int x = 0; x < img->w; x++)
                mask_add(mask, img->dst_x + x, img->dst_y + y, row[4 * x + 3]);
        }
    }
    ass_free_images_rgba(images);
    ass_free_track(track);
    return true;
}

static bool has_coverage(const Mask *mask)
{
    return mask->coverage > 0 && mask->min_x < mask->max_x &&
        mask->min_y < mask->max_y;
}

static bool same_mask(const Mask *a, const Mask *b)
{
    return a->min_x == b->min_x && a->min_y == b->min_y &&
        a->max_x == b->max_x && a->max_y == b->max_y &&
        !memcmp(a->pixels, b->pixels, sizeof(a->pixels));
}

static bool outside_rect_is_empty(const Mask *mask, int x0, int y0,
                                  int x1, int y1)
{
    for (int y = 0; y < HEIGHT; y++)
        for (int x = 0; x < WIDTH; x++)
            if (mask->pixels[y * WIDTH + x] &&
                (x < x0 || x >= x1 || y < y0 || y >= y1))
                return false;
    return true;
}

static bool inside_rect_is_empty(const Mask *mask, int x0, int y0,
                                 int x1, int y1)
{
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (mask->pixels[y * WIDTH + x])
                return false;
    return true;
}

static bool outside_right_triangle_is_empty(const Mask *mask)
{
    /* \clip(m 0 0 l 640 0 640 360): x * 360 >= y * 640 */
    for (int y = 0; y < HEIGHT; y++)
        for (int x = 0; x < WIDTH; x++)
            if (mask->pixels[y * WIDTH + x] && x * HEIGHT + 1 < y * WIDTH)
                return false;
    return true;
}

static bool corner_is_empty(const Mask *mask)
{
    if (!has_coverage(mask))
        return false;
    int x0 = mask->min_x + 1;
    int y0 = mask->min_y + 1;
    int x1 = mask->max_x - 2;
    int y1 = mask->max_y - 2;
    return !mask->pixels[y0 * WIDTH + x0] ||
        !mask->pixels[y0 * WIDTH + x1] ||
        !mask->pixels[y1 * WIDTH + x0] ||
        !mask->pixels[y1 * WIDTH + x1];
}

int main(void)
{
    ASS_Library *lib = ass_library_init();
    if (!lib)
        return 1;
    ass_set_message_cb(lib, msg_cb, NULL);

    ASS_Renderer *renderer = ass_renderer_init(lib);
    if (!renderer) {
        ass_library_done(lib);
        return 1;
    }
    ass_set_storage_size(renderer, WIDTH, HEIGHT);
    ass_set_frame_size(renderer, WIDTH, HEIGHT);
    ass_set_fonts(renderer, NULL, "sans-serif", ASS_FONTPROVIDER_AUTODETECT,
                  NULL, 1);

    const char *base =
        "{\\an5\\pos(320,180)\\bs4\\boxp20\\4c&H0000FF&\\4a&H00&"
        "\\1a&HFF&\\3a&HFF&}BOX";
    static Mask flat, rotated, clipped, inverse, vector_clip, perspective, distorted;
    static Mask anim_start, anim_mid, anim_end, anim_again, rgba;
    static Mask style_box, line_box, box_on, box_off, rings, canonical, expected;
    bool ok = true;

    ok &= render_legacy(lib, renderer, base, 1, 0, &flat);
    ok &= render_legacy(lib, renderer,
        "{\\an5\\pos(320,180)\\bs4\\boxp20\\frz35\\4c&H0000FF&\\4a&H00&"
        "\\1a&HFF&\\3a&HFF&}BOX", 1, 0, &rotated);
    if (!has_coverage(&flat) || !has_coverage(&rotated) ||
        same_mask(&flat, &rotated) || !corner_is_empty(&rotated)) {
        fprintf(stderr, "rotated BS4 box is not a rotated quadrilateral\n");
        ok = false;
    }

    ok &= render_legacy(lib, renderer,
        "{\\an5\\pos(320,180)\\bs4\\boxp20\\frz35\\clip(300,0,640,360)"
        "\\4c&H0000FF&\\4a&H00&\\1a&HFF&\\3a&HFF&}BOX", 1, 0, &clipped);
    ok &= render_legacy(lib, renderer,
        "{\\an5\\pos(320,180)\\bs4\\boxp20\\frz35\\iclip(300,0,640,360)"
        "\\4c&H0000FF&\\4a&H00&\\1a&HFF&\\3a&HFF&}BOX", 1, 0, &inverse);
    if (!has_coverage(&clipped) || !has_coverage(&inverse) ||
        !outside_rect_is_empty(&clipped, 300, 0, 640, 360) ||
        !inside_rect_is_empty(&inverse, 300, 0, 640, 360)) {
        fprintf(stderr, "BS4 rectangular clipping leaked\n");
        ok = false;
    }

    ok &= render_legacy(lib, renderer,
        "{\\an5\\pos(320,180)\\bs4\\boxp20\\frz25"
        "\\clip(m 0 0 l 640 0 640 360)\\4c&H0000FF&\\4a&H00&"
        "\\1a&HFF&\\3a&HFF&}BOX", 1, 0, &vector_clip);
    if (!has_coverage(&vector_clip) || !outside_right_triangle_is_empty(&vector_clip)) {
        fprintf(stderr, "BS4 vector clipping leaked\n");
        ok = false;
    }

    ok &= render_legacy(lib, renderer,
        "{\\an5\\pos(320,180)\\bs4\\boxp20\\frx35\\fry-20\\frz10"
        "\\4c&H0000FF&\\4a&H00&\\1a&HFF&\\3a&HFF&}BOX", 1, 0, &perspective);
    ok &= render_legacy(lib, renderer,
        "{\\an5\\pos(320,180)\\bs4\\boxp20\\distort(1.5,-0.2,1.7,1.1,-0.2,1)"
        "\\4c&H0000FF&\\4a&H00&\\1a&HFF&\\3a&HFF&}BOX", 1, 0, &distorted);
    if (!has_coverage(&perspective) || !has_coverage(&distorted) ||
        same_mask(&flat, &perspective) || same_mask(&flat, &distorted)) {
        fprintf(stderr, "BS4 perspective/distortion did not change geometry\n");
        ok = false;
    }

    ok &= render_legacy(lib, renderer,
        "{\\an5\\pos(320,180)\\bs4\\boxp12\\bbs4\\bbc&HFFFFFF&"
        "\\2bbs10\\2bbc&H000000&\\4c&H0000FF&\\4a&H00&"
        "\\1a&HFF&\\3a&HFF&\\frz30}BOX", 1, 0, &rings);
    if (!has_coverage(&rings) || rings.outlines < 2) {
        fprintf(stderr, "transformed BS4 box-border rings were not emitted\n");
        ok = false;
    }

    ok &= render_legacy(lib, renderer,
        "{\\an5\\pos(320,180)\\bs4\\boxp20\\t(0,1000,\\frz90\\fscx150\\fscy60)"
        "\\4c&H0000FF&\\4a&H00&\\1a&HFF&\\3a&HFF&}BOX", 1, 0, &anim_start);
    ok &= render_legacy(lib, renderer,
        "{\\an5\\pos(320,180)\\bs4\\boxp20\\t(0,1000,\\frz90\\fscx150\\fscy60)"
        "\\4c&H0000FF&\\4a&H00&\\1a&HFF&\\3a&HFF&}BOX", 1, 500, &anim_mid);
    ok &= render_legacy(lib, renderer,
        "{\\an5\\pos(320,180)\\bs4\\boxp20\\t(0,1000,\\frz90\\fscx150\\fscy60)"
        "\\4c&H0000FF&\\4a&H00&\\1a&HFF&\\3a&HFF&}BOX", 1, 1000, &anim_end);
    ok &= render_legacy(lib, renderer,
        "{\\an5\\pos(320,180)\\bs4\\boxp20\\t(0,1000,\\frz90\\fscx150\\fscy60)"
        "\\4c&H0000FF&\\4a&H00&\\1a&HFF&\\3a&HFF&}BOX", 1, 0, &anim_again);
    if (!has_coverage(&anim_start) || same_mask(&anim_start, &anim_mid) ||
        same_mask(&anim_mid, &anim_end) || !same_mask(&anim_start, &anim_again)) {
        fprintf(stderr, "animated BS4 geometry is stale or non-deterministic\n");
        ok = false;
    }

    ok &= render_legacy(lib, renderer,
        "{\\an5\\pos(320,180)\\bs4\\boxp20\\1a&HFF&\\frz-35}X"
        "{\\1a&H00&\\frz35}AB", 1, 0, &canonical);
    ok &= render_legacy(lib, renderer,
        "{\\an5\\pos(320,180)\\bs4\\boxp20\\1a&HFF&}X"
        "{\\1a&H00&\\frz35}AB", 1, 0, &expected);
    if (!same_mask(&canonical, &expected)) {
        fprintf(stderr, "BS4 did not use the first visible geometry state\n");
        ok = false;
    }

    ok &= render_legacy(lib, renderer,
        "{\\an5\\pos(320,180)\\boxp20\\4c&H0000FF&\\4a&H00&"
        "\\1a&HFF&\\3a&HFF&}BOX", 4, 0, &style_box);
    ok &= render_legacy(lib, renderer, base, 1, 0, &line_box);
    ok &= render_legacy(lib, renderer,
        "{\\an5\\pos(320,180)\\box1\\boxp20\\4c&H0000FF&\\4a&H00&"
        "\\1a&HFF&\\3a&HFF&}BOX", 1, 0, &box_on);
    ok &= render_legacy(lib, renderer,
        "{\\an5\\pos(320,180)\\bs4\\box0\\boxp20\\4c&H0000FF&\\4a&H00&"
        "\\1a&HFF&\\3a&HFF&}BOX", 1, 0, &box_off);
    if (!same_mask(&style_box, &line_box) || !has_coverage(&box_on) ||
        has_coverage(&box_off)) {
        fprintf(stderr, "BS4 style/box override behavior regressed\n");
        ok = false;
    }

    ok &= render_rgba(lib, renderer,
        "{\\an5\\pos(320,180)\\bs4\\boxp20\\frz35\\clip(300,0,640,360)"
        "\\4c&H0000FF&\\4a&H00&\\1a&HFF&\\3a&HFF&}BOX", 1, 0, &rgba);
    if (!has_coverage(&rgba) || rgba.min_x != clipped.min_x ||
        rgba.min_y != clipped.min_y || rgba.max_x != clipped.max_x ||
        rgba.max_y != clipped.max_y || !outside_rect_is_empty(&rgba, 300, 0, 640, 360)) {
        fprintf(stderr, "legacy/RGBA BS4 geometry differs\n");
        ok = false;
    }

    ass_renderer_done(renderer);
    ass_library_done(lib);
    return ok ? 0 : 1;
}
