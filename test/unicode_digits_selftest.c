#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ass.h"

#define MY0 "\xE1\x81\x80"
#define MY1 "\xE1\x81\x81"
#define MY2 "\xE1\x81\x82"
#define MY3 "\xE1\x81\x83"
#define MY4 "\xE1\x81\x84"
#define MY5 "\xE1\x81\x85"
#define MY6 "\xE1\x81\x86"
#define MY8 "\xE1\x81\x88"
#define AR0 "\xD9\xA0"
#define AR1 "\xD9\xA1"
#define AR2 "\xD9\xA2"
#define AR3 "\xD9\xA3"
#define AR5 "\xD9\xA5"
#define AR6 "\xD9\xA6"
#define DEV0 "\xE0\xA5\xA6"
#define DEV1 "\xE0\xA5\xA7"
#define DEV2 "\xE0\xA5\xA8"
#define DEV3 "\xE0\xA5\xA9"
#define DEV7 "\xE0\xA5\xAD"
#define FW0 "\xEF\xBC\x90"
#define FW1 "\xEF\xBC\x91"
#define FW4 "\xEF\xBC\x94"
#define FW5 "\xEF\xBC\x95"
#define TH1 "\xE0\xB9\x91"
#define TH2 "\xE0\xB9\x92"
#define TH3 "\xE0\xB9\x93"
#define KH0 "\xE1\x9F\xA0"
#define KH1 "\xE1\x9F\xA1"
#define KH3 "\xE1\x9F\xA3"

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

static ASS_Track *read_script(ASS_Library *lib, const char *script)
{
    size_t len = strlen(script);
    char *copy = malloc(len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, script, len + 1);
    ASS_Track *track = ass_read_memory(lib, copy, len, NULL);
    free(copy);
    return track;
}

