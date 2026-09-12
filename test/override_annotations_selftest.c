/* Override annotation regressions through public render entry points.
 * Compare the same renderer's output, as in the other Mangetsu selftests. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ass.h"
#include "ass_render.h"

typedef struct {
    uint64_t hash, coverage;
    int count;
    double font_size, soft_scale, object_scale, frz;
    bool explicit, source_intact, buffers_released;
} Sample;

static void msg_cb(int level, const char *fmt, va_list va, void *data)
{
    (void) level; (void) fmt; (void) va; (void) data;
}

static ASS_Track *read_track(ASS_Library *lib, const char *text)
{
    char script[16384];
    int n = snprintf(script, sizeof(script),
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 1000\nPlayResY: 700\n"
        "ScaledBorderAndShadow: yes\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,40,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,"
        "0,0,0,0,80,120,2,10,1,2,3,5,30,50,25,1\n"
        "Style: Other,Arial,20,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,"
        "0,0,0,0,120,80,-1,-15,1,5,1,7,40,60,35,1\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:02.00,Default,,0,0,0,,%s\n", text);
    if (n < 0 || n >= (int) sizeof(script))
        return NULL;
    return ass_read_memory(lib, script, strlen(script), NULL);
}

static void hash_byte(uint64_t *hash, uint8_t value)
{
    *hash ^= value;
    *hash *= UINT64_C(1099511628211);
}

static void hash_int(uint64_t *hash, uint32_t value)
{
    for (int i = 0; i < 4; i++)
        hash_byte(hash, value >> (8 * i));
}

static Sample capture(ASS_Renderer *renderer, ASS_Track *track, long long now)
{
    Sample out = {.hash = UINT64_C(1469598103934665603)};
    if (!track->n_events || !track->events[0].Text)
        return out;
    char *source = track->events[0].Text;
    size_t length = strlen(source);
    char *saved = malloc(length + 1);
    if (!saved)
        return out;
    memcpy(saved, source, length + 1);
    int change;
    ASS_Image *images = ass_render_frame(renderer, track, now, &change);
    out.source_intact = track->events[0].Text == source &&
        !strcmp(source, saved);
    free(saved);
    const RenderContext *s = &renderer->state;
    out.font_size = s->font_size;
    out.soft_scale = s->soft_scale;
    out.object_scale = s->object_scale;
    out.frz = s->frz;
    out.explicit = s->explicit;
    out.buffers_released = s->override_buffers == NULL;
    for (ASS_Image *img = images; img; img = img->next) {
        out.count++;
        hash_int(&out.hash, img->type);
        hash_int(&out.hash, img->dst_x);
        hash_int(&out.hash, img->dst_y);
        hash_int(&out.hash, img->w);
        hash_int(&out.hash, img->h);
        hash_int(&out.hash, img->color);
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w; x++) {
                uint8_t a = img->bitmap[y * img->stride + x];
                hash_byte(&out.hash, a);
                out.coverage += a;
            }
    }
    return out;
}

static bool same_pixels(Sample a, Sample b)
{
    return a.hash == b.hash && a.coverage == b.coverage && a.count == b.count;
}

static bool check_pair(ASS_Library *lib, ASS_Renderer *renderer,
                       const char *annotated, const char *clean, long long now)
{
    ASS_Track *ta = read_track(lib, annotated), *tb = read_track(lib, clean);
    bool ok = ta && tb;
    if (ok) {
        Sample a = capture(renderer, ta, now), b = capture(renderer, tb, now);
        ok = a.source_intact && b.source_intact &&
            a.buffers_released && b.buffers_released && same_pixels(a, b) &&
            fabs(a.font_size - b.font_size) < 1e-8 &&
            fabs(a.soft_scale - b.soft_scale) < 1e-8 &&
            fabs(a.object_scale - b.object_scale) < 1e-8 &&
            fabs(a.frz - b.frz) < 1e-8 && a.explicit == b.explicit;
        // Empty annotation-only blocks are tested separately below.
        ok &= a.coverage != 0;
    }
    if (!ok)
        fprintf(stderr, "annotation mismatch at %lld: %s versus %s\n",
                now, annotated, clean);
    if (ta) ass_free_track(ta);
    if (tb) ass_free_track(tb);
    return ok;
}

static bool test_pairs(ASS_Library *lib, ASS_Renderer *renderer)
{
    const struct { const char *a, *b; } cases[] = {
        {"{\\fs\\N50}Text", "{\\fs50}Text"},
        {"{\\pos(\\N500,\\N300)}Text", "{\\pos(500,300)}Text"},
        {"{\\t(\\N300,\\N500,\\N\\fs20)}Text", "{\\t(300,500,\\fs20)}Text"},
        {"{\\fs40[test]\\bord2}Text", "{\\fs40\\bord2}Text"},
        {"{\\pos([x]500,[y]300)}Text", "{\\pos(500,300)}Text"},
        {"{\\t([start]300,[end]500,[body]\\fs20)}Text", "{\\t(300,500,\\fs20)}Text"},
        {"{\\t([start]\\N300,[end]\\N500,[body]\\N\\fs20)}Text",
         "{\\t(300,500,\\fs20)}Text"},
        {"{\\fs[delta]\\N-10}Text", "{\\fs-10}Text"},
        {"{\\frz[delta]\\N~-20}Text", "{\\frz~-20}Text"},
        {"{\\t([start]\\N300,[end]\\N500,\\N\\fsc[delta]\\N+50)}Text",
         "{\\t(300,500,\\fsc+50)}Text"},
        {"{\\scale[amount]\\N+50}Text", "{\\scale+50}Text"},
        {"{\\scale50\\t(300,500,\\scale[delta]\\N~+50)}Text",
         "{\\scale50\\t(300,500,\\scale~+50)}Text"},
        {"{\\distort([x]\\N1,\\N0,\\N1,\\N1,\\N0,\\N1)}Text",
         "{\\distort(1,0,1,1,0,1)}Text"},
        {"{\\distort(\\N1,\\N0,\\N1,\\N1,\\N0,\\N1,[p0x]\\N.2,[p0y]\\N.1)}Text",
         "{\\distort(1,0,1,1,0,1,.2,.1)}Text"},
        {"{\\fs[a][b][]\\N50[c]\\bord[d]2}Text", "{\\fs50\\bord2}Text"},
        {"{[\\fs999\\bord999]\\fs40}Text", "{\\fs40}Text"},
        {"{\\t([hello, (world)]300,[,)]500,[()]\\fs20)}Text",
         "{\\t(300,500,\\fs20)}Text"},
        {"{\\fs40[unfinished\\fs999}Text", "{\\fs40}Text"},
        {"{\\fs40[unfinished}Text [visible]\\NNext{\\fs20}Small",
         "{\\fs40}Text [visible]\\NNext{\\fs20}Small"},
        {"{\\fs[abc[def]50}Text", "{\\fs50}Text"},
        {"{\\f[part]s5\\N0}Text", "{\\fs50}Text"},
        {"{\\fax~[sign]\\N-.2}Text", "{\\fax~-.2}Text"},
        {"{\\fs5[inside digits]0}Text", "{\\fs50}Text"},
        {"{\\pos([,()]500,[\\tags]300)}Text", "{\\pos(500,300)}Text"},
        {"{\\clip([x1]0,[y1]0,[x2]900,[y2]600)}Text", "{\\clip(0,0,900,600)}Text"},
        {"{\\clip(m[x] 0 0 l 900 0 900 600 0 600)}Text",
         "{\\clip(m 0 0 l 900 0 900 600 0 600)}Text"},
        {"{\\fnAr[name]ial\\fs50}Font{\\bord2}Name", "{\\fnArial\\fs50}Font{\\bord2}Name"},
        {"{\\rO[name]ther\\fs[delta]+10}Reset", "{\\rOther\\fs+10}Reset"},
        {"{\\1c&H00[colour]FF00&\\alpha&H[alpha]20&}Colour",
         "{\\1c&H00FF00&\\alpha&H20&}Colour"},
        {"{\\1vc([a]&HFF0000&,[b]&H00FF00&,[c]&H0000FF&,[d]&HFFFFFF&)}Colour",
         "{\\1vc(&HFF0000&,&H00FF00&,&H0000FF&,&HFFFFFF&)}Colour"},
        // Preliminary hard-override, border-style and column scans must agree.
        {"{[\\pos(1,1)\\clip(0,0,1,1)\\p1]}Text", "{}Text"},
        {"{\\p[split]os(500,300)}Text", "{\\pos(500,300)}Text"},
        {"{[\\bs4]\\bs1}Text", "{\\bs1}Text"},
        {"{\\b[split]s4}Text", "{\\bs4}Text"},
        {"{[\\col1]}Left|Right", "{}Left|Right"},
        {"{\\co[split]l1\\colsp[gap]8}Left|Right", "{\\col1\\colsp8}Left|Right"},
        {"{[]}Hello\\NWorld", "Hello\\NWorld"},
        {"{[]}Hello [world]", "Hello [world]"},
        {"{\\N}Text", "{}Text"}, {"{[]}Text", "{}Text"},
        {"{[abc]}Text", "{}Text"}, {"{[abc}Text", "{}Text"},
        {"{\\fs[abc]50}Text", "{\\fs50}Text"},
        {"{\\t([x]300,[y]500,\\fs20)}Text", "{\\t(300,500,\\fs20)}Text"},
        {"{\\t([)]300,[,]500,\\fs20)}Text", "{\\t(300,500,\\fs20)}Text"},
        {"{[]}Unclosed {[abc", "Unclosed {[abc"},
        {"{[]}Escaped \\{[visible]\\}", "Escaped \\{[visible]\\}"},
        {"{\\fs50\\t([x]300,[y]500,[unfinished}Text",
         "{\\fs50\\t(300,500,}Text"},
    };
    const long long times[] = {0, 300, 400, 500, 900, 1500};
    bool ok = true;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        for (size_t t = 0; t < sizeof(times) / sizeof(times[0]); t++)
            ok &= check_pair(lib, renderer, cases[i].a, cases[i].b, times[t]);

    // All transform arities and acceleration share the same lexical input.
    const char *a[] = {"[body]\\N", "[accel]\\N2,[body]\\N",
                       "[start]\\N300,[end]\\N500,[body]\\N",
                       "[start]\\N300,[end]\\N500,[accel]\\N2,[body]\\N"};
    const char *b[] = {"", "2,", "300,500,", "300,500,2,"};
    for (int i = 0; i < 4; i++) {
        char annotated[512], clean[512];
        snprintf(annotated, sizeof(annotated), "{\\t(%s\\fs[delta]\\N+20)}Text", a[i]);
        snprintf(clean, sizeof(clean), "{\\t(%s\\fs+20)}Text", b[i]);
        for (size_t t = 0; t < sizeof(times) / sizeof(times[0]); t++)
            ok &= check_pair(lib, renderer, annotated, clean, times[t]);
    }
    return ok;
}

static bool test_boundaries(ASS_Library *lib, ASS_Renderer *renderer)
{
    const char *empty[] = {"{\\N}", "{[]}", "{[abc]}", "{[abc}"};
    bool ok = true;
    for (size_t i = 0; i < sizeof(empty) / sizeof(empty[0]); i++) {
        ASS_Track *track = read_track(lib, empty[i]);
        if (!track) return false;
        Sample s = capture(renderer, track, 0);
        ok &= !s.coverage && s.source_intact && s.buffers_released;
        ass_free_track(track);
    }
    const struct { const char *a, *b; } visible[] = {
        {"Hello\\NWorld", "HelloWorld"},
        {"Hello [world]", "Hello world"},
    };
    for (size_t i = 0; i < sizeof(visible) / sizeof(visible[0]); i++) {
        ASS_Track *ta = read_track(lib, visible[i].a), *tb = read_track(lib, visible[i].b);
        if (!ta || !tb) {
            if (ta) ass_free_track(ta);
            if (tb) ass_free_track(tb);
            return false;
        }
        Sample a = capture(renderer, ta, 0), b = capture(renderer, tb, 0);
        ok &= a.coverage && b.coverage && !same_pixels(a, b);
        ass_free_track(ta); ass_free_track(tb);
    }
    // The unterminated comment belongs to event 0, including during reverse seeks.
    ASS_Track *events = read_track(lib,
        "{\\fs50[unfinished}First\n"
        "Dialogue: 0,0:00:02.00,0:00:04.00,Default,,0,0,0,,Next [visible]\\NLine");
    ASS_Track *plain = read_track(lib, "Next [visible]\\NLine");
    if (!events || !plain) {
        if (events) ass_free_track(events);
        if (plain) ass_free_track(plain);
        return false;
    }
    Sample expected = capture(renderer, plain, 500);
    for (int pass = 0; pass < 3; pass++) {
        Sample first = capture(renderer, events, 500);
        Sample next = capture(renderer, events, 2500);
        ok &= first.coverage && next.coverage && same_pixels(next, expected) &&
            first.source_intact && next.source_intact &&
            first.buffers_released && next.buffers_released;
    }
    ass_free_track(events); ass_free_track(plain);
    if (!ok) fprintf(stderr, "annotation boundary/source-lifetime check failed\n");
    return ok;
}

int main(void)
{
    ASS_Library *lib = ass_library_init();
    if (!lib) return 1;
    ass_set_message_cb(lib, msg_cb, NULL);
    ASS_Renderer *renderer = ass_renderer_init(lib);
    if (!renderer) { ass_library_done(lib); return 1; }
    ass_set_frame_size(renderer, 1000, 700);
    ass_set_storage_size(renderer, 1000, 700);
    ass_set_fonts(renderer, NULL, "sans-serif", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    bool ok = test_pairs(lib, renderer);
    ok &= test_boundaries(lib, renderer);
    ass_renderer_done(renderer);
    ass_library_done(lib);
    return ok ? 0 : 1;
}
