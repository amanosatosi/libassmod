/* Renderer-level distortion regressions, using pixel-mask comparisons as in
 * bs4_geometry_selftest.c and retained scalar glyph state for grouping checks.
 * Most cases use drawings to avoid font dependence; text-run grouping is
 * checked with same-font relative masks. */

#include <stdbool.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ass.h"
#include "ass_render.h"

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

#define MULTILINE "M\\NMMMMMM\\NMM"
#define SPLIT_LINES "M\\N{\\1c&HFEFEFE&}MMMMMM\\N{\\1c&HFFFFFF&}MM"
#define TEXT_PREFIX "\\q2\\an5\\pos(320,180)\\fs32"
#define LEGACY_PIN "\\distort(1,-.125,1.375,1.125,-.125,1)"
#define EXTENDED_PIN "\\distort(1,-.125,1.375,1.125,-.125,1,0,0)"

static ASS_Track *text_case(ASS_Library *lib, const char *tags, const char *body)
{
    char text[4096];
    int n = snprintf(text, sizeof(text), "{%s%s}%s", TEXT_PREFIX, tags, body);
    if (n < 0 || n >= (int) sizeof(text))
        return NULL;
    return read_text_track(lib, text);
}

static bool check_text_pair(ASS_Library *lib, ASS_Renderer *renderer,
                            const char *a, const char *body_a,
                            const char *b, const char *body_b,
                            bool equal, const char *label)
{
    static Mask actual, expected;
    ASS_Track *ta = text_case(lib, a, body_a);
    ASS_Track *tb = text_case(lib, b, body_b);
    bool ok = ta && tb && capture(renderer, ta, 0, &actual) &&
        capture(renderer, tb, 0, &expected);
    if (ok)
        ok = (!memcmp(actual.pixels, expected.pixels, sizeof(actual.pixels))) == equal;
    if (!ok)
        fprintf(stderr, "%s: text pixel comparison failed\n", label);
    if (ta) ass_free_track(ta);
    if (tb) ass_free_track(tb);
    return ok;
}

static bool same_box(ASS_DRect a, ASS_DRect b)
{
    return a.x_min == b.x_min && a.y_min == b.y_min &&
           a.x_max == b.x_max && a.y_max == b.y_max;
}

static bool test_multiline_identity(ASS_Library *lib, ASS_Renderer *renderer)
{
    static Mask mask, actual, expected;
    ASS_Track *plain = text_case(lib, "", MULTILINE);
    ASS_Track *identity = text_case(lib, "\\distort(1,0,1,1,0,1)", MULTILINE);
    bool ok = plain && identity && capture(renderer, plain, 0, &mask);
    ASS_Rect bbox[11];
    ASS_Vector pos[11];
    if (ok) {
        for (int i = 0; i < 11; i++) {
            const GlyphInfo *g = &renderer->state.text_info.glyphs[i];
            bbox[i] = g->bbox;
            pos[i] = g->pos;
        }
        ok = capture(renderer, identity, 0, &actual);
    }
    if (ok) {
        const GlyphInfo *g = renderer->state.text_info.glyphs;
        int dx = g[0].pos.x - pos[0].x, dy = g[0].pos.y - pos[0].y;
        for (int i = 0; i < 11; i++) {
            if (g[i].skip || g[i].symbol == '\n') continue;
            // Identity must preserve outline geometry and relative layout.
            // Distorted text already anchors its final ink bounds, whereas
            // ordinary text uses layout metrics. That common placement shift
            // is legacy behavior, even when the distortion pins are identity.
            ok &= g[i].bbox.x_min == bbox[i].x_min &&
                  g[i].bbox.y_min == bbox[i].y_min &&
                  g[i].bbox.x_max == bbox[i].x_max &&
                  g[i].bbox.y_max == bbox[i].y_max &&
                  g[i].pos.x - pos[i].x == dx && g[i].pos.y - pos[i].y == dy &&
                  !g[i].distort_extended;
        }
        // Place the independent ordinary-text reference at the same origin;
        // use a single \pos because ASS accepts only the first position tag.
        char text[256];
        snprintf(text, sizeof(text), "{\\q2\\an5\\pos(%.17g,%.17g)\\fs32}%s",
            320 + dx / 64.0, 180 + dy / 64.0, MULTILINE);
        ASS_Track *reference = read_text_track(lib, text);
        ok &= reference && capture(renderer, reference, 0, &expected) &&
              !memcmp(actual.pixels, expected.pixels, sizeof(actual.pixels));
        if (reference) ass_free_track(reference);
    }
    if (!ok) fprintf(stderr, "six-slot multiline identity geometry/mask failed\n");
    if (plain) ass_free_track(plain);
    if (identity) ass_free_track(identity);
    return ok;
}

