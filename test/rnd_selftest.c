/* Public rendering with private integer state observations and pixel masks. */
#include "ass.h"
#include "ass_render.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 640
#define H 360
#define PIXELS (W * H)
#define DRAW "m 0 0 l 180 0 180 100 90 145 0 100"
static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "rnd render line %d: %s\n", __LINE__, #c); failures++; } } while (0)

typedef struct {
    uint8_t *pixels;
    int32_t x, y, z, seed;
    uint64_t coverage[3];
    unsigned images[3];
} Sample;

/* Full GlyphInfo copies include large gradient arrays. Snapshot only logical
 * metrics so layout coverage fits within the default Windows stack budget. */
typedef struct {
    unsigned symbol;
    ASS_Vector advance, pos;
    char linebreak;
    int asc, desc;
    ASS_Rect bbox;
    int32_t effect_timing, effect_skip_timing;
} GlyphLayout;

static void quiet(int level, const char *fmt, va_list args, void *data)
{
    (void) level; (void) fmt; (void) args; (void) data;
}

static ASS_Track *read_track(ASS_Library *library, const char *text)
{
    char script[8192];
    int n = snprintf(script, sizeof(script),
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n"
        "ScaledBorderAndShadow: yes\n"
        "[V4+ Styles]\n"
        "Format: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,OutlineColour,BackColour,"
        "Bold,Italic,Underline,StrikeOut,ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,"
        "Alignment,MarginL,MarginR,MarginV,Encoding\n"
        "Style: Default,Arial,32,&H00FFFFFF,&H0000FFFF,&H00000000,&H00000000,"
        "0,0,0,0,100,100,0,0,1,0,0,5,160,160,20,1\n"
        "[Events]\n"
        "Format: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text\n"
        "Dialogue: 0,0:00:00.00,0:00:02.00,Default,,0,0,0,,%s\n", text);
    if (n < 0 || n >= (int) sizeof(script)) return NULL;
    return ass_read_memory(library, script, n, NULL);
}

static Sample capture(ASS_Renderer *renderer, ASS_Track *track, long long time)
{
    Sample out = {.pixels = calloc(3, PIXELS)};
    if (!out.pixels) { failures++; return out; }
    int changed;
    ASS_Image *images = ass_render_frame(renderer, track, time, &changed);
    const RenderContext *s = &renderer->state;
    out.x = s->rnd_x; out.y = s->rnd_y; out.z = s->rnd_z; out.seed = s->rnd_seed;
    for (ASS_Image *img = images; img; img = img->next) {
        if ((unsigned) img->type > 2) { failures++; continue; }
        out.images[img->type]++;
        for (int y = 0; y < img->h; y++) {
            int py = img->dst_y + y;
            if (py < 0 || py >= H) continue;
            for (int x = 0; x < img->w; x++) {
                int px = img->dst_x + x;
                if (px < 0 || px >= W) continue;
                uint8_t a = img->bitmap[y * img->stride + x];
                uint8_t *dest = &out.pixels[img->type * PIXELS + py * W + px];
                if (a > *dest) *dest = a;
            }
        }
    }
    for (int k = 0; k < 3; k++)
        for (int p = 0; p < PIXELS; p++) out.coverage[k] += out.pixels[k * PIXELS + p];
    return out;
}

static Sample sample(ASS_Library *lib, ASS_Renderer *renderer, const char *tags,
                      long long time)
{
    char text[4096];
    snprintf(text, sizeof(text), "{\\an7\\pos(120,90)\\org(120,90)\\p1%s}" DRAW, tags);
    ASS_Track *track = read_track(lib, text);
    if (!track) { failures++; return (Sample) {0}; }
    Sample out = capture(renderer, track, time);
    ass_free_track(track);
    return out;
}

static int equal(const Sample *a, const Sample *b, int layers)
{
    return a->pixels && b->pixels && !memcmp(a->pixels, b->pixels, layers * PIXELS);
}

static int equal_layer(const Sample *a, const Sample *b, int layer)
{
    return a->pixels && b->pixels &&
        !memcmp(a->pixels + layer * PIXELS, b->pixels + layer * PIXELS, PIXELS);
}

static uint64_t exterior_border_coverage(const Sample *sample, const Sample *fill)
{
    if (!sample->pixels || !fill->pixels) return 0;
    uint64_t coverage = 0;
    for (int p = 0; p < PIXELS; p++)
        if (!fill->pixels[p]) coverage += sample->pixels[PIXELS + p];
    return coverage;
}

