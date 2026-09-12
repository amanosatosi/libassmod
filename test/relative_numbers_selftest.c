/* Public render entry points, private numeric-state observations, and pixel
 * signatures, following the existing Mangetsu renderer selftests. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ass.h"
#include "ass_render.h"

enum Field {
    FS, SX, SY, SOFT, SCALE, SPACING, FRX, FRY, FRZ, FRS, FAX, FAY, Z,
    BX, BY, SHX, SHY, BLX, BLY, BE, PBO, BOXX, BOXY, BBS, BS2X, BS2Y,
    RNDX, RNDY, RNDZ, FURIX, FURIY, FURISX, FURISY, FURISP,
    P0X, P0Y, P1X, P1Y, P2X, P2Y, P3X, P3Y, POSX, POSY,
    CLIPX0, CLIPY0, CLIPX1, CLIPY1, JITX, JITPERIOD, IMGX, IMGY,
    FSVP, FSHP, COLSP, GLYPHX, GLYPHY, PRIMARY, COUNT
};

typedef struct {
    double values[COUNT];
    uint64_t hash, coverage;
    int images;
    int min_x, min_y, max_x, max_y;
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
    int change;
    ASS_Image *images = ass_render_frame(renderer, track, now, &change);
    const RenderContext *s = &renderer->state;
    Sample out = {
        .values = {
            s->font_size, s->scale_x * 100, s->scale_y * 100,
            s->soft_scale * 100, s->object_scale * 100, s->hspacing,
            s->frx, s->fry, s->frz, s->frs, s->fax, s->fay, s->z,
            s->border_x, s->border_y, s->shadow_x, s->shadow_y,
            s->blur_x, s->blur_y, s->be, s->pbo,
            s->box_extra_x, s->box_extra_y, s->box_border_layers[0].size_x,
            s->border_layers[1].size_x, s->border_layers[1].size_y,
            s->rnd_x, s->rnd_y, s->rnd_z,
            s->furi_offset_x, s->furi_offset_y, s->furi_scale_x, s->furi_scale_y,
            s->furi_hspacing, s->distort.u0, s->distort.v0,
            s->distort.u1, s->distort.v1, s->distort.u2, s->distort.v2,
            s->distort.u3, s->distort.v3, s->pos_x, s->pos_y,
            s->clip_x0, s->clip_y0, s->clip_x1, s->clip_y1,
            s->jitter.left / 8, s->jitter.period / 10000,
            s->image_fill.layer[0].xoffset, s->image_fill.layer[0].yoffset,
            s->fsvp, s->fshp,
            s->column_index < s->text_info.max_columns ?
                s->text_info.column_spacing[s->column_index] : 1,
            s->text_info.length ? s->text_info.glyphs[0].scale_x *
                s->text_info.glyphs[0].scale_fix * 100 : 0,
            s->text_info.length ? s->text_info.glyphs[0].scale_y *
                s->text_info.glyphs[0].scale_fix * 100 : 0,
            s->c[0],
        },
        .hash = UINT64_C(1469598103934665603),
        .min_x = 1000, .min_y = 700,
    };
    for (ASS_Image *img = images; img; img = img->next) {
        out.images++;
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
                if (!a) continue;
                int px = img->dst_x + x, py = img->dst_y + y;
                if (px < out.min_x) out.min_x = px;
                if (py < out.min_y) out.min_y = py;
                if (px + 1 > out.max_x) out.max_x = px + 1;
                if (py + 1 > out.max_y) out.max_y = py + 1;
            }
    }
    return out;
}

static bool near(double a, double b)
{
    return isfinite(a) && fabs(a - b) < 1e-8;
}

static bool same_pixels(Sample a, Sample b)
{
    return a.coverage && a.coverage == b.coverage &&
        a.images == b.images && a.hash == b.hash;
}

static bool compare_samples(ASS_Library *lib, ASS_Renderer *renderer,
                            const char *a, const char *b, long long now,
                            bool numeric_state)
{
    ASS_Track *ta = read_track(lib, a), *tb = read_track(lib, b);
    bool ok = ta && tb;
    if (ok) {
        Sample sa = capture(renderer, ta, now);
        Sample sb = capture(renderer, tb, now);
        ok = same_pixels(sa, sb);
        if (numeric_state)
            for (int f = 0; f < COUNT; f++) {
                if (!near(sa.values[f], sb.values[f])) {
                    fprintf(stderr, "state mismatch at %lld, field %d: %.17g vs %.17g\n",
                            now, f, sa.values[f], sb.values[f]);
                    ok = false;
                }
            }
    }
    if (!ok) fprintf(stderr, "pixel mismatch at %lld: %s versus %s\n", now, a, b);
    if (ta) ass_free_track(ta);
    if (tb) ass_free_track(tb);
    return ok;
}

static bool compare(ASS_Library *lib, ASS_Renderer *renderer,
                    const char *a, const char *b, long long now)
{
    return compare_samples(lib, renderer, a, b, now, false);
}

static bool test_values(ASS_Library *lib, ASS_Renderer *renderer)
{
    const struct { const char *tags; enum Field field; double expected; } cases[] = {
        {"\\fs-10", FS, 30}, {"\\fs+10", FS, 50},
        {"\\fs~-10", FS, 30}, {"\\fs~+10", FS, 50},
        {"\\fs50\\fs-10", FS, 40}, {"\\fs50\\fs~+20", FS, 70},
        {"\\fs50\\r\\fs+10", FS, 50}, {"\\fs50\\rOther\\fs+10", FS, 30},
        {"\\fs5\\fs-10", FS, 40}, // existing nonpositive-result Style fallback
        {"\\frz30\\frz-20", FRZ, -20}, {"\\frz~+20", FRZ, 30},
        {"\\frz~-20", FRZ, -10}, {"\\frz+20", FRZ, 20},
        {"\\fr~+20", FRZ, 30}, {"\\fax~+.2", FAX, .2},
        {"\\fax~-.2", FAX, -.2}, {"\\fax-0.2", FAX, -.2},
        {"\\fay.25\\fay~-.5", FAY, -.25}, {"\\frx~-20", FRX, -20},
        {"\\fry~+20", FRY, 20}, {"\\frs~-5", FRS, -5}, {"\\z~-20", Z, -20},
        {"\\fsp-2", SPACING, -2}, {"\\fsp~-2", SPACING, 0},
        {"\\fscx+20", SX, 100}, {"\\fscy-20", SY, 100},
        {"\\fscx~+20", SX, 100}, {"\\fscy~-20", SY, 100},
        {"\\fsc50", SOFT, 50}, {"\\fsc150", SOFT, 150},
        {"\\fsc50", GLYPHX, 40}, {"\\fsc50", GLYPHY, 60},
        {"\\fsc150", GLYPHX, 120}, {"\\fsc150", GLYPHY, 180},
        {"\\fsc-25", GLYPHX, 60}, {"\\fsc-25", GLYPHY, 90},
        {"\\fscx60\\fscy140\\fsc150", GLYPHX, 90},
        {"\\fscx60\\fscy140\\fsc150", GLYPHY, 210},
        {"\\fsc+50", SOFT, 150}, {"\\fsc-25", SOFT, 75},
        {"\\fsc~+50", SOFT, 150}, {"\\fsc~-25", SOFT, 75},
        {"\\fsc150\\fsc200", SOFT, 200}, {"\\fsc150\\fsc+50", SOFT, 200},
        {"\\fsc150\\fsc-50", SOFT, 100},
        {"\\fsc150\\fscx60\\fscy140", SX, 60},
        {"\\fsc150\\fscx60\\fscy140", SY, 140},
        {"\\fsc150\\fscx60\\fscy140", SOFT, 150},
        {"\\fsc150\\r\\fsc-25", SOFT, 75},
        {"\\fsc150\\rOther\\fsc+50", SX, 120},
        {"\\fsc150\\fsc", SOFT, 100},
        {"\\scale+50", SCALE, 150}, {"\\scale-20", SCALE, 80},
        {"\\scale50\\scale+50", SCALE, 100}, {"\\scale~-20", SCALE, 80},
        {"\\scale50\\r\\scale+50", SCALE, 150},
        {"\\scale50\\rOther\\scale+50", SCALE, 150},
        {"\\bord+3", BX, 5}, {"\\bord~-1", BY, 1},
        {"\\xbord5\\ybord7\\bord-2", BX, 3},
        {"\\xbord5\\ybord7\\bord-2", BY, 5},
        {"\\bord-10", BX, 0}, {"\\xbord+3", BX, 5}, {"\\ybord-1", BY, 1},
        {"\\shad-1", SHX, 2}, {"\\xshad-2", SHX, -2},
        {"\\xshad~-2", SHX, 1}, {"\\yshad~+2", SHY, 5},
        {"\\xblur2\\yblur4\\blur+1", BLX, 3},
        {"\\xblur2\\yblur4\\blur+1", BLY, 5},
        {"\\blur2\\xblur-1", BLX, 1}, {"\\blur2\\yblur~+1", BLY, 3},
        {"\\be2\\be+3", BE, 5}, {"\\be2\\be-10", BE, 0},
        {"\\2bs3\\2bs+2", BS2X, 5}, {"\\2bs3\\2bsx-2", BS2X, 1},
        {"\\2bs3\\2bsy~+2", BS2Y, 5}, {"\\bbs2\\bbs+3", BBS, 5},
        {"\\boxpx5\\boxpy8\\boxp-2", BOXX, 3},
        {"\\boxpx5\\boxpy8\\boxp-2", BOXY, 6},
        {"\\boxpx~+3", BOXX, 3}, {"\\boxpy~+4", BOXY, 4},
        {"\\rndx2\\rndy4\\rnd+1", RNDX, 3},
        {"\\rndx2\\rndy4\\rnd+1", RNDY, 5}, {"\\rndz~+2", RNDZ, 2},
        {"\\furipos(10,20)\\furipos(~+5,~-3)", FURIX, 15},
        {"\\furipos(10,20)\\furipos(~+5,~-3)", FURIY, 17},
        {"\\furisx+10", FURISX, 60}, {"\\furisy-10", FURISY, 40},
        {"\\furis+10", FURISY, 60}, {"\\furifsp~-2", FURISP, -2},
        {"\\pbo10\\pbo~-3", PBO, 7},
        {"\\clip(10,20,900,600)\\clip(~+5,~-5,~+10,~-10)", CLIPX0, 15},
        {"\\clip(10,20,900,600)\\clip(~+5,~-5,~+10,~-10)", CLIPY1, 590},
        {"\\distort(~+.1,0,1,1,0,1)", P1X, 1.1},
        {"\\distort(-.1,0,1,1,0,1)", P1X, -.1},
        {"\\distort(1,0,1,1,0,1,~+.2,~-.1)", P0X, .2},
        {"\\distort(1,0,1,1,0,1,~+.2,~-.1)", P0Y, -.1},
        {"\\distort(1,0,1,1,0,1,.2,.1)\\distort(~+.1,0,1,1,0,1)", P0X, 0},
        {"\\jitter(2,0,0,0,50)\\jitter(+1,0,0,0,+25)", JITX, 3},
        {"\\jitter(2,0,0,0,50)\\jitter(+1,0,0,0,+25)", JITPERIOD, 75},
        {"\\fs~10", FS, 40}, {"\\fs+-10", FS, 40},
        {"\\scale~+oops", SCALE, 100},
        {"\\fs50\\fs~+", FS, 50}, {"\\fs50\\fs~+10junk", FS, 50},
        {"\\fs50\\fs~+1e999", FS, 50}, {"\\fs50\\fs~+nan", FS, 50},
        {"\\fs50\\fs40junk", FS, 40}, // retain lax absolute prefixes
        {"\\scale50\\scale40junk", SCALE, 50}, // retain strict absolute tags
        {"\\fs( ~+10 )", FS, 50}, {"\\scale( ~+10 )", SCALE, 110},
        {"\\pos(500,350)\\pos(~+10junk,~-20)", POSX, 500},
        {"\\clip(10,20,900,600)\\clip(~+5,~oops,900,600)", CLIPX0, 10},
        {"\\clip(10,20,900,600)\\clip( ~+5 , ~-5 , ~+10 , ~-10 )", CLIPY1, 590},
        {"\\distort(1,0,1,1,0,1,.2,.1)\\distort(1,0,1,1,0,1,~+.3)", P0X, .2},
        {"\\fsvp10\\fsvp~-3", FSVP, 7}, {"\\fshp10\\fshp~+3", FSHP, 13},
        {"\\col1\\colsp3\\colsp+2", COLSP, 5},
        {"\\img(\"\",10,20)\\img(\"\",~+2,~-3)", IMGX, 12},
        {"\\img(\"\",10,20)\\img(\"\",~+2,~-3)", IMGY, 17},
        {"\\pos(-100,350)", POSX, -100},
        {"\\1c&H00FF00&\\alpha&H20&", PRIMARY, 0x00FF0020},
        {"\\fs~+\xEF\xBC\x91\xEF\xBC\x90", FS, 50},
        {"\\fax~-\xEF\xBC\x90.\xEF\xBC\x92", FAX, -.2},
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char text[4096];
        snprintf(text, sizeof(text), "{%s}Relative", cases[i].tags);
        ASS_Track *track = read_track(lib, text);
        if (!track) return false;
        Sample out = capture(renderer, track, 0);
        double got = out.values[cases[i].field];
        if (!near(got, cases[i].expected)) {
            fprintf(stderr, "%s field %d: got %.17g, expected %.17g\n",
                    cases[i].tags, cases[i].field, got, cases[i].expected);
            ok = false;
        }
        ass_free_track(track);
    }
    return ok;
}

static bool test_transforms(ASS_Library *lib, ASS_Renderer *renderer)
{
    const struct { const char *from, *relative, *absolute; } cases[] = {
        {"\\fs40", "\\fs+20", "\\fs60"}, {"\\fs40", "\\fs-10", "\\fs30"},
        {"\\fs40", "\\fs~+20", "\\fs60"}, {"\\fs40", "\\fs~-10", "\\fs30"},
        {"\\frz30", "\\frz-20", "\\frz-20"},
        {"\\frz30", "\\frz~-20", "\\frz10"}, {"\\frz30", "\\frz~+20", "\\frz50"},
        {"\\fsc50", "\\fsc+50", "\\fsc100"},
        {"\\fsc50", "\\fsc~+50", "\\fsc100"},
        {"\\scale50", "\\scale+50", "\\scale100"},
        {"\\fscx50", "\\fscx+50", "\\fscx100"},
        {"\\fscy150", "\\fscy-50", "\\fscy100"},
        {"\\img(\"\",10,20)", "\\img(\"\",~+2,~-3)", "\\img(\"\",12,17)"},
        {"\\fax.25\\fs40\\bord2", "\\fax~-.5\\fs+10.5\\bord+1.25", "\\fax-.25\\fs50.5\\bord3.25"},
        {"\\pos(500,500)", "\\pos(~+200,400)", "\\pos(700,400)"},
        {"\\pos(500,500)", "\\pos(~+200,~-100)", "\\pos(700,400)"},
        {"\\move(400,300,600,400,0,1000)",
         "\\move(~+20,~-10,~+40,~+10,0,1000)", "\\move(420,290,640,410,0,1000)"},
        {"\\mover(400,300,600,400,0,90,10,20)",
         "\\mover(~+20,~-10,~+40,~+10,~-10,~+10,~-5,~+5)",
         "\\mover(420,290,640,410,-10,100,5,25)"},
        {"\\moves3(400,300,500,200,600,400)",
         "\\moves3(~+20,~-10,~+30,~-20,~+40,~+10)",
         "\\moves3(420,290,530,180,640,410)"},
        {"\\moves4(400,300,450,200,550,200,600,400)",
         "\\moves4(~+20,~-10,~+30,~-20,~+30,~-20,~+40,~+10)",
         "\\moves4(420,290,480,180,580,180,640,410)"},
        {"\\distort(1,0,1,1,0,1,.125,.25)",
         "\\distort(~+.25,0,1,1,0,1,~+.125,~-.125)", "\\distort(1.25,0,1,1,0,1,.25,.125)"},
    };
    const char *timing[] = {"", "2,", "300,500,", "300,500,2,"};
    const long long times[] = {0, 300, 400, 500, 1000, 1999};
    bool ok = true;
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
        for (size_t f = 0; f < sizeof(timing) / sizeof(timing[0]); f++)
            for (size_t t = 0; t < sizeof(times) / sizeof(times[0]); t++) {
                char a[2048], b[2048];
                snprintf(a, sizeof(a), "{%s\\t(%s%s)}Relative", cases[c].from, timing[f], cases[c].relative);
                snprintf(b, sizeof(b), "{%s\\t(%s%s)}Relative", cases[c].from, timing[f], cases[c].absolute);
                ok &= compare_samples(lib, renderer, a, b, times[t], true);
            }
    return ok;
}

static bool test_automatic_position(ASS_Library *lib, ASS_Renderer *renderer)
{
    bool ok = true;
    for (int style = 0; style < 2; style++) {
        for (int align = 1; align <= 9; align++) {
            char base[256], zero[256], shifted[256], animated[256], mixed[256];
            const char *reset = style ? "\\rOther" : "";
            snprintf(base, sizeof(base), "{%s\\an%d\\bord0\\shad0\\frz0}Margins", reset, align);
            snprintf(zero, sizeof(zero), "{%s\\an%d\\bord0\\shad0\\frz0\\pos(~+0,~+0)}Margins", reset, align);
            snprintf(shifted, sizeof(shifted), "{%s\\an%d\\bord0\\shad0\\frz0"
                     "\\pos(~+20,~-10)\\pos(~+44,~+42)}Margins", reset, align);
            snprintf(animated, sizeof(animated), "{%s\\an%d\\bord0\\shad0\\frz0"
                     "\\t(250,1250,\\pos(~+128,~+64))}Margins", reset, align);
            snprintf(mixed, sizeof(mixed), "{%s\\an%d\\bord0\\shad0\\frz0\\pos(~+64,400)}Margins", reset, align);
            ok &= compare(lib, renderer, base, zero, 0);
            ok &= compare(lib, renderer, animated, shifted, 750);
            ASS_Track *tz = read_track(lib, zero), *ts = read_track(lib, shifted);
            ASS_Track *tm = read_track(lib, mixed);
            if (!tz || !ts || !tm) {
                if (tz) ass_free_track(tz);
                if (ts) ass_free_track(ts);
                if (tm) ass_free_track(tm);
                return false;
            }
            Sample a = capture(renderer, tz, 0), b = capture(renderer, ts, 0);
            Sample m = capture(renderer, tm, 0);
            if (!near(b.values[POSX] - a.values[POSX], 64) ||
                    !near(b.values[POSY] - a.values[POSY], 32) ||
                    !near(m.values[POSX] - a.values[POSX], 64) ||
                    !near(m.values[POSY], 400) ||
                    b.min_x - a.min_x != 64 || b.min_y - a.min_y != 32) {
                fprintf(stderr, "automatic placement failed for style %d, alignment %d\n", style, align);
                ok = false;
            }
            ass_free_track(tz); ass_free_track(ts); ass_free_track(tm);
        }
    }
    return ok;
}

static bool test_overlap(ASS_Library *lib, ASS_Renderer *renderer)
{
    const char *text = "{\\fs40\\t(0,1000,\\fs+20)\\t(250,1250,\\fs-10)}Relative";
    const long long times[] = {0, 250, 500, 1000, 1250};
    const double sizes[] = {40, 45, 47.5, 52.5, 50};
    bool ok = true;
    for (int i = 0; i < 5; i++) {
        char fixed[128];
        snprintf(fixed, sizeof(fixed), "{\\fs%.17g}Relative", sizes[i]);
        ok &= compare(lib, renderer, text, fixed, times[i]);
    }
    // The second position's relative target is resolved at its own start:
    // first motion is at X=450 at 250ms, so relative +200 targets X=650.
    ok &= compare(lib, renderer,
        "{\\pos(500,350)\\t(0,500,\\pos(~-100,350))"
        "\\t(250,1000,\\pos(~+200,350))}Relative",
        "{\\pos(500,350)\\t(0,500,\\pos(400,350))"
        "\\t(250,1000,\\pos(650,350))}Relative", 750);
    // An ordinary relative position translates the existing animation once.
    const struct { const char *a, *b; } positions[] = {
        {"\\pos(500,350)\\t(0,1000,\\pos(700,450))\\pos(~+20,~-10)",
         "\\pos(520,340)\\t(0,1000,\\pos(720,440))"},
        {"\\t(0,1000,\\pos(~+100,~-20))\\pos(~+20,~+10)",
         "\\pos(~+20,~+10)\\t(0,1000,\\pos(~+100,~-20))"},
        {"\\t(0,1000,\\pos(700,450))\\pos(~+20,~-10)",
         "\\pos(~+20,~-10)\\t(0,1000,\\pos(720,440))"},
        {"\\pos(500,350)\\t(0,1000,\\pos(~+100,~-20))\\pos(~+20,400)",
         "\\pos(520,400)\\t(0,1000,\\pos(~+100,400))"},
        {"\\move(400,350,600,450,0,1000)\\pos(~+20,~-10)",
         "\\move(420,340,620,440,0,1000)"},
        {"\\pos(~+20,~-10)\\pos(700,450)", "\\pos(~+20,~-10)"},
        {"\\pos(~+20,~-10)\\move(400,350,600,450)", "\\pos(~+20,~-10)"},
        {"\\pos(~+20,400)\\pos(520,~-10)", "\\pos(520,390)"},
    };
    for (size_t i = 0; i < sizeof(positions) / sizeof(positions[0]); i++) {
        char a[1024], b[1024];
        snprintf(a, sizeof(a), "{%s}Relative", positions[i].a);
        snprintf(b, sizeof(b), "{%s}Relative", positions[i].b);
        for (int t = 0; t <= 1000; t += 250)
            ok &= compare(lib, renderer, a, b, t);
    }
    return ok;
}

static bool test_event_reset(ASS_Library *lib, ASS_Renderer *renderer)
{
    // A single track alternates a modified event and an untouched event.
    const char *text = "{\\fs+20\\fsc+50\\scale+25\\pos(~+10,~-20)"
        "\\distort(1,0,1,1,0,1,~+.1,~+.1)}Changed\n"
        "Dialogue: 0,0:00:02.00,0:00:04.00,Default,,0,0,0,,Untouched";
    ASS_Track *track = read_track(lib, text);
    ASS_Track *plain = read_track(lib, "Untouched");
    if (!track || !plain) {
        if (track) ass_free_track(track);
        if (plain) ass_free_track(plain);
        return false;
    }
    Sample expected = capture(renderer, plain, 0);
    bool ok = true;
    for (int pass = 0; pass < 3; pass++) {
        capture(renderer, track, 500);
        Sample actual = capture(renderer, track, 2500);
        ok &= same_pixels(expected, actual);
        for (int f = 0; f < COUNT; f++)
            ok &= near(expected.values[f], actual.values[f]);
    }
    if (!ok) fprintf(stderr, "relative state leaked between events\n");
    ass_free_track(track);
    ass_free_track(plain);
    return ok;
}

static bool test_frame_margins(ASS_Library *lib, ASS_Renderer *renderer)
{
    bool ok = true;
    ass_set_frame_size(renderer, 1200, 900);
    ass_set_margins(renderer, 100, 100, 100, 100);
    ass_set_use_margins(renderer, 1);
    for (int an = 1; an <= 9; an++) {
        char a[256], b[256];
        // \pbo0 gives the reference the same existing hard-override policy as
        // \pos. Player margin/style preferences still obey that policy.
        snprintf(a, sizeof(a), "{\\an%d\\pbo0\\frz20\\fax.2}Margins", an);
        snprintf(b, sizeof(b), "{\\an%d\\pos(~+0,~+0)\\frz20\\fax.2}Margins", an);
        ASS_Track *ta = read_track(lib, a), *tb = read_track(lib, b);
        if (!ta || !tb) {
            if (ta) ass_free_track(ta);
            if (tb) ass_free_track(tb);
            ok = false;
            break;
        }
        ta->events[0].MarginL = tb->events[0].MarginL = 90;
        ta->events[0].MarginR = tb->events[0].MarginR = 80;
        ta->events[0].MarginV = tb->events[0].MarginV = 70;
        ok &= same_pixels(capture(renderer, ta, 0), capture(renderer, tb, 0));
        ass_free_track(ta); ass_free_track(tb);
    }
    ass_set_frame_size(renderer, 1000, 700);
    ass_set_margins(renderer, 0, 0, 0, 0);
    ass_set_use_margins(renderer, 0);
    if (!ok) fprintf(stderr, "automatic position with frame/event margins failed\n");
    return ok;
}

static bool test_seeking(ASS_Library *lib, ASS_Renderer *renderer, bool automatic)
{
    char text[1024];
    snprintf(text, sizeof(text), "{%s\\fs40\\fsc50"
        "\\t(0,1000,\\fs+20\\fsc+50\\pos(~+100,~-50))"
        "\\t(250,1250,2,\\fs-10\\fsc+25\\pos(~-100,~+80))}Relative",
        automatic ? "" : "\\pos(500,350)");
    ASS_Track *track = read_track(lib, text);
    if (!track) return false;
    const long long times[] = {0, 250, 500, 750, 1000, 1250, 1750};
    Sample direct[7];
    bool ok = true;
    for (int i = 0; i < 7; i++) {
        ASS_Renderer *fresh = ass_renderer_init(lib);
        if (!fresh) { ass_free_track(track); return false; }
        ass_set_frame_size(fresh, 1000, 700);
        ass_set_storage_size(fresh, 1000, 700);
        ass_set_fonts(fresh, NULL, "sans-serif", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
        direct[i] = capture(fresh, track, times[i]);
        ass_renderer_done(fresh);
    }
    for (int pass = 0; pass < 3; pass++)
        for (int j = 0; j < 7; j++) {
            int i = pass == 0 ? j : pass == 1 ? 6 - j : (j * 3) % 7;
            Sample out = capture(renderer, track, times[i]);
            if (!same_pixels(out, direct[i])) {
                fprintf(stderr, "seek mismatch at %lld in pass %d\n", times[i], pass);
                ok = false;
            }
            for (int f = 0; f < COUNT; f++)
                if (!near(out.values[f], direct[i].values[f])) ok = false;
        }
    ass_free_track(track);
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
    bool ok = test_values(lib, renderer);
    const struct { const char *a, *b; } pairs[] = {
        {"{\\fsc50}Ratio", "{\\fscx40\\fscy60}Ratio"},
        {"{\\fsc150}Ratio", "{\\fscx120\\fscy180}Ratio"},
        {"{\\fsc-25}Ratio", "{\\fscx60\\fscy90}Ratio"},
        {"{\\fscx60\\fscy140\\fsc150}Ratio", "{\\fscx90\\fscy210}Ratio"},
        {"{\\fsc150\\fscx60\\fscy140}Ratio", "{\\fscx90\\fscy210}Ratio"},
        {"{\\fsc50\\scale200}Ratio", "{\\bord4\\shad6}Ratio"},
        {"{\\blur1\\fsc50\\scale200}Ratio", "{\\blur2\\bord4\\shad6}Ratio"},
        {"{\\bs4\\bord0\\shad0\\boxp8\\bbs2\\fsc200}Ratio",
         "{\\bs4\\bord0\\shad0\\boxp4\\bbs1\\fscx160\\fscy240}Ratio"},
        {"{\\fs50}A{\\fs-10}B", "{\\fs50}A{\\fs40}B"},
        {"{\\fs50}{\\fs+20}A", "{\\fs70}A"},
        {"{\\fs50\\rOther\\fs+10}A", "{\\rOther\\fs30}A"},
        {"{\\pos(500,350)\\pos(~-100,~+50)}A", "{\\pos(400,400)}A"},
        {"{\\distort(1.5,-.125,1.25,1,-.25,1)}A",
         "{\\distort(1.5,-.125,1.25,1,-.25,1,0,0)}A"},
    };
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
        ok &= compare(lib, renderer, pairs[i].a, pairs[i].b, 500);
    ok &= test_transforms(lib, renderer);
    ok &= test_automatic_position(lib, renderer);
    ok &= test_overlap(lib, renderer);
    ok &= test_event_reset(lib, renderer);
    ok &= test_frame_margins(lib, renderer);
    ok &= test_seeking(lib, renderer, false);
    ok &= test_seeking(lib, renderer, true);
    ass_renderer_done(renderer);
    ass_library_done(lib);
    return ok ? 0 : 1;
}
