/*
 * Renderer-level determinism checks for event-parallel rendering.
 */

#include <stdbool.h>
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

static ASS_Renderer *create_renderer(ASS_Library *lib, unsigned threads,
                                     unsigned *normalized)
{
    ASS_Renderer *renderer = ass_renderer_init(lib);
    if (!renderer)
        return NULL;

    *normalized = threads == 1 ? 1 : ass_set_threads(renderer, threads);
    ass_set_storage_size(renderer, 640, 360);
    ass_set_frame_size(renderer, 640, 360);
    ass_set_fonts(renderer, NULL, "sans-serif",
                  ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    return renderer;
}

static bool capture_alpha(ASS_Library *lib, unsigned threads,
                          RenderSignature *sig, unsigned *normalized)
{
    ASS_Renderer *renderer = create_renderer(lib, threads, normalized);
    if (!renderer)
        return false;
    ASS_Track *track = ass_read_memory(lib, (char *) script,
                                       sizeof(script) - 1, NULL);
    if (!track) {
        ass_renderer_done(renderer);
        return false;
    }

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

    ass_renderer_done(renderer);
    ass_free_track(track);
    return sig->images > 0 && sig->coverage > 0;
}

static bool capture_rgba(ASS_Library *lib, unsigned threads,
                         RenderSignature *sig, unsigned *normalized)
{
    ASS_Renderer *renderer = create_renderer(lib, threads, normalized);
    if (!renderer)
        return false;
    ASS_Track *track = ass_read_memory(lib, (char *) script,
                                       sizeof(script) - 1, NULL);
    if (!track) {
        ass_renderer_done(renderer);
        return false;
    }

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
    ass_renderer_done(renderer);
    ass_free_track(track);
    return sig->images > 0 && sig->coverage > 0;
}

static bool same_signature(const RenderSignature *a,
                           const RenderSignature *b)
{
    return a->hash == b->hash && a->coverage == b->coverage &&
        a->images == b->images && a->needs_rgba == b->needs_rgba;
}

int main(void)
{
    ASS_Library *lib = ass_library_init();
    if (!lib)
        return 1;
    ass_set_message_cb(lib, msg_cb, NULL);

    RenderSignature alpha_serial, rgba_serial;
    unsigned normalized;
    bool ok = capture_alpha(lib, 1, &alpha_serial, &normalized) &&
              capture_rgba(lib, 1, &rgba_serial, &normalized);
    ok = ok && alpha_serial.needs_rgba && rgba_serial.needs_rgba;

    const unsigned settings[] = {2, 0};
    for (size_t i = 0; ok && i < sizeof(settings) / sizeof(settings[0]); i++) {
        for (int run = 0; ok && run < 2; run++) {
            RenderSignature alpha, rgba;
            unsigned alpha_threads, rgba_threads;
            ok = capture_alpha(lib, settings[i], &alpha, &alpha_threads) &&
                 capture_rgba(lib, settings[i], &rgba, &rgba_threads);
            if (!ok)
                break;

            if (!alpha_threads || !rgba_threads)
                continue;
            if (!same_signature(&alpha_serial, &alpha) ||
                !same_signature(&rgba_serial, &rgba)) {
                fprintf(stderr,
                        "render output differs at requested thread count %u "
                        "(alpha=%u, rgba=%u, run=%d)\n",
                        settings[i], alpha_threads, rgba_threads, run);
                ok = false;
            }
        }
    }

    ass_library_done(lib);
    return ok ? 0 : 1;
}
