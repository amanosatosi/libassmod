#include <stdarg.h>
#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ass.h"

typedef struct {
    int count;
    int min_x, min_y, max_x, max_y;
    uint64_t coverage;
    uint64_t hash;
} RenderSig;

static void msg_cb(int level, const char *fmt, va_list va, void *data)
{
    (void) level;
    (void) fmt;
    (void) va;
    (void) data;
}

static void hash_u8(uint64_t *hash, uint8_t value)
{
    *hash ^= value;
    *hash *= 1099511628211ULL;
}

static void hash_u32(uint64_t *hash, uint32_t value)
{
    for (int i = 0; i < 4; i++)
        hash_u8(hash, (uint8_t) (value >> (8 * i)));
}

static void hash_i32(uint64_t *hash, int value)
{
    hash_u32(hash, (uint32_t) value);
}

static ASS_Track *read_case_track(ASS_Library *lib, const char *text)
{
    char script[8192];
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
        "Style: Default,Arial,42,&H00FFFFFF,&H00FFFFFF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,%s\n",
        text);
    if (n < 0 || n >= (int) sizeof(script))
        return NULL;

    return ass_read_memory(lib, script, strlen(script), NULL);
}

static bool render_case(ASS_Library *lib, ASS_Renderer *renderer,
                        const char *text, RenderSig *sig)
{
    ASS_Track *track = read_case_track(lib, text);
    if (!track)
        return false;

    int change = 0;
    ASS_Image *img = ass_render_frame(renderer, track, 0, &change);
    (void) change;

    memset(sig, 0, sizeof(*sig));
    sig->min_x = sig->min_y = INT_MAX;
    sig->max_x = sig->max_y = INT_MIN;
    sig->hash = 1469598103934665603ULL;
    for (ASS_Image *cur = img; cur; cur = cur->next) {
        sig->count++;
        if (cur->dst_x < sig->min_x) sig->min_x = cur->dst_x;
        if (cur->dst_y < sig->min_y) sig->min_y = cur->dst_y;
        if (cur->dst_x + cur->w > sig->max_x)
            sig->max_x = cur->dst_x + cur->w;
        if (cur->dst_y + cur->h > sig->max_y)
            sig->max_y = cur->dst_y + cur->h;
        hash_i32(&sig->hash, cur->type);
        hash_i32(&sig->hash, cur->w);
        hash_i32(&sig->hash, cur->h);
        hash_i32(&sig->hash, cur->dst_x);
        hash_i32(&sig->hash, cur->dst_y);
        hash_u32(&sig->hash, cur->color);
        for (int y = 0; y < cur->h; y++) {
            const unsigned char *row = cur->bitmap + y * cur->stride;
            for (int x = 0; x < cur->w; x++) {
                sig->coverage += row[x];
                hash_u8(&sig->hash, row[x]);
            }
        }
    }

    ass_free_track(track);
    return sig->count > 0 && sig->coverage > 0;
}

static bool same_sig(const RenderSig *a, const RenderSig *b)
{
    return a->count == b->count &&
           a->coverage == b->coverage &&
           a->hash == b->hash;
}

static bool expect_same(ASS_Library *lib, ASS_Renderer *renderer,
                        const char *a, const char *b, const char *label)
{
    RenderSig sig_a, sig_b;
    bool ok = render_case(lib, renderer, a, &sig_a) &&
              render_case(lib, renderer, b, &sig_b);
    if (!ok || !same_sig(&sig_a, &sig_b)) {
        fprintf(stderr, "%s\n", label);
        return false;
    }
    return true;
}

static bool expect_different(ASS_Library *lib, ASS_Renderer *renderer,
                             const char *a, const char *b, const char *label)
{
    RenderSig sig_a, sig_b;
    bool ok = render_case(lib, renderer, a, &sig_a) &&
              render_case(lib, renderer, b, &sig_b);
    if (!ok || same_sig(&sig_a, &sig_b)) {
        fprintf(stderr, "%s\n", label);
        return false;
    }
    return true;
}

