/* Alpha syntax regressions: inspect exact ASS alpha bytes and actual RGBA
 * pixels. No dependence on image tile ordering or a particular system font. */
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ass.h"

enum { W = 640, H = 360, PLANES = 3 };
#define BODY "MMMMMMMM"
#define POSITION "\\an5\\pos(320,180)"
static const char header[] =
    "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n"
    "ScaledBorderAndShadow: yes\n[V4+ Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
    "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
    "Alignment, MarginL, MarginR, MarginV, Encoding\n"
    "Style: Default,sans-serif,40,&H00FFFFFF,&H00FFFFFF,&H00FFFFFF,&H00FFFFFF,"
    "0,0,0,0,100,100,0,0,1,4,6,5,20,20,20,1\n"
    "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";

typedef struct { uint8_t *pixels; uint64_t coverage[PLANES]; } Frame;

static ASS_Track *track_for(ASS_Library *lib, const char *text)
{
    size_t n = strlen(header) + strlen(text) + 128;
    char *script = malloc(n);
    if (!script) return NULL;
    snprintf(script, n, "%sDialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,%s\n", header, text);
    ASS_Track *track = ass_read_memory(lib, script, strlen(script), NULL);
    free(script);
    return track;
}

static bool render(ASS_Library *lib, ASS_Renderer *renderer, const char *text,
                   long long now, Frame *frame)
{
    ASS_Track *track = track_for(lib, text);
    if (!track) return false;
    frame->pixels = calloc((size_t) PLANES * W * H, 4);
    if (!frame->pixels) { ass_free_track(track); return false; }
    ASS_ImageRGBA *images = ass_render_frame_rgba(renderer, track, now, NULL);
    bool ok = true;
    for (ASS_ImageRGBA *img = images; img; img = img->next) {
        if (img->type >= PLANES) continue;
        for (int y = 0; y < img->h; y++) for (int x = 0; x < img->w; x++) {
            int px = img->dst_x + x, py = img->dst_y + y;
            if (px < 0 || px >= W || py < 0 || py >= H) continue;
            const uint8_t *src = img->rgba + (size_t) y * img->stride + 4 * x;
            uint8_t *dst = frame->pixels + ((size_t) img->type * W * H + py * W + px) * 4;
            for (int c = 0; c < 4; c++) {
                if (c < 3 && src[c] > src[3]) ok = false;
                dst[c] = src[c] + (dst[c] * (255 - src[3]) + 127) / 255;
            }
        }
    }
    for (int type = 0; type < PLANES; type++)
        for (size_t i = 0; i < (size_t) W * H; i++)
            frame->coverage[type] += frame->pixels[((size_t) type * W * H + i) * 4 + 3];
    ass_free_images_rgba(images); ass_free_track(track);
    return ok;
}

static bool equivalent(ASS_Library *lib, ASS_Renderer *renderer, const char *actual,
                       const char *expected, long long now, const char *label)
{
    Frame a = {0}, b = {0};
    bool ok = render(lib, renderer, actual, now, &a) && render(lib, renderer, expected, now, &b) &&
              !memcmp(a.pixels, b.pixels, (size_t) PLANES * W * H * 4);
    if (!ok) fprintf(stderr, "%s: %s != %s\n", label, actual, expected);
    free(a.pixels); free(b.pixels);
    return ok;
}

/* Verify the exact inverse-alpha byte, independent of rasterizer coverage. */
static bool alpha_byte(ASS_Library *lib, ASS_Renderer *renderer, const char *text,
                       long long now, int type, unsigned expected)
{
    ASS_Track *track = track_for(lib, text);
    if (!track) return false;
    bool ok = true;
    int covered = 0;
    for (ASS_Image *img = ass_render_frame(renderer, track, now, NULL); img; img = img->next) {
        if (img->type != type) continue;
        bool ink = false;
        for (int y = 0; y < img->h; y++) for (int x = 0; x < img->w; x++)
            ink |= img->bitmap[y * img->stride + x] != 0;
        if (!ink) continue;
        ok &= (img->color & 255) == expected;
        covered++;
    }
    ass_free_track(track);
    Frame rgba = {0};
    ok &= render(lib, renderer, text, now, &rgba);
    if (expected == 255) ok &= rgba.coverage[type] == 0;
    else ok &= covered > 0 && rgba.coverage[type] > 0;
    free(rgba.pixels);
    if (!ok) fprintf(stderr, "alpha byte %u on target %d failed: %s\n", expected, type, text);
    return ok;
}

