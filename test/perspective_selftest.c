/* Renderer-level true-perspective regressions using public libass output. */

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
    (void) level; (void) fmt; (void) va; (void) data;
}

static ASS_Track *read_track_at(ASS_Library *lib, const char *tags,
                                const char *placement)
{
    char script[8192];
    int n = snprintf(script, sizeof(script),
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n"
        "ScaledBorderAndShadow: yes\n[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,40,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,"
        "0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1\n[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:03.00,Default,,0,0,0,,"
        "{%s\\an7%s\\p1}%s\n", tags, placement, shape);
    if (n < 0 || n >= (int) sizeof(script))
        return NULL;
    return ass_read_memory(lib, script, strlen(script), NULL);
}

static ASS_Track *read_track(ASS_Library *lib, const char *tags)
{
    return read_track_at(lib, tags, "\\pos(240,120)");
}

static ASS_Track *read_text_track(ASS_Library *lib, const char *tags,
                                  const char *text)
{
    char script[8192];
    int n = snprintf(script, sizeof(script),
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n"
        "ScaledBorderAndShadow: yes\n[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,40,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,"
        "0,0,0,0,100,100,0,0,1,0,0,5,0,0,0,1\n[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:03.00,Default,,0,0,0,,"
        "{\\pos(320,180)%s}%s\n", tags, text);
    if (n < 0 || n >= (int) sizeof(script)) return NULL;
    return ass_read_memory(lib, script, strlen(script), NULL);
}

static bool test_plane_layout(ASS_Library *lib, ASS_Renderer *renderer)
{
    static const char *plane =
        "\\perspective(1,0.12,0,0.04,1,0,0.0005,0.0003,1)";
    static const char *identity =
        "\\perspective(1,0,0,0,1,0,0,0,1)";
    static const char *cases[][2] = {
        {"\\an5\\fs40", "TEST"},
        {"\\an5\\fs80", "TEST"},
        {"\\an5\\fscx150\\fscy70", "TEST"},
        {"\\an5\\scale150", "TEST"},
        {"\\an5\\fsp8", "A longer phrase"},
        {"\\an8", "Line 1\\NLine 2"},
        {"\\an8", "Line 1\\NLine 2\\NLine 3"},
        {"\\an5", "Line 1\\NLine 2\\NLine 3"},
        {"\\an2", "Line 1\\NLine 2\\NLine 3"},
        {"\\an7", "Line 1\\NLine 2\\NLine 3"},
        {"\\an9", "Line 1\\NLine 2\\NLine 3"},
        {"\\an4", "Line 1\\NLine 2\\NLine 3"},
        {"\\an6", "Line 1\\NLine 2\\NLine 3"},
        {"\\an1", "Line 1\\NLine 2\\NLine 3"},
        {"\\an3", "Line 1\\NLine 2\\NLine 3"},
        {"\\an5\\frx20\\fry-15\\frz10", "TEST"},
        {"\\an5\\t(0,1000,\\fs80\\frx20\\fry-15)", "TEST"},
        {"\\an5\\t(0,1000,\\fscx160\\fscy70)", "TEST"},
        {"\\an5\\t(0,1000,\\scale150)", "TEST"},
        {"\\an5\\furis80", "<A|B>\\N<C|D>"},
        {"\\an5\\bord4\\shad3\\box1", "BOX"},
        {"\\an5\\k20", "KARAOKE"},
    };
    bool ok = true;
    static Mask projected, plain, first, changed, identity_mask;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char tagged[512];
        snprintf(tagged, sizeof(tagged), "%s%s", plane, cases[i][0]);
        ASS_Track *a = read_text_track(lib, tagged, cases[i][1]);
        ASS_Track *b = read_text_track(lib, cases[i][0], cases[i][1]);
        snprintf(tagged, sizeof(tagged), "%s%s", identity, cases[i][0]);
        ASS_Track *c = read_text_track(lib, tagged, cases[i][1]);
        bool current = a && b && capture(renderer, a, 500, &projected) &&
            c && capture(renderer, b, 500, &plain) &&
            capture(renderer, c, 500, &identity_mask) &&
            memcmp(projected.pixels, plain.pixels, sizeof(plain.pixels)) != 0 &&
            !memcmp(identity_mask.pixels, plain.pixels, sizeof(plain.pixels));
        if (!current) fprintf(stderr, "plane layout case %zu failed\n", i);
        ok &= current;
        if (a) ass_free_track(a);
        if (b) ass_free_track(b);
        if (c) ass_free_track(c);
    }
    ASS_Track *small = read_text_track(lib, plane, "Hi");
    ASS_Track *longer = read_text_track(lib, plane, "This is a much longer sentence");
    ok &= small && longer && capture(renderer, small, 500, &first) &&
        capture(renderer, longer, 500, &changed) &&
        memcmp(first.pixels, changed.pixels, sizeof(first.pixels)) != 0;
    if (small) ass_free_track(small);
    if (longer) ass_free_track(longer);
    return ok;
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
            if (py < 0 || py >= HEIGHT) continue;
            for (int x = 0; x < img->w; x++) {
                int px = img->dst_x + x;
                if (px < 0 || px >= WIDTH) continue;
                uint8_t alpha = (img->bitmap[y * img->stride + x] *
                    (255 - (uint8_t) img->color) + 127) / 255;
                uint8_t *dst = &mask->pixels[img->type][py * WIDTH + px];
                if (alpha > *dst) *dst = alpha;
                mask->covered |= alpha != 0;
            }
        }
    }
    return mask->covered;
}

