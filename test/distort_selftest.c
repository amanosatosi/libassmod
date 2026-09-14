/* Renderer-level distortion regressions, using the public API and pixel-mask
 * comparisons as in bs4_geometry_selftest.c. Most cases use drawings to avoid
 * font dependence; text-run grouping is checked with same-font relative masks. */

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ass.h"

enum { WIDTH = 640, HEIGHT = 360 };

typedef struct {
    uint8_t pixels[3][WIDTH * HEIGHT];
    bool covered;
} Mask;

static const char shape[] = "m 0 0 l 160 0 160 96 0 96";

static void msg_cb(int level, const char *fmt, va_list va, void *data)
{
    (void) level;
    (void) fmt;
    (void) va;
    (void) data;
}

static ASS_Track *read_track(ASS_Library *lib, const char *tags)
{
    char script[8192];
    int n = snprintf(script, sizeof(script),
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: 640\n"
        "PlayResY: 360\n"
        "ScaledBorderAndShadow: yes\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,40,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,"
        "0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:03.00,Default,,0,0,0,,"
        "{%s\\an7\\pos(240,120)\\p1}%s\n", tags, shape);
    if (n < 0 || n >= (int) sizeof(script))
        return NULL;
    return ass_read_memory(lib, script, strlen(script), NULL);
}

static ASS_Track *read_text_track(ASS_Library *lib, const char *text)
{
    char script[8192];
    int n = snprintf(script, sizeof(script),
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: 640\n"
        "PlayResY: 360\n"
        "ScaledBorderAndShadow: yes\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,40,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,"
        "0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:03.00,Default,,0,0,0,,%s\n", text);
    if (n < 0 || n >= (int) sizeof(script))
        return NULL;
    ASS_Track *track = ass_read_memory(lib, script, strlen(script), NULL);
    return track;
}

static bool capture(ASS_Renderer *renderer, ASS_Track *track,
                    long long now, Mask *mask)
{
    int change;
    ASS_Image *images = ass_render_frame(renderer, track, now, &change);
    memset(mask, 0, sizeof(*mask));
    for (ASS_Image *img = images; img; img = img->next) {
        if ((unsigned) img->type >= 3)
            return false;
        for (int y = 0; y < img->h; y++) {
            int py = img->dst_y + y;
            if (py < 0 || py >= HEIGHT)
                continue;
            for (int x = 0; x < img->w; x++) {
                int px = img->dst_x + x;
                if (px < 0 || px >= WIDTH)
                    continue;
                uint8_t alpha = (img->bitmap[y * img->stride + x] *
                    (255 - (uint8_t) img->color) + 127) / 255;
                uint8_t *dst = &mask->pixels[img->type][py * WIDTH + px];
                if (alpha > *dst)
                    *dst = alpha;
                mask->covered |= alpha != 0;
            }
        }
    }
    return mask->covered;
}

static bool check_pair(ASS_Library *lib, ASS_Renderer *renderer,
                       const char *a, const char *b, long long now,
                       bool equal, const char *label)
{
    static Mask actual, expected;
    ASS_Track *ta = read_track(lib, a);
    ASS_Track *tb = read_track(lib, b);
    bool ok = ta && tb && capture(renderer, ta, now, &actual) &&
        capture(renderer, tb, now, &expected);
    if (ok)
        ok = (!memcmp(actual.pixels, expected.pixels, sizeof(actual.pixels))) == equal;
    if (!ok)
        fprintf(stderr, "%s: pixel comparison failed at %lld ms\n", label, now);
    if (ta) ass_free_track(ta);
    if (tb) ass_free_track(tb);
    return ok;
}

static bool test_text_run_grouping(ASS_Library *lib, ASS_Renderer *renderer)
{
    static Mask joined, same_style, split_style;
    const char *joined_text =
        "{\\an7\\pos(80,120)\\distort(1.35,-0.15,1.45,1.15,-0.15,1)}"
        "Testing My Text";
    const char *same_style_text =
        "{\\an7\\pos(80,120)\\distort(1.35,-0.15,1.45,1.15,-0.15,1)}"
        "Testing{\\1c&HFFFFFF&} My Text";
    const char *split_style_text =
        "{\\an7\\pos(80,120)\\distort(1.35,-0.15,1.45,1.15,-0.15,1)}"
        "Testing{\\1c&HFEFEFE&} My Text";

    ASS_Track *joined_track = read_text_track(lib, joined_text);
    ASS_Track *same_style_track = read_text_track(lib, same_style_text);
    ASS_Track *split_style_track = read_text_track(lib, split_style_text);
    bool ok = joined_track && same_style_track && split_style_track &&
        capture(renderer, joined_track, 0, &joined) &&
        capture(renderer, same_style_track, 0, &same_style) &&
        capture(renderer, split_style_track, 0, &split_style);

    if (ok) {
        ok = !memcmp(joined.pixels, same_style.pixels, sizeof(joined.pixels)) &&
             memcmp(joined.pixels, split_style.pixels, sizeof(joined.pixels));
    }
    if (!ok)
        fprintf(stderr, "distort text-run grouping across whitespace/style boundary failed\n");

    if (joined_track) ass_free_track(joined_track);
    if (same_style_track) ass_free_track(same_style_track);
    if (split_style_track) ass_free_track(split_style_track);
    return ok;
}

