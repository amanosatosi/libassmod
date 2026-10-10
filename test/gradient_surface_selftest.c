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

static bool varied_color(Frame *frame, int type)
{
    int low = 255, high = 0, count = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t *p = pixel(frame, type, x, y);
        if (p[3] < 40) continue;
        int red = p[0] * 255 / p[3];
        if (red < low) low = red;
        if (red > high) high = red;
        count++;
    }
    return count > 50 && high - low > 60;
}

/* Inline reading overrides intentionally recognize karaoke only. Isolate
 * inherited ruby paint by its actual separated vertical band, not by trying
 * to change a reading's alpha with an unsupported inline visual override. */
static bool retain_reading_band(Frame *frame, bool gyaku)
{
    int first = -1, last = -1, gap_start = -1, gap_end = -1;
    bool gap = false;
    for (int y = 0; y < H; y++) {
        bool occupied = false;
        for (int x = 0; x < W; x++) occupied |= pixel(frame, IMAGE_TYPE_CHARACTER, x, y)[3] != 0;
        if (occupied) {
            if (first < 0) first = y;
            if (gap && gap_end < 0) gap_end = y;
            last = y;
        } else if (first >= 0 && gap_start < 0) {
            gap_start = y;
            gap = true;
        }
    }
    if (first < 0 || gap_end < 0 || gap_end > last) return false;
    for (int y = 0; y < H; y++)
        if (gyaku ? y < gap_end : y >= gap_start)
            memset(pixel(frame, IMAGE_TYPE_CHARACTER, 0, y), 0, W * 4);
    return true;
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
        "\\distort(1.3,0,1.4,1.2,-0.1,1,0.1,0.1)",
        "\\clip(300,100,640,300)", "\\iclip(300,100,340,250)",
        "\\clip(m 0 0 l 640 0 640 360)", "\\iclip(m 0 0 l 640 0 640 360)",
        "\\pos(5,180)", "\\move(250,160,390,200,0,1000)",
        "\\t(0,1000,\\frz60\\fscx150\\fscy70)",
        "\\t(0,1000,\\pos(400,220))",
    };
    for (size_t i = 0; i < sizeof(geometry) / sizeof(geometry[0]); i++) {
        char gradient[1024], solid[1024];
        const char *anchor = i == 14 ? "\\pos(5,180)" :
            i == 15 ? "\\move(250,160,390,200,0,1000)" : "";
        snprintf(gradient, sizeof(gradient), "{%s" BOX "%s\\4grd" RGB "}MMMM", anchor, geometry[i]);
        snprintf(solid, sizeof(solid), "{%s" BOX "%s\\4c&H0000FF&}MMMM", anchor, geometry[i]);
        Frame a = {0}, b = {0};
        bool rendered = render(lib, renderer, gradient, 500, &a) &&
                        render(lib, renderer, solid, 500, &b);
        bool good = rendered && same_mask(&a, &b, IMAGE_TYPE_SHADOW) &&
                    /* A clip can deliberately remove either endpoint. */
                    (i >= 10 && i <= 14 ? varied_color(&a, IMAGE_TYPE_SHADOW) :
                                         red_blue(&a, IMAGE_TYPE_SHADOW));
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

static bool clipped_coordinates(ASS_Library *lib, ASS_Renderer *renderer)
{
    Frame full = {0}, clipped = {0};
    bool ok = render(lib, renderer, "{" BOX "\\boxr15\\4grd" RGB
                     "\\bbs8\\bbgrd" RGB "}MMMM", 0, &full);
    for (int inverse = 0; inverse < 2; inverse++) {
        char text[1024];
        snprintf(text, sizeof(text), "{" BOX "\\boxr15\\4grd" RGB
                 "\\bbs8\\bbgrd" RGB "\\%sclip(300,0,340,360)}MMMM", inverse ? "i" : "");
        ok &= render(lib, renderer, text, 0, &clipped);
        int common = 0;
        bool identical = true, empty = true;
        for (int type = IMAGE_TYPE_OUTLINE; type <= IMAGE_TYPE_SHADOW; type++)
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                bool inside = x >= 300 && x < 340;
                uint8_t *p = pixel(&clipped, type, x, y);
                uint8_t *q = pixel(&full, type, x, y);
                if (inside == (bool) inverse) {
                    empty &= p[3] == 0;
                } else {
                    identical &= !memcmp(p, q, 4);
                    common += p[3] > 100;
                }
            }
        ok &= check(empty && identical && common > 100,
                    "rectangular clip leaked paint or renormalized a box/ring gradient");
        release(&clipped);
    }
    release(&full);
    return ok;
}

