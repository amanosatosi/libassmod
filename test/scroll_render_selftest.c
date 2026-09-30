#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ass.h"
#include "ass_render.h"

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); \
} } while (0)

enum { W = 1000, H = 1800 };

static void quiet(int level, const char *fmt, va_list args, void *data)
{
    (void) level; (void) fmt; (void) args; (void) data;
}

static void font(ASS_Library *lib, const char *filename)
{
    FILE *file = fopen(filename, "rb");
    CHECK(file && !fseek(file, 0, SEEK_END));
    long length = ftell(file);
    CHECK(length > 0 && length <= INT_MAX && !fseek(file, 0, SEEK_SET));
    char *data = malloc(length);
    CHECK(data && fread(data, 1, length, file) == (size_t) length);
    fclose(file);
    ass_add_font(lib, filename, data, (int) length);
    free(data);
}

static ASS_Track *track(ASS_Library *lib, const char *body)
{
    const char *prefix =
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 1000\nPlayResY: 1800\n"
        "ScaledBorderAndShadow: yes\n[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,sans-serif,32,&H00FFFFFF,&H0000FFFF,&H0000FF00,&H00FF0000,"
        "0,0,0,0,100,100,0,0,1,2,2,7,100,100,100,1\n"
        "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:12.00,Default,,0,0,0,,";
    size_t length = strlen(prefix) + strlen(body) + 2;
    char *script = malloc(length);
    CHECK(script);
    snprintf(script, length, "%s%s\n", prefix, body);
    ASS_Track *result = ass_read_memory(lib, script, strlen(script), NULL);
    free(script);
    CHECK(result);
    return result;
}

static ASS_Image *render(ASS_Renderer *renderer, ASS_Track *t, long long time)
{
    int change;
    return ass_render_frame(renderer, t, time, &change);
}

static double first_y(ASS_Renderer *renderer, ASS_Track *t, long long time)
{
    CHECK(render(renderer, t, time));
    return renderer->state.text_info.glyphs[0].pos.y / 64.0;
}

static void close_to(double a, double b)
{
    CHECK(fabs(a - b) <= 1.0 / 32);
}

static ASS_ScrollDefinition *definition(ASS_Renderer *renderer, const char *source)
{
    for (ASS_ScrollDefinition *d = renderer->scroll_cache; d; d = d->next)
        if (!strcmp(d->source, source)) return d;
    CHECK(0);
    return NULL;
}

static uint64_t hash_images(ASS_Image *images, int type, uint32_t color, bool filter)
{
    uint64_t hash = 1469598103934665603ULL;
    for (ASS_Image *img = images; img; img = img->next) {
        if (filter && (img->type != type || img->color != color)) continue;
        const int values[] = {img->dst_x, img->dst_y, img->w, img->h, img->type};
        for (size_t i = 0; i < sizeof(values) / sizeof(*values); i++) {
            hash ^= (unsigned) values[i]; hash *= 1099511628211ULL;
        }
        hash ^= img->color; hash *= 1099511628211ULL;
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w; x++) {
                hash ^= img->bitmap[y * img->stride + x]; hash *= 1099511628211ULL;
            }
    }
    return hash;
}

static int count_images(ASS_Image *images, int type, uint32_t color)
{
    int count = 0;
    for (ASS_Image *img = images; img; img = img->next)
        if (img->type == type && img->color == color && img->w && img->h)
            count++;
    return count;
}

static uint64_t hash_rgba_region(ASS_ImageRGBA *images, int min_y)
{
    uint64_t hash = 1469598103934665603ULL;
    for (ASS_ImageRGBA *img = images; img; img = img->next) {
        if (img->dst_y < min_y || !img->w || !img->h) continue;
        const int values[] = {img->dst_x, img->dst_y, img->w, img->h, img->type};
        for (size_t i = 0; i < sizeof(values) / sizeof(*values); i++) {
            hash ^= (unsigned) values[i]; hash *= 1099511628211ULL;
        }
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w * 4; x++) {
                hash ^= img->rgba[y * img->stride + x]; hash *= 1099511628211ULL;
            }
    }
    return hash;
}