static bool test_p0_axes(ASS_Library *lib, ASS_Renderer *renderer)
{
    static Mask normal, x_only, y_only;
    ASS_Track *base = read_track(lib, "\\distort(1,0,1,1,0,1)");
    ASS_Track *tx = read_track(lib, "\\distort(1,0,1,1,0,1,.25,0)");
    ASS_Track *ty = read_track(lib, "\\distort(1,0,1,1,0,1,0,.25)");
    bool ok = base && tx && ty && capture(renderer, base, 0, &normal) &&
        capture(renderer, tx, 0, &x_only) && capture(renderer, ty, 0, &y_only);
    if (ok) {
        int left = WIDTH, top = HEIGHT, right = 0, bottom = 0;
        for (int y = 0; y < HEIGHT; y++)
            for (int x = 0; x < WIDTH; x++)
                if (normal.pixels[IMAGE_TYPE_CHARACTER][y * WIDTH + x]) {
                    if (x < left) left = x;
                    if (y < top) top = y;
                    if (x + 1 > right) right = x + 1;
                    if (y + 1 > bottom) bottom = y + 1;
                }
        // Well inside the old rectangle, away from antialiased edges:
        // moving P0 X cuts into the left edge; P0 Y cuts into the top edge.
        int at_left = (top + (bottom - top) / 2) * WIDTH + left + (right - left) / 16;
        int at_top = (top + (bottom - top) / 16) * WIDTH + left + (right - left) / 2;
        ok = left < right && top < bottom &&
            !x_only.pixels[IMAGE_TYPE_CHARACTER][at_left] &&
            y_only.pixels[IMAGE_TYPE_CHARACTER][at_left] &&
            x_only.pixels[IMAGE_TYPE_CHARACTER][at_top] &&
            !y_only.pixels[IMAGE_TYPE_CHARACTER][at_top];
    }
    if (!ok)
        fprintf(stderr, "P0 X/Y did not move the expected rectangle edges\n");
    if (base) ass_free_track(base);
    if (tx) ass_free_track(tx);
    if (ty) ass_free_track(ty);
    return ok;
}