static bool vector_corners(ASS_Library *lib, ASS_Renderer *renderer)
{
    Frame color = {0}, alpha = {0};
    const char *corners = "\\4vc(&H0000FF&,&H00FF00&,&HFF0000&,&HFFFFFF&)";
    char text[1024];
    snprintf(text, sizeof(text), "{" BOX "%s}MMMM", corners);
    bool ok = render(lib, renderer, text, 0, &color);
    snprintf(text, sizeof(text), "{" BOX "%s\\4va(&H00&,&HFF&,&HFF&,&H00&)}MMMM", corners);
    ok &= render(lib, renderer, text, 0, &alpha);
    int left = W, top = H, right = 0, bottom = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        if (pixel(&color, IMAGE_TYPE_SHADOW, x, y)[3] < 240) continue;
        if (x < left) left = x;
        if (x > right) right = x;
        if (y < top) top = y;
        if (y > bottom) bottom = y;
    }
    if (left >= right || top >= bottom) {
        release(&color); release(&alpha);
        return check(false, "four-corner BS4 mask is missing");
    }
    int x0 = left + (right - left) / 8, x1 = right - (right - left) / 8;
    int y0 = top + (bottom - top) / 8, y1 = bottom - (bottom - top) / 8;
    uint8_t *tl = pixel(&color, IMAGE_TYPE_SHADOW, x0, y0);
    uint8_t *tr = pixel(&color, IMAGE_TYPE_SHADOW, x1, y0);
    uint8_t *bl = pixel(&color, IMAGE_TYPE_SHADOW, x0, y1);
    uint8_t *br = pixel(&color, IMAGE_TYPE_SHADOW, x1, y1);
    ok &= check(tl[0] > tl[1] * 2 && tl[0] > tl[2] * 2 &&
                tr[1] > tr[0] * 2 && tr[1] > tr[2] * 2 &&
                bl[2] > bl[0] * 2 && bl[2] > bl[1] * 2 &&
                br[0] > 170 && br[1] > 170 && br[2] > 170,
                "BS4 four-corner RGB ordering or vertical interpolation is wrong");
    ok &= check(pixel(&alpha, IMAGE_TYPE_SHADOW, x0, y0)[3] >
                    pixel(&alpha, IMAGE_TYPE_SHADOW, x1, y0)[3] * 2 &&
                pixel(&alpha, IMAGE_TYPE_SHADOW, x1, y1)[3] >
                    pixel(&alpha, IMAGE_TYPE_SHADOW, x0, y1)[3] * 2,
                "BS4 four-corner alpha was ignored or mixed into color");
    release(&color); release(&alpha);
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
        snprintf(text, sizeof(text), "{" BOX "\\%dbbs10\\%dbbc&H00FF00&\\%dbbgrd" RGB
                 "\\%dbbgrd0}MMMM", layer, layer, layer, layer);
        ok &= render(lib, renderer, text, 0, &a);
        ok &= check(flat_green(&a, IMAGE_TYPE_OUTLINE), "box gradient reset failed to restore its solid fallback");
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
    /* Compare native composite rings in both cases. Ordinary ASS's single
     * outline has different subtraction semantics and is not this fixture. */
    /* Explicit positioning also excludes margin clearance from this domain
     * assertion when a font's ascender/descender outlines are asymmetric. */
    ok &= render(lib, renderer, "{\\an5\\pos(320,180)\\1a&HFF&\\1bs3\\1bblur0.1\\1bgrd" RGB "}MMMM", 0, &inner);
    ok &= render(lib, renderer, "{\\an5\\pos(320,180)\\1a&HFF&\\1bs3\\1bblur0.1\\1bgrd" RGB "\\10bs40\\10ba&HFF&}MMMM", 0, &padded);
    {
        int common = 0, max_alpha = 0, max_color = 0;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            uint8_t *p = pixel(&inner, IMAGE_TYPE_OUTLINE, x, y);
            uint8_t *q = pixel(&padded, IMAGE_TYPE_OUTLINE, x, y);
            int delta = abs(p[3] - q[3]);
            if (delta > max_alpha) max_alpha = delta;
            if (p[3] >= 128 && q[3] >= 128) {
                common++;
                for (int c = 0; c < 3; c++) {
                    delta = abs(p[c] * 255 / p[3] - q[c] * 255 / q[3]);
                    if (delta > max_color) max_color = delta;
                }
            }
        }
        /* A larger composite allocation can change blur's integer rounding
         * by one alpha unit (observed on CoreText/macOS). Unpremultiplying
         * 8-bit color at opacity >=128 adds at most two RGB units. Compare
         * the actual mask and gradient field within those quantization bounds,
         * rather than conflating paint coordinates with bit-exact blur output. */
        bool stable = common > 50 && max_alpha <= 1 && max_color <= 2 &&
            red_blue(&inner, IMAGE_TYPE_OUTLINE) && red_blue(&padded, IMAGE_TYPE_OUTLINE);
        if (!stable)
            fprintf(stderr, "inner ring mismatch: common=%d max-alpha=%d max-color=%d\n",
                    common, max_alpha, max_color);
        ok &= check(stable, "invisible outer glyph border changed inner mask or sampled gradient coordinates");
    }
    release(&inner); release(&padded);
    /* ASS suppresses a fill-only shadow when its primary fill is invisible. */
    ok &= render(lib, renderer, "{\\shad12\\4grd" RGB "}MMMM", 0, &a);
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
        /* Inspect the reading alone without changing its inherited paint. */
        snprintf(text, sizeof(text), "{\\an5\\pos(320,180)\\furichangepos%d\\furistyle%d"
                 "\\1grd" RGB "}<MMMM|MMMMMMMM>", mode, style);
        ok &= render(lib, renderer, text, 0, &a);
        ok &= check(retain_reading_band(&a, false) && red_blue(&a, IMAGE_TYPE_CHARACTER),
                    "normal reading did not inherit gradient");
        release(&a);
        snprintf(text, sizeof(text), "{\\an5\\pos(320,180)\\furichangepos%d\\furistyle%d"
                 "\\1grd" RGB "}<MMMM||MMMMMMMM>", mode, style);
        ok &= render(lib, renderer, text, 0, &a);
        ok &= check(retain_reading_band(&a, true) && red_blue(&a, IMAGE_TYPE_CHARACTER),
                    "gyaku reading did not inherit gradient");
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
    track = track_for(lib, "{\\move(250,160,390,200,0,1000)" BOX "\\4grd" RGB "}MMMM");
    if (!track) return false;
    ok &= render_track(renderer, track, 0, &start);
    ok &= render_track(renderer, track, 1000, &end);
    int attached = 0, detached = 0;
    for (int y = 0; y < H - 40; y++) for (int x = 0; x < W - 140; x++) {
        uint8_t *p = pixel(&start, IMAGE_TYPE_SHADOW, x, y);
        if (p[3] < 40) continue;
        uint8_t *q = pixel(&end, IMAGE_TYPE_SHADOW, x + 140, y + 40);
        attached++;
        detached += memcmp(p, q, 4) != 0;
    }
    ok &= check(attached > 200 && detached == 0, "attached BS4 gradient did not move with its box");
    release(&start); release(&end); ass_free_track(track);
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