static bool scalar_values(ASS_Library *lib, ASS_Renderer *renderer)
{
    const struct { const char *value; unsigned byte; } values[] = {
        {"F3", 243}, {"50", 80}, {"10", 16}, {"FF", 255}, {"&H80&", 128},
        {"&H50&", 80}, {"$50", 50}, {"$100", 100}, {"$128", 128}, {"$255", 255}, {"$0", 0},
        {"$200", 200}, {"$\xE1\x81\x81\xE1\x81\x82\xE1\x81\x88", 128},
        {"$\xD9\xA1\xD9\xA2\xD9\xA8", 128}, {"$1\xEF\xBC\x92\xE0\xA5\xAE", 128},
        {"000000000000FF", 255}, {"$00000000000128", 128},
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        char text[256];
        snprintf(text, sizeof(text), "{" POSITION "\\alpha%s}" BODY, values[i].value);
        for (int type = 0; type < PLANES; type++) ok &= alpha_byte(lib, renderer, text, 0, type, values[i].byte);
        for (int channel = 1; channel <= 5; channel++) {
            const char *setup = channel == 2 ? "\\kt100\\kf100" :
                                channel == 5 ? "\\u1\\1aFF\\2aFF" : "";
            int type = channel == 3 ? IMAGE_TYPE_OUTLINE : channel == 4 ? IMAGE_TYPE_SHADOW : IMAGE_TYPE_CHARACTER;
            snprintf(text, sizeof(text), "{" POSITION "%s\\%da%s}" BODY, setup, channel, values[i].value);
            ok &= alpha_byte(lib, renderer, text, 0, type, values[i].byte);
        }
    }
    const char *mixed = "{" POSITION "\\1a$100\\2a80\\3a$200\\4aFF}" BODY;
    ok &= alpha_byte(lib, renderer, mixed, 0, IMAGE_TYPE_CHARACTER, 100);
    ok &= alpha_byte(lib, renderer, mixed, 0, IMAGE_TYPE_OUTLINE, 200);
    ok &= alpha_byte(lib, renderer, mixed, 0, IMAGE_TYPE_SHADOW, 255);
    ok &= alpha_byte(lib, renderer, "{\\1a80\\2a40\\3a20\\4a10}" BODY, 0, IMAGE_TYPE_CHARACTER, 128);
    ok &= alpha_byte(lib, renderer, "{\\1a80\\2a40\\3a20\\4a10}" BODY, 0, IMAGE_TYPE_OUTLINE, 32);
    ok &= alpha_byte(lib, renderer, "{\\1a80\\2a40\\3a20\\4a10}" BODY, 0, IMAGE_TYPE_SHADOW, 16);
    ok &= alpha_byte(lib, renderer, "{\\1a80\\2a40\\3a20\\4a10\\kt100\\kf100}" BODY,
                     0, IMAGE_TYPE_CHARACTER, 64);
    ok &= equivalent(lib, renderer, "{\\c$shiro\\alpha$50}" BODY,
                     "{\\c&HFFFFFF&\\alpha&H32&}" BODY, 0, "named color unaffected");
    ok &= alpha_byte(lib, renderer, "{\\alpha$200\\alpha}" BODY, 0, IMAGE_TYPE_CHARACTER, 0);
    ok &= alpha_byte(lib, renderer, "{\\1a$200\\1a}" BODY, 0, IMAGE_TYPE_CHARACTER, 0);
    return ok;
}