static bool test_animation(ASS_Library *lib, ASS_Renderer *renderer)
{
    // Binary fractions give exact static targets at each sampled time.
    const double start[8] = {1, 0, 1, 1, 0, 1, -0.25, 0.125};
    const double end[8] = {1.25, -0.125, 1.5, 1.25, -0.25, 1, 0.25, -0.125};
    const long long times[] = {0, 250, 500, 750, 1000, 1500, 500, 0};
    static Mask actual, expected;
    bool ok = true;
    for (int from8 = 0; from8 <= 1; from8++) {
        for (int to8 = 0; to8 <= 1; to8++) {
            for (int accel = 1; accel <= 2; accel++) {
                char animated[512];
                snprintf(animated, sizeof(animated),
                    "\\distort(1,0,1,1,0,1%s)"
                    "\\t(0,1000,%d,\\distort(1.25,-0.125,1.5,1.25,-0.25,1%s))",
                    from8 ? ",-.25,+.125" : "", accel,
                    to8 ? ",+.25,-.125" : "");
                ASS_Track *track = read_track(lib, animated);
                if (!track)
                    return false;
                for (size_t t = 0; t < sizeof(times) / sizeof(times[0]); t++) {
                    double pwr = times[t] < 1000 ? times[t] / 1000.0 : 1;
                    if (accel == 2)
                        pwr *= pwr;
                    double pin[8];
                    for (int i = 0; i < 8; i++) {
                        double a = i < 6 || from8 ? start[i] : 0;
                        double b = i < 6 || to8 ? end[i] : 0;
                        pin[i] = (1 - pwr) * a + b * pwr;
                    }
                    char fixed[512];
                    snprintf(fixed, sizeof(fixed),
                        "\\distort(%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g)",
                        pin[0], pin[1], pin[2], pin[3], pin[4], pin[5], pin[6], pin[7]);
                    ASS_Track *reference = read_track(lib, fixed);
                    // Render the same animated track across frames, including
                    // repeated and backwards times, to catch stale geometry.
                    bool match = reference && capture(renderer, track, times[t], &actual) &&
                        capture(renderer, reference, times[t], &expected) &&
                        !memcmp(actual.pixels, expected.pixels, sizeof(actual.pixels));
                    if (!match) {
                        fprintf(stderr, "%d -> %d arguments, accel %d, at %lld ms failed\n",
                                from8 ? 8 : 6, to8 ? 8 : 6, accel, times[t]);
                        ok = false;
                    }
                    if (reference) ass_free_track(reference);
                }
                ass_free_track(track);
            }
        }
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
    ass_set_storage_size(renderer, WIDTH, HEIGHT);
    ass_set_frame_size(renderer, WIDTH, HEIGHT);
    ass_set_fonts(renderer, NULL, "sans-serif", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);

    const char *identity = "\\distort(1,0,1,1,0,1)";
    const char *moved = "\\distort(1,0,1,1,0,1,0.2,0.1)";
    bool ok = check_pair(lib, renderer, identity, "", 0, true, "legacy identity");
    ok &= check_pair(lib, renderer, identity, "\\distort(1,0,1,1,0,1,0,0)",
                     0, true, "explicit zero P0");
    ok &= check_pair(lib, renderer, "\\distort(1.6,-0.2,1.6,1.2,-0.25,1)",
                     "\\distort(1.6,-0.2,1.6,1.2,-0.25,1,0,0)",
                     0, true, "legacy nontrivial warp");
    ok &= check_pair(lib, renderer, moved, identity, 0, false, "P0 changes geometry");
    ok &= check_pair(lib, renderer, "\\distort(1,0,1,1,0,1,0.25,0)",
                     identity, 0, false, "P0 X only");
    ok &= check_pair(lib, renderer, "\\distort(1,0,1,1,0,1,0,0.25)",
                     identity, 0, false, "P0 Y only");
    ok &= check_pair(lib, renderer, "\\distort(1,0,1,1,0,1,-0.25,-0.125)",
                     identity, 0, false, "negative fractional P0");
    ok &= check_pair(lib, renderer, "\\distort(1,0,1,1,0,1, +.2 , +.1 )",
                     moved, 0, true, "P0 numeric syntax");
    ok &= check_pair(lib, renderer,
                     "\\distort(1,0,1,1,0,1,.2,.1)\\distort(1,0,1,1,0,1)",
                     identity, 0, true, "six arguments clear P0");
    ok &= check_pair(lib, renderer,
                     "\\distort(1,0,1,1,0,1,.2,.1)\\r\\distort(1,0,1,1,0,1)",
                     identity, 0, true, "style reset clears P0");
    ok &= check_pair(lib, renderer, "\\distort(1,0,1,1,0,1,.2,.1)\\r",
                     "", 0, true, "style reset disables distortion");
    ok &= check_pair(lib, renderer,
                     "\\distort(1,0,1,1,0,1,.2,.1)\\distort(,,,,,,,)",
                     moved, 0, true, "empty extended coordinates retain state");
    ok &= check_pair(lib, renderer,
                     "\\distort(1,0,1,1,0,1,.2,.1)\\distort(,,,,,)",
                     identity, 0, true, "empty legacy coordinates clear P0");
    ok &= check_pair(lib, renderer,
                     "\\t(0,1000,\\distort(1,0,1,1,0,1,.5,.25))",
                     "\\distort(1,0,1,1,0,1,.25,.125)",
                     500, true, "P0-only animation from defaults");
    ok &= check_pair(lib, renderer,
                     "\\distort(1,0,1,1,0,1,.5,.25)\\t(0,1000,\\distort(1,0,1,1,0,1))",
                     "\\distort(1,0,1,1,0,1,.25,.125)",
                     500, true, "P0-only animation to legacy");

    // Rejected counts must leave an existing six- or eight-slot state alone.
    const char *invalid[] = {"", "2", "2,0", "2,0,2", "2,0,2,1", "2,0,2,1,0",
        "2,0,2,1,0,1,.5", "2,0,2,1,0,1,.5,.5,9", "2,0,2,1,0,1,.5,.5,"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        const char *states[] = {"", identity, moved};
        for (size_t s = 0; s < sizeof(states) / sizeof(states[0]); s++) {
            char tags[512];
            snprintf(tags, sizeof(tags), "%s\\distort(%s)", states[s], invalid[i]);
            ok &= check_pair(lib, renderer, tags, states[s], 0, true, tags);
        }
    }
    // The historical parser accepts a seventh *empty* slot. Keep that quirk.
    ok &= check_pair(lib, renderer, "\\distort(1.5,0,1.5,1,0,1, )",
                     "\\distort(1.5,0,1.5,1,0,1)", 0, true, "legacy trailing comma");
    ok &= check_pair(lib, renderer,
                     "\\distort(1.5,0,1.5,1,0,1)\\distort(,,-0.25,,,)",
                     "\\distort(1.5,0,-0.25,1,0,1)", 0, true, "legacy empty slots");

    // Exercise the other consumer of the shared mapping and all glyph layers.
    ok &= check_pair(lib, renderer,
                     "\\bs4\\boxp8\\1a&HFF&\\distort(1,0,1,1,0,1,.25,.125)",
                     "\\bs4\\boxp8\\1a&HFF&\\distort(1,0,1,1,0,1)",
                     0, false, "BS4 P0 geometry");
    ok &= check_pair(lib, renderer,
                     "\\bord3\\shad4\\distort(1,0,1,1,0,1,.25,.125)"
                     "\\t(0,1000,\\distort(1,0,1,1,0,1))",
                     "\\bord3\\shad4\\distort(1,0,1,1,0,1,.125,.0625)",
                     500, true, "animated fill border and shadow");
    ok &= test_text_run_grouping(lib, renderer);
    ok &= test_p0_axes(lib, renderer);
    ok &= test_animation(lib, renderer);

    ass_renderer_done(renderer);
    ass_library_done(lib);
    return ok ? 0 : 1;
}
