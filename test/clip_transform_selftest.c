/* Compare rendered clip transforms to independent, explicitly rewritten
 * geometry. A solid drawing makes coverage independent of glyph metrics. */
#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ass.h"

enum { W = 640, H = 360 };
#define RECT "\\clip(100,80,300,240)"
#define IRECT "\\iclip(100,80,300,240)"
#define VECTOR "\\clip(m 100 80 l 300 80 300 240 100 240)"
#define IVECTOR "\\iclip(m 100 80 l 300 80 300 240 100 240)"
#define SCALED "\\clip(2,m 200 160 l 600 160 600 480 200 480)"
#define ISCALED "\\iclip(4,m 800 640 l 2400 640 2400 1920 800 1920)"
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); \
} } while (0)

typedef struct {
    const char *name, *tags, *expected;
    long long time;
    int tolerance;
} Case;

static void quiet(int level, const char *fmt, va_list args, void *data)
{
    (void) level; (void) fmt; (void) args; (void) data;
}

static const char *load_font(ASS_Library *lib)
{
    const char *path = getenv("CLIP_TRANSFORM_TEST_FONT");
    if (!path)
        path = "compare/test/font1.ttf";
    FILE *file = fopen(path, "rb");
    CHECK(file && !fseek(file, 0, SEEK_END));
    long size = ftell(file);
    CHECK(size > 0 && size <= INT_MAX && !fseek(file, 0, SEEK_SET));
    char *data = malloc(size);
    CHECK(data && fread(data, 1, size, file) == (size_t) size);
    fclose(file);
    ass_add_font(lib, path, data, (int) size);
    free(data);
    return path;
}

static ASS_Track *make_track(ASS_Library *lib, const char *tags, bool rgba)
{
    char script[8192];
    int n = snprintf(script, sizeof(script),
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n"
        "ScaledBorderAndShadow: yes\n[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,sans-serif,20,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,"
        "0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1\n"
        "Style: Other,sans-serif,20,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,"
        "0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1\n"
        "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:03.00,Default,,0,0,0,,"
        "{\\an7\\pos(0,0)\\p1%s%s}m 0 0 l 640 0 640 360 0 360\n",
        tags, rgba ? "\\blendmult" : "");
    CHECK(n > 0 && n < (int) sizeof(script));
    ASS_Track *track = ass_read_memory(lib, script, strlen(script), NULL);
    CHECK(track);
    return track;
}

static void add_alpha(uint8_t *frame, int width, int height,
                       int x, int y, unsigned alpha)
{
    if (x < 0 || x >= width || y < 0 || y >= height)
        return;
    uint8_t *dst = &frame[(size_t) y * width + x];
    *dst = alpha + (*dst * (255 - alpha) + 127) / 255;
}

static uint64_t capture(ASS_Renderer *renderer, ASS_Track *track, long long time,
                         bool rgba, int width, int height, uint8_t *frame)
{
    memset(frame, 0, (size_t) width * height);
    int change;
    if (rgba) {
        ASS_ImageRGBA *images = ass_render_frame_rgba(renderer, track, time, &change);
        // Force native RGBA painting so the RGBA vector-mask path is exercised.
        if (images)
            CHECK(ass_frame_needs_rgba(renderer));
        for (ASS_ImageRGBA *img = images; img; img = img->next)
            for (int y = 0; y < img->h; y++)
                for (int x = 0; x < img->w; x++)
                    add_alpha(frame, width, height, img->dst_x + x, img->dst_y + y,
                        img->rgba[(size_t) y * img->stride + 4 * x + 3]);
        ass_free_images_rgba(images);
    } else {
        ASS_Image *images = ass_render_frame(renderer, track, time, &change);
        for (ASS_Image *img = images; img; img = img->next)
            for (int y = 0; y < img->h; y++)
                for (int x = 0; x < img->w; x++)
                    add_alpha(frame, width, height, img->dst_x + x, img->dst_y + y,
                        (img->bitmap[(size_t) y * img->stride + x] *
                         (255 - (img->color & 255)) + 127) / 255);
    }
    uint64_t coverage = 0;
    for (size_t i = 0; i < (size_t) width * height; i++)
        coverage += frame[i];
    return coverage;
}