typedef struct { const char *tag, *setup; } AlphaTarget;
static const AlphaTarget targets[] = {
    {"alpha", ""}, {"1a", ""}, {"2a", "\\kt100\\kf100"}, {"3a", ""}, {"4a", ""},
    {"5a", "\\u1\\1aFF\\2aFF"}, {"1ba", ""}, {"2ba", "\\2bs12"}, {"10ba", "\\10bs12"},
    {"bba", "\\bs4\\bbs8"}, {"2bba", "\\bs4\\2bbs8"}, {"10bba", "\\bs4\\10bbs8"},
};
static const char *const invalid_decimal[] = {
    "$256", "$-1", "$abc", "$", "$12xyz", "$100%", "$1.5", "$1&",
    "$999999999999999999999999999999",
};

static bool scalar_rejection(ASS_Library *lib, ASS_Renderer *renderer)
{
    bool ok = true;
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        const AlphaTarget *t = &targets[i];
        char actual[512], expected[512];
        snprintf(expected, sizeof(expected), "{" POSITION "%s\\%s&H80&}" BODY, t->setup, t->tag);
        snprintf(actual, sizeof(actual), "{" POSITION "%s\\%s$128}" BODY, t->setup, t->tag);
        ok &= equivalent(lib, renderer, actual, expected, 0, "scalar decimal/hex equivalence");
        for (size_t j = 0; j < sizeof(invalid_decimal) / sizeof(invalid_decimal[0]); j++) {
            snprintf(actual, sizeof(actual), "{" POSITION "%s\\%s&H80&\\%s%s}" BODY,
                     t->setup, t->tag, t->tag, invalid_decimal[j]);
            ok &= equivalent(lib, renderer, actual, expected, 0, "invalid scalar must preserve alpha");
        }
    }
    ok &= equivalent(lib, renderer, "{\\bord4\\2bs12\\1ba$100\\2ba$200}" BODY,
                     "{\\bord4\\2bs12\\1ba&H64&\\2ba&HC8&}" BODY, 0, "independent native borders");
    return ok;
}

static bool gradients(ASS_Library *lib, ASS_Renderer *renderer)
{
    const AlphaTarget paints[] = {
        {"va", ""}, {"1va", ""}, {"2va", "\\kt100\\kf100"}, {"3va", ""}, {"4va", ""},
        {"1bva", ""}, {"2bva", "\\2bs12"}, {"10bva", "\\10bs12"},
        {"bbva", "\\bs4\\bbs8"}, {"2bbva", "\\bs4\\2bbs8"}, {"10bbva", "\\bs4\\10bbs8"},
        {"1gra", ""}, {"2gra", "\\kt100\\kf100"}, {"3gra", ""}, {"4gra", ""},
        {"5gra", "\\u1\\1aFF\\2aFF\\5a00"},
        {"1bga", ""}, {"2bga", "\\2bs12"}, {"10bga", "\\10bs12"},
        {"bbga", "\\bs4\\bbs8"}, {"2bbga", "\\bs4\\2bbs8"}, {"10bbga", "\\bs4\\10bbs8"},
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(paints) / sizeof(paints[0]); i++) {
        const AlphaTarget *t = &paints[i];
        bool vector = strstr(t->tag, "va") != NULL;
        const char *decimal = vector ? "($0,80,$200,$255)" : "(0,$0,50%,80,$255)";
        const char *hex = vector ? "(&H00&,&H80&,&HC8&,&HFF&)" : "(0,&H00&,50%,&H80&,&HFF&)";
        char actual[1024], expected[1024];
        snprintf(actual, sizeof(actual), "{" POSITION "%s\\%s%s}" BODY, t->setup, t->tag, decimal);
        snprintf(expected, sizeof(expected), "{" POSITION "%s\\%s%s}" BODY, t->setup, t->tag, hex);
        ok &= equivalent(lib, renderer, actual, expected, 0, "mixed gradient alpha syntax");
        for (size_t j = 0; j < sizeof(invalid_decimal) / sizeof(invalid_decimal[0]); j++) {
            snprintf(actual, sizeof(actual), "{" POSITION "%s\\%s%s\\%s(%s$0,%s)}" BODY,
                     t->setup, t->tag, hex, t->tag, vector ? "" : "0,", invalid_decimal[j]);
            ok &= equivalent(lib, renderer, actual, expected, 0, "malformed alpha tuple applied partially");
        }
        /* Invalid solid alpha cannot discard an existing gradient. */
        const char *solid = !strcmp(t->tag, "5gra") ? "5a" : !strcmp(t->tag, "1gra") ? "1a" :
                            !strcmp(t->tag, "2bga") ? "2ba" : NULL;
        if (solid) {
            snprintf(actual, sizeof(actual), "{" POSITION "%s\\%s%s\\%s$256}" BODY,
                     t->setup, t->tag, hex, solid);
            ok &= equivalent(lib, renderer, actual, expected, 0, "invalid solid cleared alpha gradient");
        }
    }
    return ok;
}