static bool precedence(ASS_Library *lib, ASS_Renderer *renderer)
{
    bool ok = true;
    Frame a = {0}, b = {0};
    const char *transition = "{" BOX "\\bbs10\\bbgrd" RGB "\\bbga" ALPHA
        "\\t(0,1000,\\bbc&H00FF00&\\bba&H40&)}MMMM";
    ok &= render(lib, renderer, transition, 500, &a);
    ok &= render(lib, renderer, transition, 1000, &b);
    ok &= check(varied_color(&a, IMAGE_TYPE_OUTLINE) && flat_green(&b, IMAGE_TYPE_OUTLINE) &&
                !same_mask(&a, &b, IMAGE_TYPE_OUTLINE), "box ring gradient-to-solid animation lost interpolation");
    release(&a); release(&b);

    /* Different logical sources must remain different during karaoke. */
    const char *phase = "{\\kt50\\k100\\1c&H00FF00&\\2grd" RGB "}MMMM";
    ok &= render(lib, renderer, phase, 250, &a);
    ok &= render(lib, renderer, phase, 1600, &b);
    ok &= check(red_blue(&a, IMAGE_TYPE_CHARACTER) && flat_green(&b, IMAGE_TYPE_CHARACTER),
                "secondary gradient leaked into primary karaoke fill");
    release(&a); release(&b);
    ok &= render(lib, renderer,
        "{\\1a&HFF&\\2a&HFF&\\5a&H00&\\u1\\5grd" RGB "\\kt50\\kf100}MMMM", 250, &a);
    ok &= check(red_blue(&a, IMAGE_TYPE_CHARACTER), "fifth-channel decoration gradient disappeared during waiting karaoke");
    release(&a);

    const char *reverse = "{\\an5\\pos(320,180)\\frz180\\1c&H00FF00&\\2c&H0000FF&"
        "\\1a&H40&\\2a&H90&\\kf100%s}MMMM";
    char plain[1024], vector[1024];
    snprintf(plain, sizeof(plain), reverse, "");
    snprintf(vector, sizeof(vector), reverse,
        "\\1vc(&H00FF00&,&H00FF00&,&H00FF00&,&H00FF00&)"
        "\\2vc(&H0000FF&,&H0000FF&,&H0000FF&,&H0000FF&)");
    ok &= render(lib, renderer, plain, 350, &a) && render(lib, renderer, vector, 350, &b);
    ok &= check(same_target(&a, &b, IMAGE_TYPE_CHARACTER), "reversed karaoke swapped only flat colors, not gradient sources/alpha");
    release(&a); release(&b);

    const char *curve = "{\\an5\\pos(320,180)\\frz180\\ct(m -230 0 b -90 -70 90 -70 230 0)"
        "\\1grd" RGB "\\2grd" RGB "\\kf100}MMMMMMMM";
    ok &= render(lib, renderer, curve, 500, &a);
    ok &= check(red_blue(&a, IMAGE_TYPE_CHARACTER), "curved rotated karaoke lost gradient paint");
    release(&a);
    ok &= render(lib, renderer, "{\\1grd" RGB "\\col1}MMMM|日本\\NMMMM|မြန်မာ", 0, &a);
    ok &= check(red_blue(&a, IMAGE_TYPE_CHARACTER), "semantic columns lost inherited gradient");
    release(&a);
    ok &= render(lib, renderer,
        "{\\1grd" RGB "\\2grd" RGB "}<MMMM|{\\kf100}MMMMMMMM>", 500, &a);
    ok &= check(retain_reading_band(&a, false) && red_blue(&a, IMAGE_TYPE_CHARACTER),
                "progressive furigana reading lost gradient");
    release(&a);

    const uint8_t green[] = {0, 255, 0, 255};
    ok &= ass_set_tag_image_rgba(renderer, "audit.png", ASS_TAG_IMAGE_FORMAT_PNG, 1, 1, 4, green) == 0;
    ok &= render(lib, renderer, "{" BOX "\\4grd" RGB "\\4img(audit.png)}MMMM", 0, &a);
    ok &= check(flat_green(&a, IMAGE_TYPE_SHADOW), "image fill did not replace BS4 gradient paint");
    release(&a);
    ok &= render(lib, renderer, "{" BOX "\\4img(audit.png)\\4grd" RGB "}MMMM", 0, &a);
    ok &= check(red_blue(&a, IMAGE_TYPE_SHADOW), "BS4 gradient did not replace image fill");
    release(&a);
    ass_clear_tag_images(renderer);

    const char *blend = "{" BOX "\\4a&H80&\\4grd" RGB "\\blend4}MMMM";
    ok &= render(lib, renderer, blend, 0, &a);
    ASS_Track *track = track_for(lib, blend);
    uint8_t *backdrop = malloc((size_t) W * H * 4);
    if (!track || !backdrop) {
        if (track) ass_free_track(track);
        free(backdrop); release(&a);
        return false;
    }
    memset(backdrop, 128, (size_t) W * H * 4);
    for (int i = 0; i < W * H; i++) backdrop[4 * i + 3] = 255;
    int change;
    ASS_ImageRGBA *images = ass_render_frame_rgba(renderer, track, 0, &change);
    ok &= ass_composite_images_bgra(images, backdrop, W, H, W * 4) == 0;
    uint8_t *p = pixel(&a, IMAGE_TYPE_SHADOW, W / 2, H / 2);
    uint8_t *q = backdrop + ((H / 2) * W + W / 2) * 4;
    ok &= check(p[3] > 100 && p[3] < 150, "semi-transparent BS4 blend mask missing");
    if (p[3]) for (int c = 0; c < 3; c++) {
        int source = p[c] * 255 / p[3];
        int multiplied = source * 128 / 255;
        int expected = (multiplied * p[3] + 128 * (255 - p[3])) / 255;
        ok &= check(abs(q[2 - c] - expected) <= 2, "BS4 gradient lost destination-aware blend metadata or applied alpha twice");
    }
    ass_free_images_rgba(images); ass_free_track(track);
    free(backdrop); release(&a);
    return ok;
}