static void parser_cases(ASS_Library *lib, ASS_Renderer *renderer)
{
    static const struct {const char *tags; int time; int32_t x,y,z,seed;} cases[] = {
        {"\\rnd1",0,8,8,8,0}, {"\\rndx1",0,8,0,0,0},
        {"\\rndy1",0,0,8,0,0}, {"\\rndz1",0,0,0,8,0},
        {"\\rnd-1",0,-8,-8,-8,0}, {"\\rndx2\\rndx+1",0,8,0,0,0},
        {"\\rndx2\\rndx~+1",0,24,0,0,0},
        {"\\rnd2\\rnd~+1",0,24,24,24,0},
        {"\\rndx2\\rndy4\\rnd~+1",0,24,40,8,0},
        {"\\rnd2\\rndx",0,0,16,16,0}, {"\\rnd2\\rndy",0,16,0,16,0},
        {"\\rnd2\\rndz",0,16,16,0,0}, {"\\rnd2\\rnd",0,0,0,0,0},
        {"\\rnd2\\rndsABC\\r",0,0,0,0,0},
        {"\\rndx0.124",0,0,0,0,0}, {"\\rndx0.125",0,1,0,0,0},
        {"\\rndx0.126",0,1,0,0,0}, {"\\rndx-0.124",0,0,0,0,0},
        {"\\rndx-0.125",0,-1,0,0,0}, {"\\rndx-0.126",0,-1,0,0,0},
        {"\\t(0,1000,\\rnd0.249)",500,0,0,0,0},
        {"\\t(0,1000,\\rnd0.249)",750,1,1,1,0},
        {"\\t(0,1000,\\rnd0.251)",500,1,1,1,0},
        {"\\rnd1\\t(0,1000,\\rnd3)",500,16,16,16,0},
        {"\\rnd1\\t(0,1000,\\rndx3)",500,16,8,8,0},
        {"\\rnd1\\t(0,1000,\\rndy3)",500,8,16,8,0},
        {"\\rnd1\\t(0,1000,\\rndz3)",500,8,8,16,0},
        {"\\rnd1\\t(0,1000,2,\\rnd3)",500,12,12,12,0},
        {"\\rnd1\\t(0,1000,\\rnd-1)",750,-4,-4,-4,0},
        {"\\rnd1\\t(0,1000,\\rnd)",500,0,0,0,0},
        {"\\rnd1\\t(1000,1500,\\rnd \t)",0,0,0,0,0},
        {"\\rndx1\\t(1000,1500,\\rndx0.999995)",0,7,0,0,0},
        {"\\rnds0",0,0,0,0,0}, {"\\rnds1",0,0,0,0,1},
        {"\\rnds10",0,0,0,0,16}, {"\\rndsabc",0,0,0,0,2748},
        {"\\rnds0x12345678",0,0,0,0,0x12345678},
        {"\\rnds-1",0,0,0,0,-1},
        {"\\rnds7fffffff",0,0,0,0,INT32_MAX},
        {"\\rndsffffffff",0,0,0,0,INT32_MAX}, // Windows wcstol saturation
        {"\\rnds1\\rnds",0,0,0,0,0},
        {"\\rnds1\\t(1000,1500,\\rnds \t)",0,0,0,0,0},
        {"\\t(0,1000,\\rnds10)",500,0,0,0,8},
        {"\\rndNaN",0,0,0,0,0}, {"\\rndinf",0,0,0,0,0},
        {"\\rndx20",0,160,0,0,0}, {"\\rndx20.375",0,163,0,0,0},
        {"\\rndx20.5",0,164,0,0,0}, {"\\rndx21",0,168,0,0,0},
        {"\\rndx30",0,240,0,0,0}, {"\\rndx100",0,800,0,0,0}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Sample s = sample(lib, renderer, cases[i].tags, cases[i].time);
        if (s.x != cases[i].x || s.y != cases[i].y || s.z != cases[i].z || s.seed != cases[i].seed) {
            fprintf(stderr, "%s @%d: got %d,%d,%d seed %d\n", cases[i].tags,
                    cases[i].time, s.x,s.y,s.z,s.seed);
            failures++;
        }
        CHECK(s.coverage[0]);
        free(s.pixels);
    }
}