static bool strict_bytes(ASS_Library *lib, ASS_Renderer *renderer)
{
    const AlphaTarget fields[] = {
        {"1ba", ""}, {"2ba", "\\2bs12"}, {"10ba", "\\10bs12"},
        {"bba", "\\bs4\\bbs8"}, {"2bba", "\\bs4\\2bbs8"},
        {"bbva", "\\bs4\\bbs8"}, {"2bbva", "\\bs4\\2bbs8"},
        {"1gra", ""}, {"2gra", "\\kt100\\kf100"}, {"3gra", ""}, {"4gra", ""},
        {"5gra", "\\u1\\1aFF\\2aFF\\5a00"},
        {"1bga", ""}, {"2bga", "\\2bs12"}, {"10bga", "\\10bs12"},
        {"bbga", "\\bs4\\bbs8"}, {"2bbga", "\\bs4\\2bbs8"},
    };
    const char *bad[] = {"100", "&H100&", "1FF", "2FF", "FFFFFFFFFF", "xyz"};
    bool ok = true;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        const AlphaTarget *t = &fields[i];
        bool vector = strstr(t->tag, "va") != NULL;
        bool gradient = strstr(t->tag, "gra") || strstr(t->tag, "ga");
        const char *paint = vector ? "(80,80,80,80)" : gradient ? "(0,80,80)" : "80";
        char actual[1024], expected[1024];
        snprintf(expected, sizeof(expected), "{" POSITION "%s\\%s%s}" BODY, t->setup, t->tag, paint);
        for (size_t j = 0; j < sizeof(bad) / sizeof(bad[0]); j++) {
            char operand[128];
            if (gradient) snprintf(operand, sizeof(operand), "(0,$0,%s)", bad[j]);
            else if (vector) snprintf(operand, sizeof(operand), "($0,%s)", bad[j]);
            else snprintf(operand, sizeof(operand), "%s", bad[j]);
            snprintf(actual, sizeof(actual), "{" POSITION "%s\\%s%s\\%s%s}" BODY,
                     t->setup, t->tag, paint, t->tag, operand);
            ok &= equivalent(lib, renderer, actual, expected, 500, "strict byte field accepted wide hex");
        }
        if (vector) {
            snprintf(actual, sizeof(actual), "{" POSITION "%s\\%s%s\\%s($0,$0,$0,$0,$256)}" BODY,
                     t->setup, t->tag, paint, t->tag);
            ok &= equivalent(lib, renderer, actual, expected, 0, "box vector must reject extra corners");
        }
    }
    return ok;
}

/* Explicit expected bytes distinguish full-value interpolation from either
 * rejection or premature clamping/casting; alpha_byte also checks RGBA ink. */
