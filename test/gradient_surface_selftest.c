/* Pixel regressions for paint transport. No assumptions about image runs:
 * composite the actual tiles by target, then compare masks and sampled RGB. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ass.h"

enum { W = 640, H = 360, CHANNELS = 3 };
typedef struct { uint8_t *pixels; } Frame;
static const char header[] =
    "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n"
    "ScaledBorderAndShadow: yes\n[V4+ Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
    "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
    "Alignment, MarginL, MarginR, MarginV, Encoding\n"
    "Style: Default,Noto Sans,48,&H00FFFFFF,&H00FFFFFF,&H00000000,&H000000FF,"
    "0,0,0,0,100,100,0,0,1,0,0,5,20,20,20,1\n"
    "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
#define BOX "\\an5\\pos(320,180)\\bs4\\boxp24\\1a&HFF&\\3a&HFF&\\4a&H00&"
#define RGB "(0,&H0000FF&,&HFF0000&)"
#define ALPHA "(0,&H00&,&HC0&)"

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

static void release(Frame *frame) { free(frame->pixels); frame->pixels = NULL; }
static uint8_t *pixel(Frame *frame, int type, int x, int y)
{ return frame->pixels + ((size_t) type * W * H + y * W + x) * 4; }

static bool render_track(ASS_Renderer *renderer, ASS_Track *track, long long now, Frame *frame)
{
    frame->pixels = calloc((size_t) CHANNELS * W * H, 4);
    if (!frame->pixels) return false;
    int change;
    ASS_ImageRGBA *images = ass_render_frame_rgba(renderer, track, now, &change);
    for (ASS_ImageRGBA *img = images; img; img = img->next) {
        if (img->type >= CHANNELS) continue;
        for (int y = 0; y < img->h; y++) {
            int py = img->dst_y + y;
            if (py < 0 || py >= H) continue;
            for (int x = 0; x < img->w; x++) {
                int px = img->dst_x + x;
                if (px < 0 || px >= W) continue;
                const uint8_t *src = img->rgba + (size_t) y * img->stride + x * 4;
                uint8_t *dst = pixel(frame, img->type, px, py);
                for (int c = 0; c < 4; c++) {
                    if (c < 3 && src[c] > src[3]) {
                        fprintf(stderr, "non-premultiplied gradient output\n");
                        ass_free_images_rgba(images);
                        return false;
                    }
                    dst[c] = src[c] + (dst[c] * (255 - src[3]) + 127) / 255;
                }
            }
        }
    }
    ass_free_images_rgba(images);
    return true;
}

static bool render(ASS_Library *lib, ASS_Renderer *renderer, const char *text,
                   long long now, Frame *frame)
{
    ASS_Track *track = track_for(lib, text);
    if (!track) return false;
    bool ok = render_track(renderer, track, now, frame);
    ass_free_track(track);
    return ok;
}

static uint64_t coverage(Frame *frame, int type)
{
    uint64_t total = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
        total += pixel(frame, type, x, y)[3];
    return total;
}

static bool same_mask(Frame *a, Frame *b, int type)
{
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
        if (pixel(a, type, x, y)[3] != pixel(b, type, x, y)[3]) return false;
    return true;
}

static bool same_target(Frame *a, Frame *b, int type)
{
    return !memcmp(pixel(a, type, 0, 0), pixel(b, type, 0, 0), (size_t) W * H * 4);
}

static bool red_blue(Frame *frame, int type)
{
    int red = 0, blue = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t *p = pixel(frame, type, x, y);
        if (p[3] < 40) continue;
        red += p[0] > p[2] * 2 && p[0] * 255 / p[3] > 150;
        blue += p[2] > p[0] * 2 && p[2] * 255 / p[3] > 150;
    }
    return red > 10 && blue > 10;
}

static bool flat_green(Frame *frame, int type)
{
    int painted = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t *p = pixel(frame, type, x, y);
        if (!p[3]) continue;
        if (p[0] || p[2] || p[1] + 2 < p[3]) return false;
        painted++;
    }
    return painted > 10;
}

static bool check(bool ok, const char *message)
{ if (!ok) fprintf(stderr, "%s\n", message); return ok; }

static bool boxes(ASS_Library *lib, ASS_Renderer *renderer)
{
    bool ok = true;
    const char *geometry[] = {
        "", "\\boxr18", "\\boxpx40\\boxpy12\\fscx140\\fscy70", "\\scale130",
        "\\frz35", "\\frx35\\fry-20\\frz10", "\\fax0.3\\fay0.1",
        "\\org(240,160)\\frz25", "\\distort(1.4,-0.2,1.6,1.1,-0.2,1)",
        "\\distort(0.1,0.1,1.3,0,1.4,1.2,-0.1,1)",
        "\\clip(300,100,640,300)", "\\iclip(300,100,340,250)",
        "\\clip(m 0 0 l 640 0 640 360)", "\\iclip(m 0 0 l 640 0 640 360)",
        "\\pos(5,180)", "\\move(250,160,390,200,0,1000)",
        "\\t(0,1000,\\frz60\\fscx150\\fscy70)",
        "\\t(0,1000,\\pos(400,220))",
    };
    for (size_t i = 0; i < sizeof(geometry) / sizeof(geometry[0]); i++) {
        char gradient[1024], solid[1024];
        snprintf(gradient, sizeof(gradient), "{" BOX "%s\\4grd" RGB "}MMMM", geometry[i]);
        snprintf(solid, sizeof(solid), "{" BOX "%s\\4c&H0000FF&}MMMM", geometry[i]);
        Frame a = {0}, b = {0};
        bool rendered = render(lib, renderer, gradient, 500, &a) &&
                        render(lib, renderer, solid, 500, &b);
        bool good = rendered && same_mask(&a, &b, IMAGE_TYPE_SHADOW) &&
                    red_blue(&a, IMAGE_TYPE_SHADOW);
        if (!good) fprintf(stderr, "BS4 transformed gradient/mask case %zu: %s\n", i, geometry[i]);
        ok &= good;
        release(&a); release(&b);
    }
    Frame color = {0}, alpha = {0}, override = {0}, vector = {0}, expected = {0};
    ok &= render(lib, renderer, "{" BOX "\\4grd" RGB "}MMMM", 0, &color);
    ok &= render(lib, renderer, "{" BOX "\\4grd" RGB "\\4gra" ALPHA "}MMMM", 0, &alpha);
    ok &= render(lib, renderer, "{" BOX "\\4grd" RGB "\\4gra" ALPHA "\\4c&H00FF00&}MMMM", 0, &override);
    ok &= check(coverage(&alpha, IMAGE_TYPE_SHADOW) < coverage(&color, IMAGE_TYPE_SHADOW) * 8 / 10 &&
                red_blue(&alpha, IMAGE_TYPE_SHADOW) && same_mask(&alpha, &override, IMAGE_TYPE_SHADOW) &&
                flat_green(&override, IMAGE_TYPE_SHADOW), "BS4 independent color/alpha or solid precedence failed");
    ok &= render(lib, renderer, "{" BOX "\\4vc(&H0000FF&,&HFF0000&,&H0000FF&,&HFF0000&)"
                 "\\4va(&H00&,&H80&,&H00&,&H80&)}MMMM", 0, &vector);
    ok &= check(red_blue(&vector, IMAGE_TYPE_SHADOW) &&
                coverage(&vector, IMAGE_TYPE_SHADOW) < coverage(&color, IMAGE_TYPE_SHADOW),
                "BS4 vector color or alpha is flat/missing");
    release(&vector);
    ok &= render(lib, renderer, "{" BOX "\\4grd" RGB "\\4grd(0,broken,&HFFFFFF&)}MMMM", 0, &vector);
    ok &= check(same_target(&color, &vector, IMAGE_TYPE_SHADOW), "invalid BS4 stops changed prior paint");
    release(&vector);
    ok &= render(lib, renderer, "{" BOX "\\4grd" RGB "\\4vc(broken,&HFFFFFF&)}MMMM", 0, &vector);
    ok &= check(same_target(&color, &vector, IMAGE_TYPE_SHADOW), "invalid vector changed prior BS4 paint");
    release(&vector);
    ok &= render(lib, renderer, "{\\4grd" RGB "\\rDefault" BOX "\\4c&H00FF00&}MMMM", 0, &vector);
    ok &= check(flat_green(&vector, IMAGE_TYPE_SHADOW), "BS4 named style reset retained gradient");
    release(&vector);
    /* Visible primary selects the canonical state. Filter the background only. */
    ok &= render(lib, renderer, "{" BOX "\\1a&H00&\\4grd" RGB "}MM"
                 "{\\4grd(90,&H00FF00&,&HFFFFFF&)\\boxp70\\boxr40}MM", 0, &vector);
    ok &= render(lib, renderer, "{" BOX "\\1a&H00&\\4grd" RGB "}MMMM", 0, &expected);
    ok &= check(same_target(&vector, &expected, IMAGE_TYPE_SHADOW), "BS4 paint did not use first visible content");
    release(&color); release(&alpha); release(&override); release(&vector); release(&expected);
    return ok;
}

