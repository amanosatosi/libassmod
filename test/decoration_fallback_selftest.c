/* SPDX-License-Identifier: ISC
 * Selected-font decoration ownership, independently of physical glyph fallback.
 * Real reproduction with MUA Better: {\u1}〈ရာဇဝင်ဖြစ်သွားသူ〉.
 * Original rectangle fixtures replace that font so CI is deterministic and
 * does not redistribute MUA Better. Expected geometry uses selected-font
 * metrics, as in VSFilter's selected GDI font, not a continuity-only heuristic.
 */
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ass.h"
#include "ass_render.h"

#define WIDTH 640
#define HEIGHT 400
#define LEFT_ANGLE "\xE3\x80\x88"
#define RIGHT_ANGLE "\xE3\x80\x89"

static int failures;

static void check(bool ok, const char *name)
{
    if (!ok) {
        fprintf(stderr, "decoration fallback: %s\n", name);
        failures++;
    }
}

static void quiet(int level, const char *fmt, va_list va, void *data)
{
    (void) level; (void) fmt; (void) va; (void) data;
}

static bool add_font(ASS_Library *lib, const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    unsigned char data[4096];
    size_t size = fread(data, 1, sizeof(data), file);
    bool ok = size && size < sizeof(data) && !ferror(file);
    fclose(file);
    if (ok) ass_add_font(lib, (char *) path, (char *) data, size);
    return ok;
}

static ASS_Track *read_track(ASS_Library *lib, const char *text)
{
    char script[8192];
    int size = snprintf(script, sizeof(script),
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 400\n"
        "ScaledBorderAndShadow: yes\n"
        "[V4+ Styles]\n"
        "Format: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,OutlineColour,BackColour,"
        "Bold,Italic,Underline,StrikeOut,ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,"
        "Alignment,MarginL,MarginR,MarginV,Encoding\n"
        "Style: Default,Deco Primary A,64,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,"
        "0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1\n"
        "Style: Other,Deco Primary B,96,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,"
        "0,0,1,0,100,100,0,0,1,0,0,7,0,0,0,1\n"
        "[Events]\nFormat: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text\n"
        "Dialogue: 0,0:00:00.00,0:00:02.00,Default,,0,0,0,,{\\pos(32,32)}%s\n", text);
    return size > 0 && size < sizeof(script) ? ass_read_memory(lib, script, size, NULL) : NULL;
}

typedef struct {
    unsigned char white[HEIGHT][WIDTH];
    unsigned char red[HEIGHT][WIDTH];
    uint64_t hash;
} Sample;

static bool capture(ASS_Renderer *renderer, ASS_Track *track, long long time, Sample *out)
{
    memset(out, 0, sizeof(*out));
    int change;
    ASS_Image *images = ass_render_frame(renderer, track, time, &change);
    if (!images) return false;
    out->hash = UINT64_C(1469598103934665603);
    for (ASS_Image *img = images; img; img = img->next) {
        unsigned char (*mask)[WIDTH] = img->color >> 8 == 0xff0000 ? out->red :
            img->color >> 8 == 0xffffff ? out->white : NULL;
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w; x++) {
                unsigned a = img->bitmap[y * img->stride + x];
                int px = img->dst_x + x, py = img->dst_y + y;
                if (mask && px >= 0 && px < WIDTH && py >= 0 && py < HEIGHT &&
                        a > mask[py][px])
                    mask[py][px] = a;
                out->hash ^= a;
                out->hash *= UINT64_C(1099511628211);
            }
        const int fields[] = {img->dst_x, img->dst_y, img->w, img->h, img->color};
        for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
            out->hash ^= (uint32_t) fields[i];
            out->hash *= UINT64_C(1099511628211);
        }
    }
    return true;
}