static bool legacy_scalars(ASS_Library *lib, ASS_Renderer *renderer)
{
    const AlphaTarget tags[] = {
        {"alpha", ""}, {"1a", ""}, {"2a", "\\kt200\\kf200"}, {"3a", ""}, {"4a", ""},
    };
    const struct { const char *hex; unsigned bytes[5]; } cases[] = {
        {"FF", {0,63,127,191,255}}, {"100", {0,64,128,192,0}},
        {"1FF", {0,127,255,127,255}}, {"2FF", {0,191,127,63,255}},
        {"&H100&", {0,64,128,192,0}}, {"&H1FF&", {0,127,255,127,255}},
        {"+1FF", {0,127,255,127,255}}, {"0x1FF", {0,127,255,127,255}},
        // mystrtoi32 saturates signed overflow; calc_anim_int32 receives
        // unsigned values and dtoi32 handles conversion overflow as upstream.
        {"7FFFFFFF", {0,255,255,255,255}}, {"80000000", {0,255,255,255,255}},
        {"FFFFFFFFFFFFFFFFFFFF", {0,255,255,255,255}},
        {"-1", {0,255,255,0,0}}, {"-80000000", {0,0,0,0,0}},
        {"-FFFFFFFFFFFFFFFFFFFF", {0,0,0,0,0}},
    };
    bool ok = true;
    char actual[512], expected[512];
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        const AlphaTarget *t = &tags[i];
        int type = !strcmp(t->tag, "3a") ? IMAGE_TYPE_OUTLINE :
                   !strcmp(t->tag, "4a") ? IMAGE_TYPE_SHADOW : IMAGE_TYPE_CHARACTER;
        for (size_t j = 0; j < sizeof(cases) / sizeof(cases[0]); j++) {
            for (int frame = 0; frame < 5; frame++) {
                int now = 250 * frame;
                snprintf(actual, sizeof(actual), "{" POSITION "%s\\%s0\\t(0,1000,\\%s%s)}" BODY,
                         t->setup, t->tag, t->tag, cases[j].hex);
                ok &= alpha_byte(lib, renderer, actual, now, type, cases[j].bytes[frame]);
                snprintf(expected, sizeof(expected), "{" POSITION "%s\\%s$%u}" BODY,
                         t->setup, t->tag, cases[j].bytes[frame]);
                ok &= equivalent(lib, renderer, actual, expected, now, "legacy full-value interpolation pixels");
            }
        }
        const struct { const char *spelling; unsigned byte; } tolerant[] = {
            {"FFG",255}, {"&H80&junk",128}, {"xyz",0}, {"&h80&",0},
            {"&H100&",0}, {"&H1FF&",255},
        };
        for (size_t j = 0; j < sizeof(tolerant) / sizeof(tolerant[0]); j++) {
            snprintf(actual, sizeof(actual), "{%s\\%s80\\%s%s}" BODY,
                     t->setup, t->tag, t->tag, tolerant[j].spelling);
            ok &= alpha_byte(lib, renderer, actual, 0, type, tolerant[j].byte);
        }
    }
    // Exact upstream PR #637 spelling, implicit event-duration transform.
    // This harness has a 10-second event; ART separately tests its 5-second event.
    const int times[] = {2040,2560,4960,5120,9920};
    const unsigned bytes[] = {104,130,253,5,250};
    for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); i++)
        ok &= alpha_byte(lib, renderer, "{\\1a0\\t(\\1a1FF)}A", times[i], IMAGE_TYPE_CHARACTER, bytes[i]);
    return ok;
}