static bool rings(ASS_Library *lib, ASS_Renderer *renderer)
{
    bool ok = true;
    for (int layer = 1; layer <= 10; layer++) {
        char text[1024], solid[1024];
        snprintf(text, sizeof(text), "{" BOX "\\%dbbs10\\%dbbgrd" RGB "}MMMM", layer, layer);
        snprintf(solid, sizeof(solid), "{" BOX "\\%dbbs10\\%dbbc&H0000FF&}MMMM", layer, layer);
        Frame a = {0}, b = {0};
        ok &= render(lib, renderer, text, 0, &a) && render(lib, renderer, solid, 0, &b);
        ok &= check(same_mask(&a, &b, IMAGE_TYPE_OUTLINE) && red_blue(&a, IMAGE_TYPE_OUTLINE) &&
                    same_target(&a, &b, IMAGE_TYPE_SHADOW), "box-border gradient changed ring/fill geometry or is flat");
        release(&b);
        snprintf(text, sizeof(text), "{" BOX "\\%dbbs10\\%dbbgrd" RGB
                 "\\%dbbga" ALPHA "\\%dbbc&H00FF00&}MMMM", layer, layer, layer, layer);
        ok &= render(lib, renderer, text, 0, &b);
        ok &= check(flat_green(&b, IMAGE_TYPE_OUTLINE) &&
                    coverage(&b, IMAGE_TYPE_OUTLINE) < coverage(&a, IMAGE_TYPE_OUTLINE) * 8 / 10,
                    "box-border solid color erased alpha or did not replace color gradient");
        release(&a); release(&b);
        snprintf(text, sizeof(text), "{" BOX "\\%dbbs10\\%dbbvc(&H0000FF&,&HFF0000&,&H0000FF&,&HFF0000&)"
                 "\\%dbbva(&H00&,&H80&,&H00&,&H80&)}MMMM", layer, layer, layer);
        ok &= render(lib, renderer, text, 0, &a);
        ok &= check(red_blue(&a, IMAGE_TYPE_OUTLINE), "numbered box-border vector gradients missing");
        release(&a);
    }
    Frame a = {0}, b = {0};
    ok &= render(lib, renderer, "{" BOX "\\bbs10\\bbgrd" RGB "\\bbga" ALPHA "}MMMM", 0, &a);
    ok &= render(lib, renderer, "{" BOX "\\1bbs10\\1bbgrd" RGB "\\1bbga" ALPHA "}MMMM", 0, &b);
    ok &= check(same_target(&a, &b, IMAGE_TYPE_OUTLINE), "layer-one box gradient aliases differ");
    release(&a); release(&b);
    ok &= render(lib, renderer, "{" BOX "\\bbs10\\bbvc(&H0000FF&,&HFF0000&,&H0000FF&,&HFF0000&)"
                 "\\bbva(&H00&,&H80&,&H00&,&H80&)}MMMM", 0, &a);
    ok &= check(red_blue(&a, IMAGE_TYPE_OUTLINE), "box vector aliases missing");
    release(&a);
    /* Two outward rings with disjoint palettes. A layer-two override must
     * leave every fully covered layer-one pixel exactly unchanged. */
    ok &= render(lib, renderer, "{" BOX "\\bbs8\\bbgrd" RGB
                 "\\2bbs12\\2bbgrd(90,&H00FF00&,&HFFFFFF&)}MMMM", 0, &a);
    ok &= render(lib, renderer, "{" BOX "\\bbs8\\bbgrd" RGB
                 "\\2bbs12\\2bbc&H00FF00&}MMMM", 0, &b);
    int unchanged = 0, changed = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t *p = pixel(&a, IMAGE_TYPE_OUTLINE, x, y), *q = pixel(&b, IMAGE_TYPE_OUTLINE, x, y);
        if (p[3] < 240) continue;
        if (p[1] == 0 && (p[0] || p[2])) unchanged += !memcmp(p, q, 4);
        else changed += memcmp(p, q, 4) != 0;
    }
    ok &= check(unchanged > 50 && changed > 50 && same_mask(&a, &b, IMAGE_TYPE_OUTLINE),
                "independent box rings leaked paint or changed cumulative geometry");
    release(&a); release(&b);
    return ok;
}

