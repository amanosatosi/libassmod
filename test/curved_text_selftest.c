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
        "{\\an5\\pos(480,280)\\ctan9\\ct(m -300 0 l 300 0)}ALIGN",
        0, &invalid_align);
    ok &= render_sample(lib, renderer,
        "{\\an5\\pos(480,280)\\ct(m -300 0 l 300 0)}ALIGN",
        0, &default_align);
    ok &= expect(same_sample(&invalid_align, &default_align),
                 "invalid ctan did not restore an-derived alignment");

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
    ok &= expect(2 * (center_y(&large_two) - center_y(&large_one)) >
                 2 * (center_y(&small_two) - center_y(&small_one)) + 20,
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
        "{\\an4\\pos(180,150)\\ct(m 0 0 l 500 180)}TEST",
        0, &diagonal_one);
    ok &= render_sample(lib, renderer,
        "{\\an4\\pos(180,150)\\ct(m 0 0 l 500 180)}TEST\\NTEST",
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

    Sample multiline_effects, multiline_plain, multiline_furi;
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