static bool permissive_paints(ASS_Library *lib, ASS_Renderer *renderer)
{
    bool ok = true;
    const struct { const char *hex; unsigned byte; } decoration[] = {
        {"100",0}, {"1FF",255}, {"2FF",255}, {"FFFFFFFFFF",255},
        {"10000000001",1}, {"&h80&",128},
    };
    for (size_t i = 0; i < sizeof(decoration) / sizeof(decoration[0]); i++) {
        char text[512];
        snprintf(text, sizeof(text), "{\\u1\\1aFF\\2aFF\\5a%s}" BODY, decoration[i].hex);
        ok &= alpha_byte(lib, renderer, text, 0, IMAGE_TYPE_CHARACTER, decoration[i].byte);
    }
    const AlphaTarget vectors[] = {
        {"va", ""}, {"1va", ""}, {"2va", "\\kt200\\kf200"}, {"3va", ""}, {"4va", ""},
        {"1bva", ""}, {"2bva", "\\2bs12"}, {"10bva", "\\10bs12"},
    };
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        const AlphaTarget *t = &vectors[i];
        char actual[1024], expected[1024];
        snprintf(actual, sizeof(actual), "{%s\\%s(0,80,100,1FF)}" BODY, t->setup, t->tag);
        snprintf(expected, sizeof(expected), "{%s\\%s($0,$128,$0,$255)}" BODY, t->setup, t->tag);
        ok &= equivalent(lib, renderer, actual, expected, 0, "native vector casts wide hex corners");
        // Ignored extra arguments never contribute a zero or replace a corner.
        snprintf(actual, sizeof(actual), "{%s\\%s(0,80,100,1FF,$256)}" BODY, t->setup, t->tag);
        ok &= equivalent(lib, renderer, actual, expected, 0, "native vector ignores fifth corner");
        snprintf(actual, sizeof(actual), "{%s\\%s(0,80,100,1FF,xyz,$abc)}" BODY, t->setup, t->tag);
        ok &= equivalent(lib, renderer, actual, expected, 0, "native vector ignores extra text");
        for (int now = 0; now <= 1000; now += 250) {
            snprintf(actual, sizeof(actual), "{%s\\%s(0,0,0,0)\\t(0,1000,\\%s(1FF,100,2FF,80))}" BODY,
                     t->setup, t->tag, t->tag);
            snprintf(expected, sizeof(expected), "{%s\\%s($0,$0,$0,$0)\\t(0,1000,\\%s($255,$0,$255,$128))}" BODY,
                     t->setup, t->tag, t->tag);
            ok &= equivalent(lib, renderer, actual, expected, now, "vector byte conversion precedes corner animation");
        }
        snprintf(actual, sizeof(actual), "{%s\\%s(10000000001,FFFFFFFFFF,7FFFFFFF,&h80&)}" BODY, t->setup, t->tag);
        snprintf(expected, sizeof(expected), "{%s\\%s($255,$255,$255,$0)}" BODY, t->setup, t->tag);
        ok &= equivalent(lib, renderer, actual, expected, 0, "vector uses legacy signed saturation");
        snprintf(actual, sizeof(actual), "{%s\\%s(80,80,80,80)\\%s$abc}" BODY,
                 t->setup, t->tag, t->tag);
        snprintf(expected, sizeof(expected), "{%s\\%s(80,80,80,80)}" BODY, t->setup, t->tag);
        ok &= equivalent(lib, renderer, actual, expected, 0, "invalid inline decimal preserves native vector");
        // Check each used corner and every count before replacing another source.
        for (int count = 1; count <= 4; count++) for (int corner = 0; corner < count; corner++) {
            char args[128] = "";
            for (int j = 0; j < count; j++) strcat(args, j == corner ? "$abc," : "$0,");
            args[strlen(args) - 1] = 0;
            snprintf(actual, sizeof(actual), "{%s\\%s(80,80,80,80)\\%s(%s)}" BODY,
                     t->setup, t->tag, t->tag, args);
            snprintf(expected, sizeof(expected), "{%s\\%s(80,80,80,80)}" BODY, t->setup, t->tag);
            ok &= equivalent(lib, renderer, actual, expected, 0, "invalid used vector corner preserves state");
        }
    }
    // Scalar-like inline aliases were not prevalidated as parenthesized tuples.
    ok &= equivalent(lib, renderer, "{\\vaFF}" BODY, "{\\va($255)}" BODY, 0, "inline vector alpha");
    ok &= equivalent(lib, renderer, "{\\va(80,80,80,80)\\va$abc}" BODY,
                     "{\\va(80,80,80,80)}" BODY, 0, "invalid inline decimal vector");
    return ok;
}