static void directions(ASS_Library *lib, ASS_Renderer *renderer)
{
    const char *names[] = {"ue", "shita", "sita"};
    char body[4096], source[256];
    double upward = 0;
    uint64_t downward_hash = 0;
    for (size_t i = 0; i < 3; i++) {
        snprintf(source, sizeof(source), "%s,3000,6", names[i]);
        snprintf(body, sizeof(body), "{\\pos(100,850)\\scroll(%s)}A\\NB\\NC\\ND\\NE\\NF", source);
        ASS_Track *t = track(lib, body);
        double y = first_y(renderer, t, 2999);
        ASS_ScrollDefinition *d = definition(renderer, source);
        double distance = d->cues[0].distance;
        if (!i) upward = distance;
        CHECK(i ? distance < 0 : distance > 0);
        close_to(distance, i ? -upward : upward);
        close_to(first_y(renderer, t, 3000), y);
        close_to(first_y(renderer, t, 3150), y - distance / 2);
        close_to(first_y(renderer, t, 3300), y - distance);
        close_to(first_y(renderer, t, 3301), y - distance);
        if (i == 1) downward_hash = hash_images(render(renderer, t, 3150), 0, 0, false);
        if (i == 2) CHECK(downward_hash == hash_images(render(renderer, t, 3150), 0, 0, false));
        close_to(first_y(renderer, t, 2999), y); // seeking is independent of history
        ass_free_track(t);

        snprintf(source, sizeof(source), "%s,1000,+3,2000|200,-1,3000,+2", names[i]);
        snprintf(body, sizeof(body), "{\\pos(100,850)\\scrollt500\\scroll(%s)}"
                 "A\\N{\\fs70\\fscy150}B\\N{\\fs25\\fscy100}C\\N{\\fs48}D\\N{\\fs32}E", source);
        t = track(lib, body);
        y = first_y(renderer, t, 0);
        d = definition(renderer, source);
        CHECK(d->rows == 5);
        TextInfo *text = &renderer->state.text_info;
        double tops[5];
        for (int row = 0; row < 5; row++)
            tops[row] = text->glyphs[row * 2].pos.y / 64.0 - text->lines[row].asc;
        double sign = i ? -1 : 1;
        CHECK(tops[2] - tops[1] > tops[1] - tops[0]);
        close_to(d->cues[0].distance, sign * (tops[3] - tops[0]));
        close_to(d->cues[1].distance, sign * (tops[2] - tops[3]));
        close_to(d->cues[2].distance, sign * (tops[4] - tops[2]));
        close_to(first_y(renderer, t, 1500), y - sign * (tops[3] - tops[0]));
        close_to(first_y(renderer, t, 2100), y - sign * ((tops[3] + tops[2]) / 2 - tops[0]));
        close_to(first_y(renderer, t, 2200), y - sign * (tops[2] - tops[0]));
        close_to(first_y(renderer, t, 3500), y - sign * (tops[4] - tops[0]));
        ass_free_track(t);

        snprintf(source, sizeof(source), "%s,3000|1000,+4,3500|500,-1", names[i]);
        snprintf(body, sizeof(body), "{\\pos(100,850)\\scroll(%s)}A\\NB\\NC\\ND\\NE", source);
        t = track(lib, body);
        y = first_y(renderer, t, 0);
        d = definition(renderer, source);
        double a = d->cues[0].distance, b = d->cues[1].distance;
        CHECK(a * b < 0);
        close_to(first_y(renderer, t, 3500), y - a / 2);
        close_to(first_y(renderer, t, 3750), y - a * .75 - b * .5);
        close_to(first_y(renderer, t, 4000), y - a - b);
        ass_free_track(t);

        snprintf(source, sizeof(source), "%s,1000,-100,2000,+100,3000,-100,4000,0", names[i]);
        snprintf(body, sizeof(body), "{\\pos(100,850)\\scrollt0\\scroll(%s)}A\\NB\\NC", source);
        t = track(lib, body);
        y = first_y(renderer, t, 0);
        d = definition(renderer, source);
        CHECK(d->cues[0].distance == 0 && d->cues[3].distance == 0);
        close_to(first_y(renderer, t, 1000), y);
        close_to(first_y(renderer, t, 2000), y - d->cues[1].distance);
        close_to(first_y(renderer, t, 3000), y);
        close_to(first_y(renderer, t, 4000), y);
        ass_free_track(t);

        snprintf(source, sizeof(source), "%s,1000,100", names[i]);
        snprintf(body, sizeof(body), "{\\pos(100,850)\\fs40\\scroll(%s)}"
                 "Wrapping counts actual rows. Wrapping counts actual rows. "
                 "Wrapping counts actual rows. Wrapping counts actual rows. "
                 "Wrapping counts actual rows. Wrapping counts actual rows.", source);
        t = track(lib, body);
        y = first_y(renderer, t, 0);
        d = definition(renderer, source);
        CHECK(d->rows > 1 && d->rows == (size_t) renderer->state.text_info.n_lines);
        close_to(first_y(renderer, t, 1300), y - d->cues[0].distance);
        ass_free_track(t);

        snprintf(source, sizeof(source), "%s,1000,1", names[i]);
        snprintf(body, sizeof(body), "{\\pos(100,850)\\scroll(%s)}"
                 "{\\fnserif}A\\N{\\fnFontThatDoesNotExist}B\\N世界", source);
        t = track(lib, body);
        y = first_y(renderer, t, 0);
        d = definition(renderer, source);
        CHECK(d->rows == 3);
        close_to(first_y(renderer, t, 1300), y - d->cues[0].distance);
        ass_free_track(t);

        /* Both signs exit opposite edges of the same stationary viewport. */
        snprintf(body, sizeof(body), "{\\pos(100,850)\\bord0\\shad0\\scrollsl2"
                 "\\scroll(%s,1000,2)}A\\N{\\fs60}B", names[i]);
        t = track(lib, body);
        CHECK(render(renderer, t, 0));
        int visible = 0;
        for (ASS_Image *img = render(renderer, t, 1300); img; img = img->next)
            visible += img->w > 0 && img->h > 0;
        CHECK(!visible);
        ass_free_track(t);

        snprintf(body, sizeof(body), "{\\pos(100,850)\\scroll(%s,1000,1)}A{\\scroll0}B", names[i]);
        t = track(lib, body);
        y = first_y(renderer, t, 0);
        double fixed_y = renderer->state.text_info.glyphs[1].pos.y;
        CHECK(render(renderer, t, 1300));
        CHECK(i ? renderer->state.text_info.glyphs[0].pos.y > y * 64 :
                  renderer->state.text_info.glyphs[0].pos.y < y * 64);
        CHECK(renderer->state.text_info.glyphs[1].pos.y == fixed_y);
        ass_free_track(t);

        snprintf(body, sizeof(body), "{\\pos(100,850)\\scroll(%s,11900|1000,1)}A\\NB", names[i]);
        t = track(lib, body);
        CHECK(render(renderer, t, 11999));
        CHECK(!render(renderer, t, 12000));
        ass_free_track(t);
    }
}