static void compare_case(ASS_Library *lib, ASS_Renderer *renderer,
                          const Case *tc, bool rgba, int width, int height)
{
    ASS_Track *a = make_track(lib, tc->tags, rgba);
    ASS_Track *b = make_track(lib, tc->expected, rgba);
    size_t size = (size_t) width * height;
    uint8_t *actual = malloc(size), *expected = malloc(size);
    CHECK(actual && expected);
    uint64_t ca = capture(renderer, a, tc->time, rgba, width, height, actual);
    uint64_t cb = capture(renderer, b, tc->time, rgba, width, height, expected);
    // Every table case has visible coverage; empty output cannot falsely pass.
    CHECK(ca && cb);
    for (size_t i = 0; i < size; i++) {
        int diff = abs((int) actual[i] - expected[i]);
        if (diff > tc->tolerance) {
            fprintf(stderr, "%s (%s, %dx%d, %lldms): pixel %zu,%zu = %u vs %u\n",
                tc->name, rgba ? "RGBA" : "legacy", width, height, tc->time,
                i % width, i / width, actual[i], expected[i]);
            exit(1);
        }
    }
    free(actual); free(expected);
    ass_free_track(a); ass_free_track(b);
}

static const Case cases[] = {
    {"rect positive", RECT "\\clippos(20,10)", "\\clip(120,90,320,250)", 0, 0},
    {"rect negative absolute", RECT "\\clippos(40,30)\\clippos(-100,50)",
        "\\clip(0,130,200,290)", 0, 0},
    {"rect bare plus absolute", RECT "\\clippos(40,30)\\clippos(+20,-10)",
        "\\clip(120,70,320,230)", 0, 0},
    {"inverse rect translation", IRECT "\\clippos(20,-10)",
        "\\iclip(120,70,320,230)", 0, 0},
    {"vector translation", VECTOR "\\clippos(20,-10)",
        "\\clip(m 120 70 l 320 70 320 230 120 230)", 0, 0},
    {"inverse vector translation", IVECTOR "\\clippos(20,-10)",
        "\\iclip(m 120 70 l 320 70 320 230 120 230)", 0, 0},
    {"drawing scale 2 translation", SCALED "\\clippos(20,-10)",
        "\\clip(2,m 240 140 l 640 140 640 460 240 460)", 0, 0},
    {"inverse drawing scale 4 translation", ISCALED "\\clippos(20,-10)",
        "\\iclip(4,m 960 560 l 2560 560 2560 1840 960 1840)", 0, 0},
    {"rect identity", RECT "\\clips100\\clippos(0,0)", RECT, 0, 0},
    {"inverse rect identity", IRECT "\\clips100", IRECT, 0, 0},
    {"vector identity", VECTOR "\\clips100\\clippos(0,0)", VECTOR, 0, 0},
    {"inverse vector identity", ISCALED "\\clips100", ISCALED, 0, 0},
    {"rect enlarge centered", RECT "\\clips125", "\\clip(75,60,325,260)", 0, 0},
    {"rect reduce centered", RECT "\\clips80", "\\clip(120,96,280,224)", 0, 0},
    // Different source bounds change matrix quantization (the renderer uses
    // 1/8-pixel transform precision). Allow boundary AA differences when
    // comparing zoom to coordinates rewritten by hand; no-op/order cases stay
    // byte-exact. Even corner coverage differences must stay within 32/255.
    {"vector enlarge centered", VECTOR "\\clips125",
        "\\clip(m 75 60 l 325 60 325 260 75 260)", 0, 32},
    {"vector reduce centered", VECTOR "\\clips80",
        "\\clip(m 120 96 l 280 96 280 224 120 224)", 0, 32},
    {"inverse rect enlarge", IRECT "\\clips125", "\\iclip(75,60,325,260)", 0, 0},
    {"inverse vector reduce", IVECTOR "\\clips80",
        "\\iclip(m 120 96 l 280 96 280 224 120 224)", 0, 32},
    {"inverse zero scale", IVECTOR "\\clips0", "\\iclip(0,0,0,0)", 0, 0},
    {"rect combined", RECT "\\clips125\\clippos(20,-10)",
        "\\clip(95,50,345,250)", 0, 0},
    {"vector combined scaled drawing", SCALED "\\clips125\\clippos(20,-10)",
        "\\clip(m 95 50 l 345 50 345 250 95 250)", 0, 32},
    {"inverse combined scaled drawing", ISCALED "\\clips125\\clippos(20,-10)",
        "\\iclip(m 95 50 l 345 50 345 250 95 250)", 0, 32},
    {"rect animated position", RECT "\\clippos(0,0)\\t(0,1000,\\clippos(120,-40))",
        "\\clip(160,60,360,220)", 500, 0},
    {"vector animated position", VECTOR "\\t(0,1000,\\clippos(120,-40))",
        "\\clip(m 160 60 l 360 60 360 220 160 220)", 500, 0},
    {"inverse vector animated position", IVECTOR "\\t(0,1000,\\clippos(120,-40))",
        "\\iclip(m 160 60 l 360 60 360 220 160 220)", 500, 0},
    {"rect animated scale", RECT "\\clips100\\t(0,1000,\\clips140)",
        "\\clip(80,64,320,256)", 500, 0},
    {"vector animated scale", VECTOR "\\t(0,1000,\\clips140)",
        "\\clip(m 80 64 l 320 64 320 256 80 256)", 500, 32},
    {"inverse vector animated scale", IVECTOR "\\t(0,1000,\\clips140)",
        "\\iclip(m 80 64 l 320 64 320 256 80 256)", 500, 32},
    {"combined animation", VECTOR "\\clippos(0,0)\\clips100"
        "\\t(0,1000,\\clippos(120,-40)\\clips140)",
        "\\clip(m 140 44 l 380 44 380 236 140 236)", 500, 32},
    {"inverse combined animation", IRECT "\\t(0,1000,\\clippos(120,-40)\\clips140)",
        "\\iclip(140,44,380,236)", 500, 0},
    {"relative position", RECT "\\clippos(20,10)\\clippos(~+20,~-10)",
        "\\clip(140,80,340,240)", 0, 0},
    {"vector relative position", VECTOR "\\clippos(20,10)\\clippos(~+20,~-10)",
        "\\clip(m 140 80 l 340 80 340 240 140 240)", 0, 0},
    {"relative scale plus", RECT "\\clips120\\clips~+10",
        "\\clip(70,56,330,264)", 0, 0},
    {"relative scale minus", VECTOR "\\clips120\\clips~-20", VECTOR, 0, 0},
    {"bare plus scale absolute", RECT "\\clips120\\clips+80",
        "\\clip(120,96,280,224)", 0, 0},
    {"relative animated position", RECT "\\clippos(20,10)\\t(0,1000,\\clippos(~+40,~-20))",
        "\\clip(140,80,340,240)", 500, 0},
    {"relative animated scale", RECT "\\clips120\\t(0,1000,\\clips~-40)", RECT, 500, 0},
    {"rect reset", RECT "\\clippos(40,-20)\\clips150\\r", RECT, 0, 0},
    {"vector reset", VECTOR "\\clippos(40,-20)\\clips150\\r", VECTOR, 0, 0},
    {"named inverse reset", IVECTOR "\\clippos(40,-20)\\clips150\\rOther", IVECTOR, 0, 0},
    {"reset then relative", RECT "\\clips150\\clippos(40,20)\\r\\clips~+25\\clippos(~+20,~-10)",
        "\\clip(95,50,345,250)", 0, 0},
    {"ordinary rect control", RECT, "\\clip(100.9,80.1,300.9,240.1)", 0, 0},
    {"ordinary vector scale control", SCALED, VECTOR, 0, 0},
    {"ordinary first vector wins", VECTOR "\\clip(m 0 0 l 20 0 20 20 0 20)", VECTOR, 0, 0},
    {"rect order", "\\clippos(20,-10)\\clips125" RECT,
        RECT "\\clippos(20,-10)\\clips125", 0, 0},
    {"inverse rect order", "\\clippos(20,-10)\\clips125" IRECT,
        IRECT "\\clippos(20,-10)\\clips125", 0, 0},
    {"vector order", "\\clippos(20,-10)\\clips125" VECTOR,
        VECTOR "\\clippos(20,-10)\\clips125", 0, 0},
    {"inverse vector order", "\\clippos(20,-10)\\clips125" IVECTOR,
        IVECTOR "\\clippos(20,-10)\\clips125", 0, 0},
    {"rect replacement keeps state", "\\clip(0,0,40,40)\\clips125\\clippos(20,-10)" RECT,
        "\\clip(95,50,345,250)", 0, 0},
    {"vector replacement keeps state", "\\clip(m 0 0 l 40 0 40 40 0 40)"
        "\\clips125\\clippos(20,-10)" VECTOR,
        "\\clip(m 95 50 l 345 50 345 250 95 250)", 0, 32},
    {"vector replacement before transforms", "\\clip(m 0 0 l 40 0 40 40 0 40)" VECTOR
        "\\clips125\\clippos(20,-10)",
        "\\clip(m 95 50 l 345 50 345 250 95 250)", 0, 32},
    {"rect to inverse vector", RECT "\\clips125\\clippos(20,-10)" IVECTOR,
        "\\iclip(m 95 50 l 345 50 345 250 95 250)", 0, 32},
    {"vector to inverse rect", VECTOR "\\clips125\\clippos(20,-10)" IRECT,
        "\\iclip(95,50,345,250)", 0, 0},
    {"equivalent rect/vector translation", RECT "\\clippos(20,-10)",
        VECTOR "\\clippos(20,-10)", 0, 32},
    {"equivalent rect/vector zoom", RECT "\\clips125\\clippos(20,-10)",
        VECTOR "\\clips125\\clippos(20,-10)", 0, 32},
    {"equivalent inverse zoom", IRECT "\\clips80", IVECTOR "\\clips80", 0, 32},
    // This curve's true Y range is 80..200, whereas its control box ends at
    // 240. The geometric center is (200,140), not (200,160).
    {"Bezier geometric center", "\\clip(m 100 80 b 100 240 300 240 300 80 l 100 80)\\clips150",
        "\\clip(m 50 50 b 50 290 350 290 350 50 l 50 50)", 0, 32},
    {"unpainted move excluded from bounds", "\\clip(m 500 320 m 100 80 l 300 80 300 240 100 240)\\clips125",
        "\\clip(m 75 60 l 325 60 325 260 75 260)", 0, 32},
    {"malformed tuple atomic", RECT "\\clippos(20,-10)\\clippos(~+50,~oops)",
        "\\clip(120,70,320,230)", 0, 0},
    {"empty tuple token rejected", RECT "\\clippos(20,,-10)", RECT, 0, 0},
    {"relative sign required", RECT "\\clips~10\\clippos(~10,~-10)", RECT, 0, 0},
    {"bare negative scale absolute rejected", RECT "\\clips120\\clips-20", RECT "\\clips120", 0, 0},
    {"annotation does not enable replacement", VECTOR
        "[\\clips125]\\clip(m 0 0 l 40 0 40 40 0 40)", VECTOR, 0, 0},
    {"whitespace relative operands", RECT "\\clippos(  ~+20 , ~-10  )\\clips125",
        "\\clip(95,50,345,250)", 0, 0},
};