static bool compare(ASS_Library *lib, ASS_Renderer *renderer,
                    const char *a, const char *b, long long now,
                    bool equal, const char *label)
{
    static Mask actual, expected;
    ASS_Track *ta = read_track(lib, a), *tb = read_track(lib, b);
    bool ok = ta && tb && capture(renderer, ta, now, &actual) &&
        capture(renderer, tb, now, &expected);
    if (ok)
        ok = (!memcmp(actual.pixels, expected.pixels, sizeof(actual.pixels))) == equal;
    if (!ok)
        fprintf(stderr, "%s failed at %lld ms\n", label, now);
    if (ta) ass_free_track(ta);
    if (tb) ass_free_track(tb);
    return ok;
}

static bool translated_equal(const Mask *a, const Mask *b, int dx, int dy)
{
    for (int type = 0; type < 3; type++) {
        for (int y = 0; y < HEIGHT; y++) {
            for (int x = 0; x < WIDTH; x++) {
                int bx = x + dx, by = y + dy;
                uint8_t expected = bx >= 0 && bx < WIDTH && by >= 0 && by < HEIGHT ?
                    b->pixels[type][by * WIDTH + bx] : 0;
                if (a->pixels[type][y * WIDTH + x] != expected)
                    return false;
            }
        }
    }
    return true;
}

static bool test_positioning(ASS_Library *lib, ASS_Renderer *renderer,
                             const char *perspective)
{
    static Mask original, shifted, moving, midpoint;
    ASS_Track *a = read_track_at(lib, perspective, "\\pos(240,120)");
    ASS_Track *b = read_track_at(lib, perspective, "\\pos(300,150)");
    ASS_Track *c = read_track_at(lib, perspective,
        "\\move(240,120,300,150,0,1000)");
    ASS_Track *d = read_track_at(lib, perspective, "\\pos(270,135)");
    bool ok = a && b && c && d && capture(renderer, a, 500, &original) &&
        capture(renderer, b, 500, &shifted) &&
        capture(renderer, c, 500, &moving) &&
        capture(renderer, d, 500, &midpoint) &&
        translated_equal(&original, &shifted, 60, 30) &&
        !memcmp(moving.pixels, midpoint.pixels, sizeof(moving.pixels));
    if (!ok)
        fprintf(stderr, "perspective pos/move anchor behavior failed\n");
    if (a) ass_free_track(a);
    if (b) ass_free_track(b);
    if (c) ass_free_track(c);
    if (d) ass_free_track(d);
    return ok;
}

