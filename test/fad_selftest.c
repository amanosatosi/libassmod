#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ass.h"

typedef struct {
    uint64_t alpha;
    uint64_t red;
    uint64_t green;
    uint64_t blue;
    int images;
} RgbaStats;

static void msg_cb(int level, const char *fmt, va_list va, void *data)
{
    (void) level;
    (void) fmt;
    (void) va;
    (void) data;
}

static char *make_script(const char *text)
{
    const char *prefix =
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
        "Style: Default,Arial,44,&H000000FF,&H0000FF00,&H00FF0000,&H00000000,0,0,0,0,100,100,0,0,1,3,4,5,10,10,10,1\n"
        "Style: Box,Arial,44,&H000000FF,&H0000FF00,&H00FF0000,&H00000000,0,0,0,0,100,100,0,0,4,3,0,5,10,10,10,1\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:02.00,Default,,0,0,0,,{\\pos(320,180)}";
    size_t len = strlen(prefix) + strlen(text) + 2;
    char *script = malloc(len);
    if (!script)
        return NULL;
    snprintf(script, len, "%s%s\n", prefix, text);
    return script;
}

static bool render_stats(ASS_Library *lib, ASS_Renderer *renderer,
                         const char *text, long long time, RgbaStats *stats)
{
    memset(stats, 0, sizeof(*stats));
    char *script = make_script(text);
    if (!script)
        return false;

    ASS_Track *track = ass_read_memory(lib, script, strlen(script), NULL);
    free(script);
    if (!track)
        return false;

    int change = 0;
    ASS_ImageRGBA *img = ass_render_frame_rgba(renderer, track, time, &change);
    (void) change;

    for (ASS_ImageRGBA *cur = img; cur; cur = cur->next) {
        stats->images++;
        for (int y = 0; y < cur->h; y++) {
            const uint8_t *row = cur->rgba + y * cur->stride;
            for (int x = 0; x < cur->w; x++) {
                uint8_t a = row[4 * x + 3];
                stats->red += row[4 * x + 0];
                stats->green += row[4 * x + 1];
                stats->blue += row[4 * x + 2];
                stats->alpha += a;
            }
        }
    }

    ass_free_images_rgba(img);
    ass_free_track(track);
    return true;
}

static bool mostly_white(const RgbaStats *stats)
{
    return stats->alpha > 0 &&
           stats->red > stats->alpha * 3 / 4 &&
           stats->green > stats->alpha * 3 / 4 &&
           stats->blue > stats->alpha * 3 / 4;
}

static bool mostly_black(const RgbaStats *stats)
{
    return stats->alpha > 0 &&
           stats->red < stats->alpha / 8 &&
           stats->green < stats->alpha / 8 &&
           stats->blue < stats->alpha / 8;
}

static bool expect(bool cond, const char *msg)
{
    if (!cond)
        fprintf(stderr, "%s\n", msg);
    return cond;
}

int main(void)
{
    ASS_Library *lib = ass_library_init();
    if (!lib) {
        fprintf(stderr, "failed to init ass library\n");
        return 1;
    }
    ass_set_message_cb(lib, msg_cb, NULL);

    ASS_Renderer *renderer = ass_renderer_init(lib);
    if (!renderer) {
        fprintf(stderr, "failed to init renderer\n");
        ass_library_done(lib);
        return 1;
    }

    ass_set_storage_size(renderer, 640, 360);
    ass_set_frame_size(renderer, 640, 360);
    ass_set_fonts(renderer, NULL, "sans-serif",
                  ASS_FONTPROVIDER_AUTODETECT, NULL, 1);

    bool ok = true;
    RgbaStats classic_start, normal, color_start, color_alpha_start;
    RgbaStats empty_start, empty_end, black_end, transform_start;
    RgbaStats mb_enabled, mb_disabled, gradient_start, box_enabled, box_disabled;

    ok &= render_stats(lib, renderer, "{\\fad(300,300)}Classic", 0, &classic_start);
    ok &= render_stats(lib, renderer, "Classic", 1000, &normal);
    ok &= expect(classic_start.alpha < normal.alpha / 8,
                 "classic \\fad did not keep alpha fade-in behavior");

    ok &= render_stats(lib, renderer,
                       "{\\fad(300,300,&HFFFFFF&,&H000000&)}Color",
                       0, &color_start);
    ok &= expect(color_start.alpha > normal.alpha / 2 &&
                 mostly_white(&color_start),
                 "extended color-only fade-in was not visible white");

    ok &= render_stats(lib, renderer,
                       "{\\fad(300,300,&HFFFFFF&+a,&H000000&+a)}ColorAlpha",
                       0, &color_alpha_start);
    ok &= expect(color_alpha_start.alpha < color_start.alpha / 8,
                 "extended +a did not combine color fade with alpha fade");

    ok &= render_stats(lib, renderer,
                       "{\\fad(300,300,,&H000000&)}EmptyStart",
                       0, &empty_start);
    ok &= expect(empty_start.alpha < normal.alpha / 8,
                 "empty start color did not keep classic alpha fade-in");

    ok &= render_stats(lib, renderer,
                       "{\\fad(300,300,&HFFFFFF&,)}EmptyEnd",
                       1999, &empty_end);
    ok &= expect(empty_end.alpha < normal.alpha / 8,
                 "empty end color did not keep classic alpha fade-out");

    ok &= render_stats(lib, renderer,
                       "{\\fad(300,300,&HFFFFFF&,&H000000&)}BlackEnd",
                       1999, &black_end);
    ok &= expect(black_end.alpha > normal.alpha / 2 &&
                 mostly_black(&black_end),
                 "extended color-only fade-out was not visible black");

    ok &= render_stats(lib, renderer,
                       "{\\1c&H0000FF&\\t(0,500,\\1c&H00FF00&)\\fad(300,300,&HFFFFFF&,&H000000&)}Transform",
                       0, &transform_start);
    ok &= expect(mostly_white(&transform_start),
                 "\\t color was not followed by final \\fad color pass");

    ok &= render_stats(lib, renderer,
                       "{\\bord2\\2bs7\\2bc&H0000FF&\\fad(300,300,&HFFFFFF&,&H000000&)}MB",
                       0, &mb_enabled);
    ok &= render_stats(lib, renderer,
                       "{\\bord2\\2bc&H0000FF&\\fad(300,300,&HFFFFFF&,&H000000&)}MB",
                       0, &mb_disabled);
    ok &= expect(mostly_white(&mb_enabled) &&
                 mb_enabled.alpha > mb_disabled.alpha,
                 "enabled native border layer did not participate, or disabled layer appeared");

    ok &= render_stats(lib, renderer,
                       "{\\bord4\\3vc(&H0000FF&,&H00FF00&,&H0000FF&,&H00FF00&)\\fad(300,300,&HFFFFFF&,&H000000&)}Grad",
                       0, &gradient_start);
    ok &= expect(mostly_white(&gradient_start),
                 "gradient output was not color-faded after sampling");

    ok &= render_stats(lib, renderer,
                       "{\\rBox\\box1\\fad(300,300,&HFFFFFF&,&H000000&)}Box",
                       0, &box_enabled);
    ok &= render_stats(lib, renderer,
                       "{\\fad(300,300,&HFFFFFF&,&H000000&)}Box",
                       0, &box_disabled);
    ok &= expect(mostly_white(&box_enabled) &&
                 box_enabled.alpha > box_disabled.alpha,
                 "box color did not participate only when box mode was enabled");

    ass_renderer_done(renderer);
    ass_library_done(lib);
    return ok ? 0 : 1;
}
