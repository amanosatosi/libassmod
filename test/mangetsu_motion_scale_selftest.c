#include <math.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ass.h"

typedef struct {
    int count;
    int min_x, min_y, max_x, max_y;
    uint64_t coverage;
    uint64_t hash;
    long double weight;
    long double weighted_x;
    long double weighted_y;
} RenderSample;

static void msg_cb(int level, const char *fmt, va_list va, void *data)
{
    (void) level;
    (void) fmt;
    (void) va;
    (void) data;
}

static void hash_byte(uint64_t *hash, uint8_t value)
{
    *hash ^= value;
    *hash *= 1099511628211ULL;
}

static void hash_int(uint64_t *hash, int value)
{
    for (int i = 0; i < 4; i++)
        hash_byte(hash, (uint8_t) ((unsigned) value >> (8 * i)));
}

static ASS_Track *read_track(ASS_Library *lib, const char *text)
{
    char script[16384];
    int n = snprintf(
        script, sizeof(script),
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: 1000\n"
        "PlayResY: 700\n"
        "ScaledBorderAndShadow: yes\n"
        "\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,48,&H00FFFFFF,&H00FFFFFF,&H00000000,&H80000000,"
        "0,0,0,0,100,100,0,0,1,0,0,5,10,10,10,1\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:02.00,Default,,0,0,0,,%s\n",
        text);
    if (n < 0 || n >= (int) sizeof(script))
        return NULL;
    return ass_read_memory(lib, script, strlen(script), NULL);
}

static bool render_sample(ASS_Library *lib, ASS_Renderer *renderer,
                          const char *text, long long now,
                          RenderSample *sample)
{
    ASS_Track *track = read_track(lib, text);
    if (!track)
        return false;

    int change = 0;
    ASS_Image *images = ass_render_frame(renderer, track, now, &change);
    (void) change;
    memset(sample, 0, sizeof(*sample));
    sample->hash = 1469598103934665603ULL;

    for (ASS_Image *img = images; img; img = img->next) {
        if (!sample->count) {
            sample->min_x = img->dst_x;
            sample->min_y = img->dst_y;
            sample->max_x = img->dst_x + img->w;
            sample->max_y = img->dst_y + img->h;
        } else {
            sample->min_x = img->dst_x < sample->min_x ? img->dst_x : sample->min_x;
            sample->min_y = img->dst_y < sample->min_y ? img->dst_y : sample->min_y;
            sample->max_x = img->dst_x + img->w > sample->max_x ?
                            img->dst_x + img->w : sample->max_x;
            sample->max_y = img->dst_y + img->h > sample->max_y ?
                            img->dst_y + img->h : sample->max_y;
        }
        sample->count++;
        hash_int(&sample->hash, img->type);
        hash_int(&sample->hash, img->dst_x);
        hash_int(&sample->hash, img->dst_y);
        hash_int(&sample->hash, img->w);
        hash_int(&sample->hash, img->h);
        hash_int(&sample->hash, (int) img->color);

        for (int y = 0; y < img->h; y++) {
            const uint8_t *row = img->bitmap + (ptrdiff_t) y * img->stride;
            for (int x = 0; x < img->w; x++) {
                uint8_t value = row[x];
                hash_byte(&sample->hash, value);
                sample->coverage += value;
                if (img->type == IMAGE_TYPE_CHARACTER && value) {
                    sample->weight += value;
                    sample->weighted_x += value * (img->dst_x + x + 0.5L);
                    sample->weighted_y += value * (img->dst_y + y + 0.5L);
                }
            }
        }
    }

    ass_free_track(track);
    return sample->count > 0 && sample->coverage > 0 && sample->weight > 0;
}

static double center_x(const RenderSample *sample)
{
    return (double) (sample->weighted_x / sample->weight);
}

static double center_y(const RenderSample *sample)
{
    return (double) (sample->weighted_y / sample->weight);
}

static int width(const RenderSample *sample)
{
    return sample->max_x - sample->min_x;
}

static int height(const RenderSample *sample)
{
    return sample->max_y - sample->min_y;
}

static bool same_sample(const RenderSample *a, const RenderSample *b)
{
    return a->count == b->count && a->min_x == b->min_x &&
           a->min_y == b->min_y && a->max_x == b->max_x &&
           a->max_y == b->max_y && a->coverage == b->coverage &&
           a->hash == b->hash;
}

static bool near(double actual, double expected, double tolerance,
                 const char *label)
{
    if (fabs(actual - expected) <= tolerance)
        return true;
    fprintf(stderr, "%s: got %.7f, expected %.7f (+/- %.3f)\n",
            label, actual, expected, tolerance);
    return false;
}