static void geometry_cases(ASS_Library *lib, ASS_Renderer *renderer)
{
    static const struct {const char *transform, *rnd;} cases[] = {
        {"","\\rndx30"}, {"","\\rndy30"}, {"","\\rndz100"}, {"","\\rnd30"},
        {"\\frx35","\\rndz100"}, {"\\fry35","\\rndz100"},
        {"\\fscx150\\fscy80","\\rnd30"}, {"\\fax0.3\\fay0.2","\\rnd30"},
        {"\\frz25","\\rnd30"}, {"\\frx25\\fry15","\\rnd30"},
        {"\\z80","\\rnd30"}, {"\\ortho1\\frx25\\fry15","\\rndz100"},
        {"\\distort(1,0,1.15,1,-0.1,1,0.1,0.05)","\\rnd30"},
        {"\\p2","\\rnd30"}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char tags[1024];
        snprintf(tags, sizeof(tags), "%s%s", cases[i].transform, cases[i].rnd);
        Sample plain = sample(lib, renderer, cases[i].transform, 0);
        Sample rnd = sample(lib, renderer, tags, 0);
        CHECK(plain.coverage[0] && rnd.coverage[0]);
        CHECK(!equal(&plain, &rnd, 1));
        Sample repeat = sample(lib, renderer, tags, 0);
        CHECK(equal(&rnd, &repeat, 3));
        free(plain.pixels); free(rnd.pixels); free(repeat.pixels);
    }
    Sample plain = sample(lib, renderer, "", 0);
    Sample negative = sample(lib, renderer, "\\rnd-100", 0);
    Sample explicit_zero = sample(lib, renderer, "\\rnd0\\rnds0", 0);
    CHECK(equal(&plain,&negative,3) && equal(&plain,&explicit_zero,3));
    Sample seed0 = sample(lib, renderer, "\\rnd30", 0);
    Sample seed0_explicit = sample(lib, renderer, "\\rnd30\\rnds0", 0);
    Sample seed1 = sample(lib, renderer, "\\rnd30\\rnds1", 0);
    CHECK(equal(&seed0,&seed0_explicit,3) && !equal(&seed0,&seed1,1));
    Sample ortho = sample(lib, renderer, "\\ortho1\\rndx30", 0);
    Sample ortho_z = sample(lib, renderer, "\\ortho1\\rndx30\\rndz100", 0);
    /* Disabled rotations mean orthographic depth alone has no XY effect.
     * Enabling Z changes RNG consumption, so isolate Z in a separate case. */
    Sample ortho_depth = sample(lib, renderer, "\\ortho1\\rndz100", 0);
    CHECK(equal(&plain,&ortho_depth,1));
    CHECK(!equal(&ortho,&ortho_z,1));
    /* Keep the shadow setting identical: the existing ASS projection origin
     * includes shadow offsets, which affect a randomized local Z coordinate. */
    Sample fill = sample(lib, renderer, "\\rnd30\\rndsabc\\shad4", 0);
    Sample border = sample(lib, renderer, "\\rnd30\\rndsabc\\bord3\\shad4", 0);
    Sample multi = sample(lib, renderer, "\\rnd30\\rndsabc\\xbord2\\ybord3\\2bs4\\3bs2\\shad4", 0);
    CHECK(equal(&fill,&border,1));
    CHECK(equal(&fill,&multi,1));
    CHECK(border.coverage[1] && border.coverage[2]);
    /* Opaque ordinary borders retain the fill, while multi-border masks are
     * disjoint rings. Compare only the exterior of the common fill. */
    CHECK(exterior_border_coverage(&multi,&fill) > exterior_border_coverage(&border,&fill));
    CHECK(multi.images[IMAGE_TYPE_OUTLINE] == 3 && multi.coverage[2]);
    /* The outer cumulative geometry is 2+4+2 by 3+4+2. Its shadow must be
     * exactly the one produced by widening the same randomized base once. */
    Sample outer = sample(lib, renderer, "\\rnd30\\rndsabc\\xbord8\\ybord9\\shad4", 0);
    CHECK(equal(&fill,&outer,1));
    CHECK(equal_layer(&multi,&outer,IMAGE_TYPE_SHADOW));
    /* Without depth perturbation, adding borders/shadow cannot change fill. */
    Sample xy_fill = sample(lib, renderer, "\\rndx30\\rndy30\\rndsabc", 0);
    Sample xy_multi = sample(lib, renderer, "\\rndx30\\rndy30\\rndsabc\\xbord2\\ybord3\\2bs4\\3bs2\\shad4", 0);
    CHECK(equal(&xy_fill,&xy_multi,1));
    Sample large30 = sample(lib, renderer, "\\rndx30", 0);
    Sample large100 = sample(lib, renderer, "\\rndx100", 0);
    Sample old_limit = sample(lib, renderer, "\\rndx21", 0);
    CHECK(!equal(&large30,&old_limit,1) && !equal(&large100,&old_limit,1));
    Sample *samples[] = {&plain,&negative,&explicit_zero,&seed0,&seed0_explicit,&seed1,
        &ortho,&ortho_z,&ortho_depth,&fill,&border,&multi,&outer,&xy_fill,&xy_multi,
        &large30,&large100,&old_limit};
    for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) free(samples[i]->pixels);
    // Finite but unsafe amplitudes must fail geometry construction safely.
    Sample invalid = sample(lib,renderer,"\\rnd1e100\\bord3\\2bs4",0);
    CHECK(invalid.x == INT32_MAX && invalid.y == INT32_MAX && invalid.z == INT32_MAX);
    free(invalid.pixels);
}