static bool test_inline_mode_boundary(ASS_Library *lib, ASS_Renderer *renderer)
{
    static Mask mask;
    ASS_Track *track = text_case(lib, EXTENDED_PIN,
        "M{\\distort(1,-.125,1.375,1.125,-.125,1)}MMMMMM\\NMM");
    bool ok = track && capture(renderer, track, 0, &mask);
    if (ok) {
        const GlyphInfo *g = renderer->state.text_info.glyphs;
        ok = g[0].distort_extended && !g[1].distort_extended &&
             g[0].outline && g[0].outline->outline[0].n_points;
        if (ok) {
            // The first extended unit contains just one M. Its source bbox
            // must exclude the following legacy unit even with identical
            // numerical pins. Observe geometry without inserting a style
            // override that would also change shaping/composite runs.
            const ASS_Outline *ol = &g[0].outline->outline[0];
            ASS_DRect box = {DBL_MAX, DBL_MAX, -DBL_MAX, -DBL_MAX};
            for (size_t i = 0; i < ol->n_points; i++) {
                double x = ol->points[i].x * g[0].transform.scale.x;
                double y = ol->points[i].y * g[0].transform.scale.y;
                box.x_min = fmin(box.x_min, x);
                box.x_max = fmax(box.x_max, x);
                box.y_min = fmin(box.y_min, y);
                box.y_max = fmax(box.y_max, y);
            }
            ok = fabs((box.x_max - box.x_min) -
                      (g[0].distort_bbox.x_max - g[0].distort_bbox.x_min)) <= 1e-6 &&
                 fabs((box.y_max - box.y_min) -
                      (g[0].distort_bbox.y_max - g[0].distort_bbox.y_min)) <= 1e-6;
        }
    }
    if (!ok) fprintf(stderr, "inline mode boundary source bbox failed\n");
    if (track) ass_free_track(track);
    return ok;
}

/* Event cleanup frees warped outlines and linked glyphs, but retains the root
 * glyphs' scalar observations. Read them before another frame can reuse them. */
