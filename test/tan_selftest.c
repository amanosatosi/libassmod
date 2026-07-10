#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ass.h"

typedef struct {
    int count;
    uint64_t coverage;
    uint64_t hash;
} RenderSig;

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

static ASS_Track *read_case_track(ASS_Library *lib, const char *text)
{
    char script[8192];
    int n = snprintf(
        script, sizeof(script),
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
        "Style: Default,Arial,42,&H00FFFFFF,&H00FFFFFF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,%s\n",
        text);
    if (n < 0 || n >= (int) sizeof(script))
        return NULL;

    return ass_read_memory(lib, script, strlen(script), NULL);
}

static bool render_case(ASS_Library *lib, ASS_Renderer *renderer,
                        const char *text, RenderSig *sig)
{
    ASS_Track *track = read_case_track(lib, text);
    if (!track)
        return false;

    int change = 0;
    ASS_Image *img = ass_render_frame(renderer, track, 0, &change);
    (void) change;

    memset(sig, 0, sizeof(*sig));
    sig->hash = 1469598103934665603ULL;
    for (ASS_Image *cur = img; cur; cur = cur->next) {
        sig->count++;
        hash_i32(&sig->hash, cur->type);
        hash_i32(&sig->hash, cur->w);
        hash_i32(&sig->hash, cur->h);
        hash_i32(&sig->hash, cur->dst_x);
        hash_i32(&sig->hash, cur->dst_y);
        hash_u32(&sig->hash, cur->color);
        for (int y = 0; y < cur->h; y++) {
            const unsigned char *row = cur->bitmap + y * cur->stride;
            for (int x = 0; x < cur->w; x++) {
                sig->coverage += row[x];
                hash_u8(&sig->hash, row[x]);
            }
        }
    }

    ass_free_track(track);
    return sig->count > 0 && sig->coverage > 0;
}

static bool same_sig(const RenderSig *a, const RenderSig *b)
{
    return a->count == b->count &&
           a->coverage == b->coverage &&
           a->hash == b->hash;
}

static bool expect_same(ASS_Library *lib, ASS_Renderer *renderer,
                        const char *a, const char *b, const char *label)
{
    RenderSig sig_a, sig_b;
    bool ok = render_case(lib, renderer, a, &sig_a) &&
              render_case(lib, renderer, b, &sig_b);
    if (!ok || !same_sig(&sig_a, &sig_b)) {
        fprintf(stderr, "%s\n", label);
        return false;
    }
    return true;
}

static bool expect_different(ASS_Library *lib, ASS_Renderer *renderer,
                             const char *a, const char *b, const char *label)
{
    RenderSig sig_a, sig_b;
    bool ok = render_case(lib, renderer, a, &sig_a) &&
              render_case(lib, renderer, b, &sig_b);
    if (!ok || same_sig(&sig_a, &sig_b)) {
        fprintf(stderr, "%s\n", label);
        return false;
    }
    return true;
}

int main(void)
{
    ASS_Library *lib = ass_library_init();
    if (!lib) {
        fprintf(stderr, "failed to init ass library\n");
        return 1;
    }
    ass_set_message_cb(lib, msg_cb, NULL);

    ASS_Renderer *renderer = ass_renderer_init(lib);
    if (!renderer) {
        fprintf(stderr, "failed to init renderer\n");
        ass_library_done(lib);
        return 1;
    }

    ass_set_storage_size(renderer, 640, 360);
    ass_set_frame_size(renderer, 640, 360);
    ass_set_fonts(renderer, NULL, "sans-serif",
                  ASS_FONTPROVIDER_AUTODETECT, NULL, 1);

    bool ok = true;

    ok &= expect_same(
        lib, renderer,
        "{\\an3\\tan3\\pos(320,180)}Hello",
        "{\\an3\\pos(320,180)}Hello",
        "\\tan matching \\an changed rendering");

    ok &= expect_same(
        lib, renderer,
        "{\\an3\\tan7\\pos(320,180)}Hello",
        "{\\an1\\pos(320,180)}Hello",
        "\\tan did not select left horizontal alignment");

    ok &= expect_same(
        lib, renderer,
        "{\\an3\\tan8\\pos(320,180)}Hello",
        "{\\an2\\pos(320,180)}Hello",
        "\\tan did not select center horizontal alignment");

    ok &= expect_different(
        lib, renderer,
        "{\\an3\\tan7\\pos(320,180)}Hello",
        "{\\an3\\pos(320,180)}Hello",
        "\\tan did not change horizontal text layout relative to the "
        "object anchor");

    ok &= expect_same(
        lib, renderer,
        "{\\an7\\tan3\\pos(320,180)}Hello",
        "{\\an9\\pos(320,180)}Hello",
        "\\tan did not preserve top vertical text alignment");

    ok &= expect_same(
        lib, renderer,
        "{\\an3\\tan7\\pos(320,180)\\frz25}Hello",
        "{\\an3\\tan7\\pos(320,180)\\org(320,180)\\frz25}Hello",
        "\\tan moved the default transform origin away from the object anchor");

    ass_renderer_done(renderer);
    ass_library_done(lib);
    return ok ? 0 : 1;
}