static bool expect_warp_anchor(ASS_Library *lib, ASS_Renderer *renderer,
                               int alignment, int x, int y,
                               const char *text, const char *label)
{
    char input[2048];
    int n = snprintf(input, sizeof(input),
        "{\\an5\\pos(%d,%d)\\fs32\\bord0\\shad0\\wtan%d"
        "\\distort(1,0,1.3,1.15,-0.2,1)}%s",
        x, y, alignment, text);
    RenderSig sig;
    if (n < 0 || n >= (int) sizeof(input) ||
            !render_case(lib, renderer, input, &sig)) {
        fprintf(stderr, "%s: could not render warped text\n", label);
        return false;
    }

    int column = (alignment - 1) % 3;
    int row = (alignment - 1) / 3;
    double anchor_x = column == 0 ? sig.min_x : column == 1 ?
        (sig.min_x + sig.max_x) * 0.5 : sig.max_x;
    double anchor_y = row == 2 ? sig.min_y : row == 1 ?
        (sig.min_y + sig.max_y) * 0.5 : sig.max_y;
    /* The outline control box is calculated before rasterization; hinting and
     * anti-aliasing may change the image rectangle by a few pixels. */
    if (anchor_x - x < -5 || anchor_x - x > 5 ||
            anchor_y - y < -5 || anchor_y - y > 5) {
        fprintf(stderr, "%s: anchor=(%.1f,%.1f), expected=(%d,%d), "
                "ink=(%d,%d)..(%d,%d)\n", label, anchor_x, anchor_y,
                x, y, sig.min_x, sig.min_y, sig.max_x, sig.max_y);
        return false;
    }
    return true;
}

typedef struct {
    int x0, y0, x1, y1;
    bool have;
} ColorBounds;

static bool render_line_bounds(ASS_Library *lib, ASS_Renderer *renderer,
                                const char *text, ColorBounds bounds[4])
{
    ASS_Track *track = read_case_track(lib, text);
    if (!track)
        return false;
    memset(bounds, 0, 4 * sizeof(*bounds));
    const uint32_t colors[] = {0xFF000000u, 0x00FF0000u, 0x0000FF00u, 0xFFFFFF00u};
    int change;
    for (ASS_Image *img = ass_render_frame(renderer, track, 0, &change);
         img; img = img->next) {
        if (img->type != IMAGE_TYPE_CHARACTER)
            continue;
        int color = -1;
        for (int i = 0; i < 4; i++)
            if (img->color == colors[i]) color = i;
        if (color < 0)
            continue;
        ColorBounds *b = &bounds[color];
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w; x++) {
                if (!img->bitmap[y * img->stride + x])
                    continue;
                int xx = img->dst_x + x, yy = img->dst_y + y;
                if (!b->have) {
                    *b = (ColorBounds) {xx, yy, xx + 1, yy + 1, true};
                } else {
                    if (xx < b->x0) b->x0 = xx;
                    if (yy < b->y0) b->y0 = yy;
                    if (xx + 1 > b->x1) b->x1 = xx + 1;
                    if (yy + 1 > b->y1) b->y1 = yy + 1;
                }
            }
    }
    ass_free_track(track);
    return bounds[0].have && bounds[1].have && bounds[2].have && bounds[3].have;
}

/* The longest (white) reference line fixes the block extent. Compare colored
 * occupied pixels, independent of image-run counts. Equal short lines let us
 * check exact left/center/right edges and unchanged vertical placement; ruby
 * uses the same color so its attachment is included in those measurements. */