static bool interval(const unsigned char mask[HEIGHT][WIDTH], int x,
                     double *top, double *bottom)
{
    int first = HEIGHT, last = -1;
    for (int y = 0; y < HEIGHT; y++)
        if (mask[y][x]) {
            if (first == HEIGHT) first = y;
            last = y;
        }
    if (last < 0) return false;
    *top = first + 1 - mask[first][x] / 255.0;
    *bottom = last + mask[last][x] / 255.0;
    return true;
}

typedef struct { int x, font, size, decoration; } Probe;

static void geometry(ASS_Library *lib, ASS_Renderer *renderer, Sample *sample,
                     const char *text, const Probe *probes, size_t count)
{
    ASS_Track *track = read_track(lib, text);
    bool ok = track && capture(renderer, track, 500, sample);
    if (!ok) { check(false, text); if (track) ass_free_track(track); return; }
    for (size_t i = 0; i < count; i++) {
        const Probe *p = probes + i;
        double top = 0, baseline = 0, deco_top = 0, deco_bottom = 0;
        bool body = interval(sample->white, p->x, &top, &baseline);
        bool deco = interval(sample->red, p->x, &deco_top, &deco_bottom);
        // Each fixture's ink ends exactly at the baseline. Get its subpixel
        // position from the independent white glyph coverage at this column.
        double height = p->font ? 1000 : 2048;
        double position = p->decoration == 2 ? (p->font ? -400 : -600) :
            (p->font ? 150 : 900);
        double thickness = p->decoration == 2 ? (p->font ? 200 : 128) :
            (p->font ? 240 : 100);
        double expected_center = baseline + p->size * position / height;
        double expected_size = p->size * thickness / height;
        bool matched = body && (p->decoration ? deco &&
            fabs((deco_top + deco_bottom) / 2 - expected_center) < 1.1 &&
            fabs(deco_bottom - deco_top - expected_size) < 1.1 : !deco);
        if (!matched) {
            fprintf(stderr, "probe x=%d font=%d fs=%d decoration=%d: "
                "body=%d deco=%d baseline=%g bounds=[%g,%g] expected center=%g size=%g\n%s\n",
                p->x, p->font, p->size, p->decoration, body, deco,
                body ? baseline : 0, deco ? deco_top : 0, deco ? deco_bottom : 0,
                body ? expected_center : 0, expected_size, text);
            failures++;
        }
    }
    ass_free_track(track);
}