static bool check_shared_bbox(ASS_Library *lib, ASS_Renderer *renderer,
                              const char *body, const char *symbols)
{
    static Mask mask;
    ASS_Track *warped = text_case(lib, EXTENDED_PIN, body);
    ASS_Track *plain = text_case(lib, "", body);
    bool ok = warped && plain && capture(renderer, warped, 0, &mask);
    ASS_DRect shared = {0};
    if (ok) {
        const GlyphInfo *glyphs = renderer->state.text_info.glyphs;
        shared = glyphs[0].distort_bbox;
        ok = shared.x_min < shared.x_max && shared.y_min < shared.y_max;
        for (size_t i = 0; symbols[i]; i++) {
            const GlyphInfo *g = &glyphs[i];
            ok &= g->symbol == (unsigned char) symbols[i] && g->distort_extended;
            if (symbols[i] != '\n' && !g->skip && g->outline &&
                    g->outline->outline[0].n_points)
                ok &= same_box(shared, g->distort_bbox);
        }
    }
    if (ok)
        ok = capture(renderer, plain, 0, &mask);
    if (ok) {
        ASS_DRect source = {DBL_MAX, DBL_MAX, -DBL_MAX, -DBL_MAX};
        const GlyphInfo *glyphs = renderer->state.text_info.glyphs;
        for (size_t i = 0; symbols[i]; i++) {
            const GlyphInfo *g = &glyphs[i];
            if (g->skip || g->symbol == '\n' || !g->outline)
                continue;
            const ASS_Outline *ol = &g->outline->outline[0];
            for (size_t j = 0; j < ol->n_points; j++) {
                double x = ol->points[j].x * g->transform.scale.x +
                           g->transform.offset.x + g->pos.x;
                double y = ol->points[j].y * g->transform.scale.y +
                           g->transform.offset.y + g->pos.y;
                source.x_min = fmin(source.x_min, x);
                source.x_max = fmax(source.x_max, x);
                source.y_min = fmin(source.y_min, y);
                source.y_max = fmax(source.y_max, y);
            }
        }
        // Device placement adds one common translation after layout; it must
        // not change the source union's dimensions. Allow one d6 rounding unit.
        ok = fabs((source.x_max - source.x_min) - (shared.x_max - shared.x_min)) <= 1 &&
             fabs((source.y_max - source.y_min) - (shared.y_max - shared.y_min)) <= 1;
    }
    if (!ok)
        fprintf(stderr, "shared source bbox failed: %s\n", body);
    if (warped) ass_free_track(warped);
    if (plain) ass_free_track(plain);
    return ok;
}

static bool test_multiline(ASS_Library *lib, ASS_Renderer *renderer)
{
    bool ok = check_shared_bbox(lib, renderer, "MM\\NMMMMMM", "MM\nMMMMMM");
    ok &= check_shared_bbox(lib, renderer, MULTILINE, "M\nMMMMMM\nMM");
    ok &= test_multiline_identity(lib, renderer);
    ok &= check_text_pair(lib, renderer, LEGACY_PIN, MULTILINE,
        LEGACY_PIN, SPLIT_LINES, true, "six slots remain line-local");
    ok &= check_text_pair(lib, renderer, LEGACY_PIN, MULTILINE,
        EXTENDED_PIN, MULTILINE, false, "zero P0 still selects multiline form");
    ok &= check_text_pair(lib, renderer, LEGACY_PIN, "MMMMMM",
        EXTENDED_PIN, "MMMMMM", true, "single-line legacy/extended equivalence");
    ok &= check_text_pair(lib, renderer, EXTENDED_PIN, MULTILINE,
        EXTENDED_PIN, SPLIT_LINES, false, "real styles still split extended units");
    ok &= check_text_pair(lib, renderer, EXTENDED_PIN, MULTILINE,
        EXTENDED_PIN, "M\\N{\\1c&HFFFFFF&}MMMMMM\\NMM", true,
        "equivalent styles preserve multiline unit");
    ok &= check_shared_bbox(lib, renderer, "M M\\NMM MM", "M M\nMM MM");
    ok &= check_text_pair(lib, renderer, EXTENDED_PIN, "M\\hM\\NMM\\hMM",
        EXTENDED_PIN, "M\\hM\\N{\\1c&HFFFFFF&}MM\\hMM", true,
        "NBSP and no-op override preserve multiline unit");

    // Equal visible corner values cannot hide a change of syntax mode.
    ok &= test_inline_mode_boundary(lib, renderer);
    ok &= check_text_pair(lib, renderer, EXTENDED_PIN,
        "M\\N{\\distort(1,-.125,1.375,1.125,-.125,1)}MMMMMM\\NMM",
        EXTENDED_PIN, "M\\N{\\1c&HFEFEFE&\\distort(1,-.125,1.375,1.125,-.125,1)}MMMMMM\\NMM",
        true, "mode change is a unit boundary");
    ok &= check_text_pair(lib, renderer, EXTENDED_PIN,
        "M\\N{\\distort(1,0,1.5,1,0,1,0,0)}MMMMMM\\NMM",
        EXTENDED_PIN, MULTILINE, false, "parameter change starts a new unit");
    const struct { const char *body; bool same_last_two; } boundaries[] = {
        {SPLIT_LINES, false},
        {"M\\N{\\distort(1,0,1.5,1,0,1,0,0)}MMMMMM\\NMM", true},
    };
    static Mask mask;
    for (size_t i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); i++) {
        ASS_Track *track = text_case(lib, EXTENDED_PIN, boundaries[i].body);
        bool match = track && capture(renderer, track, 0, &mask);
        if (match) {
            const GlyphInfo *g = renderer->state.text_info.glyphs;
            // The six Ms on line two share a domain, as do the two on line
            // three. A changed pin still lets the latter two lines compact;
            // distinct effective styles keep all three lines separate.
            match = !same_box(g[0].distort_bbox, g[2].distort_bbox) &&
                same_box(g[2].distort_bbox, g[7].distort_bbox) &&
                same_box(g[9].distort_bbox, g[10].distort_bbox) &&
                same_box(g[2].distort_bbox, g[9].distort_bbox) == boundaries[i].same_last_two;
        }
        if (!match) fprintf(stderr, "multiline unit boundary bbox failed: %s\n",
                            boundaries[i].body);
        ok &= match;
        if (track) ass_free_track(track);
    }
    // Separate drawing chunks keep their existing source rectangles.
    ok &= check_text_pair(lib, renderer, LEGACY_PIN,
        "{\\p1}m 0 0 l 40 0 40 30 0 30{\\p0}\\N{\\p1}m 0 0 l 80 0 80 20 0 20",
        EXTENDED_PIN,
        "{\\p1}m 0 0 l 40 0 40 30 0 30{\\p0}\\N{\\p1}m 0 0 l 80 0 80 20 0 20",
        true, "drawing chunks retain legacy grouping");
    return ok;
}

