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
    int line_border_style;
    bool line_border_style_set;
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
    out.line_border_style = s->line_border_style;
    out.line_border_style_set = s->line_border_style_set;
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
            fabs(a.frz - b.frz) < 1e-8 && a.explicit == b.explicit &&
            a.line_border_style_set == b.line_border_style_set &&
            a.line_border_style == b.line_border_style;
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

static bool test_prescans(ASS_Library *lib, ASS_Renderer *renderer)
{
    // Insert comments/markers between every source byte, including tag names,
    // signs, digits and UTF-8 continuation bytes. The original compacting path
    // and the new streaming prescans must agree on the same clean source.
    const char *blocks[] = {
        "\\bs4", "\\bs+0004", "\\bs( +0004 )", "\\bsjunk(4)",
        "\\bs4 \\bs1", "\\bs4\t\\bs1", "\\bs(4 )\\bs1",
        "\\bs000000000000000000000000000000000000000000000000000000000000000000004",
        "\\bs999999999999999999999999999999999999999999999999999999999999999999999\\bs1",
        "\\bs-4\\bs1", "\\bs+-4\\bs1", "\\bs4.0\\bs1", "\\bs4x\\bs1",
        "\\bs\v+4", "\\bs4\v\\bs1", "\\bs + 4\\bs1",
        "\\bs\xEF\xBC\x94", "\\bs\xF0\x9D\x9F\x9C", // Unicode 4
        "\\bs\xC0\xAB" "4", "\\bs\xC0\xA0" "4", // existing permissive UTF-8
        "\\bs4\xC0\xA0\\bs1", // trailing whitespace is still ASCII-only
        "\\bs\xEF\xBC\\bs1", // truncated UTF-8 stays invalid
        "\\t(0,500,\\bs4)", "\\t(0,500,\\t(0,500,\\bs4))",
        "\\t (0,500,\\bs4)", "\\unknown(\\bs4)\\bs1",
        "\\t(0,500,\\bs(4))\\bs1", "\\bs(4", "\\t(0,500,\\bs4",
        "\\t((\\bs4)",
        "\\col1\\colsp8", "\\col0", "\\col+1", "\\col01", "\\col(1)",
        "\\col 1 \\colsp8", "\\col\xEF\xBC\x91\\colsp8",
        "\\col\xF0\x9D\x9F\x99\\colsp8", // Unicode 1
        "\\col\xEF\xBC", "\\col\v1", "\\col1\v", "\\unknown(\\col1)",
        "\\t(0,500,\\col1)", "\\col1\\col0\\col1",
        "\\pos(500,300)", "\\move(400,300,600,300)",
        "\\mover(400,300,600,300)", "\\moves3(400,300,500,200,600,300)",
        "\\moves4(400,300,450,200,550,200,600,300)",
        "\\movevc(0,0)", "\\jitter(0,0,0,0)", "\\clip(0,0,1000,700)",
        "\\iclip(0,0,1,1)", "\\org(500,350)", "\\pbo0", "\\p0",
        "\\positions", "\\ position", // preserve prefix/whitespace quirks
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
        char annotated[4096], clean[1024];
        char *out = annotated;
        *out++ = '{';
        for (const char *p = blocks[i]; *p; p++) {
            const char ignored[] = "[,(\\bs5\\col1\\pos(1,1))]\\N";
            if ((size_t) (out - annotated) + sizeof(ignored) + 20 >= sizeof(annotated))
                return false;
            memcpy(out, ignored, sizeof(ignored) - 1);
            out += sizeof(ignored) - 1;
            *out++ = *p;
        }
        strcpy(out, "[]}Left|Right");
        snprintf(clean, sizeof(clean), "{%s}Left|Right", blocks[i]);
        ok &= check_pair(lib, renderer, annotated, clean, 0);
        ok &= check_pair(lib, renderer, annotated, clean, 400);
    }
    const struct { const char *a, *b; } edges[] = {
        {"{\\bs[unclosed\\bs5}Text", "{\\bs}Text"},
        {"{\\t([)]0,[,]500,\\b[x]s([)]4[)]))}Text", "{\\t(0,500,\\bs(4))}Text"},
        {"{\\t[ ](0,500,\\bs4)[\\bs5]}Text", "{\\t(0,500,\\bs4)}Text"},
        {"{[abc[\\bs5]\\bs4}Text", "{\\bs4}Text"},
        {"{[abc[\\col1]\\col0}Left|Right", "{\\col0}Left|Right"},
        {"{\\co[unclosed\\col1}Left|Right", "{\\co}Left|Right"},
        {"{\\cl[unclosed\\pos(1,1)}Text", "{\\cl}Text"},
        {"{[\\bs5\\col1\\pos(1,1)]}Text{\\bs1}Next", "{}Text{\\bs1}Next"},
        {"{\\[x]N\\bs4}Text", "{\\N\\bs4}Text"},
    };
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++)
        ok &= check_pair(lib, renderer, edges[i].a, edges[i].b, 400);

    // Fix baseline expectations too, so an identical regression in both the
    // clean and annotated scans cannot make the differential pairs pass.
    const struct { const char *text; int border; bool hard; } expected[] = {
        {"{\\bs+0004}Text", 4, false},
        {"{\\bsjunk(4)}Text", 4, false},
        {"{\\bs4.0\\bs1}Text", 1, false},
        {"{\\bs4 \\bs1}Text", 1, false},
        {"{\\bs4\t\\bs1}Text", 1, false},
        {"{\\bs(4 )\\bs1}Text", 1, false},
        {"{\\bs4\v\\bs1}Text", 1, false},
        {"{\\bs\v+4}Text", 4, false},
        {"{\\bs\xC0\xAB" "4}Text", 4, false},
        {"{\\bs\xC0\xA0" "4}Text", 4, false},
        {"{\\bs4\xC0\xA0\\bs1}Text", 1, false},
        {"{\\t (0,500,\\bs4)\\bs1}Text", 1, false},
        {"{\\t((\\bs4)}Text", 4, false},
        {"{[\\pos(1,1)]\\bs1}Text", 1, false},
        {"{\\positions\\bs1}Text", 1, true},
        {"{\\ position\\bs1}Text", 1, false},
        // The old hard-override scan does not strip an unmatched '{' suffix.
        {"{\\bs1}Text {[\\pos(1,1)", 1, true},
    };
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        ASS_Track *track = read_track(lib, expected[i].text);
        if (!track) return false;
        Sample s = capture(renderer, track, 400);
        if (!s.coverage || s.line_border_style != expected[i].border ||
                !s.line_border_style_set || s.explicit != expected[i].hard) {
            fprintf(stderr, "prescan baseline changed: %s\n", expected[i].text);
            ok = false;
        }
        ass_free_track(track);
    }
    ok &= check_pair(lib, renderer, "{\\col+1}Left|Right", "{}Left|Right", 0);
    ok &= check_pair(lib, renderer, "{\\col01}Left|Right", "{}Left|Right", 0);
    ok &= check_pair(lib, renderer, "{\\col(1)}Left|Right", "{}Left|Right", 0);
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
    ok &= test_prescans(lib, renderer);
    ok &= test_boundaries(lib, renderer);
    ass_renderer_done(renderer);
    ass_library_done(lib);
    return ok ? 0 : 1;
}