/* Fixed fields use exactly the same masks/coverage as solid and attached paint.
 * Compare every target plane, so parser acceptance with flat or missing output
 * cannot satisfy these regressions. Text spans most of this fixed rectangle. */
#define FIELD "(140,80,500,280,0,&H0000FF&,&HFF0000&)"
#define FIELD_BASE "\\an5\\pos(320,180)\\bord6\\shad12\\1c&H00FF00&\\2c&H00FF00&\\3c&H00FF00&\\4c&H00FF00&"
#define FIELD_TEXT "MMMMMMMM"

typedef struct {
    const char *tag, *attached, *solid, *setup;
    int type;
} PositionedCase;

static const PositionedCase positioned_cases[] = {
    {"pgrd", "1grd", "1c", "", IMAGE_TYPE_CHARACTER},
    {"1pgrd", "1grd", "1c", "", IMAGE_TYPE_CHARACTER},
    {"2pgrd", "2grd", "2c", "\\kt100\\kf100", IMAGE_TYPE_CHARACTER},
    {"3pgrd", "3grd", "3c", "", IMAGE_TYPE_OUTLINE},
    {"4pgrd", "4grd", "4c", "", IMAGE_TYPE_SHADOW},
    {"5pgrd", "5grd", "5c", "\\u1\\1a&HFF&\\2a&HFF&\\5a&H00&\\5c&H00FF00&", IMAGE_TYPE_CHARACTER},
    {"1bpgrd", "1bgrd", "1bc", "\\2bs14\\2ba&HFF&", IMAGE_TYPE_OUTLINE},
    {"2bpgrd", "2bgrd", "2bc", "\\2bs14\\1ba&HFF&\\2bc&H00FF00&", IMAGE_TYPE_OUTLINE},
    {"10bpgrd", "10bgrd", "10bc", "\\10bs14\\1ba&HFF&\\10bc&H00FF00&", IMAGE_TYPE_OUTLINE},
};