/* Reuse one track with nonmonotonic frame times to catch accumulated geometry,
 * stale transformed bitmap cache entries and animation endpoint errors. */
static void animation_frames(ASS_Library *lib, ASS_Renderer *renderer,
                              bool rgba, int width, int height)
{
    ASS_Track *track = make_track(lib, SCALED
        "\\t(200,1200,2,\\clippos(120,-40)\\clips140)", rgba);
    const long long times[] = {0, 200, 700, 1200, 1500, 700, 200};
    size_t size = (size_t) width * height;
    uint8_t *a = malloc(size), *b = malloc(size);
    CHECK(a && b);
    for (size_t f = 0; f < sizeof(times) / sizeof(times[0]); f++) {
        double t = (times[f] - 200) / 1000.0;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        double k = t * t;
        char tags[256];
        snprintf(tags, sizeof(tags), SCALED "\\clippos(%g,%g)\\clips%g",
            120 * k, -40 * k, 100 + 40 * k);
        ASS_Track *reference = make_track(lib, tags, rgba);
        CHECK(capture(renderer, track, times[f], rgba, width, height, a));
        CHECK(capture(renderer, reference, times[f], rgba, width, height, b));
        CHECK(!memcmp(a, b, size));
        ass_free_track(reference);
    }
    free(a); free(b);
    ass_free_track(track);
}

