#include <math.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ass.h"

typedef struct {
    int images, min_x, max_x, min_y, max_y;
    uint64_t fill_hash;
    double red_x, red_y, red_mass;
    double blue_x, blue_y, blue_mass;
} Sample;

static void log_message(int level, const char *fmt, va_list args, void *data)
{
    (void) level; (void) fmt; (void) args; (void) data;
}

static bool render(ASS_Library *lib, ASS_Renderer *renderer,
                   const char *text, long long time, Sample *sample)
{
    char script[16384];
    int count = snprintf(script, sizeof(script),
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 800\nPlayResY: 500\n"
        "ScaledBorderAndShadow: yes\n[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,sans-serif,48,&H000000FF,&H00FF0000,&H00000000,&H80000000,"
        "0,0,0,0,100,100,0,0,1,0,0,5,20,20,20,1\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:03.00,Default,,0,0,0,,%s\n", text);
    if (count < 0 || count >= (int) sizeof(script))
        return false;
    ASS_Track *track = ass_read_memory(lib, script, strlen(script), NULL);
    if (!track)
        return false;
    memset(sample, 0, sizeof(*sample));
    sample->fill_hash = 1469598103934665603ULL;
    ASS_Image *images = ass_render_frame(renderer, track, time, NULL);
    for (ASS_Image *img = images; img; img = img->next) {
        if (img->type != IMAGE_TYPE_CHARACTER)
            continue;
        int x0 = img->dst_x, y0 = img->dst_y;
        int x1 = x0 + img->w, y1 = y0 + img->h;
        if (!sample->images) {
            sample->min_x = x0; sample->max_x = x1;
            sample->min_y = y0; sample->max_y = y1;
        } else {
            if (x0 < sample->min_x) sample->min_x = x0;
            if (x1 > sample->max_x) sample->max_x = x1;
            if (y0 < sample->min_y) sample->min_y = y0;
            if (y1 > sample->max_y) sample->max_y = y1;
        }
        sample->images++;
        uint32_t red = (img->color >> 24) & 255;
        uint32_t blue = (img->color >> 8) & 255;
        for (int y = 0; y < img->h; y++) {
            const uint8_t *row = img->bitmap + (ptrdiff_t) y * img->stride;
            for (int x = 0; x < img->w; x++) {
                uint32_t ink = row[x];
                uint64_t value = (uint64_t) ink +
                    ((uint64_t) (x + img->dst_x) << 8) +
                    ((uint64_t) (y + img->dst_y) << 32);
                sample->fill_hash ^= value;
                sample->fill_hash *= 1099511628211ULL;
                if (red > blue) {
                    sample->red_mass += ink;
                    sample->red_x += ink * (x + img->dst_x);
                    sample->red_y += ink * (y + img->dst_y);
                } else if (blue > red) {
                    sample->blue_mass += ink;
                    sample->blue_x += ink * (x + img->dst_x);
                    sample->blue_y += ink * (y + img->dst_y);
                }
            }
        }
    }
    ass_free_track(track);
    return sample->images > 0;
}

static bool expect(bool condition, const char *message)
{
    if (!condition)
        fprintf(stderr, "%s\n", message);
    return condition;
}

static bool same_fill_center(const Sample *a, const Sample *b)
{
    return a->red_mass > 0 && b->red_mass > 0 &&
           fabs(a->red_x / a->red_mass - b->red_x / b->red_mass) < 1.5 &&
           fabs(a->red_y / a->red_mass - b->red_y / b->red_mass) < 1.5;
}