int main(void)
{
    ASS_Library *lib = ass_library_init();
    if (!lib) return 1;
    ass_set_message_cb(lib, msg_cb, NULL);
    ASS_Renderer *renderer = ass_renderer_init(lib);
    if (!renderer) { ass_library_done(lib); return 1; }
    ass_set_storage_size(renderer, WIDTH, HEIGHT);
    ass_set_frame_size(renderer, WIDTH, HEIGHT);
    ass_set_fonts(renderer, NULL, "sans-serif", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);

    const char *identity = "\\perspective(0,0,160,0,160,96,0,96)";
    const char *trapezoid = "\\perspective(30,0,130,0,180,96,-20,96)";
    const char *side = "\\perspective(0,10,150,0,130,96,20,86)";
    const char *extreme = "\\perspective(79.9,0,80.1,0,1000,96,-900,96)";
    bool ok = compare(lib, renderer, identity, "", 0, true,
                      "identity/no-perspective regression");
    ok &= compare(lib, renderer, trapezoid, identity, 0, false,
                  "trapezoid changes geometry");
    ok &= compare(lib, renderer, side, identity, 0, false,
                  "side perspective changes geometry");
    ok &= compare(lib, renderer, extreme, identity, 0, false,
                  "extreme valid perspective");
    ok &= compare(lib, renderer,
        "\\perspective(0,0,160,0,160,96,0,96)",
        "\\perspective(0,0,160,0,160,96,0)", 0, true,
        "malformed tag ignored");
    ok &= compare(lib, renderer,
        "\\perspective(1,0,0,0,1,0,0,0,2)", "", 0, true,
        "invalid plane version ignored");
    ok &= compare(lib, renderer,
        "\\perspective(0,0,10,0,20,0,30,0)", "", 0, true,
        "degenerate fallback");
    ok &= compare(lib, renderer,
        "\\perspective(0,0,160,96,160,0,0,96)", "", 0, true,
        "self-crossing fallback");
    ok &= compare(lib, renderer,
        "\\distort(1.2,-.1,1.1,1.1,-.1,1)\\perspective(30,0,130,0,180,96,-20,96)",
        "\\perspective(30,0,130,0,180,96,-20,96)", 0, false,
        "distort then perspective");
    ok &= compare(lib, renderer,
        "\\perspective(0,0,160,0,160,96,0,96)\\bord12\\shad8\\blur2",
        "\\bord12\\shad8\\blur2", 0, true,
        "border shadow blur do not redefine identity plane");

    const char *animated =
        "\\perspective(0,0,160,0,160,96,0,96)"
        "\\t(0,1000,\\perspective(30,0,130,0,180,96,-20,96))";
    const char *midpoint = "\\perspective(15,0,145,0,170,96,-10,96)";
    ok &= compare(lib, renderer, animated, midpoint, 500, true,
                  "corner interpolation at midpoint");
    ok &= compare(lib, renderer, animated, identity, 0, true,
                  "animated seek to start");
    ok &= compare(lib, renderer, animated, midpoint, 500, true,
                  "animated seek determinism");
    ok &= compare(lib, renderer,
        "\\perspective(1,0,0,0,1,0,0,0,1)"
        "\\t(0,1000,\\perspective(1,0.2,0,0.1,1,0,0.001,0,1))",
        "\\perspective(1,0.1,0,0.05,1,0,0.0005,0,1)", 500, true,
        "plane coefficient interpolation");
    ok &= test_positioning(lib, renderer, trapezoid);
    ok &= test_positioning(lib, "\\perspective(1,0.12,0,0.04,1,0,0.0005,0.0003,1)");
    ok &= test_plane_layout(lib, renderer);

    ass_renderer_done(renderer);
    ass_library_done(lib);
    return ok ? 0 : 1;
}