static bool positioned_targets(ASS_Library *lib, ASS_Renderer *renderer)
{
    bool ok = true;
    for (size_t i = 0; i < sizeof(positioned_cases) / sizeof(positioned_cases[0]); i++) {
        const PositionedCase *c = &positioned_cases[i];
        char text[2048];
        Frame field = {0}, solid = {0}, actual = {0}, expected = {0};
        snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\%s" FIELD "}" FIELD_TEXT, c->setup, c->tag);
        bool good = render(lib, renderer, text, 0, &field) && ass_frame_needs_rgba(renderer);
        snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\%s&H00FF00&}" FIELD_TEXT, c->setup, c->solid);
        good = good && render(lib, renderer, text, 0, &solid);
        good = good && red_blue(&field, c->type) && same_mask(&field, &solid, c->type);
        if (good) for (int type = 0; type < CHANNELS; type++)
            if (type != c->type) good &= same_target(&field, &solid, type);
        if (!good) fprintf(stderr, "positioned target missing, flat, or leaked: %s\n", c->tag);
        ok &= good;
        if (!good) { release(&field); release(&solid); return false; }

        /* Solid replacement, and an empty reset, affect this source only. */
        snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\%s" FIELD "\\%s&H00FF00&}" FIELD_TEXT,
                 c->setup, c->tag, c->solid);
        good = render(lib, renderer, text, 0, &actual) && same_target(&actual, &solid, c->type);
        release(&actual);
        snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\%s" FIELD "\\%s()}" FIELD_TEXT,
                 c->setup, c->tag, c->tag);
        good &= render(lib, renderer, text, 0, &actual) && same_target(&actual, &solid, c->type);
        release(&actual);

        /* Attached paint wins; a positioned reset must leave it untouched. */
        snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\%s" FIELD "\\%s" RGB "\\%s()}" FIELD_TEXT,
                 c->setup, c->tag, c->attached, c->tag);
        good &= render(lib, renderer, text, 0, &actual);
        snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\%s" RGB "}" FIELD_TEXT, c->setup, c->attached);
        good &= render(lib, renderer, text, 0, &expected) && same_target(&actual, &expected, c->type);
        release(&actual); release(&expected);
        snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\%s" RGB "\\%s" FIELD "}" FIELD_TEXT,
                 c->setup, c->attached, c->tag);
        good &= render(lib, renderer, text, 0, &actual) && same_target(&actual, &field, c->type);
        release(&actual);

        /* Incompatible transforms in either direction retain exact paint. */
        snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\%s" FIELD "\\t(0,1000,\\%s" RGB ")}" FIELD_TEXT,
                 c->setup, c->tag, c->attached);
        good &= render(lib, renderer, text, 500, &actual) && same_target(&actual, &field, c->type);
        release(&actual);
        snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\%s" RGB "\\t(0,1000,\\%s" FIELD ")}" FIELD_TEXT,
                 c->setup, c->attached, c->tag);
        good &= render(lib, renderer, text, 500, &actual);
        snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\%s" RGB "}" FIELD_TEXT, c->setup, c->attached);
        good &= render(lib, renderer, text, 500, &expected) && same_target(&actual, &expected, c->type);
        release(&actual); release(&expected);

        /* Coordinates, shortest-path angle, stop positions and RGB at t=1/2.
         * The midpoint's inserted stop is the resampled source/dest average. */
        snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\%s(140,80,500,280,350,&H000000&,&H000000&)"
                 "\\t(0,1000,\\%s(160,100,520,300,10,&HFFFFFF&,50%%,&HFFFFFF&,&HFFFFFF&))}" FIELD_TEXT,
                 c->setup, c->tag, c->tag);
        good &= render(lib, renderer, text, 500, &actual);
        snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\%s(150,90,510,290,0,&H7F7F7F&,50%%,&H7F7F7F&,&H7F7F7F&)}" FIELD_TEXT,
                 c->setup, c->tag);
        good &= render(lib, renderer, text, 500, &expected) && same_target(&actual, &expected, c->type);
        release(&actual); release(&expected);
        /* Non-flat endpoint paint ensures angle interpolation is sampled too. */
        snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\%s" FIELD
                 "\\t(0,1000,\\%s(160,100,520,300,90,&H0000FF&,&HFF0000&))}" FIELD_TEXT,
                 c->setup, c->tag, c->tag);
        good &= render(lib, renderer, text, 500, &actual);
        snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\%s(150,90,510,290,45,&H0000FF&,&HFF0000&)}" FIELD_TEXT,
                 c->setup, c->tag);
        good &= render(lib, renderer, text, 500, &expected) && same_target(&actual, &expected, c->type);
        release(&actual); release(&expected);

        /* Invalid input never replaces valid paint, for every target family. */
        const char *bad[] = {
            "(140,,500,280,0,&HFFFFFF&,&H000000&)",
            "(140,80,500,280,nan,&HFFFFFF&,&H000000&)",
            "(140,80,500,280,0,broken,&H000000&)",
            "(140,80,500,280,0,&HFFFFFF&,50%,&H000000&,)",
            "(140,80,500,280,0,&HFFFFFF&,&H000000&", "140", "(0,0,1,1,0)",
        };
        for (size_t j = 0; j < sizeof(bad) / sizeof(bad[0]); j++) {
            snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\%s" FIELD "\\%s%s}" FIELD_TEXT,
                     c->setup, c->tag, c->tag, bad[j]);
            good &= render(lib, renderer, text, 0, &actual) && same_target(&actual, &field, c->type);
            release(&actual);
        }
        if (!good) fprintf(stderr, "positioned replacement/reset/transform/validation failed: %s\n", c->tag);
        ok &= good;
        release(&field); release(&solid);
    }
    return ok;
}