#ifdef DECO_TEST_INTERNAL
static void outline_geometry(const char *primary_path, const char *fallback_path)
{
    FT_Library ft = NULL;
    FT_Face primary = NULL, fallback = NULL;
    if (FT_Init_FreeType(&ft) || FT_New_Face(ft, primary_path, 0, &primary) ||
            FT_New_Face(ft, fallback_path, 0, &fallback)) {
        check(false, "open outline fixtures");
        goto done;
    }
    check(!FT_Get_Char_Index(primary, 'B') && FT_Get_Char_Index(fallback, 'B'),
          "fixtures necessarily switch physical faces");
    // Fixed 26.6 bounds from A's metrics/cell height, independent of glyph face.
    const struct { int size, u0, u1, s0, s1; } bounds[] = {
        {32, 850, 950, -664, -536}, {64, 1700, 1900, -1328, -1072},
        {96, 2550, 2850, -1992, -1608},
    };
    for (unsigned z = 0; z < sizeof(bounds) / sizeof(bounds[0]); z++) {
        ass_face_set_size(primary, bounds[z].size);
        ass_face_set_size(fallback, bounds[z].size);
        for (int physical = 0; physical < 2; physical++) {
            FT_Face face = physical ? fallback : primary;
            for (unsigned flags = 1; flags < 16; flags++) {
                if (!(flags & (DECO_UNDERLINE | DECO_STRIKETHROUGH))) continue;
                bool loaded = !FT_Load_Glyph(face, FT_Get_Char_Index(face, physical ? 'B' : 'A'),
                                            FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP);
                ASS_Outline bare = {0}, outline = {0};
                int32_t advance = 0, bare_advance = 0;
                bool ok = loaded && ass_get_glyph_outline(&bare, &bare_advance,
                    face, primary, flags & DECO_ROTATE) &&
                    ass_get_glyph_outline(&outline, &advance, face, primary, flags);
                unsigned prefix = flags & DECO_ONLY ? 0 : bare.n_points;
                int lines = !!(flags & DECO_UNDERLINE) + !!(flags & DECO_STRIKETHROUGH);
                ok &= outline.n_points == prefix + 4 * lines && advance == bare_advance;
                for (unsigned i = 0; ok && i < prefix; i++)
                    ok &= outline.points[i].x == bare.points[i].x &&
                          outline.points[i].y == bare.points[i].y;
                for (int line = 0; ok && line < lines; line++) {
                    bool underline = line == 0 && (flags & DECO_UNDERLINE);
                    int y0 = underline ? bounds[z].u0 : bounds[z].s0;
                    int y1 = underline ? bounds[z].u1 : bounds[z].s1;
                    int min_y = INT32_MAX, max_y = INT32_MIN, max_x = 0;
                    for (int p = 0; p < 4; p++) {
                        ASS_Vector point = outline.points[prefix + 4 * line + p];
                        if (point.y < min_y) min_y = point.y;
                        if (point.y > max_y) max_y = point.y;
                        if (point.x > max_x) max_x = point.x;
                    }
                    ok &= min_y == y0 && max_y == y1 && max_x == advance;
                }
                if (!ok) fprintf(stderr, "outline fs=%d physical=%d flags=%u\n",
                                 bounds[z].size, physical, flags);
                check(ok, "exact decoration geometry, advance and unchanged rotated glyph");
                ass_outline_free(&bare);
                ass_outline_free(&outline);
            }
        }
    }
done:
    if (fallback) FT_Done_Face(fallback);
    if (primary) FT_Done_Face(primary);
    if (ft) FT_Done_FreeType(ft);
}
#endif