static bool test_form_state(ASS_Library *lib, ASS_Renderer *renderer)
{
    const struct { const char *tags; bool enabled, extended; double u0, v0; } cases[] = {
        {EXTENDED_PIN, true, true, 0, 0},
        {"\\distort(1,0,1,1,0,1,.25,.125)" LEGACY_PIN, true, false, 0, 0},
        {EXTENDED_PIN "\\r", false, false, 0, 0},
        {EXTENDED_PIN "\\distort(,,,,,)", true, false, 0, 0},
        {EXTENDED_PIN "\\distort(1,0,1,1,0,1, )", true, false, 0, 0},
        {EXTENDED_PIN "\\distort(1,0,1,1,0,1,.5)", true, true, 0, 0},
        {EXTENDED_PIN "\\distort(,,,,,,,)", true, true, 0, 0},
        {"\\distort(1,0,1,1,0,1,.25,.125)\\t(0,1000,\\distort(1,0,1,1,0,1))",
            true, false, .25, .125},
    };
    static Mask mask;
    bool ok = true;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ASS_Track *track = text_case(lib, cases[i].tags, MULTILINE);
        bool match = track && capture(renderer, track, 0, &mask);
        const RenderContext *s = &renderer->state;
        match &= s->distort_enabled == cases[i].enabled &&
                 s->distort_extended == cases[i].extended &&
                 s->distort.u0 == cases[i].u0 && s->distort.v0 == cases[i].v0;
        if (!match)
            fprintf(stderr, "distort syntax mode/reset failed: %s\n", cases[i].tags);
        ok &= match;
        if (track) ass_free_track(track);
    }
    ok &= check_text_pair(lib, renderer, EXTENDED_PIN LEGACY_PIN, MULTILINE,
        LEGACY_PIN, MULTILINE, true, "ordinary six slots restore line-local form");
    ok &= check_text_pair(lib, renderer, EXTENDED_PIN "\\r", MULTILINE,
        "\\r", MULTILINE, true, "reset disables multiline distortion");
    return ok;
}