static bool ordinary(ASS_Library *lib, ASS_Renderer *renderer)
{
    bool ok = true;
    for (int layer = 1; layer <= 10; layer++) {
        char text[512];
        snprintf(text, sizeof(text), "{\\an5\\pos(320,180)\\1a&HFF&\\%dbs10\\%dbgrd" RGB "}MMMM", layer, layer);
        Frame a = {0};
        ok &= render(lib, renderer, text, 0, &a);
        ok &= check(red_blue(&a, IMAGE_TYPE_OUTLINE), "native glyph-border attached gradient missing/flat");
        release(&a);
        snprintf(text, sizeof(text), "{\\an5\\pos(320,180)\\bs5\\1a&HFF&\\%dbs10\\%dbblur1"
                 "\\%dbgrd" RGB "\\%dbga" ALPHA "}MMMM", layer, layer, layer, layer);
        ok &= render(lib, renderer, text, 0, &a);
        ok &= check(red_blue(&a, IMAGE_TYPE_OUTLINE), "BS5 blurred translucent border lost gradient");
        release(&a);
    }
    Frame a = {0};
    Frame inner = {0}, padded = {0};
    ok &= render(lib, renderer, "{\\1a&HFF&\\1bs3\\1bgrd" RGB "}MMMM", 0, &inner);
    ok &= render(lib, renderer, "{\\1a&HFF&\\1bs3\\1bgrd" RGB "\\10bs40\\10ba&HFF&}MMMM", 0, &padded);
    ok &= check(same_target(&inner, &padded, IMAGE_TYPE_OUTLINE),
                "invisible outer glyph border changed inner gradient coordinates");
    release(&inner); release(&padded);
    ok &= render(lib, renderer, "{\\1a&HFF&\\shad12\\4grd" RGB "}MMMM", 0, &a);
    ok &= check(red_blue(&a, IMAGE_TYPE_SHADOW), "ordinary shadow lost fourth-channel gradient");
    release(&a);
    const char *layouts[] = {
        "", "\\vert1", "\\ct(m -220 0 b -100 -70 100 -70 220 0)",
        "\\q0\\fs96", "\\fax0.2\\frz20",
    };
    for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
        char text[1024];
        snprintf(text, sizeof(text), "{\\an5\\pos(320,180)%s\\1grd" RGB
                 "}MMMM 日本 မြန်မာ MMMM", layouts[i]);
        ok &= render(lib, renderer, text, 0, &a);
        ok &= check(red_blue(&a, IMAGE_TYPE_CHARACTER), "layout/shaping lost primary gradient");
        release(&a);
    }
    for (int decoration = 0; decoration < 2; decoration++) {
        char text[512];
        snprintf(text, sizeof(text), "{\\1a&HFF&\\5a&H00&\\%s1\\5grd" RGB "}MMMM", decoration ? "s" : "u");
        ok &= render(lib, renderer, text, 0, &a);
        ok &= check(red_blue(&a, IMAGE_TYPE_CHARACTER), "underline/strikeout lost fifth-channel color");
        Frame b = {0};
        snprintf(text, sizeof(text), "{\\1a&HFF&\\5a&H00&\\%s1\\5grd" RGB "\\5gra" ALPHA "}MMMM", decoration ? "s" : "u");
        ok &= render(lib, renderer, text, 0, &b);
        ok &= check(red_blue(&b, IMAGE_TYPE_CHARACTER) && coverage(&b, IMAGE_TYPE_CHARACTER) <
                    coverage(&a, IMAGE_TYPE_CHARACTER) * 8 / 10, "decoration alpha gradient missing");
        release(&a); release(&b);
    }
    for (int mode = 0; mode < 3; mode++) for (int style = 0; style < 3; style++) {
        char text[1024];
        snprintf(text, sizeof(text), "{\\an5\\pos(320,180)\\furichangepos%d\\furistyle%d\\1grd" RGB
                 "}<MMMM|日本မြန်မာ|gyaku>\\N<MMMM||gyaku> <MMMM|longreading>", mode, style);
        ok &= render(lib, renderer, text, 0, &a);
        ok &= check(red_blue(&a, IMAGE_TYPE_CHARACTER), "furigana paint transport failed");
        release(&a);
        /* Hide the base. The gradient must reach the reading itself. */
        snprintf(text, sizeof(text), "{\\an5\\pos(320,180)\\furichangepos%d\\furistyle%d"
                 "\\1a&HFF&\\1grd" RGB "}<MMMM|{\\1a&H00&}MMMMMMMM>", mode, style);
        ok &= render(lib, renderer, text, 0, &a);
        ok &= check(red_blue(&a, IMAGE_TYPE_CHARACTER), "normal reading did not inherit gradient");
        release(&a);
        snprintf(text, sizeof(text), "{\\an5\\pos(320,180)\\furichangepos%d\\furistyle%d"
                 "\\1a&HFF&\\1grd" RGB "}<MMMM||{\\1a&H00&}MMMMMMMM>", mode, style);
        ok &= render(lib, renderer, text, 0, &a);
        ok &= check(red_blue(&a, IMAGE_TYPE_CHARACTER), "gyaku reading did not inherit gradient");
        release(&a);
    }
    const char *karaoke[] = {"k", "kf", "K", "ko", "kO"};
    for (size_t i = 0; i < sizeof(karaoke) / sizeof(karaoke[0]); i++) {
        char text[1024];
        snprintf(text, sizeof(text), "{\\kt50\\%s100\\1grd" RGB "\\2grd" RGB
                 "\\bord5\\1bgrd" RGB "\\2bs6\\2bgrd" RGB "\\shad5\\4grd" RGB
                 "\\u1\\5grd" RGB "}MMMM", karaoke[i]);
        ok &= render(lib, renderer, text, 250, &a);
        if (!strcmp(karaoke[i], "kO"))
            ok &= check(coverage(&a, 0) + coverage(&a, 1) + coverage(&a, 2) == 0,
                        "kO revealed gradient paint before activation");
        else
            ok &= check(red_blue(&a, IMAGE_TYPE_CHARACTER), "waiting karaoke fill lost gradient");
        release(&a);
        ok &= render(lib, renderer, text, 1600, &a);
        ok &= check(red_blue(&a, IMAGE_TYPE_CHARACTER) && red_blue(&a, IMAGE_TYPE_OUTLINE) &&
                    red_blue(&a, IMAGE_TYPE_SHADOW), "active karaoke paint lost gradient");
        release(&a);
    }
    return ok;
}