int main(int argc, char **argv)
{
    if (argc != 5) return 1;
#ifdef DECO_TEST_INTERNAL
    outline_geometry(argv[1], argv[3]);
#endif
    ASS_Library *lib = ass_library_init();
    ASS_Renderer *renderer = lib ? ass_renderer_init(lib) : NULL;
    Sample *sample = calloc(1, sizeof(*sample));
    if (!lib || !renderer || !sample) goto fail;
    ass_set_message_cb(lib, quiet, NULL);
    for (int i = 1; i < argc; i++) if (!add_font(lib, argv[i])) goto fail;
    ass_set_frame_size(renderer, WIDTH, HEIGHT);
    ass_set_fonts(renderer, NULL, "Deco Fallback", ASS_FONTPROVIDER_NONE, NULL, 0);
    ass_set_hinting(renderer, ASS_HINTING_NONE);
    const Probe a[] = {{40,0,64,1},{72,0,64,1},{104,0,64,1}};
    const Probe long_a[] = {{40,0,64,1},{72,0,64,1},{104,0,64,1},{136,0,64,1},{168,0,64,1}};
    const Probe b[] = {{40,1,64,1},{72,1,64,1},{104,1,64,1}};
    const Probe fn[] = {{40,0,64,1},{72,1,64,1},{104,0,64,1}};
    const Probe fs[] = {{40,0,64,1},{76,0,96,1},{120,0,64,1}};
    const Probe reset[] = {{40,0,64,1},{76,1,96,1},{120,0,64,0}};
    const Probe toggle[] = {{40,0,64,1},{72,0,64,0},{104,0,64,1}};
    const Probe strike[] = {{40,0,64,2},{72,0,64,2},{104,0,64,2}};
    const Probe strike_fn[] = {{40,0,64,2},{72,1,64,2},{104,0,64,2}};
    for (int shaping = 0; shaping < 2; shaping++) {
        ass_set_shaper(renderer, shaping ? ASS_SHAPING_COMPLEX : ASS_SHAPING_SIMPLE);
        geometry(lib, renderer, sample, "{\\u1\\5c&H0000FF&}ACA", a, 3);
        geometry(lib, renderer, sample, "{\\u1\\5c&H0000FF&}ABC", a, 3);
        // Inspect surviving array entries before the next frame overwrites them.
        const GlyphInfo *glyphs = renderer->state.text_info.glyphs;
        check(glyphs[0].face_index == 0 && glyphs[1].face_index != 0 &&
              glyphs[2].face_index == 0 && glyphs[0].font == glyphs[1].font,
              "automatic fallback changes glyph face, not ASS font ownership");
        geometry(lib, renderer, sample, "{\\u1\\5c&H0000FF&}ACBCA", long_a, 5);
        geometry(lib, renderer, sample, "{\\u1\\5c&H0000FF&}" LEFT_ANGLE "ACA" RIGHT_ANGLE, long_a, 5);
        geometry(lib, renderer, sample, "{\\fnDeco Complete\\u1\\5c&H0000FF&}ABC", a, 3);
        geometry(lib, renderer, sample, "{\\u1\\5c&H0000FF&}A{\\fnDeco Primary B}B{\\fnDeco Primary A}C", fn, 3);
        geometry(lib, renderer, sample, "{\\u1\\5c&H0000FF&}A{\\fs96}B{\\fs64}C", fs, 3);
        geometry(lib, renderer, sample, "{\\u1\\5c&H0000FF&}A{\\u0}B{\\u1}C", toggle, 3);
        geometry(lib, renderer, sample, "{\\u1\\5c&H0000FF&}A{\\rOther\\5c&H0000FF&}B{\\r\\5c&H0000FF&}C", reset, 3);
        geometry(lib, renderer, sample, "{\\s1\\5c&H0000FF&}ABC", strike, 3);
        geometry(lib, renderer, sample, "{\\s1\\5c&H0000FF&}A{\\fnDeco Primary B}B{\\fnDeco Primary A}C", strike_fn, 3);
        // Reuse the same fallback glyph through two authoritative font objects,
        // with intervening size changes, and return to the earlier cache entry.
        for (int repeat = 0; repeat < 4; repeat++) {
            geometry(lib, renderer, sample, "{\\fnDeco Primary B\\u1\\5c&H0000FF&}ABC", b, 3);
            geometry(lib, renderer, sample, "{\\u1\\5c&H0000FF&}A{\\fs96}B{\\fs64}C", fs, 3);
            geometry(lib, renderer, sample, "{\\u1\\5c&H0000FF&}ABC", a, 3);
        }
    }
    const char *stress[] = {
        "{\\u1\\s1\\bord2\\shad3}" LEFT_ANGLE "ABC" RIGHT_ANGLE,
        "{\\u1\\s1\\5c&H0000FF&\\bord2\\shad3\\rnd30\\rnds42}" LEFT_ANGLE "ABC" RIGHT_ANGLE,
        "{\\fn@Deco Primary A\\u1\\s1\\5c&H0000FF&}" LEFT_ANGLE "ABC" RIGHT_ANGLE,
        "{\\vert1\\u1\\s1\\5c&H0000FF&\\rnd30\\rnds42}" LEFT_ANGLE "ABC" RIGHT_ANGLE,
    };
    for (unsigned i = 0; i < sizeof(stress) / sizeof(stress[0]); i++) {
        ASS_Track *track = read_track(lib, stress[i]);
        bool ok = track && capture(renderer, track, 500, sample);
        uint64_t expected = sample->hash;
        const long long times[] = {1500,0,700,500};
        for (unsigned t = 0; ok && t < sizeof(times) / sizeof(times[0]); t++)
            ok &= capture(renderer, track, times[t], sample) && sample->hash == expected;
        check(ok, stress[i]);
        if (track) ass_free_track(track);
    }
    free(sample);
    ass_renderer_done(renderer);
    ass_library_done(lib);
    return !!failures;
fail:
    free(sample);
    if (renderer) ass_renderer_done(renderer);
    if (lib) ass_library_done(lib);
    return 1;
}
