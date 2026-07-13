/*
 * Renderer-level determinism checks for event-parallel rendering.
 */

#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ass.h"

typedef struct {
    uint64_t hash;
    uint64_t coverage;
    int images;
    bool needs_rgba;
} RenderSignature;

static const char script[] =
    "[Script Info]\n"
    "ScriptType: v4.00+\n"
    "PlayResX: 640\n"
    "PlayResY: 360\n"
    "ScaledBorderAndShadow: yes\n"
    "\n"
    "[V4+ Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
    "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
    "Alignment, MarginL, MarginR, MarginV, Encoding\n"
    "Style: Default,Arial,38,&H00FFFFFF,&H0000FFFF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,1,2,2,5,10,10,10,1\n"
    "\n"
    "[Events]\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
    "Dialogue: 0,0:00:00.00,0:00:05.00,Default,,0,0,0,,{\\pos(105,65)}Shared glyphs\n"
    "Dialogue: 1,0:00:00.00,0:00:05.00,Default,,0,0,0,,{\\pos(320,65)\\bord2\\2bs6\\2bc&H0000FF&}Shared glyphs\n"
    "Dialogue: 1,0:00:00.00,0:00:05.00,Default,,0,0,0,,{comment-only event}\n"
    "Dialogue: 2,0:00:00.00,0:00:05.00,Default,,0,0,0,,{\\pos(535,65)\\1grd(0,&H000000&,&HFFFFFF&)}Gradient\n"
    "Dialogue: 0,0:00:00.00,0:00:05.00,Default,,0,0,0,,{\\pos(105,180)\\furi1}<Base|Ruby>\n"
    "Dialogue: 1,0:00:00.00,0:00:05.00,Default,,0,0,0,,{\\pos(320,180)\\distort(1.1,-0.1,1.2,1.1,-0.1,1)}Distort\n"
    "Dialogue: 2,0:00:00.00,0:00:05.00,Default,,0,0,0,,{\\pos(535,180)\\clip(m 430 120 l 640 120 640 240 430 240)}Clip\n"
    "Dialogue: 2,0:00:00.00,0:00:05.00,Default,,0,0,0,,{\\pos(320,245)\\col1\\colan5}A|B\\NC|D{\\col0}\n"
    "Dialogue: 0,0:00:00.00,0:00:05.00,Default,,0,0,0,,{\\pos(210,295)\\rndx6\\rndy4}Jitter\n"
    "Dialogue: 1,0:00:00.00,0:00:05.00,Default,,0,0,0,,{\\pos(430,295)\\bs4\\boxp10\\frz12}Box\n";

static const char rgba_limit_script[] =
    "[Script Info]\n"
    "ScriptType: v4.00+\n"
    "PlayResX: 640\n"
    "PlayResY: 360\n"
    "\n"
    "[V4+ Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
    "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
    "Alignment, MarginL, MarginR, MarginV, Encoding\n"
    "Style: Default,sans-serif,20,&H00FFFFFF,&H0000FFFF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1\n"
    "\n"
    "[Events]\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
    "Dialogue: 0,0:00:00.00,0:00:05.00,Default,,0,0,0,,{\\an7\\pos(0,0)\\p1\\1grd(0,&H000000&,&HFFFFFF&)}m 0 0 l 640 0 640 360 0 360\n"
    "Dialogue: 1,0:00:00.00,0:00:05.00,Default,,0,0,0,,{\\an7\\pos(0,0)\\p1\\1grd(0,&HFF0000&,&H0000FF&)}m 0 0 l 640 0 640 360 0 360\n"
    "Dialogue: 2,0:00:00.00,0:00:05.00,Default,,0,0,0,,{\\an7\\pos(0,0)\\p1\\1grd(0,&H00FF00&,&HFF00FF&)}m 0 0 l 640 0 640 360 0 360\n";

static void msg_cb(int level, const char *fmt, va_list va, void *data)
{
    (void) level;
    (void) fmt;
    (void) va;
    (void) data;
}

static void hash_u8(uint64_t *hash, uint8_t value)
{
    *hash ^= value;
    *hash *= 1099511628211ULL;
}

static void hash_u32(uint64_t *hash, uint32_t value)
{
    for (int i = 0; i < 4; i++)
        hash_u8(hash, (uint8_t) (value >> (8 * i)));
}

static void hash_i32(uint64_t *hash, int value)
{
    hash_u32(hash, (uint32_t) value);
}