static bool timing(ASS_Library *lib, ASS_Renderer *renderer)
{
    bool ok = true;
    const char *animated = "{" BOX "\\4grd" RGB "\\bbs10\\bbgrd" RGB
        "\\t(0,1000,\\4grd(90,&H00FF00&,50%,&HFFFFFF&,&H0000FF&)"
        "\\bbgrd(90,&H00FF00&,&HFFFFFF&)\\frz25)}MMMM";
    ASS_Track *track = track_for(lib, animated);
    if (!track) return false;
    Frame start = {0}, middle = {0}, end = {0}, again = {0};
    ok &= render_track(renderer, track, 0, &start);
    ok &= render_track(renderer, track, 500, &middle);
    ok &= render_track(renderer, track, 1000, &end);
    ok &= render_track(renderer, track, 0, &again);
    ok &= check(!same_target(&start, &middle, IMAGE_TYPE_SHADOW) &&
                !same_target(&middle, &end, IMAGE_TYPE_SHADOW) &&
                same_target(&start, &again, IMAGE_TYPE_SHADOW) &&
                same_target(&start, &again, IMAGE_TYPE_OUTLINE), "animated box/ring paint is stale across reverse seek");
    release(&start); release(&middle); release(&end); release(&again);
    ass_free_track(track);
    track = track_for(lib, "{\\an7\\pos(100,80)\\scrollt0\\scroll(ue,500,1)"
        "\\pgrd(0,0,640,360,90,&H0000FF&,&HFF0000&)}MMMMMMMM\\NMMMMMMMM\\NMMMMMMMM");
    if (!track) return false;
    ok &= render_track(renderer, track, 0, &start);
    ok &= render_track(renderer, track, 600, &end);
    int common = 0, mismatches = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t *p = pixel(&start, IMAGE_TYPE_CHARACTER, x, y);
        uint8_t *q = pixel(&end, IMAGE_TYPE_CHARACTER, x, y);
        if (p[3] < 100 || q[3] < 100) continue;
        common++;
        for (int c = 0; c < 3; c++)
            mismatches += abs(p[c] * 255 / p[3] - q[c] * 255 / q[3]) > 3;
    }
    ok &= check(common > 100 && mismatches == 0 &&
                !same_mask(&start, &end, IMAGE_TYPE_CHARACTER), "pgrd moved with scrolling instead of remaining fixed in frame");
    release(&start); release(&end); ass_free_track(track);
    return ok;
}