static void timing(ASS_Library *lib, ASS_Renderer *renderer)
{
    const char *rows = "A\\NB\\NC\\ND\\NE\\NF\\NG\\NH\\NI\\NJ\\NK\\NL\\NM\\NN\\NO\\NP";
    char body[4096];
    snprintf(body, sizeof(body), "{\\an7\\pos(100,850)\\scrollt300\\scroll(3000,6,5000,6)}%s", rows);
    ASS_Track *t = track(lib, body);
    double y = first_y(renderer, t, 2999);
    ASS_ScrollDefinition *d = definition(renderer, "3000,6,5000,6");
    CHECK(d->count == 2 && d->rows == 16 && d->cues[0].duration == 300);
    double a = d->cues[0].distance, b = d->cues[1].distance;
    close_to(first_y(renderer, t, 3000), y);
    close_to(first_y(renderer, t, 3150), y - a / 2);
    CHECK(definition(renderer, "3000,6,5000,6") == d);
    close_to(first_y(renderer, t, 3300), y - a);
    close_to(first_y(renderer, t, 3301), y - a);
    close_to(first_y(renderer, t, 4999), y - a);
    close_to(first_y(renderer, t, 5000), y - a);
    close_to(first_y(renderer, t, 5150), y - a - b / 2);
    close_to(first_y(renderer, t, 5300), y - a - b);
    close_to(first_y(renderer, t, 3150), y - a / 2); // backward seeking
    ass_free_track(t);

    const char *tags[] = {
        "\\scroll(3000,6)", "\\scrollt500\\scroll(3000,6)",
        "\\scrollt1\\scroll(3000|1000,6)", "\\scrollt0\\scroll(3000,6)",
        "\\scroll(3000|0,6)", "\\scrollt900\\r\\scroll(3000,6)",
        "\\scroll(3000,6)\\scrollt900", "\\scrollt500\\scrollt-1\\scroll(3000,6)"
    };
    const double progress[] = {.5, .3, .15, 1, 1, .5, .5, .3};
    for (size_t i = 0; i < sizeof(tags) / sizeof(*tags); i++) {
        snprintf(body, sizeof(body), "{\\an7\\pos(100,850)%s}%s", tags[i], rows);
        t = track(lib, body);
        y = first_y(renderer, t, 2999);
        d = renderer->scroll_cache;
        /* Find the active definition by the expected resolved duration. */
        int durations[] = {300, 500, 1, 0, 300, 300, 300, 500};
        const char *source = i == 2 ? "3000|1000,6" : i == 4 ? "3000|0,6" : "3000,6";
        while (d && (strcmp(d->source, source) || d->default_duration != durations[i])) d = d->next;
        CHECK(d);
        close_to(first_y(renderer, t, 3150), y - d->cues[0].distance * progress[i]);
        ass_free_track(t);
    }

    t = track(lib, "{\\pos(100,850)\\scroll(1000|1000,2,1200|500,3)}A\\NB\\NC\\ND\\NE\\NF");
    y = first_y(renderer, t, 0);
    d = definition(renderer, "1000|1000,2,1200|500,3");
    close_to(first_y(renderer, t, 1450), y - d->cues[0].distance * .45 - d->cues[1].distance * .5);
    ass_free_track(t);
}