static void zero_scale(ASS_Library *lib, ASS_Renderer *renderer,
                        bool rgba, int width, int height)
{
    const char *tags[] = {RECT "\\clips0", VECTOR "\\clips0"};
    uint8_t *frame = malloc((size_t) width * height);
    CHECK(frame);
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        ASS_Track *track = make_track(lib, tags[i], rgba);
        CHECK(!capture(renderer, track, 0, rgba, width, height, frame));
        ass_free_track(track);
    }
    free(frame);
}

int main(void)
{
    ASS_Library *lib = ass_library_init();
    CHECK(lib);
    ass_set_message_cb(lib, quiet, NULL);
    const char *font_path = load_font(lib);
    ASS_Renderer *renderer = ass_renderer_init(lib);
    CHECK(renderer);
    ass_set_fonts(renderer, font_path, "sans-serif", ASS_FONTPROVIDER_NONE, NULL, 1);
    ass_set_storage_size(renderer, W, H);
    for (int config = 0; config < 2; config++) {
        // Integer device scales keep rectangle hard edges comparable to vector
        // AA edges, while testing unequal axis scales and nonzero margins.
        int width = config ? 2 * W + 32 : W;
        int height = config ? 3 * H + 28 : H;
        ass_set_frame_size(renderer, width, height);
        ass_set_margins(renderer, config ? 17 : 0, config ? 11 : 0,
                        config ? 13 : 0, config ? 19 : 0);
        for (int rgba = 0; rgba < 2; rgba++) {
            for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
                compare_case(lib, renderer, &cases[i], rgba, width, height);
            animation_frames(lib, renderer, rgba, width, height);
            zero_scale(lib, renderer, rgba, width, height);
        }
    }
    ass_renderer_done(renderer);
    ass_library_done(lib);
    puts("clip transform regression cases passed");
    return 0;
}
