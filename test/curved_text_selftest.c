/* Renderer-level regressions for Mangetsu shaped-cluster text on a path. */

#include <math.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ass.h"

enum { WIDTH = 960, HEIGHT = 540 };

typedef struct {
    int images;
    int min_x, min_y, max_x, max_y;
    uint64_t coverage;
    uint64_t hash;
    long double weight;
    long double weighted_x;
    long double weighted_y;
} Sample;

static void msg_cb(int level, const char *fmt, va_list va, void *data)
{
    (void) level;
    (void) fmt;
    (void) va;
    (void) data;
}

static void hash_byte(uint64_t *hash, uint8_t byte)
{
    *hash ^= byte;
    *hash *= 1099511628211ULL;
}

static void hash_int(uint64_t *hash, int value)
{
    for (int i = 0; i < 4; i++)
        hash_byte(hash, (uint8_t) ((unsigned) value >> (8 * i)));
}

static ASS_Track *read_track(ASS_Library *lib, const char *text)
{
    char script[32768];
    int n = snprintf(script, sizeof(script),
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: 960\n"
        "PlayResY: 540\n"
        "ScaledBorderAndShadow: yes\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,sans-serif,44,&H00FFFFFF,&H00FFFFFF,&H00000000,&H80000000,"
        "0,0,0,0,100,100,0,0,1,0,0,5,20,20,20,1\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:03.00,Default,,0,0,0,,%s\n",
        text);
    if (n < 0 || n >= (int) sizeof(script))
        return NULL;
    return ass_read_memory(lib, script, strlen(script), NULL);
}

static bool render_sample(ASS_Library *lib, ASS_Renderer *renderer,
                          const char *text, long long now, Sample *sample)
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
        if (!sample->images) {
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
        sample->images++;
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
    return sample->images > 0 && sample->coverage > 0 && sample->weight > 0;
}

static double center_x(const Sample *sample)
{
    return (double) (sample->weighted_x / sample->weight);
}

static double center_y(const Sample *sample)
{
    return (double) (sample->weighted_y / sample->weight);
}

static double combined_center_x(const Sample *a, const Sample *b)
{
    return (double) ((a->weighted_x + b->weighted_x) /
                     (a->weight + b->weight));
}

static int width(const Sample *sample)
{
    return sample->max_x - sample->min_x;
}

static int height(const Sample *sample)
{
    return sample->max_y - sample->min_y;
}

static bool same_sample(const Sample *a, const Sample *b)
{
    return a->images == b->images && a->min_x == b->min_x &&
           a->min_y == b->min_y && a->max_x == b->max_x &&
           a->max_y == b->max_y && a->coverage == b->coverage &&
           a->hash == b->hash;
}

static bool near(double actual, double expected, double tolerance,
                 const char *label)
{
    if (fabs(actual - expected) <= tolerance)
        return true;
    fprintf(stderr, "%s: got %.3f, expected %.3f (+/- %.3f)\n",
            label, actual, expected, tolerance);
    return false;
}

static bool expect(bool condition, const char *label)
{
    if (!condition)
        fprintf(stderr, "%s\n", label);
    return condition;
}

typedef struct {
    uint16_t *primary, *secondary;
    uint64_t active, waiting;
    long double active_x, active_y, waiting_x, waiting_y;
} KaraokeFrame;

static void free_karaoke_frame(KaraokeFrame *frame)
{
    free(frame->primary);
    *frame = (KaraokeFrame) {0};
}

/* Compare paint coverage, independent of how the renderer tiles ASS_Image.
 * Fixtures use opaque red primary and blue secondary, without border/shadow. */
static bool karaoke_frame(ASS_Library *lib, ASS_Renderer *renderer,
                          const char *text, long long now, KaraokeFrame *frame,
                          bool rgba)
{
    *frame = (KaraokeFrame) {0};
    ASS_Track *track = read_track(lib, text);
    if (!track)
        return false;
    frame->primary = calloc(2 * WIDTH * HEIGHT, sizeof(*frame->primary));
    if (!frame->primary) {
        ass_free_track(track);
        return false;
    }
    frame->secondary = frame->primary + WIDTH * HEIGHT;
    int change;
    if (rgba) {
        ASS_ImageRGBA *images = ass_render_frame_rgba(renderer, track, now, &change);
        for (ASS_ImageRGBA *img = images; img; img = img->next) {
            if (img->type != IMAGE_TYPE_CHARACTER)
                continue;
            for (int y = 0; y < img->h; y++) {
                int py = img->dst_y + y;
                for (int x = 0; x < img->w; x++) {
                    int px = img->dst_x + x;
                    if (px < 0 || px >= WIDTH || py < 0 || py >= HEIGHT)
                        continue;
                    const uint8_t *pixel = img->rgba + (ptrdiff_t) y * img->stride + 4 * x;
                    frame->primary[py * WIDTH + px] += pixel[0];
                    frame->secondary[py * WIDTH + px] += pixel[2];
                    frame->active += pixel[0];
                    frame->waiting += pixel[2];
                }
            }
        }
        ass_free_images_rgba(images);
        ass_free_track(track);
        return expect(frame->active + frame->waiting > 0,
                      "RGBA karaoke fixture produced no text");
    }
    ASS_Image *images = ass_render_frame(renderer, track, now, &change);
    bool ok = true;
    for (ASS_Image *img = images; img; img = img->next) {
        if (img->type != IMAGE_TYPE_CHARACTER)
            continue;
        bool primary = img->color == 0xff000000u;
        ok &= expect(primary || img->color == 0x0000ff00u,
                     "karaoke fixture received unexpected paint");
        uint16_t *plane = primary ? frame->primary : frame->secondary;
        for (int y = 0; y < img->h; y++) {
            int py = img->dst_y + y;
            for (int x = 0; x < img->w; x++) {
                int px = img->dst_x + x;
                if (px < 0 || px >= WIDTH || py < 0 || py >= HEIGHT)
                    continue;
                unsigned value = img->bitmap[(ptrdiff_t) y * img->stride + x];
                plane[py * WIDTH + px] += value;
                if (primary) {
                    frame->active += value;
                    frame->active_x += value * (px + 0.5L);
                    frame->active_y += value * (py + 0.5L);
                } else {
                    frame->waiting += value;
                    frame->waiting_x += value * (px + 0.5L);
                    frame->waiting_y += value * (py + 0.5L);
                }
            }
        }
    }
    ass_free_track(track);
    return ok && expect(frame->active + frame->waiting > 0,
                        "karaoke fixture produced no text");
}