static bool positioned_isolation(ASS_Library *lib, ASS_Renderer *renderer)
{
    const char *paints = "\\1pgrd" FIELD "\\2pgrd(140,80,500,280,90,&H00FF00&,&HFFFFFF&)"
        "\\3pgrd(140,80,500,280,180,&H0000FF&,&HFF0000&)"
        "\\4pgrd(140,80,500,280,0,&H00FF00&,&HFFFFFF&)";
    Frame all = {0}, changed = {0}, expected = {0};
    char text[2048];
    snprintf(text, sizeof(text), "{" FIELD_BASE "%s}" FIELD_TEXT, paints);
    bool ok = render(lib, renderer, text, 0, &all);
    ok &= render(lib, renderer, "{" FIELD_BASE "\\pgrd" FIELD "}" FIELD_TEXT, 0, &changed) &&
          render(lib, renderer, "{" FIELD_BASE "\\1pgrd" FIELD "}" FIELD_TEXT, 0, &expected) &&
          same_target(&changed, &expected, IMAGE_TYPE_CHARACTER);
    release(&changed); release(&expected);
    /* A primary reset/replacement cannot change outline or shadow planes. */
    const char *replace[] = {"\\1c&H00FF00&", "\\1pgrd()", "\\1grd" RGB};
    for (size_t i = 0; i < sizeof(replace) / sizeof(replace[0]); i++) {
        snprintf(text, sizeof(text), "{" FIELD_BASE "%s%s}" FIELD_TEXT, paints, replace[i]);
        ok &= render(lib, renderer, text, 0, &changed) &&
            same_target(&all, &changed, IMAGE_TYPE_OUTLINE) && same_target(&all, &changed, IMAGE_TYPE_SHADOW);
        release(&changed);
    }
    /* Secondary reset must leave the active primary and both other planes. */
    snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\2pgrd()}" FIELD_TEXT, paints);
    ok &= render(lib, renderer, text, 0, &changed);
    for (int type = 0; type < CHANNELS; type++) ok &= same_target(&all, &changed, type);
    release(&changed);
    /* Explicit/ordinary outline aliases sample the same storage exactly. */
    snprintf(text, sizeof(text), "{" FIELD_BASE "\\1bpgrd" FIELD "}" FIELD_TEXT);
    ok &= render(lib, renderer, text, 0, &changed);
    snprintf(text, sizeof(text), "{" FIELD_BASE "\\3pgrd" FIELD "}" FIELD_TEXT);
    ok &= render(lib, renderer, text, 0, &expected) && same_target(&changed, &expected, IMAGE_TYPE_OUTLINE);
    release(&changed); release(&expected);
    /* Two independently painted native rings both produce visible pixels. */
    ok &= render(lib, renderer, "{" FIELD_BASE "\\2bs16\\1bpgrd" FIELD
                 "\\2bpgrd(140,80,500,280,0,&H00FF00&,&H00FF00&)}" FIELD_TEXT, 0, &changed);
    int green = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t *p = pixel(&changed, IMAGE_TYPE_OUTLINE, x, y);
        green += p[3] > 40 && p[1] > p[0] * 2 && p[1] > p[2] * 2;
    }
    ok &= green > 50 && red_blue(&changed, IMAGE_TYPE_OUTLINE);
    release(&changed);
    const char *invalid[] = {"0pgrd", "6pgrd", "11pgrd", "0bpgrd", "11bpgrd", "999999bpgrd", "2bpgrdjunk"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\%s" FIELD "}" FIELD_TEXT, paints, invalid[i]);
        ok &= render(lib, renderer, text, 0, &changed);
        for (int type = 0; type < CHANNELS; type++) ok &= same_target(&all, &changed, type);
        release(&changed);
    }
    /* The same event selects secondary while waiting and primary afterward. */
    snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\kt100\\kf100}" FIELD_TEXT, paints);
    ok &= render(lib, renderer, text, 0, &changed);
    ok &= render(lib, renderer, "{" FIELD_BASE "\\2pgrd(140,80,500,280,90,&H00FF00&,&HFFFFFF&)"
                 "\\kt100\\kf100}" FIELD_TEXT, 0, &expected) &&
          same_target(&changed, &expected, IMAGE_TYPE_CHARACTER);
    release(&changed); release(&expected);
    ok &= render(lib, renderer, text, 3000, &changed) && same_target(&all, &changed, IMAGE_TYPE_CHARACTER);
    release(&changed);
    for (int layer = 3; layer < 10; layer++) {
        snprintf(text, sizeof(text), "{" FIELD_BASE "\\%dbs14\\1ba&HFF&\\%dbpgrd" FIELD "}" FIELD_TEXT,
                 layer, layer);
        ok &= render(lib, renderer, text, 0, &changed) && red_blue(&changed, IMAGE_TYPE_OUTLINE);
        release(&changed);
    }
    release(&all);
    return check(ok, "simultaneous positioned channels, outline aliases, or border isolation failed");
}