static void geometry(ASS_Library *lib, ASS_Renderer *renderer)
{
    ASS_Track *t = track(lib,
        "{\\pos(100,850)\\scroll(1000,1,2000,1)}A\\N{\\fs70\\fscy150}B\\N{\\fs25\\fscy100}C");
    double y = first_y(renderer, t, 0);
    TextInfo *text = &renderer->state.text_info;
    CHECK(text->n_lines == 3);
    double top0 = text->glyphs[0].pos.y / 64.0 - text->lines[0].asc;
    double top1 = text->glyphs[2].pos.y / 64.0 - text->lines[1].asc;
    double top2 = text->glyphs[4].pos.y / 64.0 - text->lines[2].asc;
    CHECK(top1 - top0 < top2 - top1);
    close_to(first_y(renderer, t, 1300), y - (top1 - top0));
    close_to(first_y(renderer, t, 2300), y - (top2 - top0));
    ass_free_track(t);

    t = track(lib,
        "{\\pos(100,850)\\scrollsl2\\scroll(1000,1)\\fs25\\t(0,1000,\\fs70)}A\\NB\\NC");
    CHECK(render(renderer, t, 0));
    double before = definition(renderer, "1000,1")->cues[0].distance;
    CHECK(render(renderer, t, 1000));
    CHECK(definition(renderer, "1000,1")->cues[0].distance > before);
    ass_free_track(t);

    t = track(lib,
        "{\\pos(100,850)\\fs40\\scroll(1000,100)}"
        "Wrapping counts actual rows. Wrapping counts actual rows. "
        "Wrapping counts actual rows. Wrapping counts actual rows. "
        "Wrapping counts actual rows. Wrapping counts actual rows.");
    y = first_y(renderer, t, 0);
    ASS_ScrollDefinition *d = definition(renderer, "1000,100");
    CHECK(d->rows > 1 && d->rows == (size_t) renderer->state.text_info.n_lines);
    close_to(first_y(renderer, t, 1300), y - d->cues[0].distance);
    ass_free_track(t);

    t = track(lib, "{\\pos(100,850)\\scroll(1000,1)}{\\fnserif}A\\N{\\fnFontThatDoesNotExist}B\\N世界");
    y = first_y(renderer, t, 0);
    d = definition(renderer, "1000,1");
    CHECK(d->rows == 3);
    close_to(first_y(renderer, t, 1300), y - d->cues[0].distance);
    ass_free_track(t);
}

