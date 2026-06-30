#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ass.h"

typedef struct {
    int count;
    int outline_count;
    uint64_t coverage;
    uint64_t hash;
    uint32_t colors[32];
    int n_colors;
} RenderSig;

static void msg_cb(int level, const char *fmt, va_list va, void *data)
{
    (void) level;
    (void) fmt;
    (void) va;
    (void) data;
}

static char *make_script(const char *text)
{
    const char *prefix =
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
        "Style: Default,Arial,42,&H00FFFFFF,&H00FFFFFF,&H80000000,&H80000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:02.00,Default,,0,0,0,,";
    size_t len = strlen(prefix) + strlen(text) + 2;
    char *script = malloc(len);
    if (!script)
        return NULL;
    snprintf(script, len, "%s%s\n", prefix, text);
    return script;
}

static void hash_u8(uint64_t *hash, uint8_t value)
{
    *hash ^= value;
    *hash *= 1099511628211ULL;
}

static void hash_i32(uint64_t *hash, int value)
{
    for (int i = 0; i < 4; i++)
        hash_u8(hash, (uint8_t) ((unsigned) value >> (8 * i)));
}

static void hash_u32(uint64_t *hash, uint32_t value)
{
    for (int i = 0; i < 4; i++)
        hash_u8(hash, (uint8_t) (value >> (8 * i)));
}

static bool add_color(RenderSig *sig, uint32_t color)
{
    for (int i = 0; i < sig->n_colors; i++)
        if (sig->colors[i] == color)
            return true;
    if (sig->n_colors >= (int) (sizeof(sig->colors) / sizeof(sig->colors[0])))
        return false;
    sig->colors[sig->n_colors++] = color;
    return true;
}

static bool render_sig(ASS_Library *lib, ASS_Renderer *renderer,
                       const char *text, long long time, RenderSig *sig)
{
    memset(sig, 0, sizeof(*sig));
    sig->hash = 1469598103934665603ULL;
    char *script = make_script(text);
    if (!script)
        return false;

    ASS_Track *track = ass_read_memory(lib, script, strlen(script), NULL);
    free(script);
    if (!track)
        return false;

    int change = 0;
    ASS_Image *img = ass_render_frame(renderer, track, time, &change);
    (void) change;

    for (ASS_Image *cur = img; cur; cur = cur->next) {
        sig->count++;
        if (cur->type == IMAGE_TYPE_OUTLINE)
            sig->outline_count++;
        if (!add_color(sig, cur->color)) {
            ass_free_track(track);
            return false;
        }
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
           a->outline_count == b->outline_count &&
           a->coverage == b->coverage &&
           a->hash == b->hash &&
           a->n_colors == b->n_colors &&
           !memcmp(a->colors, b->colors, sizeof(a->colors));
}

static bool expect_same_render(ASS_Library *lib, ASS_Renderer *renderer,
                               const char *text, const char *expected,
                               long long time, const char *msg)
{
    RenderSig got, want;
    bool ok = render_sig(lib, renderer, text, time, &got) &&
              render_sig(lib, renderer, expected, time, &want) &&
              same_sig(&got, &want);
    if (!ok)
        fprintf(stderr, "%s\n", msg);
    return ok;
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

    ok &= expect_same_render(lib, renderer,
        "{\\pos(320,180)\\fs\xDB\xB2\xDB\xB0}ExtendedArabic",
        "{\\pos(320,180)\\fs20}ExtendedArabic",
        0, "extended Arabic-Indic \\fs did not match ASCII");

    ok &= expect_same_render(lib, renderer,
        "{\\pos(320,180)\\fs\xE1\x81\x82\xE1\x81\x80}Myanmar",
        "{\\pos(320,180)\\fs20}Myanmar",
        0, "Myanmar \\fs did not match ASCII");

    ok &= expect_same_render(lib, renderer,
        "{\\pos(320,180)\\fs\xEF\xBC\x93\xEF\xBC\x90}Fullwidth",
        "{\\pos(320,180)\\fs30}Fullwidth",
        0, "fullwidth \\fs did not match ASCII");

    ok &= expect_same_render(lib, renderer,
        "{\\pos(320,180)\\bord\xD9\xA3\\shad\xD9\xA2}Border",
        "{\\pos(320,180)\\bord3\\shad2}Border",
        0, "Arabic-Indic border/shadow did not match ASCII");

    ok &= expect_same_render(lib, renderer,
        "{\\pos(\xEF\xBC\x91\xEF\xBC\x90\xEF\xBC\x90,\xEF\xBC\x92\xEF\xBC\x90\xEF\xBC\x90)}Position",
        "{\\pos(100,200)}Position",
        0, "fullwidth \\pos did not match ASCII");

    ok &= expect_same_render(lib, renderer,
        "{\\pos(320,180)\\t(\xEF\xBC\x90,\xDB\xB5\xDB\xB0\xDB\xB0,\\fs\xDB\xB3\xDB\xB0)}Transform",
        "{\\pos(320,180)\\t(0,500,\\fs30)}Transform",
        250, "mixed Unicode digits in \\t did not match ASCII");

    ok &= expect_same_render(lib, renderer,
        "{\\pos(320,180)\\bord\xEF\xBC\x91.\xEF\xBC\x95}Decimal",
        "{\\pos(320,180)\\bord1.5}Decimal",
        0, "fullwidth decimal \\bord did not match ASCII");

    ok &= expect_same_render(lib, renderer,
        "{\\pos(320,180)\\frz-\xD9\xA3\xD9\xA0}Rotate",
        "{\\pos(320,180)\\frz-30}Rotate",
        0, "Arabic-Indic signed \\frz did not match ASCII");

    ok &= expect_same_render(lib, renderer,
        "{\\pos(320,180)\\fs+\xEF\xBC\x92\xEF\xBC\x90}SignedSize",
        "{\\pos(320,180)\\fs+20}SignedSize",
        0, "fullwidth signed \\fs did not match ASCII");

    ok &= expect_same_render(lib, renderer,
        "{\\pos(320,180)\\fs\xEF\xBC\x92\xEF\xBC\x90x}Trailing",
        "{\\pos(320,180)\\fs20x}Trailing",
        0, "Unicode digits with trailing junk did not stop like ASCII");

    ok &= expect_same_render(lib, renderer,
        "{\\pos(320,180)\\fsx\xEF\xBC\x92\xEF\xBC\x90}Malformed",
        "{\\pos(320,180)\\fsx20}Malformed",
        0, "malformed Unicode number did not fail like ASCII");

    ass_renderer_done(renderer);
    ass_library_done(lib);
    return ok ? 0 : 1;
}