static bool same_karaoke_frame(const KaraokeFrame *a, const KaraokeFrame *b)
{
    return a->primary && b->primary &&
        !memcmp(a->primary, b->primary, 2 * WIDTH * HEIGHT * sizeof(*a->primary));
}

static bool karaoke_progression(ASS_Library *lib, ASS_Renderer *renderer,
                                const char *geometry, const char *word,
                                bool vertical)
{
    /* ASS karaoke tags use centiseconds; the renderer clock uses milliseconds. */
    enum {
        START_CS = 50, DURATION_CS = 100,
        START_MS = START_CS * 10, DURATION_MS = DURATION_CS * 10,
    };
    char text[1024], alias[1024];
    const char *format = "{\\bord0\\shad0\\1c&H0000FF&\\2c&HFF0000&%s"
                         "\\kt%d\\%s%d}%s";
    snprintf(text, sizeof(text), format, geometry, START_CS, "kf", DURATION_CS, word);
    snprintf(alias, sizeof(alias), format, geometry, START_CS, "K", DURATION_CS, word);
    KaraokeFrame start = {0}, end = {0}, previous = {0};
    bool ok = karaoke_frame(lib, renderer, text, 0, &start, false);
    ok &= karaoke_frame(lib, renderer, text, START_MS + DURATION_MS + 100, &end, false);
    if (!ok)
        goto done;
    ok &= expect(start.active == 0 && end.waiting == 0 &&
                 start.waiting == end.active,
                 "karaoke endpoints changed coverage or paint");
    const int times[] = {
        START_MS + DURATION_MS / 4,
        START_MS + DURATION_MS / 2,
        START_MS + 3 * DURATION_MS / 4,
    };
    for (size_t t = 0; t < sizeof(times) / sizeof(times[0]); t++) {
        KaraokeFrame current = {0}, upper = {0}, repeated = {0}, rgba = {0};
        bool rendered = karaoke_frame(lib, renderer, text, times[t], &current, false);
        rendered &= karaoke_frame(lib, renderer, alias, times[t], &upper, false);
        /* Revisiting an earlier time after a completed frame exercises caches. */
        rendered &= karaoke_frame(lib, renderer, text, times[t], &repeated, false);
        rendered &= karaoke_frame(lib, renderer, text, times[t], &rgba, true);
        ok &= rendered;
        if (rendered) {
            bool advanced = current.active > 0 && current.waiting > 0 &&
                            current.active > previous.active;
            ok &= expect(advanced,
                         "progressive karaoke did not advance through shaped text");
            if (!advanced)
                fprintf(stderr, "time=%d ms, active=%llu, waiting=%llu, previous=%llu\n",
                        times[t], (unsigned long long) current.active,
                        (unsigned long long) current.waiting,
                        (unsigned long long) previous.active);
            ok &= expect(same_karaoke_frame(&current, &upper) &&
                         same_karaoke_frame(&current, &repeated) &&
                         same_karaoke_frame(&current, &rgba),
                         "curved kf/K progression, RGBA or cached paint differed");
            bool stable = true;
            for (int p = 0; p < WIDTH * HEIGHT; p++) {
                stable &= current.primary[p] + current.secondary[p] == start.secondary[p];
                stable &= current.primary[p] == 0 || start.secondary[p] > 0;
                if (previous.primary)
                    stable &= current.primary[p] >= previous.primary[p];
            }
            ok &= expect(stable, "karaoke masks moved text, lost coverage or regressed");
            if (vertical && current.active && current.waiting)
                ok &= expect(current.active_y / current.active + 10 <
                             current.waiting_y / current.waiting,
                             "vertical curved wipe progressed on screen X");
            if (!strstr(geometry, "\\ct") && current.active && current.waiting)
                ok &= expect(current.active_x / current.active + 10 <
                             current.waiting_x / current.waiting,
                             "horizontal control stopped wiping from left to right");
        }
        free_karaoke_frame(&previous);
        previous = current;
        free_karaoke_frame(&upper);
        free_karaoke_frame(&repeated);
        free_karaoke_frame(&rgba);
    }
done:
    if (!ok)
        fprintf(stderr, "karaoke fixture: %s\n", text);
    free_karaoke_frame(&start);
    free_karaoke_frame(&end);
    free_karaoke_frame(&previous);
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
    ass_set_storage_size(renderer, WIDTH, HEIGHT);
    ass_set_frame_size(renderer, WIDTH, HEIGHT);
    ass_set_fonts(renderer, NULL, "sans-serif",
                  ASS_FONTPROVIDER_AUTODETECT, NULL, 1);

    bool ok = true;
    /* The horizontal control retains the legacy screen-X wipe. Curved paths
     * must progress by shaped distance even when X is constant or reverses. */
    ok &= karaoke_progression(lib, renderer, "\\an5\\pos(480,270)",
                              "MMMMMMMM", false);
    /* Path coordinates are local to pos: center the vertical path at zero
     * so the entire word stays on screen throughout the sampled sweep. */
    ok &= karaoke_progression(lib, renderer,
        "\\an5\\ctan5\\pos(480,270)\\ct(m 0 -220 l 0 220)",
        "MMMMMMMM", true);
    ok &= karaoke_progression(lib, renderer,
        "\\an5\\ctan5\\pos(480,270)\\ct(m 0 0 l 160 0 160 160 0 160)",
        "MMMMMMMMMMMM", false);
    ok &= karaoke_progression(lib, renderer,
        "\\an5\\ctan5\\pos(480,300)\\ct(m -300 0 b -220 -180 220 -180 300 0)",
        "MMMMMMMM", false);
    ok &= karaoke_progression(lib, renderer,
        "\\an5\\ctan5\\pos(480,270)\\ct(m 0 0 l 180 300)",
        "M", false);
    ok &= karaoke_progression(lib, renderer,
        "\\an5\\ctan5\\pos(480,270)\\frz180\\fscx120\\fscy85\\fax0.2"
        "\\ct(m -300 0 b -220 -100 220 -100 300 0)",
        "MMMMMMMM", false);
    ok &= karaoke_progression(lib, renderer,
        "\\an5\\ctan5\\pos(480,270)\\clip(480,0,960,540)"
        "\\ct(m 0 -220 l 0 220)", "MMMMMMMM", true);
    ok &= karaoke_progression(lib, renderer,
        "\\an5\\ctan5\\pos(480,270)\\iclip(0,0,480,540)"
        "\\ct(m 0 -220 l 0 220)", "MMMMMMMM", true);

    KaraokeFrame offset = {0}, translated = {0};
    ok &= karaoke_frame(lib, renderer,
        "{\\an5\\ctan5\\pos(480,270)\\ctx35\\cty20"
        "\\1c&H0000FF&\\2c&HFF0000&\\ct(m -300 0 l 300 0)\\kf100}MMMMMMMM",
        500, &offset, false);
    ok &= karaoke_frame(lib, renderer,
        "{\\an5\\ctan5\\pos(515,290)"
        "\\1c&H0000FF&\\2c&HFF0000&\\ct(m -300 0 l 300 0)\\kf100}MMMMMMMM",
        500, &translated, false);
    ok &= expect(same_karaoke_frame(&offset, &translated),
                 "ctx/cty moved karaoke independently of its glyphs");
    free_karaoke_frame(&offset);
    free_karaoke_frame(&translated);

    KaraokeFrame explicit_anchor = {0}, inherited_anchor = {0};
    ok &= karaoke_frame(lib, renderer,
        "{\\an7\\ctan5\\ta2\\pos(480,270)\\1c&H0000FF&\\2c&HFF0000&"
        "\\ct(m -300 0 b -220 -100 220 -100 300 0)\\kf100}MMMMMMMM\\N{\\ta2}MMM",
        500, &explicit_anchor, false);
    ok &= karaoke_frame(lib, renderer,
        "{\\an5\\ta2\\pos(480,270)\\1c&H0000FF&\\2c&HFF0000&"
        "\\ct(m -300 0 b -220 -100 220 -100 300 0)\\kf100}MMMMMMMM\\N{\\ta2}MMM",
        500, &inherited_anchor, false);
    ok &= expect(same_karaoke_frame(&explicit_anchor, &inherited_anchor),
                 "ctan override and inherited an disagreed on karaoke attachment");
    free_karaoke_frame(&explicit_anchor);
    free_karaoke_frame(&inherited_anchor);
    /* Uneven visual lines exercise the complete-block attachment and ta;
     * every row retains its own baseline while sharing karaoke timing. */
    for (int anchor = 1; anchor <= 9; anchor++) {
        char geometry[256];
        snprintf(geometry, sizeof(geometry),
            "\\an5\\ctan%d\\ta%d\\pos(480,300)\\ctx27\\cty19"
            "\\ct(m -300 0 b -220 -100 220 -100 300 0)",
            anchor, (anchor - 1) % 3 + 1);
        ok &= karaoke_progression(lib, renderer, geometry,
                                  "MMMMMMMM\\NMMM", false);
    }

    Sample flat, horizontal, diagonal, cubic;
    ok &= render_sample(lib, renderer,
        "{\\an7\\pos(180,180)}CURVED TEXT", 0, &flat);
    ok &= render_sample(lib, renderer,
        "{\\an7\\pos(180,180)\\ctan1\\ct(m 0 0 l 500 0)}CURVED TEXT",
        0, &horizontal);
    /* Curved placement can change subpixel overlap at cluster boundaries,
     * so exact coverage is not an orientation invariant.  Keep this as a
     * renderer-level sanity check; the exact zero-angle transform is covered
     * by shaper_chain_cleanup_selftest. */
    ok &= expect(horizontal.coverage > 0 &&
                 width(&horizontal) > height(&horizontal) &&
                 width(&horizontal) * 4 >= width(&flat) * 3 &&
                 width(&horizontal) * 4 <= width(&flat) * 5 &&
                 height(&horizontal) * 4 >= height(&flat) * 3 &&
                 height(&horizontal) * 4 <= height(&flat) * 5,
                 "horizontal \\ct changed ordinary glyph orientation");

    ok &= render_sample(lib, renderer,
        "{\\an7\\pos(180,120)\\ctan1\\ct(m 0 0 l 350 220)}CURVED TEXT",
        0, &diagonal);
    ok &= expect(height(&diagonal) > height(&horizontal),
                 "diagonal path did not rotate clusters");

    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(480,300)\\ct(m -350 0 b -220 -180 220 -180 350 0)}CURVED TEXT",
        0, &cubic);
    ok &= expect(height(&cubic) > height(&horizontal) &&
                 cubic.hash != horizontal.hash,
                 "cubic path did not curve text");

    Sample start, center, end;
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(200,260)\\ctan1\\ct(m 0 0 l 600 0)}ALIGN", 0, &start);
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(200,260)\\ctan2\\ct(m 0 0 l 600 0)}ALIGN", 0, &center);
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(200,260)\\ctan3\\ct(m 0 0 l 600 0)}ALIGN", 0, &end);
    ok &= expect(center_x(&start) + 20 < center_x(&center) &&
                 center_x(&center) + 20 < center_x(&end),
                 "ctan start/center/end ordering failed");

    Sample offset_x, offset_y, offset_y_negative;
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(200,260)\\ctan1\\ctx60\\ct(m 0 0 l 600 0)}ALIGN", 0,
        &offset_x);
    ok &= near(center_x(&offset_x) - center_x(&start), 60.0, 1.0,
               "ctx horizontal offset");
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(200,260)\\ctan1\\cty40\\ct(m 0 0 l 600 0)}ALIGN", 0,
        &offset_y);
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(200,260)\\ctan1\\cty-40\\ct(m 0 0 l 600 0)}ALIGN", 0,
        &offset_y_negative);
    ok &= near(center_y(&offset_y) - center_y(&start), 40.0, 1.0,
               "positive cty did not move below a horizontal path");
    ok &= near(center_y(&offset_y_negative) - center_y(&start), -40.0, 1.0,
               "negative cty did not move above a horizontal path");

    Sample positioned, moved0, moved1;
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(300,320)\\ctan1\\ct(m 0 0 l 600 0)}ALIGN", 0,
        &positioned);
    ok &= near(center_x(&positioned) - center_x(&start), 100.0, 1.0,
               "pos did not move the complete path");
    ok &= near(center_y(&positioned) - center_y(&start), 60.0, 1.0,
               "pos Y did not move the complete path");
    const char *moving =
        "{\\an5\\move(200,260,400,320,0,1000)\\ctan1\\ct(m 0 0 l 600 0)}ALIGN";
    ok &= render_sample(lib, renderer, moving, 0, &moved0);
    ok &= render_sample(lib, renderer, moving, 500, &moved1);
    ok &= near(center_x(&moved1) - center_x(&moved0), 100.0, 1.5,
               "move X did not carry the path");
    ok &= near(center_y(&moved1) - center_y(&moved0), 30.0, 1.5,
               "move Y did not carry the path");

    Sample effects, plain_effects;
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(480,300)"
        "\\ct(m -300 0 b -180 -120 180 -120 300 0)}EFFECTS",
        0, &plain_effects);
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(480,300)\\bord4\\blur2\\shad3"
        "\\ct(m -300 0 b -180 -120 180 -120 300 0)}EFFECTS",
        0, &effects);
    ok &= expect(effects.images >= 2 &&
                 effects.coverage > plain_effects.coverage,
                 "border/blur curved rendering produced no effect layers");
    Sample transforms;
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(480,300)\\fscx120\\fscy85\\fsp3\\frz12\\scale110"
        "\\ct(m -280 0 b -170 -100 170 -100 280 0)}TRANSFORMS",
        0, &transforms);
    ok &= expect(transforms.coverage > 0,
                 "scale/fsc/fsp/frz interaction produced no curved text");
    Sample distorted;
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(480,300)\\distort(1,0,1.15,1,-0.1,1)"
        "\\ct(m -280 0 b -170 -100 170 -100 280 0)}DISTORT",
        0, &distorted);
    ok &= expect(distorted.coverage > 0,
                 "distort interaction produced no curved text");

    Sample normal_invalid, malformed, empty, nonfinite;
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(480,280)}FALLBACK", 0, &normal_invalid);
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(480,280)\\ct(l 10 10)}FALLBACK", 0, &malformed);
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(480,280)\\ct()}FALLBACK", 0, &empty);
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(480,280)\\ct(m 0 0 l 1e999 0)}FALLBACK",
        0, &nonfinite);
    ok &= expect(same_sample(&normal_invalid, &malformed),
                 "malformed path did not fall back to normal text");
    ok &= expect(same_sample(&normal_invalid, &empty),
                 "empty path did not fall back to normal text");
    ok &= expect(same_sample(&normal_invalid, &nonfinite),
                 "non-finite path did not fall back to normal text");

    Sample first_valid, selected_valid, reset_path, invalid_align, default_align;
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(480,280)\\ct(l 0 0)"
        "\\ct(m -250 0 b -150 -100 150 -100 250 0)}FIRST VALID",
        0, &first_valid);
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(480,280)\\ct(m -250 0 b -150 -100 150 -100 250 0)}FIRST VALID",
        0, &selected_valid);
    ok &= expect(same_sample(&first_valid, &selected_valid),
                 "first valid path selection failed");
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(480,280)\\ct(m -250 0 b -150 -100 150 -100 250 0)"
        "\\r}FIRST VALID", 0, &reset_path);
    ok &= expect(same_sample(&reset_path, &selected_valid),
                 "style reset destroyed the event-wide path");
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(480,280)\\ctan10\\ct(m -300 0 l 300 0)}ALIGN",
        0, &invalid_align);
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(480,280)\\ct(m -300 0 l 300 0)}ALIGN",
        0, &default_align);
    ok &= expect(same_sample(&invalid_align, &default_align),
                 "invalid ctan did not restore an-derived alignment");

    Sample anchors[10];
    for (int value = 1; value <= 9; value++) {
        char text[160];
        snprintf(text, sizeof(text),
            "{\\an7\\pos(180,260)\\ctan%d\\ct(m 0 0 l 600 0)}ALIGN",
            value);
        ok &= render_sample(lib, renderer, text, 0, &anchors[value]);
    }
    for (int row = 0; row < 3; row++) {
        int first = row * 3 + 1;
        ok &= expect(center_x(&anchors[first]) + 30 <
                     center_x(&anchors[first + 1]) &&
                     center_x(&anchors[first + 1]) + 30 <
                     center_x(&anchors[first + 2]),
                     "ctan horizontal path anchoring failed");
    }
    for (int column = 1; column <= 3; column++) {
        ok &= expect(center_y(&anchors[column + 6]) >
                     center_y(&anchors[column + 3]) + 5 &&
                     center_y(&anchors[column + 3]) >
                     center_y(&anchors[column]) + 5,
                     "ctan top/middle/bottom path anchoring failed");
    }

    /* The second line is widest.  A flat path makes the selected point of
     * the complete three-line block directly measurable against the path. */
    Sample block_anchors[10] = {{0}};
    for (int value = 1; value <= 9; value++) {
        char text[256];
        snprintf(text, sizeof(text),
            "{\\an5\\ta2\\pos(480,270)\\fs44\\bord0\\shad0"
            "\\ctan%d\\ct(m -300 0 l 300 0)}"
            "A\\NTHE LONG SECOND LINE\\NABC", value);
        ok &= render_sample(lib, renderer, text, 0, &block_anchors[value]);
        if (!block_anchors[value].images)
            continue;
        int column = (value - 1) % 3;
        int row = (value - 1) / 3;
        double x = column == 0 ? block_anchors[value].min_x :
            column == 1 ? (block_anchors[value].min_x +
                           block_anchors[value].max_x) * 0.5 :
                          block_anchors[value].max_x;
        double y = row == 2 ? block_anchors[value].min_y :
            row == 1 ? (block_anchors[value].min_y +
                        block_anchors[value].max_y) * 0.5 :
                       block_anchors[value].max_y;
        double path_x = column == 0 ? 180 : column == 1 ? 480 : 780;
        if (fabs(x - path_x) > 18 || fabs(y - 270) > 18) {
            fprintf(stderr,
                "multiline ctan%d anchored (%.1f,%.1f), expected (%.1f,270); "
                "block=(%d,%d)..(%d,%d)\n", value, x, y, path_x,
                block_anchors[value].min_x, block_anchors[value].min_y,
                block_anchors[value].max_x, block_anchors[value].max_y);
            ok = false;
        }
    }
    ok &= expect(block_anchors[2].max_y <= 288 &&
                 block_anchors[2].min_y < 190 &&
                 block_anchors[8].min_y >= 252 &&
                 block_anchors[8].max_y > 350,
                 "multiline ctan2/8 did not place the entire block above/below the curve");
    ok &= near(center_y(&block_anchors[8]) - center_y(&block_anchors[2]),
               height(&block_anchors[2]), 7.0,
               "multiline top/bottom anchors ignored final text bounds");

    /* An absent ctan is exactly the corresponding an for the completed
     * three-line object, including its uneven widths and curved bounds. */
    for (int value = 1; value <= 9; value++) {
        char implicit[256], explicit[256];
        const char *format =
            "{\\an%d\\ta2\\pos(480,270)\\fs44\\bord0\\shad0%s"
            "\\ct(m -300 0 b -180 -150 180 -150 300 0)}"
            "testing\\Nsuper testing\\Nend";
        char override[16];
        snprintf(override, sizeof(override), "\\ctan%d", value);
        snprintf(implicit, sizeof(implicit), format, value, "");
        snprintf(explicit, sizeof(explicit), format, value, override);
        Sample inherited, specified;
        ok &= render_sample(lib, renderer, implicit, 0, &inherited);
        ok &= render_sample(lib, renderer, explicit, 0, &specified);
        ok &= expect(same_sample(&inherited, &specified),
                     "implicit an-to-ctan inheritance differed from explicit ctan");
    }

    Sample curved_block_bottom = {0}, curved_block_middle = {0},
           curved_block_top = {0};
    const char *curved_block_format =
        "{\\an5\\ta2\\pos(480,270)\\fs44\\bord0\\shad0"
        "\\ctan%d\\ct(m -300 0 b -180 -150 180 -150 300 0)}"
        "A\\NTHE LONG SECOND LINE\\NABC";
    char curved_block_text[256];
    snprintf(curved_block_text, sizeof(curved_block_text),
             curved_block_format, 2);
    ok &= render_sample(lib, renderer, curved_block_text, 0,
                        &curved_block_bottom);
    snprintf(curved_block_text, sizeof(curved_block_text),
             curved_block_format, 5);
    ok &= render_sample(lib, renderer, curved_block_text, 0,
                        &curved_block_middle);
    snprintf(curved_block_text, sizeof(curved_block_text),
             curved_block_format, 8);
    ok &= render_sample(lib, renderer, curved_block_text, 0,
                        &curved_block_top);
    ok &= expect(curved_block_bottom.weight > 0 &&
                 curved_block_middle.weight > 0 &&
                 curved_block_top.weight > 0 &&
                 center_y(&curved_block_bottom) + 35 <
                     center_y(&curved_block_middle) &&
                 center_y(&curved_block_middle) + 35 <
                     center_y(&curved_block_top),
                 "strong curve reverted to per-line ctan vertical anchors");

    Sample override_event_an7, override_event_an5, inherited_an7;
    ok &= render_sample(lib, renderer,
        "{\\an7\\ta2\\ctan5\\pos(480,270)\\fs44"
        "\\ct(m -300 0 b -180 -150 180 -150 300 0)}"
        "testing\\N{\\ta2}super testing",
        0, &override_event_an7);
    ok &= render_sample(lib, renderer,
        "{\\an5\\ta2\\ctan5\\pos(480,270)\\fs44"
        "\\ct(m -300 0 b -180 -150 180 -150 300 0)}"
        "testing\\N{\\ta2}super testing",
        0, &override_event_an5);
    ok &= render_sample(lib, renderer,
        "{\\an7\\ta2\\pos(480,270)\\fs44"
        "\\ct(m -300 0 b -180 -150 180 -150 300 0)}"
        "testing\\N{\\ta2}super testing",
        0, &inherited_an7);
    ok &= expect(same_sample(&override_event_an7, &override_event_an5) &&
                 !same_sample(&override_event_an7, &inherited_an7),
                 "explicit ctan did not override curved attachment independently of an");
    Sample margin_an7, margin_an5;
    ok &= render_sample(lib, renderer,
        "{\\an7\\ctan5\\ct(m 0 0 l 400 0)}MARGIN",
        0, &margin_an7);
    ok &= render_sample(lib, renderer,
        "{\\an5\\ctan5\\ct(m 0 0 l 400 0)}MARGIN",
        0, &margin_an5);
    ok &= expect(center_x(&margin_an7) + 100 < center_x(&margin_an5) &&
                 center_y(&margin_an7) + 100 < center_y(&margin_an5),
                 "an stopped controlling the event margin anchor with ctan");

    Sample diagonal_top, diagonal_middle, diagonal_bottom;
    const char *diagonal_format =
        "{\\an5\\ta2\\ctan%d\\pos(320,250)\\fs44"
        "\\ct(m 0 0 l 500 180)}testing\\Nsuper testing";
    char diagonal_text[180];
    snprintf(diagonal_text, sizeof(diagonal_text), diagonal_format, 7);
    ok &= render_sample(lib, renderer, diagonal_text, 0, &diagonal_top);
    snprintf(diagonal_text, sizeof(diagonal_text), diagonal_format, 4);
    ok &= render_sample(lib, renderer, diagonal_text, 0, &diagonal_middle);
    snprintf(diagonal_text, sizeof(diagonal_text), diagonal_format, 1);
    ok &= render_sample(lib, renderer, diagonal_text, 0, &diagonal_bottom);
    ok &= expect(center_x(&diagonal_top) + 5 <
                 center_x(&diagonal_middle) &&
                 center_x(&diagonal_middle) + 5 <
                 center_x(&diagonal_bottom) &&
                 center_y(&diagonal_top) >
                 center_y(&diagonal_middle) + 15 &&
                 center_y(&diagonal_middle) >
                 center_y(&diagonal_bottom) + 15,
                 "diagonal whole-block anchor did not follow the path normal");

    Sample first_ctan, reset_ctan, ignored_ctan,
           curved_anchor, diagonal_anchor;
    ok &= render_sample(lib, renderer,
        "{\\an7\\pos(180,260)\\ctan5\\ctan9\\ct(m 0 0 l 600 0)}ALIGN",
        0, &first_ctan);
    ok &= expect(same_sample(&first_ctan, &anchors[5]),
                 "ctan did not keep the first valid override");
    ok &= render_sample(lib, renderer,
        "{\\an7\\pos(180,260)\\ctan5\\r\\ct(m 0 0 l 600 0)}ALIGN",
        0, &reset_ctan);
    ok &= render_sample(lib, renderer,
        "{\\an7\\pos(180,260)\\ct(m 0 0 l 600 0)}ALIGN",
        0, &default_align);
    ok &= expect(same_sample(&reset_ctan, &default_align),
                 "style reset did not restore inheritance from event an7");
    ok &= render_sample(lib, renderer,
        "{\\an7\\pos(180,260)\\t(0,1000,\\ctan5)\\ct(m 0 0 l 600 0)}ALIGN",
        500, &ignored_ctan);
    ok &= expect(same_sample(&ignored_ctan, &default_align),
                 "animated ctan changed the static path anchor");
    ok &= render_sample(lib, renderer,
        "{\\an7\\pos(480,280)\\ctan8"
        "\\ct(m -300 0 b -220 -130 160 -60 300 40)}ASYMMETRIC",
        0, &curved_anchor);
    ok &= render_sample(lib, renderer,
        "{\\an7\\pos(180,180)\\ctan8\\ct(m 0 0 l 450 180)}DIAGONAL",
        0, &diagonal_anchor);
    ok &= expect(curved_anchor.coverage > 0 && diagonal_anchor.coverage > 0,
                 "ctan failed on curved or diagonal paths");

    Sample ta_left, ta_center, ta_right, ctan_left, ctan_center;
    ok &= render_sample(lib, renderer,
        "{\\an5\\ta1\\ctan5\\pos(480,240)\\ct(m -300 0 l 300 0)}LONG FIRST LINE\\N{\\ta1}short",
        0, &ta_left);
    ok &= render_sample(lib, renderer,
        "{\\an5\\ta2\\ctan5\\pos(480,240)\\ct(m -300 0 l 300 0)}LONG FIRST LINE\\N{\\ta2}short",
        0, &ta_center);
    ok &= render_sample(lib, renderer,
        "{\\an5\\ta3\\ctan5\\pos(480,240)\\ct(m -300 0 l 300 0)}LONG FIRST LINE\\N{\\ta3}short",
        0, &ta_right);
    ok &= expect(center_x(&ta_left) + 5 < center_x(&ta_center) &&
                 center_x(&ta_center) + 5 < center_x(&ta_right),
                 "ta did not align curved visual lines within the text block");
    ok &= render_sample(lib, renderer,
        "{\\an5\\ta2\\ctan4\\pos(480,240)\\ct(m -300 0 l 300 0)}LONG FIRST LINE\\Nshort",
        0, &ctan_left);
    ok &= render_sample(lib, renderer,
        "{\\an5\\ta2\\ctan5\\pos(480,240)\\ct(m -300 0 l 300 0)}LONG FIRST LINE\\Nshort",
        0, &ctan_center);
    ok &= expect(center_x(&ctan_left) + 30 < center_x(&ctan_center),
                 "ctan path anchor was overwritten by ta");

    Sample small_inherited, small_top, large_inherited, large_top;
    ok &= render_sample(lib, renderer,
        "{\\an7\\pos(180,260)\\fs30\\bord0\\shad0\\ct(m 0 0 l 600 0)}METRICS",
        0, &small_inherited);
    ok &= render_sample(lib, renderer,
        "{\\an7\\pos(180,260)\\fs30\\bord0\\shad0\\ctan7\\ct(m 0 0 l 600 0)}METRICS",
        0, &small_top);
    ok &= render_sample(lib, renderer,
        "{\\an7\\pos(180,260)\\fs60\\bord0\\shad0\\ct(m 0 0 l 600 0)}METRICS",
        0, &large_inherited);
    ok &= render_sample(lib, renderer,
        "{\\an7\\pos(180,260)\\fs60\\bord0\\shad0\\ctan7\\ct(m 0 0 l 600 0)}METRICS",
        0, &large_top);
    ok &= expect(same_sample(&small_inherited, &small_top) &&
                 same_sample(&large_inherited, &large_top),
                 "single-line ctan did not inherit an7 at either font size");

    Sample short0, short1;
    const char *short_path =
        "{\\an5\\pos(480,280)\\ct(m 0 0 l 1 0)}A VERY LONG LINE";
    ok &= render_sample(lib, renderer, short_path, 0, &short0);
    ok &= render_sample(lib, renderer, short_path, 0, &short1);
    ok &= expect(same_sample(&short0, &short1),
                 "very short path rendering was not deterministic");

    Sample animated, fixed;
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(200,260)\\ctan1\\ct(m 0 0 l 600 0)"
        "\\t(0,1000,\\ctx100\\cty40)}ANIMATE", 500, &animated);
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(200,260)\\ctan1\\ctx50\\cty20"
        "\\ct(m 0 0 l 600 0)}ANIMATE", 500, &fixed);
    ok &= expect(same_sample(&animated, &fixed),
                 "animated ctx/cty did not match the interpolated static state");

    Sample multiline, multiline_normal, multiline_one, multiline_three;
    ok &= render_sample(lib, renderer,
        "{\\an4\\pos(180,150)\\ct(m 0 0 l 600 0)}TEST",
        0, &multiline_one);
    ok &= render_sample(lib, renderer,
        "{\\an4\\pos(180,150)\\ct(m 0 0 l 600 0)}TEST\\NTEST",
        0, &multiline);
    ok &= render_sample(lib, renderer,
        "{\\an4\\pos(180,150)\\ct(m 0 0 l 600 0)}TEST\\NTEST\\NTEST",
        0, &multiline_three);
    ok &= render_sample(lib, renderer,
        "{\\an4\\pos(180,150)}TEST\\NTEST", 0, &multiline_normal);
    ok &= expect(height(&multiline) > height(&multiline_one) + 25 &&
                 height(&multiline_three) > height(&multiline) + 25,
                 "explicit lines did not receive separate curved baselines");
    ok &= near(center_x(&multiline), center_x(&multiline_one), 2.0,
               "second line did not restart path progression");
    ok &= near(center_x(&multiline_three), center_x(&multiline_one), 2.0,
               "third line did not restart path progression");
    ok &= expect(!same_sample(&multiline, &multiline_normal),
                 "multiline path fell back to ordinary layout");

    Sample acceptance_one, acceptance_two;
    ok &= render_sample(lib, renderer,
        "{\\pos(418,360)\\ct(m -134.95 0 b -44.98 -45.33 44.98 -45.33 134.95 0)}testingly test",
        0, &acceptance_one);
    ok &= render_sample(lib, renderer,
        "{\\pos(418,360)\\ct(m -134.95 0 b -44.98 -45.33 44.98 -45.33 134.95 0)}testingly test\\Nand test again",
        0, &acceptance_two);
    ok &= expect(acceptance_two.coverage > acceptance_one.coverage &&
                 height(&acceptance_two) > height(&acceptance_one) + 20,
                 "acceptance example did not render a second curved line");

    Sample small_one, small_two, large_one, large_two;
    ok &= render_sample(lib, renderer,
        "{\\an4\\pos(180,150)\\fs40\\ct(m 0 0 l 600 0)}TEST",
        0, &small_one);
    ok &= render_sample(lib, renderer,
        "{\\an4\\pos(180,150)\\fs40\\ct(m 0 0 l 600 0)}TEST\\NTEST",
        0, &small_two);
    ok &= render_sample(lib, renderer,
        "{\\an4\\pos(180,150)\\fs80\\ct(m 0 0 l 600 0)}TEST",
        0, &large_one);
    ok &= render_sample(lib, renderer,
        "{\\an4\\pos(180,150)\\fs80\\ct(m 0 0 l 600 0)}TEST\\NTEST",
        0, &large_two);
    ok &= expect(height(&large_two) - height(&large_one) >
                 height(&small_two) - height(&small_one) + 20,
                 "curved line spacing did not follow font metrics");

    const int alignments[] = {4, 5, 6};
    for (size_t i = 0; i < sizeof(alignments) / sizeof(alignments[0]); i++) {
        char long_text[256], short_text[256], both_text[256];
        const char *format =
            "{\\an%d\\pos(480,280)\\ct(m -300 0 l 300 0)}%s";
        snprintf(long_text, sizeof(long_text), format, alignments[i],
                 "THIS IS A LONG LINE");
        snprintf(short_text, sizeof(short_text), format, alignments[i],
                 "short");
        snprintf(both_text, sizeof(both_text), format, alignments[i],
                 "THIS IS A LONG LINE\\Nshort");
        Sample long_line, short_line, both_lines;
        ok &= render_sample(lib, renderer, long_text, 0, &long_line);
        ok &= render_sample(lib, renderer, short_text, 0, &short_line);
        ok &= render_sample(lib, renderer, both_text, 0, &both_lines);
        ok &= near(center_x(&both_lines),
                   combined_center_x(&long_line, &short_line), 2.0,
                   "visual lines did not align independently on the path");
    }

    Sample diagonal_one, diagonal_two;
    ok &= render_sample(lib, renderer,
        "{\\an7\\pos(180,150)\\ct(m 0 0 l 500 180)}TEST",
        0, &diagonal_one);
    ok &= render_sample(lib, renderer,
        "{\\an7\\pos(180,150)\\ct(m 0 0 l 500 180)}TEST\\NTEST",
        0, &diagonal_two);
    ok &= expect(center_x(&diagonal_two) < center_x(&diagonal_one) - 5 &&
                 center_y(&diagonal_two) > center_y(&diagonal_one) + 15,
                 "multiline offset did not follow the diagonal path normal");

    Sample cubic_one, cubic_two, wrapped, wrapped_normal, unwrapped_normal;
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(480,180)\\ct(m -300 0 b -200 -150 200 -150 300 0)}TEST",
        0, &cubic_one);
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(480,180)\\ct(m -300 0 b -200 -150 200 -150 300 0)}TEST\\NTEST",
        0, &cubic_two);
    ok &= expect(height(&cubic_two) > height(&cubic_one) + 20,
                 "strongly curved lines overlapped their baselines");
    const char *wrap_words =
        "A LONG AUTOMATIC WRAPPING LINE WITH ENOUGH WORDS TO CROSS THE "
        "SUBTITLE MARGINS AND FORM MULTIPLE VISUAL LINES OF TEXT";
    char wrap_text[512], wrap_normal_text[512];
    snprintf(wrap_text, sizeof(wrap_text),
        "{\\an5\\pos(480,200)\\ct(m -350 0 b -230 -90 230 -90 350 0)}%s",
        wrap_words);
    snprintf(wrap_normal_text, sizeof(wrap_normal_text),
        "{\\an5\\pos(480,200)}%s", wrap_words);
    ok &= render_sample(lib, renderer, wrap_text, 0, &wrapped);
    ok &= render_sample(lib, renderer, wrap_normal_text, 0, &wrapped_normal);
    snprintf(wrap_normal_text, sizeof(wrap_normal_text),
        "{\\q2\\an5\\pos(480,200)}%s", wrap_words);
    ok &= render_sample(lib, renderer, wrap_normal_text, 0,
                        &unwrapped_normal);
    ok &= expect(height(&wrapped_normal) > height(&unwrapped_normal) + 25,
                 "automatic-wrap fixture did not produce visual lines");
    ok &= expect(height(&wrapped) > height(&cubic_one) + 25 &&
                 !same_sample(&wrapped, &wrapped_normal),
                 "automatically wrapped lines did not follow the path");

    Sample soft_break, hard_break, soft_as_space, space_text;
    ok &= render_sample(lib, renderer,
        "{\\q2\\an4\\pos(180,150)\\ct(m 0 0 l 600 0)}ONE\\nTWO",
        0, &soft_break);
    ok &= render_sample(lib, renderer,
        "{\\q2\\an4\\pos(180,150)\\ct(m 0 0 l 600 0)}ONE\\NTWO",
        0, &hard_break);
    ok &= expect(same_sample(&soft_break, &hard_break),
                 "WrapStyle 2 soft break missed curved visual-line layout");
    ok &= render_sample(lib, renderer,
        "{\\an4\\pos(180,150)\\ct(m 0 0 l 600 0)}ONE\\nTWO",
        0, &soft_as_space);
    ok &= render_sample(lib, renderer,
        "{\\an4\\pos(180,150)\\ct(m 0 0 l 600 0)}ONE TWO",
        0, &space_text);
    ok &= expect(same_sample(&soft_as_space, &space_text),
                 "default soft-break semantics changed");

    Sample multiline_effects, multiline_plain, multiline_furi, aligned_furi;
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(480,280)\\ct(m -300 0 b -200 -80 200 -80 300 0)}FIRST\\NSECOND",
        0, &multiline_plain);
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(480,280)\\bord5\\blur1\\ct(m -300 0 b -200 -80 200 -80 300 0)}FIRST\\NSECOND",
        0, &multiline_effects);
    ok &= expect(multiline_effects.coverage > multiline_plain.coverage,
                 "border/blur failed on multiline curved text");
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(480,280)\\ct(m -300 0 b -200 -80 200 -80 300 0)}<A|B>\\N<C|D>",
        0, &multiline_furi);
    ok &= expect(multiline_furi.coverage > 0,
                 "multiline furigana fallback stopped rendering");
    ok &= render_sample(lib, renderer,
        "{\\an5\\ta2\\ctan5\\pos(480,280)"
        "\\ct(m -300 0 b -200 -80 200 -80 300 0)}"
        "<漢字|ふりがな>\\Nshort",
        0, &aligned_furi);
    ok &= expect(aligned_furi.coverage > 0,
                 "ta/ctan furigana fallback stopped rendering");

    Sample multiline_transforms, multiline_move0, multiline_move1;
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(480,280)\\fscx120\\fscy85\\frz12\\scale110"
        "\\shad3\\clip(0,0,960,540)"
        "\\ct(m -300 0 b -200 -80 200 -80 300 0)}FIRST\\NSECOND",
        0, &multiline_transforms);
    ok &= expect(multiline_transforms.coverage > 0,
                 "multiline curved text failed with transforms or clipping");
    const char *moving_lines =
        "{\\an5\\move(380,230,480,280,0,1000)"
        "\\ct(m -300 0 b -200 -80 200 -80 300 0)}FIRST\\NSECOND";
    ok &= render_sample(lib, renderer, moving_lines, 0,
                        &multiline_move0);
    ok &= render_sample(lib, renderer, moving_lines, 500,
                        &multiline_move1);
    ok &= near(center_x(&multiline_move1) - center_x(&multiline_move0),
               50.0, 2.0, "move X did not carry multiline curved text");
    ok &= near(center_y(&multiline_move1) - center_y(&multiline_move0),
               25.0, 2.0, "move Y did not carry multiline curved text");

    /* Script coverage.  The synthetic internal-chain regression separately
     * verifies that the multiple glyphs of one Myanmar cluster stay rigid. */
    const char *scripts[] = {
        "CURVED LATIN",
        "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x83\x86"
        "\xE3\x82\xAD\xE3\x82\xB9\xE3\x83\x88", /* Japanese */
        "\xE1\x80\x99\xE1\x80\xBC\xE1\x80\x94\xE1\x80\xBA"
        "\xE1\x80\x99\xE1\x80\xAC\xE1\x80\x85\xE1\x80\xAC", /* Myanmar 1 */
        "\xE1\x80\x9E\xE1\x80\x84\xE1\x80\xBA\xE1\x80\xB9"
        "\xE1\x80\x81\xE1\x80\xBB\xE1\x80\xAC", /* Myanmar 2 */
        "\xE1\x80\x80\xE1\x80\xBC\xE1\x80\xAD\xE1\x80\xAF"
        "\xE1\x80\x86\xE1\x80\xAD\xE1\x80\xAF\xE1\x80\x95"
        "\xE1\x80\xAB\xE1\x80\x90\xE1\x80\x9A\xE1\x80\xBA", /* Myanmar 3 */
        "\xD8\xA7\xD9\x84\xD8\xB3\xD9\x8E\xD9\x91\xD9\x84"
        "\xD9\x8E\xD8\xA7\xD9\x85\xD9\x8F\x20\xD8\xB9\xD9\x8E"
        "\xD9\x84\xD9\x8E\xD9\x8A\xD9\x92\xD9\x83\xD9\x8F\xD9\x85\xD9\x92",
    };
    for (size_t i = 0; i < sizeof(scripts) / sizeof(scripts[0]); i++) {
        char text[2048];
        snprintf(text, sizeof(text),
            "{\\an5\\pos(480,300)\\ct(m -340 0 b -220 -120 220 -120 340 0)}%s",
            scripts[i]);
        Sample script_sample;
        ok &= render_sample(lib, renderer, text, 0, &script_sample);
        ok &= expect(script_sample.coverage > 0,
                     "script coverage case produced no curved glyphs");
    }
    char myanmar_lines[2048];
    snprintf(myanmar_lines, sizeof(myanmar_lines),
        "{\\an5\\pos(480,220)\\ct(m -340 0 b -220 -100 220 -100 340 0)}%s\\N%s",
        scripts[2], scripts[3]);
    Sample myanmar_multiline;
    ok &= render_sample(lib, renderer, myanmar_lines, 0,
                        &myanmar_multiline);
    ok &= expect(myanmar_multiline.coverage > 0,
                 "multiline Myanmar shaping produced no curved glyphs");

    ass_renderer_done(renderer);
    ass_library_done(lib);
    return ok ? 0 : 1;
}