static void fixed_and_clip(ASS_Library *lib, ASS_Renderer *renderer)
{
    char body[2048];
    const char *directions[] = {"ue", "shita"};
    ASS_Track *t = track(lib,
        "{\\pos(100,850)\\scrollsl2\\scroll(1000,1)}A\\NB\\NC\\ND\\N"
        "{\\scroll0\\1c&H0000FF&\\3c&HFFFF00&\\4c&HFF00FF&}FOOTER\\N"
        /* Re-enabled runs inherit paint overrides. Give all their layers
         * distinct colors so the footer hashes cannot include moving E/F. */
        "{\\scroll(2000,1)\\1c&HFFFFFF&\\3c&H00FF00&\\4c&HFF0000&}E\\NF");
    ASS_Image *images = render(renderer, t, 0);
    CHECK(images);
    uint64_t footer[3];
    int footer_count[3];
    const uint32_t colors[] = {0xff000000, 0x00ffff00, 0xff00ff00};
    for (int type = 0; type < 3; type++) {
        footer_count[type] = count_images(images, type, colors[type]);
        CHECK(footer_count[type] > 0);
        footer[type] = hash_images(images, type, colors[type], true);
    }
    images = render(renderer, t, 2300);
    CHECK(images);
    for (int type = 0; type < 3; type++) {
        CHECK(footer_count[type] == count_images(images, type, colors[type]));
        CHECK(footer[type] == hash_images(images, type, colors[type], true));
    }
    CHECK(renderer->state.text_info.glyphs[0].scroll_id == 1);
    CHECK(renderer->state.text_info.glyphs[8].scroll_id == 0);
    CHECK(definition(renderer, "1000,1")->rows == 4);
    CHECK(definition(renderer, "2000,1")->rows == 2);
    ass_free_track(t);

    t = track(lib, "{\\pos(100,850)\\scroll(1000,1)}A{\\scroll0}B");
    CHECK(render(renderer, t, 0));
    double fixed_y = renderer->state.text_info.glyphs[1].pos.y;
    double moving_y = renderer->state.text_info.glyphs[0].pos.y;
    CHECK(render(renderer, t, 1300));
    CHECK(renderer->state.text_info.glyphs[0].pos.y < moving_y);
    CHECK(renderer->state.text_info.glyphs[1].pos.y == fixed_y);
    ass_free_track(t);

    for (size_t direction = 0; direction < 2; direction++) {
        snprintf(body, sizeof(body),
            "{\\pos(100,850)\\scrollsl2\\scroll(%s,1000,1)}<A|a>\\NB\\NC\\ND\\N"
            "{\\scroll0\\1vc(&H0000FF&,&H00FF00&,&HFF0000&,&HFFFFFF&)}FOOTER", directions[direction]);
        t = track(lib, body);
        int changed;
        ASS_ImageRGBA *rgba = ass_render_frame_rgba(renderer, t, 0, &changed);
        CHECK(rgba);
        int footer_top = (int) (renderer->state.text_info.glyphs[8].pos.y / 64.0 -
                                renderer->state.text_info.lines[4].asc - 10);
        uint64_t fixed_hash = hash_rgba_region(rgba, footer_top);
        ass_free_images_rgba(rgba);
        rgba = ass_render_frame_rgba(renderer, t, 1300, &changed);
        CHECK(rgba && fixed_hash == hash_rgba_region(rgba, footer_top));
        ass_free_images_rgba(rgba);
        ass_free_track(t);
    }

    const char *clips[] = {"", "\\clip(80,840,450,950)",
        "\\iclip(150,860,200,900)", "\\clip(m 80 840 l 450 840 450 950 80 950)",
        "\\iclip(m 150 860 l 200 860 200 900 150 900)"};
    for (size_t direction = 0; direction < 2; direction++) {
        for (size_t i = 0; i < sizeof(clips) / sizeof(*clips); i++) {
            snprintf(body, sizeof(body), "{\\pos(100,850)\\scrollsl2%s\\scroll(%s,1000,1)}A\\NB\\NC\\ND",
                     clips[i], directions[direction]);
            t = track(lib, body);
            CHECK(render(renderer, t, 0));
            double top = renderer->state.text_info.glyphs[0].pos.y / 64.0 - renderer->state.text_info.lines[0].asc;
            double bottom = top + renderer->state.text_info.lines[0].asc + renderer->state.text_info.lines[0].desc +
                renderer->state.text_info.lines[1].asc + renderer->state.text_info.lines[1].desc;
            ASS_Image *clipped = render(renderer, t, 1150);
            CHECK(clipped); // viewport checks must not pass on an empty render
            for (ASS_Image *img = clipped; img; img = img->next) {
                if (!img->w || !img->h) continue;
                CHECK(img->dst_y >= floor(top) && img->dst_y + img->h <= ceil(bottom));
                if (i == 1) CHECK(img->dst_x >= 80 && img->dst_x + img->w <= 450 &&
                                  img->dst_y >= 840 && img->dst_y + img->h <= 950);
            }
            ass_free_track(t);
        }
        snprintf(body, sizeof(body), "{\\pos(100,850)\\clip(80,840,450,950)\\scroll(%s,1000,1)}A\\NB\\NC",
                 directions[direction]);
        t = track(lib, body);
        CHECK(render(renderer, t, 1150));
        ass_free_track(t);
    }

    /* Opposite directions in separate contexts; every footer layer stays fixed
     * and remains visible outside both logical viewports. */
    t = track(lib,
        "{\\pos(100,850)\\scrollsl2\\scroll(shita,1000,1)}A\\NB\\NC\\ND\\N"
        "{\\scroll0\\1c&H0000FF&\\3c&HFFFF00&\\4c&HFF00FF&}FOOTER\\N"
        "{\\scroll(ue,2000,1)\\1c&HFFFFFF&\\3c&H00FF00&\\4c&HFF0000&}E\\NF");
    images = render(renderer, t, 0);
    CHECK(images);
    double a_y = renderer->state.text_info.glyphs[0].pos.y;
    for (int type = 0; type < 3; type++) {
        footer_count[type] = count_images(images, type, colors[type]);
        CHECK(footer_count[type] > 0);
        footer[type] = hash_images(images, type, colors[type], true);
    }
    images = render(renderer, t, 2300);
    CHECK(images && renderer->state.text_info.glyphs[0].pos.y > a_y);
    for (int type = 0; type < 3; type++) {
        CHECK(footer_count[type] == count_images(images, type, colors[type]));
        CHECK(footer[type] == hash_images(images, type, colors[type], true));
    }
    CHECK(definition(renderer, "shita,1000,1")->cues[0].distance < 0);
    CHECK(definition(renderer, "ue,2000,1")->cues[0].distance > 0);
    ass_free_track(t);
}