static bool animation(ASS_Library *lib, ASS_Renderer *renderer)
{
    bool ok = true;
    for (int now = 0; now <= 1000; now += 250) {
        ok &= equivalent(lib, renderer, "{\\alpha$0\\t(0,1000,\\alpha$255)}" BODY,
                         "{\\alpha&H00&\\t(0,1000,\\alpha&HFF&)}" BODY, now, "alpha interpolation");
        ok &= equivalent(lib, renderer, "{\\1gra(0,$0,$128)\\t(0,1000,\\1gra(0,$100,$200))}" BODY,
                         "{\\1gra(0,&H00&,&H80&)\\t(0,1000,\\1gra(0,&H64&,&HC8&))}" BODY,
                         now, "alpha gradient interpolation");
        ok &= equivalent(lib, renderer, "{\\bord4\\2bs12\\2ba$0\\t(0,1000,\\2ba$200)}" BODY,
                         "{\\bord4\\2bs12\\2ba00\\t(0,1000,\\2baC8)}" BODY, now, "border interpolation");
        ok &= equivalent(lib, renderer, "{\\1a80\\t(0,1000,\\1a$256)}" BODY,
                         "{\\1a80}" BODY, now, "invalid animated scalar");
    }
    ok &= alpha_byte(lib, renderer, "{\\alpha$0\\t(0,1000,\\alpha$255)}" BODY,
                     500, IMAGE_TYPE_CHARACTER, 127);
    /* Standard ASS fade alpha parameters are decimal, as are its timings. */
    ok &= alpha_byte(lib, renderer, "{\\fade(0,128,255,0,1000,5000,6000)}" BODY,
                     1000, IMAGE_TYPE_CHARACTER, 128);
    ok &= equivalent(lib, renderer, "{\\fad(1000,1000)}" BODY,
                     "{\\fade(255,0,255,0,1000,9000,10000)}" BODY, 500, "regular ASS fading");
    return ok;
}