static ASS_Renderer *fresh_renderer(ASS_Library *lib)
{
    ASS_Renderer *renderer = ass_renderer_init(lib);
    if (renderer) {
        ass_set_storage_size(renderer, WIDTH, HEIGHT);
        ass_set_frame_size(renderer, WIDTH, HEIGHT);
        ass_set_fonts(renderer, NULL, "sans-serif", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    }
    return renderer;
}

static bool test_multiline_animation(ASS_Library *lib, ASS_Renderer *renderer)
{
    const double from[8] = {1, 0, 1, 1, 0, 1, -.25, .125};
    const double to[8] = {1.25, -.125, 1.5, 1.25, -.25, 1, .25, -.125};
    const long long times[] = {1500, 0, 500, 250, 1000, 750, 500, 0};
    static Mask actual, expected, fresh;
    bool ok = true;
    for (int from8 = 0; from8 <= 1; from8++)
        for (int to8 = 0; to8 <= 1; to8++)
            for (int accel = 1; accel <= 2; accel++) {
                char tags[512];
                snprintf(tags, sizeof(tags),
                    "\\distort(1,0,1,1,0,1%s)"
                    "\\t(0,1000,%d,\\distort(1.25,-.125,1.5,1.25,-.25,1%s))",
                    from8 ? ",-.25,.125" : "", accel, to8 ? ",.25,-.125" : "");
                ASS_Track *track = text_case(lib, tags, MULTILINE);
                if (!track) return false;
                for (size_t t = 0; t < sizeof(times) / sizeof(times[0]); t++) {
                    double k = times[t] < 1000 ? times[t] / 1000.0 : 1;
                    if (accel == 2) k *= k;
                    double pin[8];
                    for (int i = 0; i < 8; i++)
                        pin[i] = (1 - k) * (i < 6 || from8 ? from[i] : 0) +
                                 k * (i < 6 || to8 ? to[i] : 0);
                    snprintf(tags, sizeof(tags),
                        "\\distort(%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g)",
                        pin[0], pin[1], pin[2], pin[3], pin[4], pin[5], pin[6], pin[7]);
                    // Explicit style boundaries give the static eight-pin
                    // reference legacy line-local domains even during a
                    // six-slot target's still-nonzero P0 interpolation.
                    ASS_Track *reference = text_case(lib, tags, to8 ? MULTILINE : SPLIT_LINES);
                    bool match = reference && capture(renderer, track, times[t], &actual) &&
                        renderer->state.distort_extended == (bool) to8 &&
                        renderer->state.text_info.glyphs[0].distort_extended == (bool) to8 &&
                        capture(renderer, reference, times[t], &expected) &&
                        !memcmp(actual.pixels, expected.pixels, sizeof(actual.pixels));
                    ASS_Renderer *direct = fresh_renderer(lib);
                    match &= direct && capture(direct, track, times[t], &fresh) &&
                        direct->state.distort_extended == (bool) to8 &&
                        !memcmp(actual.pixels, fresh.pixels, sizeof(actual.pixels));
                    if (!match)
                        fprintf(stderr, "multiline %d->%d, accel %d, seek %lld failed\n",
                            from8 ? 8 : 6, to8 ? 8 : 6, accel, times[t]);
                    ok &= match;
                    if (direct) ass_renderer_done(direct);
                    if (reference) ass_free_track(reference);
                }
                ass_free_track(track);
            }
    return ok;
}

static bool test_multiline_layers_and_anchor(ASS_Library *lib, ASS_Renderer *renderer)
{
    static Mask mask;
    ASS_Track *track = text_case(lib, EXTENDED_PIN "\\bord3\\shad4", MULTILINE);
    bool ok = track && capture(renderer, track, 0, &mask);
    if (ok) {
        bool fill = false, border = false, shadow = false;
        for (int y = 0; y < HEIGHT - 4; y++)
            for (int x = 0; x < WIDTH - 4; x++) {
                size_t p = y * WIDTH + x, shifted = (y + 4) * WIDTH + x + 4;
                fill |= mask.pixels[IMAGE_TYPE_CHARACTER][p] != 0;
                border |= mask.pixels[IMAGE_TYPE_OUTLINE][p] != 0;
                shadow |= mask.pixels[IMAGE_TYPE_SHADOW][shifted] != 0;
                ok &= mask.pixels[IMAGE_TYPE_OUTLINE][p] ==
                      mask.pixels[IMAGE_TYPE_SHADOW][shifted];
            }
        ok &= fill && border && shadow;
    }
    if (track) ass_free_track(track);
    if (!ok) fprintf(stderr, "multiline outline/shadow alignment failed\n");
    ok &= check_text_pair(lib, renderer,
        "\\bord3\\shad4\\2bs2" LEGACY_PIN, "MMMMMM",
        "\\bord3\\shad4\\2bs2" EXTENDED_PIN, "MMMMMM", true,
        "single-line multi-border compatibility");
    ok &= check_text_pair(lib, renderer, "\\bs4\\boxp8\\distort(1,0,1,1,0,1)", MULTILINE,
        "\\bs4\\boxp8\\distort(1,0,1,1,0,1,0,0)", MULTILINE, true,
        "multiline BS4 identity compatibility");
    ok &= check_text_pair(lib, renderer, "\\bs4\\boxp8" EXTENDED_PIN, MULTILINE,
        "\\bs4\\boxp8" LEGACY_PIN, MULTILINE, false,
        "BS4 follows extended multiline geometry");

    for (int anchor = 1; anchor <= 9; anchor++) {
        char tags[256];
        snprintf(tags, sizeof(tags), "\\wtan%d%s", anchor, EXTENDED_PIN);
        track = text_case(lib, tags, MULTILINE);
        bool match = track && capture(renderer, track, 0, &mask);
        ASS_Rect bounds = {INT_MAX, INT_MAX, INT_MIN, INT_MIN};
        if (match) {
            // Eleven retained roots: M, break, six Ms, break, two Ms.
            for (int i = 0; i < 11; i++) {
                const GlyphInfo *g = &renderer->state.text_info.glyphs[i];
                if (g->skip || g->symbol == '\n') continue;
                rectangle_update(&bounds, g->pos.x + g->bbox.x_min,
                    g->pos.y + g->bbox.y_min, g->pos.x + g->bbox.x_max,
                    g->pos.y + g->bbox.y_max);
            }
            int hx = (anchor - 1) % 3, vy = (anchor - 1) / 3;
            double x = hx == 0 ? bounds.x_min : hx == 2 ? bounds.x_max :
                (bounds.x_min + bounds.x_max) / 2.0;
            double y = vy == 0 ? bounds.y_max : vy == 2 ? bounds.y_min :
                (bounds.y_min + bounds.y_max) / 2.0;
            match &= fabs(x - 320 * 64) <= 1 && fabs(y - 180 * 64) <= 1;
        }
        if (!match) fprintf(stderr, "extended multiline wtan%d anchor failed\n", anchor);
        ok &= match;
        if (track) ass_free_track(track);
    }
    // Preserve ordinary projection toggles after distortion in both forms.
    ok &= check_pair(lib, renderer, "\\frx30\\fry20\\ortho0" LEGACY_PIN,
        "\\frx30\\fry20\\ortho0" EXTENDED_PIN, 0, true, "ordinary 3D projection remains");
    ok &= check_pair(lib, renderer, "\\frx30\\fry20\\ortho1" LEGACY_PIN,
        "\\frx30\\fry20\\ortho1" EXTENDED_PIN, 0, true, "orthographic projection remains");
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
    ok &= test_multiline(lib, renderer);
    ok &= test_form_state(lib, renderer);
    ok &= test_multiline_animation(lib, renderer);
    ok &= test_multiline_layers_and_anchor(lib, renderer);

    ass_renderer_done(renderer);
    ass_library_done(lib);
    return ok ? 0 : 1;
}
