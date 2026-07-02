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
    uint32_t colors[64];
    int n_colors;
} RenderSig;

typedef struct {
    int count;
    int outline_count;
    uint64_t alpha_coverage;
    uint64_t hash;
    bool needs_rgba;
} RgbaSig;

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

static char *make_script(const char *metadata, const char *dialogue)
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
        "Style: Alt,Arial,42,&H0000FF00,&H0000FF00,&H80000000,&H80000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1\n"
        "Style: Sign,Arial,42,&H00FF0000,&H00FF0000,&H80000000,&H80000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
    const char *event =
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,Nene,0,0,0,,"
        "{\\pos(320,180)}";
    size_t len = strlen(prefix) + strlen(metadata) + strlen(event) +
                 strlen(dialogue) + 2;
    char *script = malloc(len);
    if (!script)
        return NULL;
    snprintf(script, len, "%s%s%s%s\n", prefix, metadata, event, dialogue);
    return script;
}

static ASS_Track *read_case_track(ASS_Library *lib, const char *metadata,
                                  const char *dialogue)
{
    char *script = make_script(metadata, dialogue);
    if (!script)
        return NULL;

    ASS_Track *track = ass_read_memory(lib, script, strlen(script), NULL);
    free(script);
    return track;
}

static bool render_case(ASS_Library *lib, ASS_Renderer *renderer,
                        const char *metadata, const char *dialogue,
                        RenderSig *sig)
{
    ASS_Track *track = read_case_track(lib, metadata, dialogue);
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
        if (!add_color(sig, cur->color)) {
            ass_free_track(track);
            return false;
        }
        if (cur->type == IMAGE_TYPE_OUTLINE)
            sig->outline_count++;
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

static bool render_rgba_case(ASS_Library *lib, ASS_Renderer *renderer,
                             const char *metadata, const char *dialogue,
                             RgbaSig *sig)
{
    ASS_Track *track = read_case_track(lib, metadata, dialogue);
    if (!track)
        return false;

    int change = 0;
    ASS_ImageRGBA *img = ass_render_frame_rgba(renderer, track, 0, &change);
    (void) change;

    memset(sig, 0, sizeof(*sig));
    sig->hash = 1469598103934665603ULL;
    sig->needs_rgba = ass_frame_needs_rgba(renderer) != 0;

    for (ASS_ImageRGBA *cur = img; cur; cur = cur->next) {
        sig->count++;
        hash_i32(&sig->hash, cur->type);
        hash_i32(&sig->hash, cur->w);
        hash_i32(&sig->hash, cur->h);
        hash_i32(&sig->hash, cur->dst_x);
        hash_i32(&sig->hash, cur->dst_y);
        if (cur->type == IMAGE_TYPE_OUTLINE)
            sig->outline_count++;
        for (int y = 0; y < cur->h; y++) {
            const uint8_t *row = cur->rgba + y * cur->stride;
            for (int x = 0; x < cur->w; x++) {
                for (int c = 0; c < 4; c++)
                    hash_u8(&sig->hash, row[4 * x + c]);
                sig->alpha_coverage += row[4 * x + 3];
            }
        }
    }

    ass_free_images_rgba(img);
    ass_free_track(track);
    return sig->count > 0 && sig->alpha_coverage > 0;
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

static bool same_rgba_sig(const RgbaSig *a, const RgbaSig *b)
{
    return a->count == b->count &&
           a->outline_count == b->outline_count &&
           a->alpha_coverage == b->alpha_coverage &&
           a->hash == b->hash &&
           a->needs_rgba == b->needs_rgba;
}

static bool expect_same(ASS_Library *lib, ASS_Renderer *renderer,
                        const char *metadata, const char *dialogue,
                        const char *expected_dialogue, const char *label)
{
    RenderSig got, expected;
    bool ok = render_case(lib, renderer, metadata, dialogue, &got) &&
              render_case(lib, renderer, "", expected_dialogue, &expected);
    if (!ok || !same_sig(&got, &expected)) {
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
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1c&H0000FF&}\n",
        "Basic",
        "{\\1c&H0000FF&}Basic",
        "actor primary color did not match explicit color");

    ok &= expect_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\fs42}\n",
        "{\\fs60}A{\\fs}B",
        "{\\fs60}A{\\fs42}B",
        "bare \\fs did not reset to actor font size");

    ok &= expect_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1c&H0000FF&}\n",
        "A{\\rAlt}B",
        "{\\1c&H0000FF&}A{\\rAlt}B",
        "explicit \\rStyle reapplied actor color without whitelist");

    ok &= expect_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,mangetsu-colorcode-applied-styles,0,0,0,mangetsu-colorcoding,{Default}{Alt}\n"
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1c&H0000FF&}\n",
        "A{\\rAlt}B{\\rSign}C",
        "{\\1c&H0000FF&}A{\\rAlt\\1c&H0000FF&}B{\\rSign}C",
        "applied-styles whitelist did not gate actor color");

    ok &= expect_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\2bc&H0000FF&}\n",
        "{\\bord2\\2bs5}Sparse",
        "{\\bord2\\2bs5\\2bc&H0000FF&}Sparse",
        "sparse extra border color did not inherit alpha");

    ok &= expect_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\2bc&H0000FF&}\n",
        "{\\bord2}NoLayer",
        "{\\bord2}NoLayer",
        "extra border color enabled a layer without size");

    ok &= expect_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\2bs5\\2bc&H0000FF&}\n",
        "{\\bord2}Layer",
        "{\\bord2\\2bs5\\2bc&H0000FF&}Layer",
        "actor extra border size/color did not enable layer");

    ok &= expect_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\3a&H80&}\n",
        "{\\bord2\\2bs5}Alpha",
        "{\\bord2\\2bs5\\3a&H80&}Alpha",
        "actor \\3a did not apply to all enabled borders");

    ok &= expect_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1c&H0000FF&\\pos(10,10)}\n",
        "Forbidden",
        "{\\1c&H0000FF&}Forbidden",
        "forbidden colorcoding tag was not ignored");

    RgbaSig rgba_actor, rgba_explicit;
    ok &= render_rgba_case(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\2bvc(&H0000FF&,&HFF0000&,&H0000FF&,&HFF0000&)\\2bva(&H00&,&H80&,&H00&,&H80&)}\n",
        "{\\bord2\\2bs8}Grad",
        &rgba_actor);
    ok &= render_rgba_case(
        lib, renderer,
        "",
        "{\\bord2\\2bs8\\2bvc(&H0000FF&,&HFF0000&,&H0000FF&,&HFF0000&)\\2bva(&H00&,&H80&,&H00&,&H80&)}Grad",
        &rgba_explicit);
    if (ok && (!rgba_actor.needs_rgba ||
               !same_rgba_sig(&rgba_actor, &rgba_explicit))) {
        fprintf(stderr, "actor border gradient did not match explicit gradient\n");
        ok = false;
    }

    ok &= render_rgba_case(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1grd(0,&H0000FF&,&HFF0000&)}\n",
        "Grad",
        &rgba_actor);
    ok &= render_rgba_case(
        lib, renderer,
        "",
        "{\\1grd(0,&H0000FF&,&HFF0000&)}Grad",
        &rgba_explicit);
    if (ok && (!rgba_actor.needs_rgba ||
               !same_rgba_sig(&rgba_actor, &rgba_explicit))) {
        fprintf(stderr, "actor Mangetsu gradient did not match explicit gradient\n");
        ok = false;
    }

    ok &= render_rgba_case(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1grd(0,&H0000FF&,&HFF0000&)}\n",
        "{\\1c&H00FF00&}Flat",
        &rgba_actor);
    ok &= render_rgba_case(
        lib, renderer,
        "",
        "{\\1c&H00FF00&}Flat",
        &rgba_explicit);
    if (ok && (rgba_actor.needs_rgba ||
               !same_rgba_sig(&rgba_actor, &rgba_explicit))) {
        fprintf(stderr, "inline \\1c did not replace actor Mangetsu gradient\n");
        ok = false;
    }

    ass_renderer_done(renderer);
    ass_library_done(lib);
    return ok ? 0 : 1;
}