int main(void)
{
    ASS_Library *lib = ass_library_init();
    if (!lib)
        return 1;
    ass_set_message_cb(lib, log_message, NULL);
    ASS_Renderer *renderer = ass_renderer_init(lib);
    if (!renderer) {
        ass_library_done(lib);
        return 1;
    }
    ass_set_storage_size(renderer, 800, 500);
    ass_set_frame_size(renderer, 800, 500);
    ass_set_fonts(renderer, NULL, "sans-serif",
                  ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    bool ok = true;
    Sample a, b, c;
    const char *smoke[] = {
        "{\\vert1}日本語です。", "{\\vert1}「縦書き」", "{\\vert1}一二三",
        "{\\vert1}中文垂直排版。", "{\\vert1}한국어세로쓰기",
        "{\\vert1}한", "{\\vert1}日本2026年", "{\\vert1}TVアニメ",
        "{\\vert1}minimum", "{\\vert1}illogical", "{\\vert1}WIMig.",
        "{\\vert1}မြန်မာ", "{\\vert1}မြန်မာနိုင်ငံ",
        "{\\vert1}မြန်မာ\\Nနိုင်ငံ", "{\\vert1}A B​C",
        "{\\vert1}A\\N\\NB", "{\\vert1}A<base|ruby>B",
        "{\\vert1}A{\\t(\\bord20)}BC",
        "{\\an5\\vert1}မြန်မာ日本ngar harနိုင်ငံ",
        "{\\an5\\ta5\\vert1\\vdir1}မြန်မာ日本ngar harနိုင်ငံ\\N次列",
        "{\\an5\\ta5\\vert1\\vdir2}မြန်မာ日本ngar harနိုင်ငံ\\N次列",
        "{\\vert1}မင့်မြန်မာ",
    };
    for (size_t i = 0; i < sizeof(smoke) / sizeof(smoke[0]); i++)
        ok &= expect(render(lib, renderer, smoke[i], 500, &a),
                     "native vertical smoke render failed");
    ok &= expect(render(lib, renderer,
        "{\\an5\\pos(400,250)\\vert1\\bord1}ABC", 0, &a), "bord1 render");
    ok &= expect(render(lib, renderer,
        "{\\an5\\pos(400,250)\\vert1\\bord20}ABC", 0, &b), "bord20 render");
    ok &= expect(same_fill_center(&a, &b),
                 "border changed base fill glyph placement");
    ok &= expect(render(lib, renderer,
        "{\\an5\\pos(400,250)\\vert1\\shad20}ABC", 0, &b), "shadow render");
    ok &= expect(same_fill_center(&a, &b),
                 "shadow changed base fill glyph placement");
    ok &= expect(render(lib, renderer,
        "{\\an5\\pos(400,250)\\vert1\\blur10}ABC", 0, &b), "blur render");
    ok &= expect(same_fill_center(&a, &b),
                 "blur changed base fill glyph placement");
    ok &= expect(render(lib, renderer,
        "{\\an5\\pos(400,250)\\vert1\\bord1\\t(0,1000,\\bord20)}ABC",
        0, &a), "animated border initial render");
    ok &= expect(render(lib, renderer,
        "{\\an5\\pos(400,250)\\vert1\\bord1\\t(0,1000,\\bord20)}ABC",
        1000, &b), "animated border final render");
    ok &= expect(same_fill_center(&a, &b),
                 "animated border changed base vertical positions");
    ok &= expect(render(lib, renderer,
        "{\\an5\\pos(400,250)\\vert1\\fs40}ABC", 0, &a), "fs40 render");
    ok &= expect(render(lib, renderer,
        "{\\an5\\pos(400,250)\\vert1\\fs80}ABC", 0, &b), "fs80 render");
    ok &= expect(b.max_y - b.min_y > a.max_y - a.min_y + 20,
                 "font size did not alter native vertical spacing");
    ok &= expect(render(lib, renderer,
        "{\\vert1\\vtype1}日本語\\N次列", 0, &a), "CJK columns");
    ok &= expect(render(lib, renderer,
        "{\\vert1\\vtype1\\vdir2}日本語\\N次列", 0, &b), "reversed columns");
    ok &= expect(a.fill_hash != b.fill_hash,
                 "vdir2 did not change rendered column order");
    ok &= expect(render(lib, renderer,
        "{\\vert1\\fn@sans-serif}ABC", 0, &a), "native @font");
    ok &= expect(render(lib, renderer,
        "{\\vert1\\fnsans-serif}ABC", 0, &b), "native plain font");
    ok &= expect(a.fill_hash == b.fill_hash,
                 "native @font used legacy rotation or metrics");
    ok &= expect(render(lib, renderer, "ABC", 500, &a), "horizontal baseline");
    ok &= expect(render(lib, renderer, "{\\vert0}ABC", 500, &b), "vert0 baseline");
    ok &= expect(a.fill_hash == b.fill_hash,
                 "vert0 changed horizontal rendering");
    ok &= expect(render(lib, renderer, "{\\t(\\vert1)}ABC", 500, &b),
                 "nonanimating vert in transform");
    ok &= expect(a.fill_hash == b.fill_hash,
                 "vert animated inside t");
    ok &= expect(render(lib, renderer,
        "{\\fn@sans-serif}日本語", 0, &a), "legacy @font before native");
    ok &= expect(render(lib, renderer,
        "{\\vert1\\fn@sans-serif}日本語", 0, &b), "native @font event");
    ok &= expect(render(lib, renderer,
        "{\\fn@sans-serif}日本語", 0, &c), "legacy @font after native");
    ok &= expect(a.fill_hash == c.fill_hash,
                 "legacy @font changed after a native vertical event");

    const int anchor_x[] = {20, 400, 780};
    const int anchor_y[] = {480, 250, 20};
    for (int an = 1; an <= 9; an++) {
        char ordinary[128], positioned[160];
        snprintf(ordinary, sizeof(ordinary),
                 "{\\an%d\\vert1}A\\NB", an);
        snprintf(positioned, sizeof(positioned),
                 "{\\an%d\\pos(%d,%d)\\vert1}A\\NB", an,
                 anchor_x[(an - 1) % 3], anchor_y[(an - 1) / 3]);
        bool anchor_ok = render(lib, renderer, ordinary, 0, &a) &&
                         render(lib, renderer, positioned, 0, &b) &&
                         a.fill_hash == b.fill_hash;
        if (!anchor_ok)
            fprintf(stderr, "native vertical an%d margin/pos anchor mismatch\n", an);
        ok &= anchor_ok;
    }
    // Both explicit columns must request the same alignment now; keep the
    // original unequal-unit and block-anchor assertions for that layout.
    ok &= expect(render(lib, renderer,
        "{\\an9\\ta1\\vert1}ကမြန်\\N{\\ta1}နိုင်", 0, &a) &&
        render(lib, renderer,
        "{\\an9\\ta9\\vert1}ကမြန်\\N{\\ta9}နိုင်", 0, &b) &&
        a.fill_hash != b.fill_hash,
        "ta did not change internal unit alignment");
    ok &= expect(render(lib, renderer,
        "{\\an9\\ta1\\pos(780,20)\\vert1}ကမြန်\\N{\\ta1}နိုင်", 0, &c) &&
        a.fill_hash == c.fill_hash,
        "ta changed the an9 block anchor");
    ok &= expect(render(lib, renderer,
        "{\\an5\\pos(400,250)}horizontal after vertical", 0, &c),
                 "horizontal render after native vertical");

    ok &= expect(render(lib, renderer,
        "{\\an5\\pos(400,250)\\vert1\\kf100}minimum", 500, &a),
                 "vertical karaoke render");
    ok &= expect(a.red_mass > 0 && a.blue_mass > 0 &&
                 a.red_y / a.red_mass < a.blue_y / a.blue_mass,
                 "vertical kf did not wipe top to bottom");
    ok &= expect(render(lib, renderer,
        "{\\an5\\pos(400,250)\\vert1\\K100}minimum", 500, &a) &&
        a.red_mass > 0 && a.blue_mass > 0 &&
        a.red_y / a.red_mass < a.blue_y / a.blue_mass,
        "vertical K did not wipe top to bottom");
    ok &= expect(render(lib, renderer,
        "{\\an5\\pos(400,250)\\kf100}minimum", 500, &b),
                 "horizontal karaoke render");
    ok &= expect(b.red_mass > 0 && b.blue_mass > 0 &&
                 b.red_x / b.red_mass < b.blue_x / b.blue_mass,
                 "horizontal kf wipe changed direction");
    ass_renderer_done(renderer);
    ass_library_done(lib);
    return ok ? 0 : 1;
}