static bool render_script_sig(ASS_Library *lib, ASS_Renderer *renderer,
                              const char *script, long long time,
                              RenderSig *sig)
{
    memset(sig, 0, sizeof(*sig));
    sig->hash = 1469598103934665603ULL;
    ASS_Track *track = read_script(lib, script);
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

static bool render_sig(ASS_Library *lib, ASS_Renderer *renderer,
                       const char *text, long long time, RenderSig *sig)
{
    char *script = make_script(text);
    if (!script)
        return false;
    bool ok = render_script_sig(lib, renderer, script, time, sig);
    free(script);
    return ok;
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

static bool same_numeric_track(const ASS_Track *a, const ASS_Track *b)
{
    if (a->PlayResX != b->PlayResX || a->PlayResY != b->PlayResY ||
            a->LayoutResX != b->LayoutResX ||
            a->LayoutResY != b->LayoutResY ||
            a->Timer != b->Timer || a->WrapStyle != b->WrapStyle ||
            a->ScaledBorderAndShadow != b->ScaledBorderAndShadow ||
            a->Kerning != b->Kerning ||
            a->n_styles != b->n_styles || a->n_events != b->n_events ||
            a->n_styles < 1 || a->n_events < 1)
        return false;

    const ASS_Style *x = &a->styles[0];
    const ASS_Style *y = &b->styles[0];
    if (x->FontSize != y->FontSize || x->Bold != y->Bold ||
            x->Italic != y->Italic || x->Underline != y->Underline ||
            x->StrikeOut != y->StrikeOut || x->ScaleX != y->ScaleX ||
            x->ScaleY != y->ScaleY || x->Spacing != y->Spacing ||
            x->Angle != y->Angle || x->BorderStyle != y->BorderStyle ||
            x->Outline != y->Outline || x->Shadow != y->Shadow ||
            x->Alignment != y->Alignment || x->MarginL != y->MarginL ||
            x->MarginR != y->MarginR || x->MarginV != y->MarginV ||
            x->Encoding != y->Encoding)
        return false;

    const ASS_Event *u = &a->events[0];
    const ASS_Event *v = &b->events[0];
    return u->Layer == v->Layer && u->Start == v->Start &&
           u->Duration == v->Duration && u->MarginL == v->MarginL &&
           u->MarginR == v->MarginR && u->MarginV == v->MarginV;
}

static bool expect_same_script(ASS_Library *lib, ASS_Renderer *renderer,
                               const char *script, const char *expected,
                               long long time, const char *msg)
{
    ASS_Track *got_track = read_script(lib, script);
    ASS_Track *want_track = read_script(lib, expected);
    RenderSig got, want;
    bool ok = got_track && want_track &&
              same_numeric_track(got_track, want_track) &&
              render_script_sig(lib, renderer, script, time, &got) &&
              render_script_sig(lib, renderer, expected, time, &want) &&
              same_sig(&got, &want);
    ass_free_track(got_track);
    ass_free_track(want_track);
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
        "{\\pos(320,180)\\fs" MY4 MY8 "}Myanmar",
        "{\\pos(320,180)\\fs48}Myanmar",
        0, "Myanmar \\fs48 did not match ASCII");

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
        "{\\an3\\pos(" MY6 MY4 MY0 "," MY3 MY6 MY0 ")}Position",
        "{\\an3\\pos(640,360)}Position",
        0, "Myanmar \\pos(640,360) did not match ASCII");

    ok &= expect_same_render(lib, renderer,
        "{\\pos(320,180)\\scale" MY1 MY2 MY5 "}Scale",
        "{\\pos(320,180)\\scale125}Scale",
        0, "Myanmar \\scale125 did not match ASCII");

    ok &= expect_same_render(lib, renderer,
        "{\\pos(320,180)\\scale100\\t(" MY0 "," MY5 MY0 MY0
        ",\\scale" MY1 MY5 MY0 ")}Animated scale",
        "{\\pos(320,180)\\scale100\\t(0,500,\\scale150)}Animated scale",
        250, "Unicode digits in animated \\scale did not match ASCII");

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

    ok &= expect_same_render(lib, renderer,
        "{\\pos(320,180)\\frz1" MY2 AR3 FW4 "}Mixed",
        "{\\pos(320,180)\\frz1234}Mixed",
        0, "mixed-script decimal digits did not match ASCII");

    ok &= expect_same_render(lib, renderer,
        "{\\pos(320,180)\\bord1\\" MY2 "bs3}Layer",
        "{\\pos(320,180)\\bord1\\2bs3}Layer",
        0, "Unicode numbered-border layer did not match ASCII");

    ok &= expect_same_render(lib, renderer,
        "{\\pos(320,180)\\rnd" TH2 "}Random",
        "{\\pos(320,180)\\rnd2}Random",
        0, "Unicode inline \\rnd value did not match ASCII");

    ok &= expect_same_render(lib, renderer,
        "{\\pos(320,180)\\col" KH1 "}Column",
        "{\\pos(320,180)\\col1}Column",
        0, "Unicode \\col value did not match ASCII");

    const char *unicode_script =
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: " MY6 MY4 MY0 "\n"
        "PlayResY: " AR3 AR6 AR0 "\n"
        "LayoutResX: " MY6 MY4 MY0 "\n"
        "LayoutResY: " AR3 AR6 AR0 "\n"
        "Timer: " FW1 FW0 FW0 "." FW0 FW0 "\n"
        "WrapStyle: " DEV2 "\n"
        "ScaledBorderAndShadow: " TH1 "\n"
        "Kerning: " KH1 "\n"
        "\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial," MY4 MY8 ",&H00FFFFFF,&H00FFFFFF,&H80000000,&H80000000,"
        "-" AR1 "," TH1 "," KH0 "," FW0 "," FW1 MY2 AR5 "," DEV1 DEV1 DEV0 ","
        TH2 "." AR5 "," AR1 AR5 "," MY1 "," KH3 "," TH2 "," DEV7 ","
        MY1 MY0 MY0 "," AR1 AR2 AR0 "," FW5 FW0 "," KH1 "\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: " TH2 "," KH0 ":" DEV0 DEV1 ":" DEV2 DEV3 "." FW4 FW5 ","
        AR0 ":" AR0 AR1 ":" AR2 AR5 "." AR5 AR0 ",Default,,"
        MY1 MY1 "," AR2 AR2 "," TH3 TH3 ",,Unicode digits\n";

    const char *ascii_script =
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: 640\n"
        "PlayResY: 360\n"
        "LayoutResX: 640\n"
        "LayoutResY: 360\n"
        "Timer: 100.00\n"
        "WrapStyle: 2\n"
        "ScaledBorderAndShadow: 1\n"
        "Kerning: 1\n"
        "\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,48,&H00FFFFFF,&H00FFFFFF,&H80000000,&H80000000,"
        "-1,1,0,0,125,110,2.5,15,1,3,2,7,100,120,50,1\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 2,0:01:23.45,0:01:25.50,Default,,11,22,33,,Unicode digits\n";

    ok &= expect_same_script(lib, renderer, unicode_script, ascii_script,
                             84000,
        "Unicode Script Info, style, event, or timestamp number did not match ASCII");

    ass_renderer_done(renderer);
    ass_library_done(lib);
    return ok ? 0 : 1;
}