static void effects(ASS_Library *lib, ASS_Renderer *renderer)
{
    const char *effects[] = {"", "\\bord4\\shad3", "\\2bs5\\3bs8",
        "\\bs3", "\\bs4\\boxp5", "\\alpha&H60&",
        "\\1vc(&H0000FF&,&H00FF00&,&HFF0000&,&HFFFFFF&)",
        "\\1grd(90,&H0000FF&,&HFF0000&)", "\\polc&H00FF00&\\pols3",
        "\\1cyc(1,&H0000FF&,&H00FF00&)\\2cyc(2,&HFF0000&,&HFFFFFF&)",
        "\\u1", "\\img(test-texture)", "\\fscx150\\fscy150",
        "\\org(100,850)\\frx10\\fry10\\frz15", "\\move(100,850,200,900,0,4000)"};
    unsigned char rgba[] = {255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
    CHECK(ass_set_tag_image_rgba(renderer, "test-texture", ASS_TAG_IMAGE_FORMAT_PNG,
                                 2, 2, 8, rgba) == 0);
    char body[2048];
    const char *directions[] = {"ue", "shita"};
    for (size_t direction = 0; direction < 2; direction++) {
        char source[64];
        snprintf(source, sizeof(source), "%s,1000,1", directions[direction]);
        for (size_t i = 0; i < sizeof(effects) / sizeof(*effects); i++) {
            snprintf(body, sizeof(body), "{\\an7\\pos(100,850)%s\\scroll(%s)}<A|a>\\NB\\NC", effects[i], source);
            ASS_Track *t = track(lib, body);
            ASS_Image *images = render(renderer, t, 0);
            CHECK(images);
            CHECK(definition(renderer, source)->rows == 3); // ruby adds no row
            double distance = definition(renderer, source)->cues[0].distance;
            int top[3] = {INT_MAX, INT_MAX, INT_MAX};
            for (ASS_Image *img = images; img; img = img->next)
                if (img->w && img->h && img->dst_y < top[img->type])
                    top[img->type] = img->dst_y;
            CHECK(render(renderer, t, 1150));
            images = render(renderer, t, 1300);
            CHECK(images);
            int moved[3] = {INT_MAX, INT_MAX, INT_MAX};
            for (ASS_Image *img = images; img; img = img->next)
                if (img->w && img->h && img->dst_y < moved[img->type])
                    moved[img->type] = img->dst_y;
            for (int type = 0; type < 3; type++) {
                CHECK((top[type] == INT_MAX) == (moved[type] == INT_MAX));
                if (top[type] != INT_MAX && i + 1 < sizeof(effects) / sizeof(*effects))
                    CHECK(fabs(top[type] - moved[type] - distance) <= 2);
            }
            int changed;
            ASS_ImageRGBA *color = ass_render_frame_rgba(renderer, t, 1150, &changed);
            CHECK(color);
            ass_free_images_rgba(color);
            ass_free_track(t);
        }
        /* BS4 fixed section keeps its own background geometry and pixels. */
        snprintf(body, sizeof(body), "{\\bs4\\pos(100,850)\\scrollsl2\\scroll(%s)}A\\NB\\NC\\N{\\scroll0}FOOTER", source);
        ASS_Track *t = track(lib, body);
        ASS_Image *images = render(renderer, t, 0);
        int bottom = 0, x = 0, y = 0, w = 0, h = 0;
        for (ASS_Image *img = images; img; img = img->next)
            if (img->type == IMAGE_TYPE_SHADOW && img->dst_y + img->h > bottom) {
                bottom = img->dst_y + img->h;
                x = img->dst_x; y = img->dst_y; w = img->w; h = img->h;
            }
        CHECK(w > 0 && h > 0);
        bool found = false;
        for (ASS_Image *img = render(renderer, t, 1300); img; img = img->next)
            if (img->type == IMAGE_TYPE_SHADOW && img->dst_x == x && img->dst_y == y && img->w == w && img->h == h)
                found = true;
        CHECK(found);
        ass_free_track(t);
    }
}

static void invalid_and_lifetime(ASS_Library *lib, ASS_Renderer *renderer)
{
    ASS_Track *plain = track(lib, "{\\pos(100,850)}A\\NB");
    uint64_t reference = hash_images(render(renderer, plain, 0), 0, 0, false);
    CHECK(reference == hash_images(render(renderer, plain, 5000), 0, 0, false));
    const char *invalid[] = {"\\scroll()", "\\scroll(3000)", "\\scroll(3000,6,5000)",
        "\\scroll(abc,6)", "\\scroll(3000,abc)", "\\scroll(3000|-5,6)",
        "\\scroll(3000|abc,6)", "\\scrollt", "\\scrollt-100",
        "\\scrollsl", "\\scrollsl0", "\\scrollsl-3", "\\scroll(1000,1",
        "\\scroll(ue)", "\\scroll(nope,3000,1)", "\\scroll(ue,3000)",
        "\\scroll(ue,3000,1,5000)", "\\scroll(ue,abc,1)", "\\scroll(shita,3000,abc)",
        "\\scroll(sita,3000|abc,1)", "\\scroll(ue,3000|-5,1)", "\\scroll(ue,1,-2147483649)"};
    char body[20000];
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        snprintf(body, sizeof(body), "{\\pos(100,850)%s}A\\NB", invalid[i]);
        ASS_Track *t = track(lib, body);
        CHECK(reference == hash_images(render(renderer, t, 5000), 0, 0, false));
        ass_free_track(t);
    }
    ass_free_track(plain);
    size_t used = snprintf(body, sizeof(body), "{\\pos(100,850)\\scroll(");
    for (int i = 0; i < 1024; i++)
        used += snprintf(body + used, sizeof(body) - used, "%s%d|500,1", i ? "," : "", i);
    snprintf(body + used, sizeof(body) - used, ")}A\\NB\\NC");
    ASS_Track *t = track(lib, body);
    CHECK(render(renderer, t, 0));
    CHECK(renderer->scroll_cache->count == 1024);
    CHECK(render(renderer, t, 2000));
    ass_free_track(t);
    used = snprintf(body, sizeof(body), "{\\pos(100,850)\\scroll(sita,");
    for (int i = 0; i < 1024; i++)
        used += snprintf(body + used, sizeof(body) - used, "%s%d|500,%s",
                         i ? "," : "", i, i % 2 ? "-1" : "+1");
    snprintf(body + used, sizeof(body) - used, ")}A\\NB\\NC");
    t = track(lib, body);
    double y = first_y(renderer, t, 0);
    CHECK(renderer->scroll_cache->count == 1024 &&
          renderer->scroll_cache->direction == ASS_SCROLL_DOWN);
    close_to(first_y(renderer, t, 2000), y);
    ass_free_track(t);
    t = track(lib,
        "{\\pos(100,850)\\scroll(1000,1)\\scroll(2000,2,3000)\\scrollsl-1}A\\NB");
    double initial = first_y(renderer, t, 0);
    double distance = definition(renderer, "1000,1")->cues[0].distance;
    close_to(first_y(renderer, t, 1300), initial - distance);
    ass_free_track(t);
    t = track(lib, "{\\pos(100,850)\\scroll(11900|1000,1)}A\\NB");
    CHECK(render(renderer, t, 11999));
    CHECK(!render(renderer, t, 12000));
    ass_free_track(t);
}

int main(void)
{
    ASS_Library *lib = ass_library_init();
    CHECK(lib);
    ass_set_message_cb(lib, quiet, NULL);
    const char *path = getenv("SCROLL_TEST_FONT");
    font(lib, path ? path : "compare/test/font1.ttf");
    ASS_Renderer *renderer = ass_renderer_init(lib);
    CHECK(renderer);
    ass_set_frame_size(renderer, W, H);
    ass_set_storage_size(renderer, W, H);
    ass_set_fonts(renderer, NULL, "sans-serif", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    directions(lib, renderer);
    timing(lib, renderer);
    geometry(lib, renderer);
    fixed_and_clip(lib, renderer);
    effects(lib, renderer);
    invalid_and_lifetime(lib, renderer);
    ass_renderer_done(renderer);
    ass_library_done(lib);
    return 0;
}