static bool expect_per_line_geometry(ASS_Library *lib, ASS_Renderer *renderer,
                                     int anchor, bool positioned,
                                     const char *tags, const char *short_text,
                                     const char *const selections[3],
                                     const int alignments[3])
{
    char prefix[256], pos[64] = "", control[2048], actual[2048];
    int y = anchor == 8 ? 65 : anchor == 2 ? 295 : 180;
    if (positioned) snprintf(pos, sizeof(pos), "\\pos(320,%d)", y);
    snprintf(prefix, sizeof(prefix), "{\\an%d%s\\fs30\\bord0\\shad0\\q2%s}",
             anchor, pos, tags);
    const char *format = "%s{\\c&H0000FF&}%s%s\\N{\\c&H00FF00&}%s%s\\N"
                         "{\\c&HFF0000&}%s%s\\N{\\c&HFFFFFF&}WWWWWW";
    snprintf(control, sizeof(control), format, prefix, "", short_text,
             "", short_text, "", short_text);
    snprintf(actual, sizeof(actual), format, prefix, selections[0], short_text,
             selections[1], short_text, selections[2], short_text);
    ColorBounds a[4], b[4];
    bool ok = render_line_bounds(lib, renderer, control, a) &&
              render_line_bounds(lib, renderer, actual, b);
    if (ok) {
        ok = a[3].x0 == b[3].x0 && a[3].x1 == b[3].x1 &&
             a[3].y0 == b[3].y0 && a[3].y1 == b[3].y1;
        for (int line = 0; line < 3; line++) {
            int edge = alignments[line] == 1 ? b[line].x0 - b[3].x0 :
                       alignments[line] == 3 ? b[line].x1 - b[3].x1 :
                       b[line].x0 + b[line].x1 - b[3].x0 - b[3].x1;
            ok &= abs(edge) <= 2 && a[line].y0 == b[line].y0 &&
                  a[line].y1 == b[line].y1;
        }
    }
    if (!ok)
        fprintf(stderr, "::error title=per-line ta geometry::an%d pos=%d "
                "line alignment or original block anchor changed: `%s`\n",
                anchor, positioned, actual);
    return ok;
}