static void animation(ASS_Library *lib, ASS_Renderer *renderer)
{
    const char *text = "{\\an7\\pos(120,90)\\org(120,90)\\rndx1\\rndy2\\rndz3\\rnds1"
        "\\t(0,1000,\\rnd30\\rnds100)\\p1}" DRAW;
    ASS_Track *track = read_track(lib,text);
    CHECK(track);
    if (!track) return;
    Sample midpoint = capture(renderer,track,500);
    Sample later = capture(renderer,track,900);
    Sample back = capture(renderer,track,0);
    Sample seek = capture(renderer,track,500);
    CHECK(midpoint.x == 124 && midpoint.y == 128 && midpoint.z == 132 && midpoint.seed == 128);
    CHECK(equal(&midpoint,&seek,3));
    CHECK(!equal(&midpoint,&back,1) && !equal(&midpoint,&later,1));
    Sample explicit_mid = sample(lib,renderer,"\\rndx15.5\\rndy16\\rndz16.5\\rnds80",500);
    CHECK(equal(&midpoint,&explicit_mid,3));
    track->events[0].ReadOrder = 999;
    Sample reordered = capture(renderer,track,500);
    CHECK(equal(&midpoint,&reordered,3));
    free(reordered.pixels);
    free(midpoint.pixels); free(later.pixels); free(back.pixels); free(seek.pixels); free(explicit_mid.pixels);
    ass_free_track(track);
}

static void drawing_oracle(ASS_Library *lib, ASS_Renderer *renderer)
{
    /* Five control points: the last takes the eighth X random value, not the
     * fifth. Expected points are fixed, rounded 1/64-pixel path coordinates.
     * p7 supplies those coordinates to the ordinary rasterization path. */
    static const struct {const char *tags, *expected;} cases[] = {
        {"\\rndx30",
         "m 216 0 l 11548 0 11683 6400 6000 9280 -83 6400"},
        {"\\rndx30\\frz90",
         "m 0 -216 l 0 -11548 6400 -11683 9280 -6000 6400 83"},
        {"\\rndx30\\fscx150\\fax0.3\\frz90",
         "m 0 -323 l 0 -17321 6400 -20404 9280 -13175 6400 -2756"},
        {"\\rndy30\\fscx150\\fax0.3\\frz90",
         "m 216 -97 l 28 -17292 6563 -20233 9520 -12924 6317 -2843"}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char text[2048];
        snprintf(text,sizeof(text),"{\\an7\\pos(320,220)\\org(320,220)\\p1%s}" DRAW,cases[i].tags);
        ASS_Track *actual_track = read_track(lib,text);
        snprintf(text,sizeof(text),"{\\an7\\pos(320,220)\\p7}%s",cases[i].expected);
        ASS_Track *expected_track = read_track(lib,text);
        CHECK(actual_track && expected_track);
        if (actual_track && expected_track) {
            Sample actual = capture(renderer,actual_track,0);
            Sample expected = capture(renderer,expected_track,0);
            CHECK(actual.pixels && expected.pixels && actual.coverage[0] && expected.coverage[0]);
            if (actual.pixels && expected.pixels) {
                uint64_t difference = 0;
                int max_difference = 0;
                for (int p = 0; p < PIXELS; p++) {
                    int delta = abs(actual.pixels[p] - expected.pixels[p]);
                    difference += delta;
                    if (delta > max_difference) max_difference = delta;
                }
                /* The cached control path quantizes transforms to 1/8 pixel.
                 * Allow up to two such steps along a 1000-pixel perimeter;
                 * the fixed path/RNG unit vectors themselves have no slack. */
                CHECK(difference <= 1000 * 255 / 4 && max_difference <= 80);
            }
            free(actual.pixels); free(expected.pixels);
        }
        if (actual_track) ass_free_track(actual_track);
        if (expected_track) ass_free_track(expected_track);
    }
}

