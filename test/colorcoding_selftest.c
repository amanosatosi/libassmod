#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ass.h"
#include "ass_render.h"
#include "gradient.h"

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

typedef struct {
    bool needs_rgba;
    int primary_pixels;
    bool has_white;
    bool has_colored;
} RgbaColorStats;

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

static ASS_Track *read_header_track(ASS_Library *lib)
{
    return read_events_track(lib, "");
}

static bool render_track_at(ASS_Renderer *renderer, ASS_Track *track,
                            long long now, RenderSig *sig)
{
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
        if (!add_color(sig, cur->color))
            return false;
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

    return sig->count > 0 && sig->coverage > 0;
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

static bool render_rgba_case_at(ASS_Library *lib, ASS_Renderer *renderer,
                                const char *metadata, const char *dialogue,
                                long long now, RgbaSig *sig)
{
    ASS_Track *track = read_case_track(lib, metadata, dialogue);
    if (!track)
        return false;

    int change = 0;
    ASS_ImageRGBA *img = ass_render_frame_rgba(renderer, track, now, &change);
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

static bool render_rgba_case(ASS_Library *lib, ASS_Renderer *renderer,
                             const char *metadata, const char *dialogue,
                             RgbaSig *sig)
{
    return render_rgba_case_at(lib, renderer, metadata, dialogue, 0, sig);
}

static bool render_rgba_events_color_stats(ASS_Library *lib,
                                            ASS_Renderer *renderer,
                                            const char *events, long long now,
                                            RgbaColorStats *stats)
{
    ASS_Track *track = read_events_track(lib, events);
    if (!track)
        return false;

    int change = 0;
    ASS_ImageRGBA *img = ass_render_frame_rgba(renderer, track, now, &change);
    (void) change;

    *stats = (RgbaColorStats) {
        .needs_rgba = ass_frame_needs_rgba(renderer) != 0,
    };
    for (ASS_ImageRGBA *cur = img; cur; cur = cur->next) {
        if (cur->type != IMAGE_TYPE_CHARACTER)
            continue;
        for (int y = 0; y < cur->h; y++) {
            const uint8_t *row = cur->rgba + y * cur->stride;
            for (int x = 0; x < cur->w; x++) {
                uint8_t a = row[4 * x + 3];
                if (a < 192)
                    continue;
                unsigned r = (row[4 * x + 0] * 255u + a / 2u) / a;
                unsigned g = (row[4 * x + 1] * 255u + a / 2u) / a;
                unsigned b = (row[4 * x + 2] * 255u + a / 2u) / a;
                unsigned min_rgb = r < g ? (r < b ? r : b) :
                                   (g < b ? g : b);
                unsigned max_rgb = r > g ? (r > b ? r : b) :
                                   (g > b ? g : b);
                stats->primary_pixels++;
                stats->has_white |= min_rgb > 220;
                stats->has_colored |= max_rgb - min_rgb > 32;
            }
        }
    }

    ass_free_images_rgba(img);
    ass_free_track(track);
    return stats->primary_pixels > 0;
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

static bool expect_one_positioned_gradient_at(
    ASS_Library *lib, ASS_Renderer *renderer, const char *dialogue,
    long long now,
    int stops, double angle, double x1, double y1, double x2, double y2,
    bool rect_valid, const char *label, MangetsuGradientDebugState *debug_out)
{
    MangetsuGradientDebugState debug;
    if (!expect_one_mangetsu_segment_at(lib, renderer, dialogue, now, stops,
                                        angle, label, &debug))
        return false;

    const MangetsuGradientDebugSegment *segment = &debug.segments[0];
    if (segment->target != MANGETSU_GRADIENT_TARGET_COLOR ||
            segment->layer != 0 || segment->coordinate_mode !=
                MANGETSU_GRADIENT_POSITIONED_RECT ||
            !close_double(segment->script_x1, x1) ||
            !close_double(segment->script_y1, y1) ||
            !close_double(segment->script_x2, x2) ||
            !close_double(segment->script_y2, y2) ||
            segment->positioned_rect_valid != rect_valid) {
        fprintf(stderr, "%s\n", label);
        return false;
    }
    if (debug_out)
        *debug_out = debug;
    return true;
}

static bool expect_one_positioned_gradient(
    ASS_Library *lib, ASS_Renderer *renderer, const char *dialogue,
    int stops, double angle, double x1, double y1, double x2, double y2,
    bool rect_valid, const char *label, MangetsuGradientDebugState *debug_out)
{
    return expect_one_positioned_gradient_at(
        lib, renderer, dialogue, 0, stops, angle, x1, y1, x2, y2, rect_valid,
        label, debug_out);
}

static bool test_positioned_gradient_math(void)
{
    MangetsuGradientLayer layer = {
        .active = true,
        .type = MANGETSU_GRADIENT_TYPE_LINEAR,
        .coordinate_mode = MANGETSU_GRADIENT_POSITIONED_RECT,
        .angle = 0.0,
        .n_stops = 2,
        .stops = {
            { .offset = 0.0, .color = 0xFF000000 },
            { .offset = 1.0, .color = 0x0000FF00 },
        },
    };
    uint32_t color = 0;
    ass_mangetsu_gradient_prepare_positioned(&layer, 100, 200, 700, 500);
    if (!layer.positioned_rect.valid ||
            !ass_mangetsu_positioned_gradient_sample_color(&layer, 100, 200,
                                                            &color) ||
            color != 0xFF000000 ||
            !ass_mangetsu_positioned_gradient_sample_color(&layer, 700, 500,
                                                            &color) ||
            color != 0x0000FF00 ||
            ass_mangetsu_positioned_gradient_sample_color(&layer, 99.999, 200,
                                                            &color))
        return false;

    ass_mangetsu_gradient_prepare_positioned(&layer, 700, 500, 100, 200);
    if (!layer.positioned_rect.valid ||
            !ass_mangetsu_positioned_gradient_sample_color(&layer, 100, 350,
                                                            &color) ||
            color != 0xFF000000)
        return false;

    ass_mangetsu_gradient_prepare_positioned(&layer, 100, 200, 100, 500);
    return !layer.positioned_rect.valid &&
           !ass_mangetsu_positioned_gradient_sample_color(&layer, 100, 200,
                                                           &color);
}

static bool test_positioned_gradient_max_stops(ASS_Library *lib,
                                               ASS_Renderer *renderer)
{
    char dialogue[4096];
    int len = snprintf(dialogue, sizeof(dialogue),
                       "{\\pgrd(0,0,640,360,0,&H000000&");
    if (len < 0 || len >= (int) sizeof(dialogue))
        return false;

    for (int i = 1; i < MANGETSU_GRADIENT_MAX_STOPS - 1; i++) {
        int written = snprintf(dialogue + len, sizeof(dialogue) - len,
                               ",%d%%,&H%06X&", i * 100 / 63,
                               (unsigned) (i & 1 ? 0x0000FF : 0xFF0000));
        if (written < 0 || written >= (int) sizeof(dialogue) - len)
            return false;
        len += written;
    }
    int written = snprintf(dialogue + len, sizeof(dialogue) - len,
                           ",&HFFFFFF&)}Maximum");
    if (written < 0 || written >= (int) sizeof(dialogue) - len)
        return false;

    return expect_one_positioned_gradient(
        lib, renderer, dialogue, MANGETSU_GRADIENT_MAX_STOPS, 0.0,
        0, 0, 640, 360, true,
        "positioned gradient did not preserve the Mangetsu maximum stop count",
        NULL);
}

static bool test_positioned_gradient_motion(ASS_Library *lib,
                                            ASS_Renderer *renderer)
{
    const char *events =
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,"
        "{\\move(60,360,580,360)\\1c&HFFFFFF&"
        "\\pgrd(200,300,440,420,0,&H0000FF&,&HFF0000&)}MOVE\n";
    RgbaColorStats before, entering, inside, leaving, after;
    bool ok = render_rgba_events_color_stats(lib, renderer, events, 0, &before) &&
              render_rgba_events_color_stats(lib, renderer, events, 3000, &entering) &&
              render_rgba_events_color_stats(lib, renderer, events, 5000, &inside) &&
              render_rgba_events_color_stats(lib, renderer, events, 7000, &leaving) &&
              render_rgba_events_color_stats(lib, renderer, events, 9000, &after);
    if (!ok)
        return false;

    return before.needs_rgba && entering.needs_rgba && inside.needs_rgba &&
           leaving.needs_rgba && after.needs_rgba &&
           before.has_white && !before.has_colored &&
           entering.has_white && entering.has_colored &&
           !inside.has_white && inside.has_colored &&
           leaving.has_white && leaving.has_colored &&
           after.has_white && !after.has_colored;
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

    ASS_Track *chunk_track = read_header_track(lib);
    if (!chunk_track) {
        ok = false;
    } else {
        const char *chunk =
            "0,0,Default,Nene,0,0,0,,{\\pos(320,180)}Chunk";
        RenderSig got, expected;
        int parsed = ass_process_mangetsu_colorcoding_line(
            chunk_track, "Nene", "mangetsu-colorcoding",
            "{\\1c&HFFB6D9&}", 1, 1);
        ass_process_chunk(chunk_track, chunk, strlen(chunk), 0, 10000);
        bool same = parsed == 1 &&
                    render_track_at(renderer, chunk_track, 0, &got) &&
                    render_events(
                        lib, renderer,
                        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,Nene,0,0,0,,{\\pos(320,180)\\1c&HFFB6D9&}Chunk\n",
                        &expected) &&
                    same_sig(&got, &expected);
        if (!same) {
            fprintf(stderr, "chunk/direct colorcoding metadata API did not apply actor color\n");
            ok = false;
        }
        ass_free_track(chunk_track);
    }

    ASS_Track *stripped_track = read_header_track(lib);
    if (!stripped_track) {
        ok = false;
    } else {
        const char *chunk =
            "0,0,Default,Nene,0,0,0,,{\\pos(320,180)}Stripped";
        RenderSig got, expected;
        ass_process_chunk(stripped_track, chunk, strlen(chunk), 0, 10000);
        bool same = stripped_track->colorcode.n_actors == 0 &&
                    render_track_at(renderer, stripped_track, 0, &got) &&
                    render_events(
                        lib, renderer,
                        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,Nene,0,0,0,,{\\pos(320,180)}Stripped\n",
                        &expected) &&
                    same_sig(&got, &expected);
        if (!same) {
            fprintf(stderr, "host-stripped comments unexpectedly produced actor colorcoding\n");
            ok = false;
        }
        ass_free_track(stripped_track);
    }

    ASS_Track *non_comment_track = read_header_track(lib);
    if (!non_comment_track) {
        ok = false;
    } else {
        int parsed = ass_process_mangetsu_colorcoding_line(
            non_comment_track, "Nene", "mangetsu-colorcoding",
            "{\\1c&HFFB6D9&}", 0, 1);
        if (parsed != 0 || non_comment_track->colorcode.n_actors != 0) {
            fprintf(stderr, "non-comment colorcoding metadata was accepted\n");
            ok = false;
        }
        ass_free_track(non_comment_track);
    }

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

    ok &= expect_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,mangetsu-colorcode-applied-styles,0,0,0,mangetsu-colorcoding,{}{}\n"
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1c&HFFB6D9&}\n",
        "EmptyWhitelist",
        "{\\1c&HFFB6D9&}EmptyWhitelist",
        "empty applied-styles whitelist did not behave as absent");

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

    {
        RenderSig actor_mb, base_mb;
        bool rendered =
            render_case(
                lib, renderer,
                "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\2bs3\\2bc&H7161DF&}\n",
                "NativeMB",
                &actor_mb) &&
            render_case(lib, renderer, "", "NativeMB", &base_mb);
        if (!rendered || same_sig(&actor_mb, &base_mb)) {
            fprintf(stderr, "actor native border size/color did not affect rendering\n");
            ok = false;
        }
    }

    ok &= expect_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\2bs3\\2bc&H7161DF&}\n",
        "NativeMB",
        "{\\2bs3\\2bc&H7161DF&}NativeMB",
        "actor native border size/color did not match inline tags");

    ok &= expect_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\2bc&H7161DF&}\n",
        "{\\bord2}NativeSparse",
        "{\\bord2}NativeSparse",
        "actor native border color enabled a layer without size");

    ok &= expect_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\2bs3\\2bc&H7161DF&\\2ba&H60&}\n",
        "NativeAlpha",
        "{\\2bs3\\2bc&H7161DF&\\2ba&H60&}NativeAlpha",
        "actor native border alpha did not match inline tags");

    ok &= expect_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\2bs3\\2bc&H7161DF&}\n",
        "{\\2bs8\\2bc&H0000FF&}Hello{\\2bs\\2bc} world",
        "{\\2bs8\\2bc&H0000FF&}Hello{\\2bs3\\2bc&H7161DF&} world",
        "blank native border reset did not return to actor defaults");

    ok &= expect_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,mangetsu-colorcode-applied-styles,0,0,0,mangetsu-colorcoding,{Default}{Alt}\n"
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\2bs3\\2bc&H7161DF&}\n",
        "A{\\rAlt}B",
        "{\\2bs3\\2bc&H7161DF&}A{\\rAlt\\2bs3\\2bc&H7161DF&}B",
        "whitelisted \\rStyle did not reapply actor native border defaults");

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
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\2bs5\\2bc&H7161DF&}\n",
        "{\\bord5}ActorAdd",
        "{\\bord5\\2bs5\\2bc&H7161DF&}ActorAdd",
        "actor additive border thickness did not match inline tags");

    ok &= expect_same(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\bord5\\2bs1\\2bc&H7161DF&}\n",
        "{\\2bs5}Hello{\\2bs} world",
        "{\\bord5\\2bs5\\2bc&H7161DF&}Hello{\\2bs1} world",
        "blank actor border thickness reset did not return to actor default");

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

    {
        const char *nested_transition =
            "{\\t(0,1000,\\t(0,500,\\1vc(&H0000FF&,&H00FF00&,"
            "&HFF0000&,&HFFFFFF&))\\bord8)}NestedTransition";
        const char *outer_transition =
            "{\\t(0,1000,\\bord8)}NestedTransition";
        RgbaSig nested_rgba, outer_rgba;
        bool rendered =
            render_rgba_case_at(lib, renderer, "", nested_transition,
                                500, &nested_rgba) &&
            render_rgba_case_at(lib, renderer, "", outer_transition,
                                500, &outer_rgba);
        if (!rendered || nested_rgba.needs_rgba ||
                !same_rgba_sig(&nested_rgba, &outer_rgba)) {
            fprintf(stderr,
                    "non-terminal nested \\t did not fall back to the enclosing transform\n");
            ok = false;
        }

        ASS_Track *track = read_case_track(lib, "", nested_transition);
        if (!track) {
            fprintf(stderr, "could not create nested transition fallback track\n");
            ok = false;
        } else {
            int change = 0;
            ASS_RenderResult result =
                ass_render_frame_auto(renderer, track, 500, &change);
            if (result.use_rgba || !result.imgs) {
                fprintf(stderr,
                        "automatic renderer did not use the legacy fallback for nested \\t\n");
                ok = false;
            }
            ass_render_result_free(&result);
            ass_free_track(track);
        }
    }

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
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\vc(&H0000FF&,&H00FF00&,&HFF0000&,&HFFFFFF&)}\n",
        "VectorAlias", &rgba_actor);
    ok &= render_rgba_case(
        lib, renderer, "",
        "{\\1vc(&H0000FF&,&H00FF00&,&HFF0000&,&HFFFFFF&)}VectorAlias",
        &rgba_explicit);
    if (ok && (!rgba_actor.needs_rgba ||
               !same_rgba_sig(&rgba_actor, &rgba_explicit))) {
        fprintf(stderr, "actor \\vc did not match explicit \\1vc\n");
        ok = false;
    }

    static const char *const vc_tags[] = { "1vc", "2vc", "3vc", "4vc" };
    for (int i = 0; i < 4; i++) {
        char metadata[512];
        char dialogue[512];
        snprintf(metadata, sizeof(metadata),
                 "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,"
                 "mangetsu-colorcoding,{\\%s(&H0000FF&,&H00FF00&,"
                 "&HFF0000&,&HFFFFFF&)}\n",
                 vc_tags[i]);
        snprintf(dialogue, sizeof(dialogue),
                 "{\\shad4\\kf100\\%s(&H0000FF&,&H00FF00&,"
                 "&HFF0000&,&HFFFFFF&)}Vector%d",
                 vc_tags[i], i + 1);
        char actor_dialogue[64];
        snprintf(actor_dialogue, sizeof(actor_dialogue),
                 "{\\shad4\\kf100}Vector%d", i + 1);

        ok &= render_rgba_case(lib, renderer, metadata,
                               actor_dialogue, &rgba_actor);
        ok &= render_rgba_case(lib, renderer, "", dialogue, &rgba_explicit);
        if (ok && (!rgba_actor.needs_rgba ||
                   !same_rgba_sig(&rgba_actor, &rgba_explicit))) {
            fprintf(stderr, "actor \\%s did not match its explicit tag\n",
                    vc_tags[i]);
            ok = false;
        }
    }

    ok &= render_rgba_case(
        lib, renderer,
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\\va(&H00&,&H40&,&H80&,&HC0&)}\n",
        "VectorAlphaAlias", &rgba_actor);
    ok &= render_rgba_case(
        lib, renderer, "",
        "{\\1va(&H00&,&H40&,&H80&,&HC0&)}VectorAlphaAlias",
        &rgba_explicit);
    if (ok && (!rgba_actor.needs_rgba ||
               !same_rgba_sig(&rgba_actor, &rgba_explicit))) {
        fprintf(stderr, "actor \\va did not match explicit \\1va\n");
        ok = false;
    }

    static const char *const va_tags[] = { "1va", "2va", "3va", "4va" };
    for (int i = 0; i < 4; i++) {
        char metadata[512];
        char dialogue[512];
        snprintf(metadata, sizeof(metadata),
                 "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,"
                 "mangetsu-colorcoding,{\\%s(&H00&,&H40&,&H80&,&HC0&)}\n",
                 va_tags[i]);
        snprintf(dialogue, sizeof(dialogue),
                 "{\\shad4\\kf100\\%s(&H00&,&H40&,&H80&,&HC0&)}"
                 "VectorAlpha%d",
                 va_tags[i], i + 1);
        char actor_dialogue[64];
        snprintf(actor_dialogue, sizeof(actor_dialogue),
                 "{\\shad4\\kf100}VectorAlpha%d", i + 1);

        ok &= render_rgba_case(lib, renderer, metadata,
                               actor_dialogue, &rgba_actor);
        ok &= render_rgba_case(lib, renderer, "", dialogue, &rgba_explicit);
        if (ok && (!rgba_actor.needs_rgba ||
                   !same_rgba_sig(&rgba_actor, &rgba_explicit))) {
            fprintf(stderr, "actor \\%s did not match its explicit tag\n",
                    va_tags[i]);
            ok = false;
        }
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

    if (!test_positioned_gradient_math()) {
        fprintf(stderr, "positioned gradient boundary/projection math failed\n");
        ok = false;
    }
    if (!test_positioned_gradient_max_stops(lib, renderer)) {
        fprintf(stderr, "positioned gradient maximum-stop parser test failed\n");
        ok = false;
    }
    if (!test_positioned_gradient_motion(lib, renderer)) {
        fprintf(stderr, "positioned gradient did not remain fixed during motion\n");
        ok = false;
    }

    MangetsuGradientDebugState positioned_debug;
    ok &= expect_one_positioned_gradient(
        lib, renderer,
        "{\\pgrd(100,200,700,500,0,&H000000&,50%,&H0000FF&,&HFFFFFF&)}Positioned",
        3, 0.0, 100, 200, 700, 500, true,
        "valid \\pgrd did not parse as a positioned primary gradient",
        &positioned_debug);
    ok &= expect_one_positioned_gradient(
        lib, renderer,
        "{\\1pgrd(700,500,100,200,450,&H000000&,&HFFFFFF&)}Alias",
        2, 450.0, 100, 200, 700, 500, true,
        "\\1pgrd alias did not normalize the positioned rectangle", NULL);
    if (ok && positioned_debug.segments[0].target !=
            MANGETSU_GRADIENT_TARGET_COLOR) {
        fprintf(stderr, "positioned gradient did not target primary fill\n");
        ok = false;
    }
    RgbaSig positioned_rgba;
    ok &= render_rgba_case(
        lib, renderer, "",
        "{\\1c&HFFFFFF&\\pgrd(100,100,540,260,0,&H000000&,&HFFFFFF&)}RGBA",
        &positioned_rgba);
    if (ok && !positioned_rgba.needs_rgba) {
        fprintf(stderr, "positioned gradient did not require RGBA output\n");
        ok = false;
    }

    ok &= expect_one_positioned_gradient(
        lib, renderer,
        "{\\pgrd(-10.5,20.25,800.75,340.5,-45.5,&H000000&,&HFFFFFF&)}Decimal",
        2, -45.5, -10.5, 20.25, 800.75, 340.5, true,
        "decimal or off-frame positioned coordinates did not parse", NULL);
    ok &= expect_one_positioned_gradient(
        lib, renderer,
        "{\\pgrd(100,100,100,300,0,&H000000&,&HFFFFFF&)}Degenerate",
        2, 0.0, 100, 100, 100, 300, false,
        "zero-width positioned rectangle was not safely disabled", NULL);

    ok &= expect_mangetsu_segments(
        lib, renderer, "{\\pgrd(100,200,700,500,&H000000&,&HFFFFFF&)}Missing",
        0, "positioned gradient missing its angle was accepted");
    ok &= expect_mangetsu_segments(
        lib, renderer, "{\\pgrd(100,200,700,500,0)}Missing",
        0, "positioned gradient missing stops was accepted");
    ok &= expect_mangetsu_segments(
        lib, renderer, "{\\pgrd(100x,200,700,500,0,&H000000&,&HFFFFFF&)}Garbage",
        0, "positioned gradient coordinate garbage was accepted");
    ok &= expect_mangetsu_segments(
        lib, renderer, "{\\pgrd(100,200,700,500,nan,&H000000&,&HFFFFFF&)}NaN",
        0, "positioned gradient NaN angle was accepted");
    ok &= expect_mangetsu_segments(
        lib, renderer, "{\\pgrd(100,200,700,500,inf,&H000000&,&HFFFFFF&)}Infinity",
        0, "positioned gradient infinite angle was accepted");
    ok &= expect_one_positioned_gradient(
        lib, renderer,
        "{\\pgrd(100,200,700,500,0,&H000000&,&HFFFFFF&)\\pgrd(100,,700,500,0,&HFFFFFF&,&H000000&)}Keep",
        2, 0.0, 100, 200, 700, 500, true,
        "rejected positioned gradient changed the active state", NULL);

    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\pgrd(100,200,700,500,0,&H000000&,&HFFFFFF&)\\pgrd()}Reset",
        0, "\\pgrd() did not reset the positioned gradient");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\1pgrd(100,200,700,500,0,&H000000&,&HFFFFFF&)\\1pgrd()}Reset",
        0, "\\1pgrd() did not reset the positioned gradient");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\pgrd(100,200,700,500,0,&H000000&,&HFFFFFF&)\\1c&HFFFFFF&}Color",
        0, "\\1c did not disable the positioned gradient");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\pgrd(100,200,700,500,0,&H000000&,&HFFFFFF&)\\r}Reset",
        0, "\\r did not disable the positioned gradient");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\pgrd(100,200,700,500,0,&H000000&,&HFFFFFF&)\\rAlt}Reset",
        0, "\\rStyleName did not disable the positioned gradient");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\pgrd(100,200,700,500,0,&H000000&,&HFFFFFF&)\\1vc(&H000000&,&HFFFFFF&)}Vector",
        0, "\\1vc did not replace the positioned gradient");
    ok &= expect_mangetsu_segments(
        lib, renderer,
        "{\\pgrd(100,200,700,500,0,&H000000&,&HFFFFFF&)\\1img(missing)}Image",
        0, "\\1img did not replace the positioned gradient");
    ok &= expect_one_positioned_gradient(
        lib, renderer,
        "{\\1grd(0,&H000000&,&HFFFFFF&)\\pgrd(100,200,700,500,0,&H000000&,&HFFFFFF&)}Positioned",
        2, 0.0, 100, 200, 700, 500, true,
        "positioned gradient did not replace attached gradient", NULL);
    ok &= expect_one_mangetsu_segment(
        lib, renderer,
        "{\\pgrd(100,200,700,500,0,&H000000&,&HFFFFFF&)\\1grd(90,&H000000&,&HFFFFFF&)}Attached",
        2, 90.0, "attached gradient did not replace positioned gradient", NULL);

    ok &= expect_one_positioned_gradient_at(
        lib, renderer,
        "{\\pgrd(100,200,700,500,0,&H000000&,&HFFFFFF&)\\t(0,1000,\\pgrd(200,220,800,520,90,&HFFFFFF&,&H000000&))}Transform",
        500, 2, 45.0, 150, 210, 750, 510, true,
        "positioned gradient transform did not interpolate coordinates and angle",
        NULL);
    ok &= expect_one_positioned_gradient_at(
        lib, renderer,
        "{\\pgrd(100,200,700,500,0,&H000000&,&HFFFFFF&)\\t(0,1000,\\pgrd(100,200,700,500,0,&H000000&,50%,&H0000FF&,&HFFFFFF&))}Stops",
        500, 3, 0.0, 100, 200, 700, 500, true,
        "positioned gradient transform did not merge stop positions", NULL);
    ok &= expect_one_positioned_gradient_at(
        lib, renderer,
        "{\\1c&H000000&\\t(0,1000,\\pgrd(100,200,700,500,0,&H0000FF&,&HFFFFFF&))}Solid",
        500, 2, 0.0, 100, 200, 700, 500, true,
        "solid-to-positioned transform did not synthesize a source gradient",
        NULL);
    ok &= expect_one_mangetsu_segment_at(
        lib, renderer,
        "{\\1grd(0,&H000000&,&HFFFFFF&)\\t(0,1000,\\pgrd(100,200,700,500,90,&HFFFFFF&,&H000000&))}Cross",
        500, 2, 0.0,
        "attached-to-positioned transform was not rejected safely", NULL);
    ok &= expect_one_positioned_gradient_at(
        lib, renderer,
        "{\\pgrd(100,200,700,500,0,&H000000&,&HFFFFFF&)\\t(0,1000,\\1grd(90,&HFFFFFF&,&H000000&))}Cross",
        500, 2, 0.0, 100, 200, 700, 500, true,
        "positioned-to-attached transform was not rejected safely", NULL);

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