static bool test_per_line_alignment(ASS_Library *lib, ASS_Renderer *renderer)
{
    bool ok = true;
    const char *lcr[] = {"{\\ta7}", "{\\ta5}", "{\\ta9}"};
    const int lcr_alignment[] = {1, 2, 3};
    const int anchors[] = {8, 2, 5};
    for (int i = 0; i < 3; i++)
        for (int positioned = 0; positioned <= 1; positioned++) {
            ok &= expect_per_line_geometry(lib, renderer, anchors[i], positioned,
                "", "WW", lcr, lcr_alignment);
            ok &= expect_per_line_geometry(lib, renderer, anchors[i], positioned,
                "\\furichangepos2", "<WW|MM|MM>", lcr, lcr_alignment);
        }
    const char *first_missing[] = {"", "{\\ta7}", "{\\ta9}"};
    ok &= expect_per_line_geometry(lib, renderer, 5, true, "", "WW",
        first_missing, (int[]) {2, 1, 3});
    const char *middle_missing[] = {"{\\ta7}", "", "{\\ta9}"};
    ok &= expect_per_line_geometry(lib, renderer, 8, true, "", "WW",
        middle_missing, lcr_alignment);
    const char *tan_fallback[] = {"{\\ta9}", "", "{\\ta5}"};
    ok &= expect_per_line_geometry(lib, renderer, 5, true, "\\tan7", "WW",
        tan_fallback, (int[]) {3, 1, 2});
    // A flat curve makes each line's along-path placement measurable while
    // retaining the global curved attachment selected by ctan5.
    ok &= expect_per_line_geometry(lib, renderer, 5, true,
        "\\ctan5\\ct(m -280 0 l 280 0)", "WW", lcr, lcr_alignment);
    ColorBounds vertical_plain[4], vertical_selected[4];
    const char *vertical_control =
        "{\\an5\\pos(320,180)\\fs30\\bord0\\shad0\\q2\\vert1\\vtype1\\vdir2}"
        "{\\c&H0000FF&}日\\N{\\c&H00FF00&}日\\N{\\c&HFF0000&}日\\N"
        "{\\c&HFFFFFF&}日日日日";
    const char *vertical_tags =
        "{\\an5\\pos(320,180)\\fs30\\bord0\\shad0\\q2\\vert1\\vtype1\\vdir2}"
        "{\\c&H0000FF&\\ta7}日{\\ta3}\\N{\\c&H00FF00&}日{\\ta5}\\N"
        "{\\c&HFF0000&\\ta3}日\\N{\\c&HFFFFFF&}日日日日";
    bool vertical_ok = render_line_bounds(lib, renderer, vertical_control, vertical_plain) &&
                       render_line_bounds(lib, renderer, vertical_tags, vertical_selected);
    if (vertical_ok) {
        vertical_ok = abs(vertical_selected[0].y0 - vertical_selected[3].y0) <= 1 &&
            abs(vertical_selected[1].y0 + vertical_selected[1].y1 -
                vertical_selected[3].y0 - vertical_selected[3].y1) <= 2 &&
            abs(vertical_selected[2].y1 - vertical_selected[3].y1) <= 1;
        for (int i = 0; i < 4; i++)
            vertical_ok &= vertical_selected[i].x0 == vertical_plain[i].x0 &&
                           vertical_selected[i].x1 == vertical_plain[i].x1;
        vertical_ok &= vertical_selected[3].y0 == vertical_plain[3].y0 &&
                       vertical_selected[3].y1 == vertical_plain[3].y1;
    }
    if (!vertical_ok)
        fprintf(stderr, "::error title=per-column ta geometry::column selection "
                "or original native block anchor changed\n");
    ok &= vertical_ok;

    const char *prefix = "{\\an8\\pos(320,60)\\fs28\\bord0\\shad0}";
    const char *cases[][2] = {
        {"{\\ta7}Hello {\\ta9}world\\N{\\ta3}Goodbye {\\ta1}world",
         "{\\ta7}Hello world\\N{\\ta3}Goodbye world"},
        {"First line{\\ta7}\\NSecond line{\\ta9}",
         "{\\ta7}First line\\N{\\ta9}Second line"},
        {"{\\ta7}A{\\r\\ta9}BC\\N{\\ta9}DEF",
         "{\\ta7}A{\\r}BC\\N{\\ta9}DEF"},
        {"{\\ta0\\ta10\\ta-1\\tafoo\\ta2.5\\ta7}AB\\N{\\ta99\\ta9}CD",
         "{\\ta7}AB\\N{\\ta9}CD"},
        {"{\\t(0,1000,\\ta7)\\ta9}AB\\N{\\t(0,1000,\\ta7)}CD{\\ta5}",
         "{\\ta9}AB\\N{\\ta5}CD"},
        {"{\\ta7\\N\\ta9}AB\\N{\\ta9}CD",
         "{\\ta7}AB\\N{\\ta9}CD"},
        {"{\\ta7}ABC\\N\\N{\\ta9}DEF\\N",
         "{\\ta7}ABC\\N{\\ta5}\\N{\\ta9}DEF\\N"},
        {"{\\q1}ONE TWO THREE FOUR FIVE SIX SEVEN EIGHT NINE TEN "
         "ELEVEN TWELVE THIRTEEN{\\ta9}\\N{\\ta7}END",
         "{\\q1\\ta9}ONE TWO THREE FOUR FIVE SIX SEVEN EIGHT NINE TEN "
         "ELEVEN TWELVE THIRTEEN\\N{\\ta7}END"},
        {"{\\q2\\ta7}LONG LINE\\nshort{\\ta9}\\N{\\ta9}END",
         "{\\q2\\ta7}LONG LINE\\nshort\\N{\\ta9}END"},
        {"{\\ta7}日本 Latin ဆာတို့{\\ta9}\\Nစူဇူကီ 日本{\\ta9}\\N{\\ta5}ABC אבג",
         "{\\ta7}日本 Latin ဆာတို့\\N{\\ta9}စူဇူကီ 日本\\N{\\ta5}ABC אבג"},
        {"{\\furichangepos2}A<WW|MM|MM>{\\ta7}\\N{\\ta9}<WW|MM|MM>A",
         "{\\furichangepos2\\ta7}A<WW|MM|MM>\\N{\\ta9}<WW|MM|MM>A"},
        {"{\\vert1\\vdir2\\ta7}日本{\\ta9}\\N次列{\\ta9}",
         "{\\vert1\\vdir2\\ta7}日本\\N{\\ta9}次列"},
        {"{\\ctan5\\ct(m -250 0 b -150 -60 150 -60 250 0)}"
         "AB{\\ta7}\\NCD{\\ta9}",
         "{\\ctan5\\ct(m -250 0 b -150 -60 150 -60 250 0)\\ta7}"
         "AB\\N{\\ta9}CD"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char actual[2048], expected[2048];
        snprintf(actual, sizeof(actual), "%s%s", prefix, cases[i][0]);
        snprintf(expected, sizeof(expected), "%s%s", prefix, cases[i][1]);
        ok &= expect_same(lib, renderer, actual, expected,
                          "per-line ta first-win/inheritance geometry failed");
    }
    return ok;
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
    ok &= test_per_line_alignment(lib, renderer);

    ok &= expect_same(
        lib, renderer,
        "{\\an3\\tan3\\pos(320,180)}Hello",
        "{\\an3\\pos(320,180)}Hello",
        "\\tan matching \\an changed rendering");

    ok &= expect_same(
        lib, renderer,
        "{\\an3\\tan7\\pos(320,180)}Hello",
        "{\\an1\\pos(320,180)}Hello",
        "\\tan did not select left horizontal alignment");

    ok &= expect_same(
        lib, renderer,
        "{\\an3\\tan8\\pos(320,180)}Hello",
        "{\\an2\\pos(320,180)}Hello",
        "\\tan did not select center horizontal alignment");

    ok &= expect_different(
        lib, renderer,
        "{\\an3\\tan7\\pos(320,180)}Hello",
        "{\\an3\\pos(320,180)}Hello",
        "\\tan did not change horizontal text layout relative to the "
        "object anchor");

    ok &= expect_same(
        lib, renderer,
        "{\\an7\\tan3\\pos(320,180)}Hello",
        "{\\an9\\pos(320,180)}Hello",
        "\\tan did not preserve top vertical text alignment");

    ok &= expect_same(
        lib, renderer,
        "{\\an3\\tan7\\pos(320,180)\\frz25}Hello",
        "{\\an3\\tan7\\pos(320,180)\\org(320,180)\\frz25}Hello",
        "\\tan moved the default transform origin away from the object anchor");

    const int equivalent[][3] = {{1, 4, 7}, {2, 5, 8}, {3, 6, 9}};
    for (int group = 0; group < 3; group++) {
        char first[160], other[160];
        snprintf(first, sizeof(first),
            "{\\an7\\ta%d\\pos(320,180)}LONG FIRST LINE\\N{\\ta%d}short",
            equivalent[group][0], equivalent[group][0]);
        for (int member = 1; member < 3; member++) {
            snprintf(other, sizeof(other),
                "{\\an7\\ta%d\\pos(320,180)}LONG FIRST LINE\\N{\\ta%d}short",
                equivalent[group][member], equivalent[group][member]);
            ok &= expect_same(lib, renderer, first, other,
                              "equivalent ta numpad values rendered differently");
        }
    }

    RenderSig legacy, centered, right_legacy, right_left, bottom_legacy,
              bottom_right;
    ok &= render_case(lib, renderer,
        "{\\an7\\pos(320,180)}LONG FIRST LINE\\Nshort", &legacy);
    ok &= render_case(lib, renderer,
        "{\\an7\\ta2\\pos(320,180)}LONG FIRST LINE\\N{\\ta2}short", &centered);
    /* Image ink can differ by a few pixels when a different line supplies
     * the outermost side bearing; the positioned block anchor must not move. */
    if (same_sig(&legacy, &centered) || abs(legacy.min_x - centered.min_x) > 3 ||
            legacy.min_y != centered.min_y) {
        fprintf(stderr, "ta2 changed the top-left block anchor or left lines unchanged: "
                "legacy=(%d,%d)..(%d,%d), ta=(%d,%d)..(%d,%d), same=%d\n",
                legacy.min_x, legacy.min_y, legacy.max_x, legacy.max_y,
                centered.min_x, centered.min_y, centered.max_x, centered.max_y,
                same_sig(&legacy, &centered));
        ok = false;
    }
    ok &= render_case(lib, renderer,
        "{\\an9\\pos(320,180)}LONG FIRST LINE\\Nshort", &right_legacy);
    ok &= render_case(lib, renderer,
        "{\\an9\\ta1\\pos(320,180)}LONG FIRST LINE\\N{\\ta1}short", &right_left);
    if (same_sig(&right_legacy, &right_left) ||
            abs(right_legacy.max_x - right_left.max_x) > 3 ||
            right_legacy.min_y != right_left.min_y) {
        fprintf(stderr, "ta1 changed the top-right block anchor or left lines unchanged: "
                "legacy=(%d,%d)..(%d,%d), ta=(%d,%d)..(%d,%d), same=%d\n",
                right_legacy.min_x, right_legacy.min_y, right_legacy.max_x, right_legacy.max_y,
                right_left.min_x, right_left.min_y, right_left.max_x, right_left.max_y,
                same_sig(&right_legacy, &right_left));
        ok = false;
    }
    ok &= render_case(lib, renderer,
        "{\\an1\\pos(320,180)}LONG FIRST LINE\\Nshort", &bottom_legacy);
    ok &= render_case(lib, renderer,
        "{\\an1\\ta3\\pos(320,180)}LONG FIRST LINE\\N{\\ta3}short", &bottom_right);
    if (same_sig(&bottom_legacy, &bottom_right) ||
            abs(bottom_legacy.min_x - bottom_right.min_x) > 3 ||
            bottom_legacy.max_y != bottom_right.max_y) {
        fprintf(stderr, "ta3 changed the bottom-left block anchor or right lines unchanged: "
                "legacy=(%d,%d)..(%d,%d), ta=(%d,%d)..(%d,%d), same=%d\n",
                bottom_legacy.min_x, bottom_legacy.min_y, bottom_legacy.max_x, bottom_legacy.max_y,
                bottom_right.min_x, bottom_right.min_y, bottom_right.max_x, bottom_right.max_y,
                same_sig(&bottom_legacy, &bottom_right));
        ok = false;
    }

    ok &= expect_same(lib, renderer,
        "{\\an7\\ta2\\ta3\\pos(320,180)}LONG FIRST LINE\\Nshort",
        "{\\an7\\ta2\\pos(320,180)}LONG FIRST LINE\\Nshort",
        "ta did not keep the first valid override");
    ok &= expect_same(lib, renderer,
        "{\\an7\\ta0\\ta2\\pos(320,180)}LONG FIRST LINE\\Nshort",
        "{\\an7\\ta2\\pos(320,180)}LONG FIRST LINE\\Nshort",
        "invalid ta prevented a later valid override");
    ok &= expect_same(lib, renderer,
        "{\\an7\\ta2\\r\\ta3\\pos(320,180)}LONG FIRST LINE\\Nshort",
        "{\\an7\\r\\ta2\\pos(320,180)}LONG FIRST LINE\\Nshort",
        "style reset erased the line's first ta decision");
    ok &= expect_same(lib, renderer,
        "{\\an7\\t(0,1000,\\ta2)\\pos(320,180)}LONG FIRST LINE\\Nshort",
        "{\\an7\\pos(320,180)}LONG FIRST LINE\\Nshort",
        "ta inside a transform affected static line layout");

    ok &= expect_same(lib, renderer,
        "{\\q2\\an7\\ta2\\pos(320,180)}LONG LINE\\nshort",
        "{\\q2\\an7\\ta2\\pos(320,180)}LONG LINE\\N{\\ta2}short",
        "ta missed a WrapStyle 2 soft line break");
    ok &= expect_same(lib, renderer,
        "{\\an7\\ta2\\pos(320,180)}LONG\\nLINE",
        "{\\an7\\ta2\\pos(320,180)}LONG LINE",
        "ta changed default soft-break semantics");

    char const *wrap_words =
        "THIS IS A LONG AUTOMATICALLY WRAPPED SUBTITLE LINE WITH ENOUGH "
        "WORDS TO CREATE SEVERAL VISUAL LINES";
    char wrap_left[256], wrap_center[256], wrap_none[256];
    snprintf(wrap_left, sizeof(wrap_left),
        "{\\an7\\ta1\\pos(100,80)}%s", wrap_words);
    snprintf(wrap_center, sizeof(wrap_center),
        "{\\an7\\ta2\\pos(100,80)}%s", wrap_words);
    snprintf(wrap_none, sizeof(wrap_none),
        "{\\q2\\an7\\ta2\\pos(100,80)}%s", wrap_words);
    RenderSig wrapped_left, wrapped_center, unwrapped;
    ok &= render_case(lib, renderer, wrap_left, &wrapped_left);
    ok &= render_case(lib, renderer, wrap_center, &wrapped_center);
    ok &= render_case(lib, renderer, wrap_none, &unwrapped);
    if (wrapped_left.max_y - wrapped_left.min_y <=
            unwrapped.max_y - unwrapped.min_y + 20 ||
            same_sig(&wrapped_left, &wrapped_center)) {
        fprintf(stderr, "ta did not align automatically wrapped visual lines\n");
        ok = false;
    }

    const char *three_lines = "A\\NA MUCH LONGER SECOND LINE\\NABC";
    ok &= expect_warp_anchor(lib, renderer, 8, 320, 60, three_lines,
                             "wtan8 multiline top center");
    ok &= expect_warp_anchor(lib, renderer, 5, 320, 180, three_lines,
                             "wtan5 multiline center");
    ok &= expect_warp_anchor(lib, renderer, 2, 320, 300, three_lines,
                             "wtan2 multiline bottom center");
    ok &= expect_warp_anchor(lib, renderer, 4, 20, 180, three_lines,
                             "wtan4 multiline left edge");
    ok &= expect_warp_anchor(lib, renderer, 6, 620, 180, three_lines,
                             "wtan6 multiline right edge");
    ok &= expect_warp_anchor(lib, renderer, 8, 320, 55,
        "A\\N{\\distort(1,-3,1.35,1.2,-0.1,1.1)}LONG SECOND LINE"
        "\\N{\\distort(1,0.1,1.35,1.7,-0.1,1.3)}ABC",
        "wtan8 follows later lines' warped vertical extent");
    ok &= expect_warp_anchor(lib, renderer, 8, 320, 60,
        "<A|ruby>\\NSECOND\\Nthird",
        "wtan8 includes ruby above the first line");

    for (int alignment = 1; alignment <= 9; alignment++) {
        int column = (alignment - 1) % 3;
        int row = (alignment - 1) / 3;
        int x = column == 0 ? 30 : column == 1 ? 320 : 610;
        int y = row == 2 ? 60 : row == 1 ? 180 : 300;
        ok &= expect_warp_anchor(lib, renderer, alignment, x, y, "SINGLE",
                                 "single-line wtan anchor");
    }
    ok &= expect_same(lib, renderer,
        "{\\an5\\pos(320,180)\\wtan8}PLAIN\\NLINE",
        "{\\an5\\pos(320,180)}PLAIN\\NLINE",
        "wtan changed rendering without a warp");
    ok &= expect_same(lib, renderer,
        "{\\an8\\pos(320,180)\\distort(1,0,1.3,1,-.2,1)}A\\NABC",
        "{\\an8\\pos(320,180)\\wtan8\\distort(1,0,1.3,1,-.2,1)}A\\NABC",
        "distorted text did not inherit the explicit an anchor");
    ok &= expect_same(lib, renderer,
        "{\\pos(320,180)\\distort(1,0,1.3,1,-.2,1)}A\\NABC",
        "{\\pos(320,180)\\wtan2\\distort(1,0,1.3,1,-.2,1)}A\\NABC",
        "distorted text did not inherit the style alignment");
    ok &= expect_different(lib, renderer,
        "{\\an8\\pos(320,180)\\distort(1,0,1.3,1,-.2,1)}A\\NABC",
        "{\\an8\\pos(320,180)\\wtan2\\distort(1,0,1.3,1,-.2,1)}A\\NABC",
        "explicit wtan did not override the inherited an anchor");
    ok &= expect_same(lib, renderer,
        "{\\an5\\pos(320,180)\\wtan0\\wtan8\\distort(1,0,1.3,1,-.2,1)}A\\NB",
        "{\\an5\\pos(320,180)\\wtan8\\distort(1,0,1.3,1,-.2,1)}A\\NB",
        "invalid wtan blocked the next valid value");
    ok &= expect_same(lib, renderer,
        "{\\an5\\pos(320,180)\\wtan8\\r\\distort(1,0,1.3,1,-.2,1)}A\\NB",
        "{\\an5\\pos(320,180)\\distort(1,0,1.3,1,-.2,1)}A\\NB",
        "style reset did not clear wtan");
    ok &= expect_same(lib, renderer,
        "{\\an5\\pos(320,180)\\t(0,1000,\\wtan8)\\distort(1,0,1.3,1,-.2,1)}A\\NB",
        "{\\an5\\pos(320,180)\\distort(1,0,1.3,1,-.2,1)}A\\NB",
        "wtan inside transform affected static alignment");

    ass_renderer_done(renderer);
    ass_library_done(lib);
    return ok ? 0 : 1;
}