static void layout(ASS_Library *lib, ASS_Renderer *renderer)
{
    const char *content = "WRAP WRAP WRAP WRAP WRAP";
    const char *texts[] = {
        "{\\kf100}WRAP WRAP WRAP WRAP WRAP",
        "{\\rnd100\\kf100}WRAP WRAP WRAP WRAP WRAP"
    };
    GlyphLayout logical[32];
    struct {int32_t leftmost, timing; double origin_y;} karaoke[32];
    unsigned runs = 0;
    int count = (int) strlen(content);
    CHECK(count <= 32);
    if (count > 32) return;
    Sample samples[2];
    for (int pass = 0; pass < 2; pass++) {
        ASS_Track *track = read_track(lib,texts[pass]);
        CHECK(track);
        if (!track) return;
        samples[pass] = capture(renderer,track,500);
        /* Cleanup clears the count and frees owned geometry, but root scalar
         * layout fields remain in the allocated glyph array. Never inspect
         * freed linked glyphs or bitmap pointers here. */
        GlyphInfo *glyphs = renderer->state.text_info.glyphs;
        CHECK(glyphs && renderer->state.text_info.max_glyphs >= count);
        if (glyphs && renderer->state.text_info.max_glyphs >= count) {
            for (int i = 0; i < count; i++) {
                if (!pass) logical[i] = (GlyphLayout) {
                    .symbol = glyphs[i].symbol,
                    .advance = glyphs[i].advance,
                    .pos = glyphs[i].pos,
                    .linebreak = glyphs[i].linebreak,
                    .asc = glyphs[i].asc,
                    .desc = glyphs[i].desc,
                    .bbox = glyphs[i].bbox,
                    .effect_timing = glyphs[i].effect_timing,
                    .effect_skip_timing = glyphs[i].effect_skip_timing,
                };
                else {
                    CHECK(logical[i].symbol == glyphs[i].symbol);
                    CHECK(logical[i].advance.x == glyphs[i].advance.x);
                    CHECK(logical[i].advance.y == glyphs[i].advance.y);
                    CHECK(logical[i].pos.x == glyphs[i].pos.x && logical[i].pos.y == glyphs[i].pos.y);
                    CHECK(logical[i].linebreak == glyphs[i].linebreak);
                    CHECK(logical[i].asc == glyphs[i].asc && logical[i].desc == glyphs[i].desc);
                    CHECK(!memcmp(&logical[i].bbox,&glyphs[i].bbox,sizeof(ASS_Rect)));
                    CHECK(logical[i].effect_timing == glyphs[i].effect_timing);
                    CHECK(logical[i].effect_skip_timing == glyphs[i].effect_skip_timing);
                }
            }
        }
        unsigned run_count = renderer->state.text_info.n_bitmaps;
        CHECK(run_count <= 32);
        if (!pass) runs = run_count;
        else CHECK(runs == run_count);
        for (unsigned i = 0; i < run_count && i < 32; i++) {
            const CombinedBitmapInfo *info = &renderer->state.text_info.combined_bitmaps[i];
            if (!pass) {
                karaoke[i].leftmost = info->leftmost_x;
                karaoke[i].timing = info->effect_timing;
                karaoke[i].origin_y = info->karaoke_origin_y;
            } else if (i < runs) {
                CHECK(karaoke[i].leftmost == info->leftmost_x);
                CHECK(karaoke[i].timing == info->effect_timing);
                CHECK(karaoke[i].origin_y == info->karaoke_origin_y);
            }
        }
        ass_free_track(track);
    }
    CHECK(!equal(&samples[0],&samples[1],1));
    free(samples[0].pixels); free(samples[1].pixels);
}

int main(void)
{
    ASS_Library *lib = ass_library_init();
    if (!lib) return 1;
    ass_set_message_cb(lib,quiet,NULL);
    ASS_Renderer *renderer = ass_renderer_init(lib);
    if (!renderer) { ass_library_done(lib); return 1; }
    ass_set_frame_size(renderer,W,H);
    ass_set_fonts(renderer,getenv("RND_TEST_FONT"),NULL,1,NULL,1);
    parser_cases(lib,renderer);
    geometry_cases(lib,renderer);
    animation(lib,renderer);
    drawing_oracle(lib,renderer);
    layout(lib,renderer);
    ass_renderer_done(renderer);
    ass_library_done(lib);
    return failures ? 1 : 0;
}
