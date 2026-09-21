#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ass.h"
#include "ass_render.h"

enum { WIDTH = 64, HEIGHT = 48 };

static void msg_cb(int level, const char *fmt, va_list va, void *data)
{
    (void) level; (void) fmt; (void) va; (void) data;
}

static bool expect(bool condition, const char *message)
{
    if (!condition)
        fprintf(stderr, "%s\n", message);
    return condition;
}

static bool test_channel_boundaries(void)
{
    static const uint8_t source[] = {0, 1, 127, 128, 254, 255};
    static const uint8_t destination[] = {255, 254, 128, 127, 1, 0};
    static const uint8_t expected[7][6] = {
        {0, 1, 127, 128, 254, 255},
        {255, 255, 129, 126, 0, 0},
        {255, 255, 255, 255, 255, 255},
        {255, 253, 1, 0, 0, 0},
        {0, 0, 63, 63, 0, 0},
        {255, 255, 192, 192, 255, 255},
        {255, 253, 1, 1, 253, 255},
    };
    bool ok = true;
    for (int mode = ASS_BLEND_NORMAL; mode <= ASS_BLEND_DIFFERENCE; mode++)
        for (int i = 0; i < 6; i++)
            ok &= expect(ass_blend_channel((ASS_BlendMode) mode, source[i],
                                           destination[i]) == expected[mode][i],
                         "blend channel boundary mismatch");
    uint8_t multiplied = ass_blend_channel(ASS_BLEND_MULTIPLY, 128, 127);
    ok &= expect(ass_blend_compose_channel(ASS_BLEND_MULTIPLY, 128, 127, 0) == 127,
                 "zero coverage changed destination");
    ok &= expect(ass_blend_compose_channel(ASS_BLEND_MULTIPLY, 128, 127, 128) ==
                 (uint8_t) ((127 * 128 + multiplied * 129) >> 8),
                 "half coverage used incorrect VSFilterMod weighting");
    ok &= expect(ass_blend_compose_channel(ASS_BLEND_MULTIPLY, 128, 127, 255) ==
                 multiplied, "full coverage did not produce blend result");
    return ok;
}

static char *make_script(const char *tags)
{
    static const char prefix[] =
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 64\nPlayResY: 48\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,20,&H00204080,&H00204080,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1\n"
        "Style: Alt,Arial,20,&H00204080,&H00204080,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:02.00,Default,,0,0,0,,";
    size_t size = sizeof(prefix) + strlen(tags) + 2;
    char *script = malloc(size);
    if (script)
        snprintf(script, size, "%s%s\n", prefix, tags);
    return script;
}

static bool render_hash(ASS_Library *library, ASS_Renderer *renderer,
                        const char *tags, long long time, uint64_t *hash)
{
    char *script = make_script(tags);
    if (!script)
        return false;
    ASS_Track *track = ass_read_memory(library, script, strlen(script), NULL);
    free(script);
    if (!track)
        return false;

    int change = 0;
    ASS_ImageRGBA *images = ass_render_frame_rgba(renderer, track, time, &change);
    uint8_t frame[HEIGHT][WIDTH][4];
    for (int y = 0; y < HEIGHT; y++)
        for (int x = 0; x < WIDTH; x++) {
            frame[y][x][0] = (uint8_t) (20 + x * 3);
            frame[y][x][1] = (uint8_t) (30 + y * 4);
            frame[y][x][2] = (uint8_t) (220 - x * 2);
            frame[y][x][3] = 0;
        }
    bool ok = images && ass_composite_images_bgra(
        images, &frame[0][0][0], WIDTH, HEIGHT, WIDTH * 4) == 0;
    *hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < sizeof(frame); i++) {
        *hash ^= ((uint8_t *) frame)[i];
        *hash *= UINT64_C(1099511628211);
    }
    ass_free_images_rgba(images);
    ass_free_track(track);
    return ok;
}

static bool same_render(ASS_Library *library, ASS_Renderer *renderer,
                        const char *a, const char *b, long long time,
                        const char *message)
{
    uint64_t ah, bh;
    return expect(render_hash(library, renderer, a, time, &ah) &&
                  render_hash(library, renderer, b, time, &bh) && ah == bh,
                  message);
}

static bool different_render(ASS_Library *library, ASS_Renderer *renderer,
                             const char *a, const char *b, long long time,
                             const char *message)
{
    uint64_t ah, bh;
    return expect(render_hash(library, renderer, a, time, &ah) &&
                  render_hash(library, renderer, b, time, &bh) && ah != bh,
                  message);
}