static bool positioned_sources(ASS_Library *lib, ASS_Renderer *renderer)
{
    const uint8_t green[] = {0, 255, 0, 255};
    if (ass_set_tag_image_rgba(renderer, "positioned.png", ASS_TAG_IMAGE_FORMAT_PNG, 1, 1, 4, green))
        return false;
    const char *sources[][3] = {
        {"\\1vc(&H00FF00&,&H00FF00&)", "\\1img(positioned.png)", "\\1cyc(1,&H00FF00&,&H00FF00&)"},
        {"\\1vc(&H00FF00&,&H00FF00&)", "\\1img(positioned.png)", "\\1cyc(1,&H00FF00&,&H00FF00&)"},
        {"\\2vc(&H00FF00&,&H00FF00&)", "\\2img(positioned.png)", "\\2cyc(1,&H00FF00&,&H00FF00&)"},
        {"\\3vc(&H00FF00&,&H00FF00&)", "\\3img(positioned.png)", "\\3cyc(1,&H00FF00&,&H00FF00&)"},
        {"\\4vc(&H00FF00&,&H00FF00&)", "\\4img(positioned.png)", NULL},
        {NULL, NULL, NULL},
        {"\\1bvc(&H00FF00&,&H00FF00&)", "\\3img(positioned.png)", "\\1bcyc(1,&H00FF00&,&H00FF00&)"},
        {"\\2bvc(&H00FF00&,&H00FF00&)", NULL, "\\2bcyc(1,&H00FF00&,&H00FF00&)"},
        {"\\10bvc(&H00FF00&,&H00FF00&)", NULL, NULL},
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(positioned_cases) / sizeof(positioned_cases[0]); i++) {
        const PositionedCase *c = &positioned_cases[i];
        Frame field = {0}, source = {0}, actual = {0};
        char text[2048];
        snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\%s" FIELD "}" FIELD_TEXT, c->setup, c->tag);
        ok &= render(lib, renderer, text, 0, &field);
        for (int j = 0; j < 3; j++) {
            if (!sources[i][j]) continue;
            const char *paint = sources[i][j];
            snprintf(text, sizeof(text), "{" FIELD_BASE "%s%s}" FIELD_TEXT, c->setup, paint);
            bool good = render(lib, renderer, text, 0, &source) && flat_green(&source, c->type);
            if (!good) fprintf(stderr, "positioned source baseline failed: %s %s\n", c->tag, paint);
            ok &= good;
            snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\%s" FIELD "%s\\%s()}" FIELD_TEXT,
                     c->setup, c->tag, paint, c->tag);
            good = render(lib, renderer, text, 0, &actual) && same_target(&source, &actual, c->type);
            if (!good) fprintf(stderr, "positioned source replacement/reset failed: %s %s\n", c->tag, paint);
            ok &= good;
            release(&actual);
            snprintf(text, sizeof(text), "{" FIELD_BASE "%s%s\\%s" FIELD "}" FIELD_TEXT,
                     c->setup, paint, c->tag);
            good = render(lib, renderer, text, 0, &actual) && same_target(&field, &actual, c->type);
            if (!good) fprintf(stderr, "positioned source supersession failed: %s %s\n", c->tag, paint);
            ok &= good;
            release(&actual); release(&source);
        }
        if (sources[i][1]) {
            /* Missing host images must not resurrect replaced positioned paint. */
            const char *image_tag = i == 2 ? "2img" : i == 3 || i == 6 ? "3img" :
                                    i == 4 ? "4img" : "1img";
            snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\%s(missing.png)}" FIELD_TEXT,
                     c->setup, image_tag);
            ok &= render(lib, renderer, text, 0, &source);
            snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\%s" FIELD "\\%s(missing.png)}" FIELD_TEXT,
                     c->setup, c->tag, image_tag);
            bool good = render(lib, renderer, text, 0, &actual) && same_target(&source, &actual, c->type);
            if (!good) fprintf(stderr, "positioned missing-image replacement failed: %s\n", c->tag);
            ok &= good;
            release(&source); release(&actual);
        }
        release(&field);
    }
    ass_clear_tag_images(renderer);
    return check(ok, "positioned/vector/image/cycle precedence or reset failed");
}

static bool positioned_transform_order(ASS_Library *lib, ASS_Renderer *renderer)
{
    bool ok = true;
    for (size_t i = 0; i < sizeof(positioned_cases) / sizeof(positioned_cases[0]); i++) {
        const PositionedCase *c = &positioned_cases[i];
        char text[2048], reference[2048];
        Frame nested = {0}, simple = {0}, seq[3] = {{0}};
        snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\%s" FIELD
                 "\\t(0,1000,\\t(0,1000,\\%s(160,100,520,300,90,&H0000FF&,&HFF0000&)))}" FIELD_TEXT,
                 c->setup, c->tag, c->tag);
        /* Existing nested-transform guards ignore this parenthesized inner
         * tag. Keep that behavior and verify that it leaves paint intact. */
        snprintf(reference, sizeof(reference), "{" FIELD_BASE "%s\\%s" FIELD "}" FIELD_TEXT,
                 c->setup, c->tag);
        bool good = render(lib, renderer, text, 500, &nested) && render(lib, renderer, reference, 500, &simple) &&
                    same_target(&nested, &simple, c->type);
        if (!good) fprintf(stderr, "positioned nested transform failed: %s\n", c->tag);
        ok &= good;
        release(&nested); release(&simple);
        snprintf(text, sizeof(text), "{" FIELD_BASE "%s\\%s" FIELD
                 "\\t(0,1000,\\%s(160,100,520,300,90,&H0000FF&,&HFF0000&))"
                 "\\t(0,1000,\\%s" FIELD ")}" FIELD_TEXT, c->setup, c->tag, c->tag, c->tag);
        snprintf(reference, sizeof(reference), "{" FIELD_BASE "%s\\%s(145,85,505,285,22.5,&H0000FF&,&HFF0000&)}" FIELD_TEXT,
                 c->setup, c->tag);
        good = render(lib, renderer, text, 500, &nested) && render(lib, renderer, reference, 500, &simple) &&
               same_target(&nested, &simple, c->type);
        if (!good) fprintf(stderr, "positioned overlapping transform failed: %s\n", c->tag);
        ok &= good;
        release(&nested); release(&simple);
        ASS_Track *track = track_for(lib, text);
        if (!track) return false;
        /* This round-trip transform has equal values at 100 and 900 ms;
         * use 250 ms to assert a genuinely different sampled field. */
        const long long times[] = {900, 250, 900};
        for (int j = 0; j < 3; j++) ok &= render_track(renderer, track, times[j], &seq[j]);
        good = render(lib, renderer, text, 250, &simple) && same_target(&seq[0], &seq[2], c->type) &&
               same_target(&seq[1], &simple, c->type) && !same_target(&seq[0], &seq[1], c->type);
        if (!good) fprintf(stderr, "positioned reverse seek failed: %s\n", c->tag);
        ok &= good;
        for (int j = 0; j < 3; j++) release(&seq[j]);
        release(&simple); ass_free_track(track);
    }
    return check(ok, "positioned nested/overlapping transforms or reverse seek changed state");
}