static bool check_motion(ASS_Library *lib, ASS_Renderer *renderer,
                         const char *text, const long long *times,
                         const double *expected_x, int count,
                         const char *label)
{
    RenderSample base;
    if (!render_sample(lib, renderer, text, times[0], &base))
        return false;
    double base_center = center_x(&base);
    bool ok = true;
    for (int i = 1; i < count; i++) {
        RenderSample sample;
        if (!render_sample(lib, renderer, text, times[i], &sample))
            return false;
        double resolved = expected_x[0] + center_x(&sample) - base_center;
        ok &= near(resolved, expected_x[i], 0.6, label);
    }
    return ok;
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
    ass_set_storage_size(renderer, 1000, 700);
    ass_set_frame_size(renderer, 1000, 700);
    ass_set_fonts(renderer, NULL, "sans-serif",
                  ASS_FONTPROVIDER_AUTODETECT, NULL, 1);

    bool ok = true;

    {
        const long long times[] = {0, 250, 500};
        const double xs[] = {500, 350, 200};
        const char *basic =
            "{\\an5\\pos(500,500)\\t(0,500,\\pos(200,500))\\p1}"
            "m -10 -10 l 10 -10 10 10 -10 10";
        ok &= check_motion(lib, renderer, basic, times, xs, 3,
                           "basic animated \\pos");
        RenderSample start, middle;
        ok &= render_sample(lib, renderer, basic, 0, &start);
        ok &= render_sample(lib, renderer, basic, 250, &middle);
        ok &= near(center_y(&middle), center_y(&start), 0.1,
                   "animated \\pos changed constant Y");
    }

    {
        const long long times[] = {0, 1000};
        const double xs[] = {500, 350};
        ok &= check_motion(lib, renderer,
            "{\\an5\\pos(500,500)\\t(\\pos(200,500))\\p1}"
            "m -10 -10 l 10 -10 10 10 -10 10",
            times, xs, 2, "duration-form animated \\pos");
    }

    {
        const long long times[] = {0, 500, 750, 1000};
        const double xs[] = {500, 200, 450, 700};
        ok &= check_motion(lib, renderer,
            "{\\an5\\pos(500,500)"
            "\\t(0,500,\\pos(200,500))"
            "\\t(500,1000,\\pos(700,500))\\p1}"
            "m -10 -10 l 10 -10 10 10 -10 10",
            times, xs, 4, "sequential animated \\pos");
    }

    {
        const long long times[] = {0, 250, 500};
        const double xs[] = {500, 350, 200};
        ok &= check_motion(lib, renderer,
            "{\\an5\\move(500,500,200,500,0,500)\\p1}"
            "m -10 -10 l 10 -10 10 10 -10 10",
            times, xs, 3, "legacy \\move regression");
    }

    {
        const long long times[] = {0, 250, 375, 500, 650, 800};
        const double xs[] = {500, 350, 354.5454545, 359.0909091,
                             529.5454545, 700};
        const char *overlap =
            "{\\an5\\pos(500,500)"
            "\\t(0,500,\\pos(200,500))"
            "\\t(250,800,\\pos(700,500))\\p1}"
            "m -10 -10 l 10 -10 10 10 -10 10";
        ok &= check_motion(lib, renderer, overlap, times, xs, 6,
                           "overlapping \\pos");
        RenderSample start, boundary;
        ok &= render_sample(lib, renderer, overlap, 0, &start);
        ok &= render_sample(lib, renderer, overlap, 500, &boundary);
        ok &= near(center_y(&boundary), center_y(&start), 0.1,
                   "overlapping \\pos changed constant Y");
    }

    {
        const long long times[] = {0, 150, 350, 500, 650, 900};
        const double xs[] = {640, 568, 524.8, 589.9272727,
                             816.2772727, 900};
        const char *three =
            "{\\an5\\pos(640,360)"
            "\\t(0,500,\\pos(400,360))"
            "\\t(150,650,\\pos(700,200))"
            "\\t(350,900,\\pos(900,500))\\p1}"
            "m -10 -10 l 10 -10 10 10 -10 10";
        ok &= check_motion(lib, renderer, three, times, xs, 6,
                           "three overlapping \\pos transforms");

        RenderSample first, out_of_order, repeat;
        ok &= render_sample(lib, renderer, three, 500, &first);
        ok &= render_sample(lib, renderer, three, 875, &out_of_order);
        ok &= render_sample(lib, renderer, three, 500, &repeat);
        if (ok && !same_sample(&first, &repeat)) {
            fprintf(stderr, "animated \\pos depends on render order/frame history\n");
            ok = false;
        }

        const long long edge_times[] = {149, 150, 151, 349, 350, 351,
                                        499, 500, 501, 649, 650, 651};
        double previous = 0.0;
        for (int i = 0; i < (int) (sizeof(edge_times) / sizeof(edge_times[0])); i++) {
            RenderSample edge;
            ok &= render_sample(lib, renderer, three, edge_times[i], &edge);
            double current = center_x(&edge);
            if (i && edge_times[i] - edge_times[i - 1] <= 2 &&
                    fabs(current - previous) > 5.0) {
                fprintf(stderr, "animated \\pos discontinuity near %lld ms\n",
                        edge_times[i]);
                ok = false;
            }
            previous = current;
        }
    }

    {
        const long long times[] = {0, 250, 375, 500, 800};
        double at_375 = 425.0 + (200.0 - 425.0) * 0.25 +
            (700.0 - 425.0) * sqrt(125.0 / 550.0);
        double at_500 = 425.0 + (200.0 - 425.0) +
            (700.0 - 425.0) * sqrt(250.0 / 550.0);
        const double xs[] = {500, 425, at_375, at_500, 700};
        ok &= check_motion(lib, renderer,
            "{\\an5\\pos(500,500)"
            "\\t(0,500,2,\\pos(200,500))"
            "\\t(250,800,0.5,\\pos(700,500))\\p1}"
            "m -10 -10 l 10 -10 10 10 -10 10",
            times, xs, 5, "accelerated overlapping \\pos");
    }

    RenderSample no_scale, scale_50, scale_100, scale_200;
    ok &= render_sample(lib, renderer,
                        "{\\an5\\pos(500,300)\\p1}"
                        "m -20 -10 l 20 -10 20 10 -20 10", 0, &no_scale);
    ok &= render_sample(lib, renderer,
                        "{\\an5\\pos(500,300)\\scale50\\p1}"
                        "m -20 -10 l 20 -10 20 10 -20 10", 0, &scale_50);
    ok &= render_sample(lib, renderer,
                        "{\\an5\\pos(500,300)\\scale100\\p1}"
                        "m -20 -10 l 20 -10 20 10 -20 10", 0, &scale_100);
    ok &= render_sample(lib, renderer,
                        "{\\an5\\pos(500,300)\\scale200\\p1}"
                        "m -20 -10 l 20 -10 20 10 -20 10", 0, &scale_200);
    if (ok && !same_sample(&no_scale, &scale_100)) {
        fprintf(stderr, "\\scale100 is not identical to legacy rendering\n");
        ok = false;
    }
    if (ok && !(width(&scale_50) < width(&scale_100) &&
                width(&scale_100) < width(&scale_200) &&
                height(&scale_50) < height(&scale_100) &&
                height(&scale_100) < height(&scale_200))) {
        fprintf(stderr, "basic \\scale percentages did not resize geometry\n");
        ok = false;
    }
    RenderSample position_reference;
    ok &= render_sample(lib, renderer,
                        "{\\an5\\pos(500,300)\\fsc200\\p1}"
                        "m -20 -10 l 20 -10 20 10 -20 10", 0,
                        &position_reference);
    if (ok && !same_sample(&scale_200, &position_reference)) {
        fprintf(stderr, "\\scale changed absolute \\pos coordinates\n");
        ok = false;
    }

    {
        RenderSample hierarchy, effective;
        ok &= render_sample(lib, renderer,
            "{\\an5\\pos(500,300)\\scale120\\fscx80\\fscy110\\p1}"
            "m -20 -10 l 20 -10 20 10 -20 10", 0, &hierarchy);
        ok &= render_sample(lib, renderer,
            "{\\an5\\pos(500,300)\\fscx96\\fscy132\\p1}"
            "m -20 -10 l 20 -10 20 10 -20 10", 0, &effective);
        if (ok && !same_sample(&hierarchy, &effective)) {
            fprintf(stderr, "\\scale did not multiply the glyph fsc hierarchy\n");
            ok = false;
        }
    }

    {
        RenderSample scaled_border, manual_border, legacy_fsc;
        const char *shape = "m -20 -10 l 20 -10 20 10 -20 10";
        char a[512], b[512], c[512];
        snprintf(a, sizeof(a), "{\\an5\\pos(500,300)\\bord4\\scale200\\p1}%s", shape);
        snprintf(b, sizeof(b), "{\\an5\\pos(500,300)\\bord8\\fsc200\\p1}%s", shape);
        snprintf(c, sizeof(c), "{\\an5\\pos(500,300)\\bord4\\fsc200\\p1}%s", shape);
        ok &= render_sample(lib, renderer, a, 0, &scaled_border);
        ok &= render_sample(lib, renderer, b, 0, &manual_border);
        ok &= render_sample(lib, renderer, c, 0, &legacy_fsc);
        if (ok && !same_sample(&scaled_border, &manual_border)) {
            fprintf(stderr, "\\scale200 did not turn \\bord4 into an effective 8px border\n");
            ok = false;
        }
        if (ok && width(&scaled_border) <= width(&legacy_fsc)) {
            fprintf(stderr, "legacy \\fsc unexpectedly scaled border thickness\n");
            ok = false;
        }
    }

    {
        RenderSample scaled_multi, manual_multi, legacy_multi;
        ok &= render_sample(lib, renderer,
            "{\\an5\\pos(500,300)\\bord2\\2bs3\\3bs4\\scale200\\p1}"
            "m -20 -10 l 20 -10 20 10 -20 10", 0, &scaled_multi);
        ok &= render_sample(lib, renderer,
            "{\\an5\\pos(500,300)\\bord4\\2bs6\\3bs8\\fsc200\\p1}"
            "m -20 -10 l 20 -10 20 10 -20 10", 0, &manual_multi);
        ok &= render_sample(lib, renderer,
            "{\\an5\\pos(500,300)\\bord2\\2bs3\\3bs4\\fsc200\\p1}"
            "m -20 -10 l 20 -10 20 10 -20 10", 0, &legacy_multi);
        if (ok && !same_sample(&scaled_multi, &manual_multi)) {
            fprintf(stderr, "\\scale did not scale multiple border layers\n");
            ok = false;
        }
        if (ok && width(&scaled_multi) <= width(&legacy_multi)) {
            fprintf(stderr, "multi-border effective size did not grow with \\scale\n");
            ok = false;
        }
    }

    {
        RenderSample scaled_local, manual_local;
        ok &= render_sample(lib, renderer,
            "{\\an5\\pos(500,300)\\bord2\\shad3\\blur1\\scale200\\p1}"
            "m -20 -10 l 20 -10 20 10 -20 10", 0, &scaled_local);
        ok &= render_sample(lib, renderer,
            "{\\an5\\pos(500,300)\\bord4\\shad6\\blur2\\fsc200\\p1}"
            "m -20 -10 l 20 -10 20 10 -20 10", 0, &manual_local);
        if (ok && !same_sample(&scaled_local, &manual_local)) {
            fprintf(stderr, "\\scale did not scale shadow/blur local geometry\n");
            ok = false;
        }
    }

    {
        RenderSample box_100, box_200;
        ok &= render_sample(lib, renderer,
            "{\\an5\\pos(500,300)\\bs4\\boxp8\\bbs3\\2bbs4\\scale100}Box",
            0, &box_100);
        ok &= render_sample(lib, renderer,
            "{\\an5\\pos(500,300)\\bs4\\boxp8\\bbs3\\2bbs4\\scale200}Box",
            0, &box_200);
        if (ok && !(width(&box_200) > width(&box_100) &&
                    height(&box_200) > height(&box_100))) {
            fprintf(stderr, "box padding/border geometry did not scale\n");
            ok = false;
        }
    }

    {
        RenderSample animated[3], fixed[3];
        const long long times[] = {0, 500, 1000};
        const int scales[] = {100, 150, 200};
        for (int i = 0; i < 3; i++) {
            char fixed_tag[256];
            ok &= render_sample(lib, renderer,
                "{\\an5\\pos(500,300)\\scale100"
                "\\t(0,1000,\\scale200)\\p1}"
                "m -20 -10 l 20 -10 20 10 -20 10",
                times[i], &animated[i]);
            snprintf(fixed_tag, sizeof(fixed_tag),
                "{\\an5\\pos(500,300)\\scale%d\\p1}"
                "m -20 -10 l 20 -10 20 10 -20 10", scales[i]);
            ok &= render_sample(lib, renderer, fixed_tag, 0, &fixed[i]);
            if (ok && !same_sample(&animated[i], &fixed[i])) {
                fprintf(stderr, "animated \\scale mismatch at %lld ms\n", times[i]);
                ok = false;
            }
        }
    }

    {
        RenderSample malformed;
        ok &= render_sample(lib, renderer,
                            "{\\an5\\pos(500,300)\\scaleoops\\p1}"
                            "m -20 -10 l 20 -10 20 10 -20 10", 0,
                            &malformed);
        if (ok && !same_sample(&no_scale, &malformed)) {
            fprintf(stderr, "malformed \\scale did not fail safely\n");
            ok = false;
        }
    }

    ass_renderer_done(renderer);
    ass_library_done(lib);
    return ok ? 0 : 1;
}