static bool automatic(ASS_Library *lib, ASS_Renderer *renderer)
{
    ASS_Track *track = track_for(lib, "{" BOX "\\bbs10\\bbgrd" RGB "}MMMM");
    if (!track) return false;
    int change;
    ASS_RenderResult result = ass_render_frame_auto(renderer, track, 0, &change);
    bool ok = check(result.use_rgba && result.imgs_rgba && ass_frame_needs_rgba(renderer),
                    "box-border-only gradient was lost by automatic RGBA detection");
    int red = 0, blue = 0;
    for (ASS_ImageRGBA *img = result.imgs_rgba; img; img = img->next) {
        if (img->type != IMAGE_TYPE_OUTLINE) continue;
        for (int y = 0; y < img->h; y++) for (int x = 0; x < img->w; x++) {
            const uint8_t *p = img->rgba + (size_t) y * img->stride + 4 * x;
            if (p[3] < 40) continue;
            red += p[0] > p[2] * 2;
            blue += p[2] > p[0] * 2;
        }
    }
    ok &= check(red > 10 && blue > 10, "automatic output contained flat box-border paint");
    ass_render_result_free(&result);
    ass_free_track(track);
    return ok;
}

int main(void)
{
    ASS_Library *lib = ass_library_init();
    ASS_Renderer *renderer = lib ? ass_renderer_init(lib) : NULL;
    if (!renderer) return 2;
    ass_set_frame_size(renderer, W, H);
    ass_set_fonts(renderer, NULL, "Noto Sans", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    bool ok = boxes(lib, renderer);
    ok &= rings(lib, renderer);
    ok &= ordinary(lib, renderer);
    ok &= timing(lib, renderer);
    ok &= automatic(lib, renderer);
    ass_renderer_done(renderer); ass_library_done(lib);
    return ok ? 0 : 1;
}