int main(void)
{
    bool ok = test_channel_boundaries();
    ASS_Library *library = ass_library_init();
    if (!library)
        return 1;
    ass_set_message_cb(library, msg_cb, NULL);
    ASS_Renderer *renderer = ass_renderer_init(library);
    if (!renderer) {
        ass_library_done(library);
        return 1;
    }
    ass_set_frame_size(renderer, WIDTH, HEIGHT);
    ass_set_storage_size(renderer, WIDTH, HEIGHT);

    static const char *numeric[] = {
        "\\blend0", "\\blend1", "\\blend2", "\\blend3", "\\blend4",
        "\\blend5", "\\blend6"
    };
    static const char *named[] = {
        "", "\\blend(over)", "\\blend(add)", "\\blend(sub)",
        "\\blend(mult)", "\\blend(scr)", "\\blend(diff)"
    };
    uint64_t mode_hash[7];
    const char *draw_prefix = "{\\an7\\pos(8,8)\\p1\\bord0\\shad0";
    const char *drawing = "}m 0 0 l 32 0 32 24 0 24";
    char first[256], second[256];
    for (int mode = 0; mode <= 6; mode++) {
        snprintf(first, sizeof(first), "%s%s%s", draw_prefix, numeric[mode], drawing);
        snprintf(second, sizeof(second), "%s%s%s", draw_prefix, named[mode], drawing);
        ok &= same_render(library, renderer, first, second, 0,
                          "numeric and named blend syntax differ");
        ok &= render_hash(library, renderer, first, 0, &mode_hash[mode]);
    }
    for (int mode = 1; mode <= 6; mode++)
        ok &= expect(mode_hash[mode] != mode_hash[0],
                     "non-normal mode rendered as normal");
    snprintf(first, sizeof(first), "%s\\blend7%s", draw_prefix, drawing);
    snprintf(second, sizeof(second), "%s\\blend(rsub)%s", draw_prefix, drawing);
    ok &= same_render(library, renderer, first, second, 0,
                      "undocumented reverse-subtract syntax differs");
    snprintf(first, sizeof(first), "%s\\blend8%s", draw_prefix, drawing);
    snprintf(second, sizeof(second), "%s\\blend(isub)%s", draw_prefix, drawing);
    ok &= same_render(library, renderer, first, second, 0,
                      "undocumented inverse-subtract syntax differs");

    snprintf(first, sizeof(first), "%s%s", draw_prefix, drawing);
    snprintf(second, sizeof(second), "%s\\blend9%s", draw_prefix, drawing);
    ok &= same_render(library, renderer, first, second, 0,
                      "out-of-range numeric blend did not fall back to normal");
    snprintf(second, sizeof(second), "%s\\blend(multiply)%s", draw_prefix, drawing);
    ok &= same_render(library, renderer, first, second, 0,
                      "unsupported long blend alias was accepted");
    snprintf(second, sizeof(second), "%s\\blend4\\blend(multiply)%s",
             draw_prefix, drawing);
    ok &= same_render(library, renderer, first, second, 0,
                      "malformed named blend did not reset to normal");
    snprintf(second, sizeof(second), "%s\\blend4\\blend%s", draw_prefix, drawing);
    ok &= same_render(library, renderer, first, second, 0,
                      "empty blend did not reset to normal");
    snprintf(second, sizeof(second), "%s\\blend(MULT)%s", draw_prefix, drawing);
    ok &= same_render(library, renderer, first, second, 0,
                      "named blend parsing was not case-sensitive");
    static const char *invalid_names[] = {
        "overlay", "subtract", "substract", "multiply", "screen", "difference"
    };
    for (size_t i = 0; i < sizeof(invalid_names) / sizeof(invalid_names[0]); i++) {
        snprintf(second, sizeof(second), "%s\\blend(%s)%s",
                 draw_prefix, invalid_names[i], drawing);
        ok &= same_render(library, renderer, first, second, 0,
                          "unsupported long blend name was accepted");
    }

    const char *reset =
        "{\\an7\\pos(4,4)\\p1\\bord0\\shad0\\blend4}m 0 0 l 20 0 20 16 0 16"
        "{\\r\\an7\\pos(30,20)\\p1\\bord0\\shad0}m 0 0 l 20 0 20 16 0 16";
    const char *reset_expected =
        "{\\an7\\pos(4,4)\\p1\\bord0\\shad0\\blend4}m 0 0 l 20 0 20 16 0 16"
        "{\\r\\an7\\pos(30,20)\\p1\\bord0\\shad0\\blend0}m 0 0 l 20 0 20 16 0 16";
    ok &= same_render(library, renderer, reset, reset_expected, 0,
                      "\\r did not reset blend mode");
    const char *style_reset =
        "{\\an7\\pos(4,4)\\p1\\bord0\\shad0\\blend4}m 0 0 l 20 0 20 16 0 16"
        "{\\rAlt\\an7\\pos(30,20)\\p1\\bord0\\shad0}m 0 0 l 20 0 20 16 0 16";
    ok &= same_render(library, renderer, style_reset, reset_expected, 0,
                      "named style reset did not reset blend mode");

    snprintf(first, sizeof(first), "%s\\t(500,1000,\\blend4)%s", draw_prefix, drawing);
    snprintf(second, sizeof(second), "%s\\blend4%s", draw_prefix, drawing);
    ok &= same_render(library, renderer, first, second, 0,
                      "blend inside \\t was not applied immediately");

    const uint8_t texture[] = {
        255, 32, 16, 255, 16, 255, 32, 160,
        32, 16, 255, 96, 240, 160, 32, 255,
    };
    ok &= expect(ass_set_tag_image_rgba(renderer, "texture.png",
                                        ASS_TAG_IMAGE_FORMAT_PNG,
                                        2, 2, 8, texture) == 0,
                 "failed to register blend image texture");
    const char *image_normal =
        "{\\an7\\pos(8,8)\\p1\\bord0\\shad0\\blend0\\1img(texture.png)}"
        "m 0 0 l 32 0 32 24 0 24";
    const char *image_multiply =
        "{\\an7\\pos(8,8)\\p1\\bord0\\shad0\\blend4\\1img(texture.png)}"
        "m 0 0 l 32 0 32 24 0 24";
    ok &= different_render(library, renderer, image_normal, image_multiply, 0,
                            "\\img texture did not use blend mode");

    const char *components_normal =
        "{\\an7\\pos(8,8)\\blend0\\fad(500,500)\\clip(4,4,56,42)"
        "\\p1\\bord4\\shad3\\1bs3\\1bbc&HFF0000&\\2bs2\\2bbc&H00FF00&}"
        "m 0 0 l 32 0 32 24 0 24";
    const char *components_multiply =
        "{\\an7\\pos(8,8)\\blend4\\fad(500,500)\\clip(4,4,56,42)"
        "\\p1\\bord4\\shad3\\1bs3\\1bbc&HFF0000&\\2bs2\\2bbc&H00FF00&}"
        "m 0 0 l 32 0 32 24 0 24";
    ok &= different_render(library, renderer, components_normal,
                            components_multiply, 250,
                            "fill/border/shadow/multi-border blend was ineffective");

    const char *order_abc =
        "{\\an7\\pos(8,8)\\p1\\bord0\\shad0\\1c&H0000FF&}m 0 0 l 28 0 28 24 0 24\n"
        "Dialogue: 1,0:00:00.00,0:00:02.00,Default,,0,0,0,,"
        "{\\an7\\pos(14,12)\\p1\\bord0\\shad0\\1c&H00FF00&\\blend4}m 0 0 l 28 0 28 24 0 24\n"
        "Dialogue: 2,0:00:00.00,0:00:02.00,Default,,0,0,0,,"
        "{\\an7\\pos(20,16)\\p1\\bord0\\shad0\\1c&HFF0000&}m 0 0 l 28 0 28 24 0 24";
    const char *order_bac =
        "{\\an7\\pos(14,12)\\p1\\bord0\\shad0\\1c&H00FF00&\\blend4}m 0 0 l 28 0 28 24 0 24\n"
        "Dialogue: 1,0:00:00.00,0:00:02.00,Default,,0,0,0,,"
        "{\\an7\\pos(8,8)\\p1\\bord0\\shad0\\1c&H0000FF&}m 0 0 l 28 0 28 24 0 24\n"
        "Dialogue: 2,0:00:00.00,0:00:02.00,Default,,0,0,0,,"
        "{\\an7\\pos(20,16)\\p1\\bord0\\shad0\\1c&HFF0000&}m 0 0 l 28 0 28 24 0 24";
    ok &= different_render(library, renderer, order_abc, order_bac, 0,
                            "overlapping event order did not affect blend destination");

    const char *mixed_modes =
        "{\\an7\\pos(4,8)\\p1\\bord0\\shad0\\blend4}m 0 0 l 24 0 24 24 0 24"
        "{\\an7\\pos(34,8)\\p1\\bord0\\shad0\\blend5}m 0 0 l 24 0 24 24 0 24";
    const char *multiply_only =
        "{\\an7\\pos(4,8)\\p1\\bord0\\shad0\\blend4}m 0 0 l 24 0 24 24 0 24"
        "{\\an7\\pos(34,8)\\p1\\bord0\\shad0\\blend4}m 0 0 l 24 0 24 24 0 24";
    ok &= different_render(library, renderer, mixed_modes, multiply_only, 0,
                            "mid-event blend mode change was not preserved");

    ass_renderer_done(renderer);
    ass_library_done(library);
    return ok ? 0 : 1;
}