static ASS_Renderer *create_renderer(ASS_Library *lib)
{
    ASS_Renderer *renderer = ass_renderer_init(lib);
    if (!renderer)
        return NULL;

    ass_set_storage_size(renderer, 640, 360);
    ass_set_frame_size(renderer, 640, 360);
    ass_set_fonts(renderer, NULL, "sans-serif",
                  ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    return renderer;
}

static void reset_render_caches(ASS_Renderer *renderer)
{
    ass_set_frame_size(renderer, 639, 360);
    ass_set_frame_size(renderer, 640, 360);
}

static bool capture_alpha(ASS_Renderer *renderer, ASS_Track *track,
                          RenderSignature *sig)
{
    reset_render_caches(renderer);
    *sig = (RenderSignature) { .hash = 1469598103934665603ULL };
    ASS_Image *images = ass_render_frame(renderer, track, 1000, NULL);
    sig->needs_rgba = ass_frame_needs_rgba(renderer) != 0;
    for (ASS_Image *img = images; img; img = img->next) {
        sig->images++;
        hash_i32(&sig->hash, img->type);
        hash_i32(&sig->hash, img->w);
        hash_i32(&sig->hash, img->h);
        hash_i32(&sig->hash, img->dst_x);
        hash_i32(&sig->hash, img->dst_y);
        hash_u32(&sig->hash, img->color);
        for (int y = 0; y < img->h; y++) {
            const uint8_t *row = img->bitmap + y * img->stride;
            for (int x = 0; x < img->w; x++) {
                sig->coverage += row[x];
                hash_u8(&sig->hash, row[x]);
            }
        }
    }

    return sig->images > 0 && sig->coverage > 0;
}

static bool capture_rgba(ASS_Renderer *renderer, ASS_Track *track,
                         RenderSignature *sig)
{
    reset_render_caches(renderer);
    *sig = (RenderSignature) { .hash = 1469598103934665603ULL };
    ASS_ImageRGBA *images = ass_render_frame_rgba(renderer, track, 1000, NULL);
    sig->needs_rgba = ass_frame_needs_rgba(renderer) != 0;
    for (ASS_ImageRGBA *img = images; img; img = img->next) {
        sig->images++;
        hash_i32(&sig->hash, img->type);
        hash_i32(&sig->hash, img->w);
        hash_i32(&sig->hash, img->h);
        hash_i32(&sig->hash, img->dst_x);
        hash_i32(&sig->hash, img->dst_y);
        for (int y = 0; y < img->h; y++) {
            const uint8_t *row = img->rgba + y * img->stride;
            for (int x = 0; x < img->w; x++) {
                for (int c = 0; c < 4; c++)
                    hash_u8(&sig->hash, row[4 * x + c]);
                sig->coverage += row[4 * x + 3];
            }
        }
    }

    ass_free_images_rgba(images);
    return sig->images > 0 && sig->coverage > 0;
}

static bool same_signature(const RenderSignature *a,
                           const RenderSignature *b)
{
    return a->hash == b->hash && a->coverage == b->coverage &&
        a->images == b->images && a->needs_rgba == b->needs_rgba;
}

static void print_signature(const char *label, const RenderSignature *sig)
{
    fprintf(stderr,
            "%s: hash=%016" PRIx64 " coverage=%" PRIu64
            " images=%d needs_rgba=%d\n",
            label, sig->hash, sig->coverage, sig->images, sig->needs_rgba);
}

static bool verify_rgba_limit_fallback(ASS_Renderer *renderer,
                                       ASS_Track *track)
{
    RenderSignature serial, threaded;
    unsigned threads = ass_set_threads(renderer, 2);
    if (threads < 2)
        return true;

    ass_set_threads(renderer, 1);
    ass_set_cache_limits(renderer, 10000, 1);
    if (!capture_rgba(renderer, track, &serial) || !serial.needs_rgba)
        return false;

    ass_set_threads(renderer, threads);
    if (!capture_rgba(renderer, track, &threaded))
        return false;
    if (same_signature(&serial, &threaded))
        return true;

    fprintf(stderr, "RGBA limit fallback differs with %u threads\n", threads);
    print_signature("RGBA limit serial", &serial);
    print_signature("RGBA limit threaded", &threaded);
    return false;
}

int main(void)
{
    ASS_Library *lib = ass_library_init();
    if (!lib)
        return 1;
    ass_set_message_cb(lib, msg_cb, NULL);

    ASS_Renderer *renderer = create_renderer(lib);
    ASS_Track *track = ass_read_memory(lib, (char *) script,
                                       sizeof(script) - 1, NULL);
    ASS_Track *limit_track = ass_read_memory(lib, (char *) rgba_limit_script,
                                             sizeof(rgba_limit_script) - 1,
                                             NULL);
    if (!renderer || !track || !limit_track) {
        ass_renderer_done(renderer);
        ass_free_track(track);
        ass_free_track(limit_track);
        ass_library_done(lib);
        return 1;
    }

    RenderSignature alpha_serial, rgba_serial;
    bool ok = capture_alpha(renderer, track, &alpha_serial) &&
              capture_rgba(renderer, track, &rgba_serial);
    ok = ok && alpha_serial.needs_rgba && rgba_serial.needs_rgba;
    if (!ok) {
        fprintf(stderr, "SKIP: no usable font/output for threading fixture\n");
        ass_renderer_done(renderer);
        ass_free_track(track);
        ass_free_track(limit_track);
        ass_library_done(lib);
        return 77;
    }

    const unsigned settings[] = {2, 0};
    for (size_t i = 0; ok && i < sizeof(settings) / sizeof(settings[0]); i++) {
        unsigned threads = ass_set_threads(renderer, settings[i]);
        if (!threads)
            continue;
        for (int run = 0; ok && run < 2; run++) {
            RenderSignature alpha, rgba;
            ok = capture_alpha(renderer, track, &alpha) &&
                 capture_rgba(renderer, track, &rgba);
            if (!ok)
                break;

            bool alpha_same = same_signature(&alpha_serial, &alpha);
            bool rgba_same = same_signature(&rgba_serial, &rgba);
            if (!alpha_same || !rgba_same) {
                fprintf(stderr,
                        "render output differs at requested thread count %u "
                        "(effective=%u, alpha=%s, rgba=%s, run=%d)\n",
                        settings[i], threads, alpha_same ? "same" : "different",
                        rgba_same ? "same" : "different", run);
                if (!alpha_same) {
                    print_signature("alpha serial", &alpha_serial);
                    print_signature("alpha threaded", &alpha);
                }
                if (!rgba_same) {
                    print_signature("rgba serial", &rgba_serial);
                    print_signature("rgba threaded", &rgba);
                }
                ok = false;
            }
        }
    }

    if (ok)
        ok = verify_rgba_limit_fallback(renderer, limit_track);

    ass_renderer_done(renderer);
    ass_free_track(track);
    ass_free_track(limit_track);
    ass_library_done(lib);
    return ok ? 0 : 1;
}