static bool chat(ASS_Library *lib, ASS_Renderer *renderer)
{
    const char *tags[] = {"alpha", "1a", "2a", "3a", "4a", "buba", "bubba", "ba"};
    bool ok = true;
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        char actual[512], expected[512];
        snprintf(actual, sizeof(actual), "{\\chatmode2\\msgshowname1}|{\\bord4\\bubbs4\\%s$128}Miku:\\NMMMM|", tags[i]);
        snprintf(expected, sizeof(expected), "{\\chatmode2\\msgshowname1}|{\\bord4\\bubbs4\\%s80}Miku:\\NMMMM|", tags[i]);
        ok &= equivalent(lib, renderer, actual, expected, 0, "chat decimal/hex alpha");
        for (size_t j = 0; j < sizeof(invalid_decimal) / sizeof(invalid_decimal[0]); j++) {
            snprintf(actual, sizeof(actual), "{\\chatmode2\\msgshowname1}|{\\bord4\\bubbs4\\%s80\\%s%s}Miku:\\NMMMM|",
                     tags[i], tags[i], invalid_decimal[j]);
            ok &= equivalent(lib, renderer, actual, expected, 0, "invalid chat alpha changed state");
        }
    }
    const char *legacy_tags[] = {"alpha", "1a", "2a", "3a", "4a", "buba", "bubba", "ba"};
    const unsigned legacy_bytes[] = {0,127,255,127,255};
    for (size_t i = 0; i < sizeof(legacy_tags) / sizeof(legacy_tags[0]); i++)
        for (int frame = 0; frame < 5; frame++) {
            char actual[512], expected[512];
            snprintf(actual, sizeof(actual), "{\\chatmode2\\msgshowname1}|{\\bord4\\bubbs4\\%s0\\t(0,1000,\\%s1FF)}Miku:\\NMMMM|",
                     legacy_tags[i], legacy_tags[i]);
            snprintf(expected, sizeof(expected), "{\\chatmode2\\msgshowname1}|{\\bord4\\bubbs4\\%s$%u}Miku:\\NMMMM|",
                     legacy_tags[i], legacy_bytes[frame]);
            ok &= equivalent(lib, renderer, actual, expected, frame * 250, "chat full-value alpha interpolation");
        }
    const char *preset = "\\msgright(&HFFFFFF&,80,&H0000FF&,40,&H00FF00&,20,&HFF0000&,10,4,&HFFFFFF&,50,4)";
    const char *decimal = "\\msgright(&HFFFFFF&,$128,&H0000FF&,$64,&H00FF00&,$32,&HFF0000&,$16,4,&HFFFFFF&,$80,4)";
    char actual[1024], expected[1024];
    snprintf(actual, sizeof(actual), "{\\chatmode2\\msgm(Miku)%s}|Miku:\\NMMMM|", decimal);
    snprintf(expected, sizeof(expected), "{\\chatmode2\\msgm(Miku)%s}|Miku:\\NMMMM|", preset);
    ok &= equivalent(lib, renderer, actual, expected, 0, "chat preset alpha fields");
    for (int field = 0; field < 5; field++) {
        const char *a[] = {"$0", "$0", "$0", "$0", "$0"};
        for (int notation = 0; notation < 2; notation++) {
            a[field] = notation ? "100" : "$256";
            snprintf(actual, sizeof(actual), "{\\chatmode2\\msgm(Miku)%s"
                 "\\msgright(&H000000&,%s,&H000000&,%s,&H000000&,%s,&H000000&,%s,1,&H000000&,%s,1)}|Miku:\\NMMMM|",
                     preset, a[0], a[1], a[2], a[3], a[4]);
            ok &= equivalent(lib, renderer, actual, expected, 0, "malformed chat preset applied partially");
        }
    }
    return ok;
}

static bool load_font(ASS_Library *lib, const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool ok = false;
    char *data = NULL;
    if (fseek(file, 0, SEEK_END)) goto done;
    long length = ftell(file);
    if (length <= 0 || length > INT_MAX || fseek(file, 0, SEEK_SET)) goto done;
    data = malloc(length);
    if (!data || fread(data, 1, length, file) != (size_t) length) goto done;
    ass_add_font(lib, path, data, (int) length);
    ok = true;
done:
    free(data); fclose(file);
    return ok;
}

int main(void)
{
    ASS_Library *lib = ass_library_init();
    const char *font_path = getenv("ALPHA_TEST_FONT");
    if (!font_path) font_path = "compare/test/font1.ttf";
    if (!lib || !load_font(lib, font_path)) {
        if (lib) ass_library_done(lib);
        fprintf(stderr, "cannot load alpha regression font: %s\n", font_path);
        return 2;
    }
    ASS_Renderer *renderer = ass_renderer_init(lib);
    if (!renderer) { if (lib) ass_library_done(lib); return 2; }
    ass_set_frame_size(renderer, W, H);
    ass_set_storage_size(renderer, W, H);
    ass_set_fonts(renderer, font_path, "sans-serif", ASS_FONTPROVIDER_NONE, NULL, 1);
    bool ok = scalar_values(lib, renderer);
    ok &= scalar_rejection(lib, renderer);
    ok &= gradients(lib, renderer);
    ok &= strict_bytes(lib, renderer);
    ok &= legacy_scalars(lib, renderer);
    ok &= permissive_paints(lib, renderer);
    ok &= animation(lib, renderer);
    ok &= chat(lib, renderer);
    ass_renderer_done(renderer); ass_library_done(lib);
    return ok ? 0 : 1;
}