static bool positioned_coverage(ASS_Library *lib, ASS_Renderer *renderer)
{
    Frame full = {0}, clipped = {0}, faded = {0}, fallback = {0}, solid = {0};
    bool ok = render(lib, renderer, "{\\an7\\pos(160,120)\\bord8\\shad12\\p1\\1c&H00FF00&"
        "\\1pgrd" FIELD "\\3pgrd" FIELD "\\4pgrd" FIELD "}m 0 0 l 320 0 320 100 0 100", 0, &full);
    ok &= red_blue(&full, IMAGE_TYPE_CHARACTER) && red_blue(&full, IMAGE_TYPE_OUTLINE) &&
          red_blue(&full, IMAGE_TYPE_SHADOW);
    ok &= render(lib, renderer, "{\\an7\\pos(160,120)\\bord8\\shad12\\p1\\1c&H00FF00&"
        "\\1pgrd" FIELD "\\3pgrd" FIELD "\\4pgrd" FIELD
        "\\clip(250,0,390,360)}m 0 0 l 320 0 320 100 0 100", 0, &clipped);
    ok &= render(lib, renderer, "{\\an7\\pos(160,120)\\bord8\\shad12\\p1\\1c&H00FF00&"
        "\\1pgrd" FIELD "\\3pgrd" FIELD "\\4pgrd" FIELD
        "\\fad(1000,1000)}m 0 0 l 320 0 320 100 0 100", 500, &faded);
    for (int type = 0; type < CHANNELS; type++) {
        ok &= coverage(&faded, type) > 0 && coverage(&faded, type) < coverage(&full, type) * 6 / 10;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            uint8_t *p = pixel(&clipped, type, x, y), *q = pixel(&full, type, x, y);
            ok &= x >= 250 && x < 390 ? !memcmp(p, q, 4) : p[3] == 0;
        }
    }
    /* A narrow rectangle leaves each channel's green solid outside it. */
    ok &= render(lib, renderer, "{" FIELD_BASE
        "\\1pgrd(300,0,340,360,0,&H0000FF&,&HFF0000&)"
        "\\3pgrd(300,0,340,360,0,&H0000FF&,&HFF0000&)"
        "\\4pgrd(300,0,340,360,0,&H0000FF&,&HFF0000&)}" FIELD_TEXT, 0, &fallback);
    ok &= render(lib, renderer, "{" FIELD_BASE "}" FIELD_TEXT, 0, &solid);
    for (int type = 0; type < CHANNELS; type++) {
        int inside = 0, outside = 0;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            uint8_t *p = pixel(&fallback, type, x, y), *q = pixel(&solid, type, x, y);
            if (x < 300 || x >= 340) { ok &= !memcmp(p, q, 4); outside += p[3] > 40; }
            else inside += p[3] > 40 && (p[0] > 0 || p[2] > 0);
        }
        ok &= inside > 10 && outside > 50;
    }
    release(&full); release(&clipped); release(&faded); release(&fallback); release(&solid);
    return check(ok, "positioned drawing/clip/fade or outside solid fallback failed");
}

int main(void)
{
    ASS_Library *lib = ass_library_init();
    ASS_Renderer *renderer = lib ? ass_renderer_init(lib) : NULL;
    if (!renderer) return 2;
    ass_set_frame_size(renderer, W, H);
    ass_set_fonts(renderer, NULL, "Noto Sans", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    bool ok = boxes(lib, renderer);
    ok &= vector_corners(lib, renderer);
    ok &= clipped_coordinates(lib, renderer);
    ok &= rings(lib, renderer);
    ok &= ordinary(lib, renderer);
    ok &= timing(lib, renderer);
    ok &= automatic(lib, renderer);
    ok &= precedence(lib, renderer);
    ok &= positioned_targets(lib, renderer);
    ok &= positioned_isolation(lib, renderer);
    ok &= positioned_coverage(lib, renderer);
    ok &= positioned_sources(lib, renderer);
    ok &= positioned_transform_order(lib, renderer);
    ass_renderer_done(renderer); ass_library_done(lib);
    return ok ? 0 : 1;
}
