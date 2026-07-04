#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ass.h"
#include "ass_render.h"

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

static char *make_script_from_events(const char *events)
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
        "Style: Other,Arial,42,&H000000FF,&H000000FF,&H80000000,&H80000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
    size_t len = strlen(prefix) + strlen(events) + 1;
    char *script = malloc(len);
    if (!script)
        return NULL;
    snprintf(script, len, "%s%s", prefix, events);
    return script;
}

static char *make_script(const char *metadata, const char *dialogue)
{
    const char *event =
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,Nene,0,0,0,,"
        "{\\pos(320,180)}";
    size_t len = strlen(metadata) + strlen(event) + strlen(dialogue) + 2;
    char *events = malloc(len);
    if (!events)
        return NULL;
    snprintf(events, len, "%s%s%s\n", metadata, event, dialogue);
    char *script = make_script_from_events(events);
    free(events);
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

static ASS_Track *read_events_track(ASS_Library *lib, const char *events)
{
    char *script = make_script_from_events(events);
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

static bool render_events_at(ASS_Library *lib, ASS_Renderer *renderer,
                             const char *events, long long now,
                             RenderSig *sig)
{
    ASS_Track *track = read_events_track(lib, events);
    if (!track)
        return false;

    int change = 0;
    ASS_Image *img = ass_render_frame(renderer, track, now, &change);
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
    return true;
}

static bool render_events(ASS_Library *lib, ASS_Renderer *renderer,
                          const char *events, RenderSig *sig)
{
    return render_events_at(lib, renderer, events, 0, sig);
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

static bool expect_events_same(ASS_Library *lib, ASS_Renderer *renderer,
                               const char *events, const char *expected_events,
                               const char *label)
{
    RenderSig got, expected;
    bool ok = render_events(lib, renderer, events, &got) &&
              render_events(lib, renderer, expected_events, &expected);
    if (!ok || !same_sig(&got, &expected)) {
        fprintf(stderr, "%s\n", label);
        return false;
    }
    return true;
}

static bool render_mangetsu_debug_case_at(ASS_Library *lib,
                                          ASS_Renderer *renderer,
                                          const char *dialogue, long long now,
                                          MangetsuGradientDebugState *debug)
{
    ASS_Track *track = read_case_track(lib, "", dialogue);
    if (!track)
        return false;

    int change = 0;
    ass_render_frame(renderer, track, now, &change);
    (void) change;

    *debug = renderer->mangetsu_gradient_debug;
    ass_free_track(track);
    return true;
}

static bool render_mangetsu_debug_case(ASS_Library *lib, ASS_Renderer *renderer,
                                       const char *dialogue,
                                       MangetsuGradientDebugState *debug)
{
    return render_mangetsu_debug_case_at(lib, renderer, dialogue, 0, debug);
}

static bool close_double(double a, double b)
{
    return fabs(a - b) < 0.000001;
}

static bool expect_mangetsu_segments(ASS_Library *lib, ASS_Renderer *renderer,
                                     const char *dialogue, int segments,
                                     const char *label)
{
    MangetsuGradientDebugState debug;
    if (!render_mangetsu_debug_case(lib, renderer, dialogue, &debug) ||
            debug.n_segments != segments) {
        fprintf(stderr, "%s\n", label);
        return false;
    }
    return true;
}

static bool expect_one_mangetsu_segment(ASS_Library *lib,
                                        ASS_Renderer *renderer,
                                        const char *dialogue, int stops,
                                        double angle, const char *label,
                                        MangetsuGradientDebugState *debug_out)
{
    MangetsuGradientDebugState debug;
    if (!render_mangetsu_debug_case(lib, renderer, dialogue, &debug) ||
            debug.n_segments != 1 ||
            !debug.segments[0].active ||
            debug.segments[0].type != MANGETSU_GRADIENT_TYPE_LINEAR ||
            debug.segments[0].n_stops != stops ||
            !close_double(debug.segments[0].angle, angle)) {
        fprintf(stderr, "%s\n", label);
        return false;
    }
    if (debug_out)
        *debug_out = debug;
    return true;
}

static bool expect_one_mangetsu_segment_at(ASS_Library *lib,
                                           ASS_Renderer *renderer,
                                           const char *dialogue,
                                           long long now, int stops,
                                           double angle, const char *label,
                                           MangetsuGradientDebugState *debug_out)
{
    MangetsuGradientDebugState debug;
    if (!render_mangetsu_debug_case_at(lib, renderer, dialogue, now, &debug) ||
            debug.n_segments != 1 ||
            !debug.segments[0].active ||
            debug.segments[0].type != MANGETSU_GRADIENT_TYPE_LINEAR ||
            debug.segments[0].n_stops != stops ||
            !close_double(debug.segments[0].angle, angle)) {
        fprintf(stderr, "%s\n", label);
        return false;
    }
    if (debug_out)
        *debug_out = debug;
    return true;
}

static bool expect_one_mangetsu_target(ASS_Library *lib,
                                       ASS_Renderer *renderer,
                                       const char *dialogue,
                                       MangetsuGradientTarget target,
                                       int layer, const char *label)
{
    MangetsuGradientDebugState debug;
    if (!expect_one_mangetsu_segment(lib, renderer, dialogue, 2, 0.0,
                                     label, &debug))
        return false;
    if (debug.segments[0].target != target ||
            debug.segments[0].layer != layer) {
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
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\fs42\\1c&HFFB6D9&}\n",
        "Basic",
        "{\\fs42\\1c&HFFB6D9&}Basic",
        "basic actor font size/color did not match explicit tags");

    ok &= expect_events_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1c&HFFB6D9&}\n"
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,Maki,0,0,0,,{\\pos(320,180)}Unknown\n",
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,Maki,0,0,0,,{\\pos(320,180)}Unknown\n",
        "unknown actor received actor colorcoding");

    ok &= expect_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1c&H0000FF&}\n"
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1c&HFFB6D9&}\n",
        "Duplicate",
        "{\\1c&HFFB6D9&}Duplicate",
        "later duplicate actor metadata did not replace earlier defaults");

    ok &= expect_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,mangetsu-colorcode-applied-styles,0,0,0,mangetsu-colorcoding,{Default}{Alt}\n"
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1c&HFFB6D9&}\n",
        "WhitelistDefault",
        "{\\1c&HFFB6D9&}WhitelistDefault",
        "whitelist did not apply actor colorcoding to Default");

    ok &= expect_events_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,mangetsu-colorcode-applied-styles,0,0,0,mangetsu-colorcoding,{Default}{Alt}\n"
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1c&HFFB6D9&}\n"
        "Dialogue: 0,0:00:00.00,0:00:10.00,Other,Nene,0,0,0,,{\\pos(320,180)}WhitelistOther\n",
        "Dialogue: 0,0:00:00.00,0:00:10.00,Other,Nene,0,0,0,,{\\pos(320,180)}WhitelistOther\n",
        "whitelist applied actor colorcoding to non-whitelisted style");

    ok &= expect_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,mangetsu-colorcode-applied-styles,0,0,0,mangetsu-colorcoding,{Default}{Alt}\n"
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1c&HFFB6D9&}\n",
        "A{\\rAlt}B{\\rOther}C",
        "{\\1c&HFFB6D9&}A{\\rAlt\\1c&HFFB6D9&}B{\\rOther}C",
        "whitelist reset behavior did not match active style membership");

    ASS_Style override_style = {0};
    override_style.FontName = "Arial";
    override_style.FontSize = 42;
    override_style.PrimaryColour = 0x00FFFF00;
    override_style.SecondaryColour = 0x00FFFF00;
    override_style.OutlineColour = 0x80000000;
    override_style.BackColour = 0x80000000;
    override_style.ScaleX = 1.0;
    override_style.ScaleY = 1.0;
    override_style.BorderStyle = 1;
    override_style.Outline = 2;
    override_style.Alignment = 2;
    override_style.Encoding = 1;
    ass_set_selective_style_override(renderer, &override_style);
    ass_set_selective_style_override_enabled(renderer, ASS_OVERRIDE_FULL_STYLE);
    ok &= expect_events_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,mangetsu-colorcode-applied-styles,0,0,0,mangetsu-colorcoding,{Default}\n"
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1c&HFFB6D9&}\n"
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,Nene,0,0,0,,Override\n",
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,Nene,0,0,0,,{\\1c&HFFB6D9&}Override\n",
        "selective style override changed actor colorcoding whitelist style");
    ass_set_selective_style_override_enabled(
        renderer, ASS_OVERRIDE_BIT_SELECTIVE_FONT_SCALE);

    ok &= expect_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1c&HFFB6D9&}\n",
        "A{\\r}B",
        "{\\1c&HFFB6D9&}A{\\r\\1c&HFFB6D9&}B",
        "bare reset did not reapply actor colorcoding without whitelist");

    ok &= expect_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1c&HFFB6D9&}\n",
        "A{\\rMissingStyle}B",
        "{\\1c&HFFB6D9&}A{\\rMissingStyle}B",
        "explicit reset to missing style reapplied actor color without whitelist");

    ok &= expect_events_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1c&H0000FF&}\n"
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,Nene,0,0,0,,{\\pos(320,180)}BlockStop\n"
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1c&HFFB6D9&}\n",
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,Nene,0,0,0,,{\\pos(320,180)\\1c&H0000FF&}BlockStop\n",
        "later colorcoding comment outside top block was parsed");

    ok &= expect_events_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1c&H0000FF&}\n"
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,Nene,0,0,0,,{\\pos(320,180)}FormatStop\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1c&HFFB6D9&}\n",
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,Nene,0,0,0,,{\\pos(320,180)\\1c&H0000FF&}FormatStop\n",
        "later Format line reopened actor colorcoding metadata block");

    ok &= expect_events_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1c&HFFB6D9&}\n",
        "",
        "metadata comments produced renderable images");

    ok &= expect_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1c&HFFB6D9&}\n",
        "Sparse",
        "{\\1c&HFFB6D9&}Sparse",
        "sparse actor color did not preserve inherited style fields");

    ok &= expect_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\fs42\\1c&HFFB6D9&}\n",
        "{\\fs60\\1c&H0000FF&}A{\\fs\\1c}B",
        "{\\fs60\\1c&H0000FF&}A{\\fs42\\1c&HFFB6D9&}B",
        "blank font/color reset did not return to actor defaults");

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

    MangetsuGradientDebugState mangetsu_debug;
    ok &= expect_one_mangetsu_segment(
        lib, renderer,
        "{\\1grd(0,&H000000&,&HFFFFFF&)}Simple",
        2, 0.0, "simple Mangetsu gradient did not parse",
        &mangetsu_debug);
    if (ok && (!close_double(mangetsu_debug.segments[0].stops[0].offset, 0.0) ||
               !close_double(mangetsu_debug.segments[0].stops[1].offset, 1.0))) {
        fprintf(stderr, "simple Mangetsu gradient stop offsets were wrong\n");
        ok = false;
    }

    ok &= expect_one_mangetsu_segment(
        lib, renderer,
        "{\\1grd(90,&HFFFFFF&,40%,&H0000FF&,&H000000&)}Multi",
        3, 90.0, "multi-stop Mangetsu gradient did not parse",
        &mangetsu_debug);
    if (ok && !close_double(mangetsu_debug.segments[0].stops[1].offset, 0.4)) {
        fprintf(stderr, "multi-stop Mangetsu gradient percentage was wrong\n");
        ok = false;
    }

    ok &= expect_one_mangetsu_segment(
        lib, renderer,
        "{\\1grd(0,&H000000&,30%,&H000000&,45%,&H0700B7&,70%,&H0700B7&,85%,&H000000&,&H000000&)}Duplicate",
        6, 0.0, "duplicate-color Mangetsu gradient did not parse",
        &mangetsu_debug);
    if (ok && (mangetsu_debug.segments[0].stops[0].color !=
               mangetsu_debug.segments[0].stops[1].color ||
               !close_double(mangetsu_debug.segments[0].stops[1].offset, 0.3))) {
        fprintf(stderr, "duplicate Mangetsu gradient stops were not preserved\n");
        ok = false;
    }

    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\1grd(0,&H000000&,&HFFFFFF&)\\1grd()}Reset",
        0, "empty Mangetsu gradient reset did not disable state");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\1grd(0,&H000000&,&HFFFFFF&)\\1grd0}Reset",
        0, "zero Mangetsu gradient reset did not disable state");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\1grd(0,&H000000&,&HFFFFFF&)\\c&H00FF00&}Flat",
        0, "\\c did not disable Mangetsu gradient state");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\1grd(0,&H000000&,&HFFFFFF&)\\1c&H00FF00&}Flat",
        0, "\\1c did not disable Mangetsu gradient state");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\1grd(0,&H000000&,&HFFFFFF&)\\r}Reset",
        0, "\\r did not reset Mangetsu gradient state");

    ok &= expect_one_mangetsu_target(
        lib, renderer,
        "{\\2grd(0,&H000000&,&HFFFFFF&)}Secondary",
        MANGETSU_GRADIENT_TARGET_COLOR, 1,
        "\\2grd did not map to secondary fill");
    ok &= expect_one_mangetsu_target(
        lib, renderer,
        "{\\bord8\\3grd(0,&H000000&,&HFFFFFF&)}Border",
        MANGETSU_GRADIENT_TARGET_BORDER, 0,
        "\\3grd did not map to border layer 1");
    ok &= expect_one_mangetsu_target(
        lib, renderer,
        "{\\bord8\\1bgrd(0,&H000000&,&HFFFFFF&)}Border",
        MANGETSU_GRADIENT_TARGET_BORDER, 0,
        "\\1bgrd did not map to border layer 1");
    ok &= expect_one_mangetsu_target(
        lib, renderer,
        "{\\shad5\\4grd(0,&H000000&,&HFFFFFF&)}Shadow",
        MANGETSU_GRADIENT_TARGET_COLOR, 3,
        "\\4grd did not map to shadow color");
    ok &= expect_one_mangetsu_target(
        lib, renderer,
        "{\\5grd(0,&H000000&,&HFFFFFF&)}Fifth",
        MANGETSU_GRADIENT_TARGET_COLOR, 4,
        "\\5grd did not parse into fifth channel");
    ok &= expect_one_mangetsu_target(
        lib, renderer,
        "{\\2bs7\\2bgrd(0,&H000000&,&HFFFFFF&)}Border",
        MANGETSU_GRADIENT_TARGET_BORDER, 1,
        "\\2bgrd did not map to border layer 2");
    ok &= expect_one_mangetsu_target(
        lib, renderer,
        "{\\1gra(0,&H00&,&HFF&)}Alpha",
        MANGETSU_GRADIENT_TARGET_ALPHA, 0,
        "\\1gra did not map to primary alpha");
    ok &= expect_one_mangetsu_segment(
        lib, renderer,
        "{\\1gra(90,&HFF&,40%,&H80&,&H00&)}AlphaMulti",
        3, 90.0, "multi-stop Mangetsu alpha gradient did not parse",
        &mangetsu_debug);
    if (ok && !close_double(mangetsu_debug.segments[0].stops[1].offset, 0.4)) {
        fprintf(stderr, "multi-stop Mangetsu alpha gradient percentage was wrong\n");
        ok = false;
    }
    ok &= expect_one_mangetsu_segment(
        lib, renderer,
        "{\\1gra(0,&HFF&,30%,&HFF&,45%,&H00&,70%,&H00&,85%,&HFF&,&HFF&)}AlphaDuplicate",
        6, 0.0, "duplicate-alpha Mangetsu gradient did not parse",
        &mangetsu_debug);
    if (ok && (mangetsu_debug.segments[0].stops[0].color !=
               mangetsu_debug.segments[0].stops[1].color ||
               mangetsu_debug.segments[0].stops[2].color !=
               mangetsu_debug.segments[0].stops[3].color ||
               !close_double(mangetsu_debug.segments[0].stops[1].offset, 0.3))) {
        fprintf(stderr, "duplicate Mangetsu alpha gradient stops were not preserved\n");
        ok = false;
    }
    ok &= expect_one_mangetsu_target(
        lib, renderer,
        "{\\bord8\\3gra(0,&H00&,&HFF&)}BorderAlpha",
        MANGETSU_GRADIENT_TARGET_BORDER_ALPHA, 0,
        "\\3gra did not map to border layer 1 alpha");
    ok &= expect_one_mangetsu_target(
        lib, renderer,
        "{\\2bs7\\2bga(0,&H00&,&HFF&)}BorderAlpha",
        MANGETSU_GRADIENT_TARGET_BORDER_ALPHA, 1,
        "\\2bga did not map to border layer 2 alpha");

    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\2grd(0,&H000000&,&HFFFFFF&)\\2c&H00FF00&}Flat",
        0, "\\2c did not disable \\2grd");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\bord8\\3grd(0,&H000000&,&HFFFFFF&)\\3c&H00FF00&}Flat",
        0, "\\3c did not disable \\3grd/\\1bgrd");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\shad5\\4grd(0,&H000000&,&HFFFFFF&)\\4c&H00FF00&}Flat",
        0, "\\4c did not disable \\4grd");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\5grd(0,&H000000&,&HFFFFFF&)\\5c&H00FF00&}Flat",
        0, "\\5c did not disable \\5grd");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\1bgrd(0,&H000000&,&HFFFFFF&)\\2bgrd(0,&HFFFFFF&,&H000000&)\\2bgrd()}Reset",
        1, "\\2bgrd reset did not preserve \\1bgrd");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\2bs7\\2bgrd(0,&H000000&,&HFFFFFF&)\\1bc&H00FF00&}Border",
        1, "\\1bc incorrectly disabled \\2bgrd");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\2bs7\\2bgrd(0,&H000000&,&HFFFFFF&)\\2bc&H00FF00&}Border",
        0, "\\2bc did not disable \\2bgrd");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\2bs7\\2bgrd(0,&H000000&,&HFFFFFF&)\\2bvc(&H000000&,&HFFFFFF&,&H000000&,&HFFFFFF&)}Border",
        0, "\\2bvc did not replace \\2bgrd as color source");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\1gra(0,&H00&,&HFF&)\\1gra()}Reset",
        0, "empty Mangetsu alpha gradient reset did not disable state");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\1gra(0,&H00&,&HFF&)\\1gra0}Reset",
        0, "zero Mangetsu alpha gradient reset did not disable state");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\1gra(0,&H00&,&HFF&)\\1a&H80&}Flat",
        0, "\\1a did not disable \\1gra");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\1gra(0,&H00&,&HFF&)\\1c&H00FF00&}Color",
        1, "\\1c incorrectly disabled \\1gra");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\1grd(0,&H000000&,&HFFFFFF&)\\1gra(0,&H00&,&HFF&)\\1grd()}Alpha",
        1, "\\1grd reset incorrectly disabled \\1gra");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\1gra(0,&H00&,&HFF&)\\3gra(0,&H00&,&HFF&)\\alpha&H80&}Flat",
        0, "\\alpha did not disable active Mangetsu alpha gradients");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\2bs7\\2bga(0,&H00&,&HFF&)\\1ba&H80&}Border",
        1, "\\1ba incorrectly disabled \\2bga");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\2bs7\\2bga(0,&H00&,&HFF&)\\2ba&H80&}Border",
        0, "\\2ba did not disable \\2bga");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\2bs7\\2bga(0,&H00&,&HFF&)\\2bva(&H00&,&H80&,&H00&,&H80&)}Border",
        0, "\\2bva did not replace \\2bga as alpha source");

    ok &= expect_one_mangetsu_segment_at(
        lib, renderer,
        "{\\1grd(0,&H000000&,&HFFFFFF&)\\t(0,1000,\\1grd(90,&H000000&,&HFFFFFF&))}Anim",
        500, 2, 45.0, "animated \\1grd angle did not interpolate",
        NULL);
    ok &= expect_one_mangetsu_segment_at(
        lib, renderer,
        "{\\1grd(0,&H000000&,&HFFFFFF&)\\t(0,1000,\\1grd(0,&H000000&,50%,&H0000FF&,&HFFFFFF&))}Stops",
        500, 3, 0.0, "animated \\1grd stop union did not parse",
        &mangetsu_debug);
    if (ok && !close_double(mangetsu_debug.segments[0].stops[1].offset, 0.5)) {
        fprintf(stderr, "animated Mangetsu stop union offset was wrong\n");
        ok = false;
    }
    ok &= expect_one_mangetsu_segment_at(
        lib, renderer,
        "{\\c&H000000&\\t(0,1000,\\1grd(0,&H000000&,&HFFFFFF&))}Solid",
        500, 2, 0.0, "solid-to-\\1grd animation did not create a segment",
        NULL);
    ok &= expect_one_mangetsu_segment_at(
        lib, renderer,
        "{\\bord8\\3grd(0,&H000000&,&HFFFFFF&)\\t(0,1000,\\3grd(90,&H0000FF&,&HFFFFFF&))}Border",
        500, 2, 45.0, "animated \\3grd border angle did not interpolate",
        &mangetsu_debug);
    if (ok && (mangetsu_debug.segments[0].target !=
               MANGETSU_GRADIENT_TARGET_BORDER ||
               mangetsu_debug.segments[0].layer != 0)) {
        fprintf(stderr, "animated \\3grd target was not border layer 1\n");
        ok = false;
    }
    ok &= expect_one_mangetsu_segment_at(
        lib, renderer,
        "{\\2bs8\\2bgrd(0,&H000000&,&HFFFFFF&)\\t(0,1000,\\2bgrd(90,&H0000FF&,&HFFFFFF&))}Border",
        500, 2, 45.0, "animated \\2bgrd angle did not interpolate",
        &mangetsu_debug);
    if (ok && (mangetsu_debug.segments[0].target !=
               MANGETSU_GRADIENT_TARGET_BORDER ||
               mangetsu_debug.segments[0].layer != 1)) {
        fprintf(stderr, "animated \\2bgrd target was not border layer 2\n");
        ok = false;
    }
    ok &= expect_one_mangetsu_segment_at(
        lib, renderer,
        "{\\bord8\\3grd(0,&H000000&,&HFFFFFF&)\\t(0,1000,\\1bgrd(90,&HFFFFFF&,&H000000&))}Alias",
        500, 2, 45.0, "animated \\3grd/\\1bgrd alias did not interpolate",
        NULL);
    ok &= expect_one_mangetsu_segment_at(
        lib, renderer,
        "{\\1grd(350,&H000000&,&HFFFFFF&)\\t(0,1000,\\1grd(10,&H000000&,&HFFFFFF&))}Angle",
        500, 2, 0.0, "animated \\1grd angle did not use shortest path",
        NULL);
    ok &= expect_one_mangetsu_segment_at(
        lib, renderer,
        "{\\1grd(0,&H000000&,&HFFFFFF&)\\t(0,1000,\\1grd())}Reset",
        500, 2, 0.0, "animated \\1grd reset corrupted state",
        NULL);
    ok &= expect_one_mangetsu_segment_at(
        lib, renderer,
        "{\\1grd(0,&H000000&,&HFFFFFF&)\\t(0,1000,\\1c&H0000FF&)}Solid",
        500, 2, 0.0, "\\t(\\1c) corrupted active Mangetsu gradient",
        NULL);
    ok &= expect_one_mangetsu_segment_at(
        lib, renderer,
        "{\\1gra(0,&H00&,&HFF&)\\t(0,1000,\\1gra(90,&H00&,&HFF&))}Alpha",
        500, 2, 45.0, "animated \\1gra angle did not interpolate",
        &mangetsu_debug);
    if (ok && (mangetsu_debug.segments[0].target !=
               MANGETSU_GRADIENT_TARGET_ALPHA ||
               mangetsu_debug.segments[0].layer != 0)) {
        fprintf(stderr, "animated \\1gra target was not primary alpha\n");
        ok = false;
    }
    ok &= expect_one_mangetsu_segment_at(
        lib, renderer,
        "{\\1gra(0,&H00&,&HFF&)\\t(0,1000,\\1gra(0,&H00&,50%,&HFF&,&H00&))}AlphaStops",
        500, 3, 0.0, "animated \\1gra stop union did not parse",
        &mangetsu_debug);
    if (ok && !close_double(mangetsu_debug.segments[0].stops[1].offset, 0.5)) {
        fprintf(stderr, "animated Mangetsu alpha stop union offset was wrong\n");
        ok = false;
    }
    ok &= expect_one_mangetsu_segment_at(
        lib, renderer,
        "{\\1a&H00&\\t(0,1000,\\1gra(0,&H00&,&HFF&))}SolidAlpha",
        500, 2, 0.0, "solid-to-\\1gra animation did not create a segment",
        NULL);
    ok &= expect_one_mangetsu_segment_at(
        lib, renderer,
        "{\\2bs8\\2bga(0,&H00&,&HFF&)\\t(0,1000,\\2bga(90,&HFF&,&H00&))}BorderAlpha",
        500, 2, 45.0, "animated \\2bga angle did not interpolate",
        &mangetsu_debug);
    if (ok && (mangetsu_debug.segments[0].target !=
               MANGETSU_GRADIENT_TARGET_BORDER_ALPHA ||
               mangetsu_debug.segments[0].layer != 1)) {
        fprintf(stderr, "animated \\2bga target was not border layer 2 alpha\n");
        ok = false;
    }
    ok &= expect_one_mangetsu_segment_at(
        lib, renderer,
        "{\\bord8\\3gra(0,&H00&,&HFF&)\\t(0,1000,\\1bga(90,&HFF&,&H00&))}AliasAlpha",
        500, 2, 45.0, "animated \\3gra/\\1bga alias did not interpolate",
        NULL);
    ok &= expect_one_mangetsu_segment_at(
        lib, renderer,
        "{\\1gra(0,&H00&,&HFF&)\\t(0,1000,\\1gra())}AlphaReset",
        500, 2, 0.0, "animated \\1gra reset corrupted state",
        NULL);
    ok &= expect_one_mangetsu_segment_at(
        lib, renderer,
        "{\\1gra(0,&H00&,&HFF&)\\t(0,1000,\\1a&H80&)}AlphaSolid",
        500, 2, 0.0, "\\t(\\1a) corrupted active Mangetsu alpha gradient",
        NULL);

    ok &= expect_one_mangetsu_segment(
        lib, renderer,
        "{\\1grd(0,&H000000&,&HFFFFFF&)}A{\\fs60}B{\\b1}C",
        2, 0.0, "font changes split or disabled Mangetsu gradient segment",
        &mangetsu_debug);
    if (ok && mangetsu_debug.segments[0].bitmap_count < 2) {
        fprintf(stderr, "font changes did not exercise multiple Mangetsu bitmap runs\n");
        ok = false;
    }

    ok &= expect_one_mangetsu_segment(
        lib, renderer,
        "{\\1grd(90,&HFFFFFF&,&H000000&)}TOP\\NBOTTOM",
        2, 90.0, "\\N split Mangetsu gradient segment",
        NULL);
    ok &= expect_one_mangetsu_segment(
        lib, renderer,
        "{\\1gra(90,&H00&,&HFF&)}TOP\\NBOTTOM",
        2, 90.0, "\\N split Mangetsu alpha gradient segment",
        NULL);
    ok &= expect_one_mangetsu_segment(
        lib, renderer,
        "{\\1gra(0,&H00&,&HFF&)}A{\\fnArial}B{\\fs80}C",
        2, 0.0, "font changes split Mangetsu alpha gradient segment",
        NULL);

    ok &= expect_one_mangetsu_segment(
        lib, renderer,
        "{\\1grd(0,&H000000&,&HFFFFFF&)}A{\\1grd(0,&H000000&,,&HFFFFFF&)}B",
        2, 0.0, "malformed Mangetsu gradient did not preserve previous state",
        NULL);
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\1grd(,&H000000&,&HFFFFFF&)}Malformed",
        0, "malformed Mangetsu gradient did not get ignored safely");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\1gra(,&H00&,&HFF&)}Malformed",
        0, "malformed Mangetsu alpha gradient did not get ignored safely");

    RgbaSig grd_border, bgrd_border;
    ok &= render_rgba_case(
        lib, renderer, "",
        "{\\bord8\\1c&HFFFFFF&\\3grd(0,&H000000&,&H0000FF&)}Border",
        &grd_border);
    ok &= render_rgba_case(
        lib, renderer, "",
        "{\\bord8\\1c&HFFFFFF&\\1bgrd(0,&H000000&,&H0000FF&)}Border",
        &bgrd_border);
    if (ok && (!grd_border.needs_rgba ||
               !same_rgba_sig(&grd_border, &bgrd_border))) {
        fprintf(stderr, "\\3grd and \\1bgrd did not render as aliases\n");
        ok = false;
    }

    ass_renderer_done(renderer);
    ass_library_done(lib);
    return ok ? 0 : 1;
}
