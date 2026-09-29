#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ass.h"

typedef struct {
    int count;
    int outline_count;
    int min_x, min_y;
    int max_x, max_y;
    uint64_t coverage;
    uint32_t colors[32];
    int n_colors;
} RenderSig;

typedef struct {
    int count;
    int outline_count;
    uint64_t alpha_coverage;
    uint64_t hash;
    bool needs_rgba;
    bool outline_red;
    bool outline_blue;
} RgbaSig;

typedef struct {
    int count;
    int min_x, min_y;
    int max_x, max_y;
    uint64_t coverage;
    uint64_t hash;
    uint64_t visible_hash;
} ShadowSig;

typedef struct {
    int count;
    int min_x, min_y;
    int max_x, max_y;
    uint64_t coverage;
    uint64_t hash;
    uint64_t visible_hash;
} BorderSig;

static void msg_cb(int level, const char *fmt, va_list va, void *data)
{
    (void) level;
    (void) fmt;
    (void) va;
    (void) data;
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

static bool has_color(const RenderSig *sig, uint32_t color)
{
    for (int i = 0; i < sig->n_colors; i++)
        if (sig->colors[i] == color)
            return true;
    return false;
}

static void add_bounds(RenderSig *sig, const ASS_Image *img)
{
    int x0 = img->dst_x;
    int y0 = img->dst_y;
    int x1 = img->dst_x + img->w;
    int y1 = img->dst_y + img->h;
    if (!sig->count) {
        sig->min_x = x0;
        sig->min_y = y0;
        sig->max_x = x1;
        sig->max_y = y1;
    } else {
        if (x0 < sig->min_x)
            sig->min_x = x0;
        if (y0 < sig->min_y)
            sig->min_y = y0;
        if (x1 > sig->max_x)
            sig->max_x = x1;
        if (y1 > sig->max_y)
            sig->max_y = y1;
    }
}

static int sig_height(const RenderSig *sig)
{
    return sig->max_y - sig->min_y;
}

static int sig_width(const RenderSig *sig)
{
    return sig->max_x - sig->min_x;
}

static ASS_Track *read_case_track_with_border_style(ASS_Library *lib,
                                                    const char *text,
                                                    int border_style)
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
        "Style: Default,Arial,42,&H00FFFFFF,&H00FFFFFF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,%d,2,0,2,10,10,10,1\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,{\\pos(320,180)}%s\n",
        border_style, text);
    if (n < 0 || n >= (int) sizeof(script))
        return NULL;

    return ass_read_memory(lib, script, strlen(script), NULL);
}

static ASS_Track *read_case_track(ASS_Library *lib, const char *text)
{
    return read_case_track_with_border_style(lib, text, 1);
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
    for (ASS_Image *cur = img; cur; cur = cur->next) {
        add_bounds(sig, cur);
        sig->count++;
        if (!add_color(sig, cur->color)) {
            ass_free_track(track);
            return false;
        }
        if (cur->type == IMAGE_TYPE_OUTLINE)
            sig->outline_count++;
        for (int y = 0; y < cur->h; y++) {
            const unsigned char *row = cur->bitmap + y * cur->stride;
            for (int x = 0; x < cur->w; x++)
                sig->coverage += row[x];
        }
    }

    ass_free_track(track);
    return sig->count > 0 && sig->coverage > 0;
}

static bool render_case_with_border_style(ASS_Library *lib,
                                          ASS_Renderer *renderer,
                                          int border_style,
                                          const char *text,
                                          RenderSig *sig)
{
    ASS_Track *track =
        read_case_track_with_border_style(lib, text, border_style);
    if (!track)
        return false;

    int change = 0;
    ASS_Image *img = ass_render_frame(renderer, track, 0, &change);
    (void) change;

    memset(sig, 0, sizeof(*sig));
    for (ASS_Image *cur = img; cur; cur = cur->next) {
        add_bounds(sig, cur);
        sig->count++;
        if (!add_color(sig, cur->color)) {
            ass_free_track(track);
            return false;
        }
        if (cur->type == IMAGE_TYPE_OUTLINE)
            sig->outline_count++;
        for (int y = 0; y < cur->h; y++) {
            const unsigned char *row = cur->bitmap + y * cur->stride;
            for (int x = 0; x < cur->w; x++)
                sig->coverage += row[x];
        }
    }

    ass_free_track(track);
    return sig->count > 0 && sig->coverage > 0;
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

static bool render_border_case_at(ASS_Library *lib, ASS_Renderer *renderer,
                                  const char *text, long long time,
                                  uint32_t color, BorderSig *sig)
{
    ASS_Track *track = read_case_track(lib, text);
    if (!track)
        return false;
    int change = 0;
    ASS_Image *img = ass_render_frame(renderer, track, time, &change);
    (void) change;
    memset(sig, 0, sizeof(*sig));
    sig->hash = 1469598103934665603ULL;
    sig->visible_hash = 1469598103934665603ULL;
    bool have_pixel = false;
    for (ASS_Image *cur = img; cur; cur = cur->next) {
        if (cur->type != IMAGE_TYPE_OUTLINE || cur->color != color)
            continue;
        sig->count++;
        hash_i32(&sig->hash, cur->dst_x);
        hash_i32(&sig->hash, cur->dst_y);
        hash_i32(&sig->hash, cur->w);
        hash_i32(&sig->hash, cur->h);
        for (int y = 0; y < cur->h; y++) {
            const uint8_t *row = cur->bitmap + y * cur->stride;
            for (int x = 0; x < cur->w; x++) {
                uint8_t value = row[x];
                hash_u8(&sig->hash, value);
                if (!value)
                    continue;
                int px = cur->dst_x + x;
                int py = cur->dst_y + y;
                if (!have_pixel) {
                    sig->min_x = sig->max_x = px;
                    sig->min_y = sig->max_y = py;
                    have_pixel = true;
                } else {
                    if (px < sig->min_x)
                        sig->min_x = px;
                    if (py < sig->min_y)
                        sig->min_y = py;
                    if (px > sig->max_x)
                        sig->max_x = px;
                    if (py > sig->max_y)
                        sig->max_y = py;
                }
                sig->coverage += value;
                hash_i32(&sig->visible_hash, px);
                hash_i32(&sig->visible_hash, py);
                hash_u8(&sig->visible_hash, value);
            }
        }
    }
    ass_free_track(track);
    return sig->count > 0 && have_pixel;
}

static bool render_border_case(ASS_Library *lib, ASS_Renderer *renderer,
                               const char *text, uint32_t color,
                               BorderSig *sig)
{
    return render_border_case_at(lib, renderer, text, 0, color, sig);
}

static bool same_border(const BorderSig *a, const BorderSig *b)
{
    return a->count == b->count && a->coverage == b->coverage &&
           a->min_x == b->min_x && a->min_y == b->min_y &&
           a->max_x == b->max_x && a->max_y == b->max_y &&
           a->hash == b->hash;
}

static bool same_border_visible(const BorderSig *a, const BorderSig *b)
{
    return a->count == b->count && a->coverage == b->coverage &&
           a->min_x == b->min_x && a->min_y == b->min_y &&
           a->max_x == b->max_x && a->max_y == b->max_y &&
           a->visible_hash == b->visible_hash;
}

static bool expect_border_filter(ASS_Library *lib, ASS_Renderer *renderer,
                                 const char *first, const char *second,
                                 uint32_t color, bool equal, const char *name)
{
    BorderSig a, b;
    bool ok = render_border_case(lib, renderer, first, color, &a) &&
              render_border_case(lib, renderer, second, color, &b);
    if (ok)
        ok = same_border(&a, &b) == equal;
    if (!ok)
        fprintf(stderr, "%s border filter mismatch\n", name);
    return ok;
}

static bool expect_border_visible_same(ASS_Library *lib,
                                       ASS_Renderer *renderer,
                                       const char *first,
                                       const char *second,
                                       uint32_t color, const char *name)
{
    BorderSig a, b;
    bool ok = render_border_case(lib, renderer, first, color, &a) &&
              render_border_case(lib, renderer, second, color, &b);
    if (ok)
        ok = same_border_visible(&a, &b);
    if (!ok)
        fprintf(stderr, "%s changed visible border geometry\n", name);
    return ok;
}

static bool expect_border_spread(ASS_Library *lib, ASS_Renderer *renderer,
                                 const char *sharp_text,
                                 const char *soft_text, uint32_t color,
                                 int min_growth, const char *name)
{
    BorderSig sharp, soft;
    bool ok = render_border_case(lib, renderer, sharp_text, color, &sharp) &&
              render_border_case(lib, renderer, soft_text, color, &soft);
    if (ok)
        ok = soft.min_x <= sharp.min_x - min_growth &&
             soft.max_x >= sharp.max_x + min_growth &&
             soft.min_y <= sharp.min_y - min_growth &&
             soft.max_y >= sharp.max_y + min_growth;
    if (!ok)
        fprintf(stderr, "%s did not spread the border mask\n", name);
    return ok;
}

static bool render_shadow_case(ASS_Library *lib, ASS_Renderer *renderer,
                               const char *text, ShadowSig *sig)
{
    ASS_Track *track = read_case_track(lib, text);
    if (!track)
        return false;

    int change = 0;
    ASS_Image *img = ass_render_frame(renderer, track, 0, &change);
    (void) change;

    memset(sig, 0, sizeof(*sig));
    sig->hash = 1469598103934665603ULL;
    sig->visible_hash = 1469598103934665603ULL;
    bool have_pixel = false;
    for (ASS_Image *cur = img; cur; cur = cur->next) {
        if (cur->type != IMAGE_TYPE_SHADOW)
            continue;
        sig->count++;
        hash_i32(&sig->hash, cur->dst_x);
        hash_i32(&sig->hash, cur->dst_y);
        hash_i32(&sig->hash, cur->w);
        hash_i32(&sig->hash, cur->h);
        for (int y = 0; y < cur->h; y++) {
            const uint8_t *row = cur->bitmap + y * cur->stride;
            for (int x = 0; x < cur->w; x++) {
                uint8_t value = row[x];
                hash_u8(&sig->hash, value);
                if (!value)
                    continue;
                int px = cur->dst_x + x;
                int py = cur->dst_y + y;
                hash_i32(&sig->visible_hash, px);
                hash_i32(&sig->visible_hash, py);
                hash_u8(&sig->visible_hash, value);
                if (!have_pixel) {
                    sig->min_x = sig->max_x = px;
                    sig->min_y = sig->max_y = py;
                    have_pixel = true;
                } else {
                    if (px < sig->min_x)
                        sig->min_x = px;
                    if (py < sig->min_y)
                        sig->min_y = py;
                    if (px > sig->max_x)
                        sig->max_x = px;
                    if (py > sig->max_y)
                        sig->max_y = py;
                }
                sig->coverage += value;
            }
        }
    }

    ass_free_track(track);
    return sig->count > 0 && have_pixel;
}

static bool shadow_grew(const ShadowSig *inner, const ShadowSig *outer,
                        int min_x, int min_y)
{
    return inner->count == outer->count &&
           outer->coverage > inner->coverage &&
           outer->min_x <= inner->min_x - min_x &&
           outer->max_x >= inner->max_x + min_x &&
           outer->min_y <= inner->min_y - min_y &&
           outer->max_y >= inner->max_y + min_y;
}

static bool same_shadow(const ShadowSig *a, const ShadowSig *b)
{
    return a->count == b->count && a->coverage == b->coverage &&
           a->min_x == b->min_x && a->min_y == b->min_y &&
           a->max_x == b->max_x && a->max_y == b->max_y &&
           a->visible_hash == b->visible_hash;
}

static bool expect_shadow_growth(ASS_Library *lib, ASS_Renderer *renderer,
                                 const char *inner_text,
                                 const char *outer_text,
                                 int min_x, int min_y, const char *name)
{
    ShadowSig inner, outer;
    bool ok = render_shadow_case(lib, renderer, inner_text, &inner) &&
              render_shadow_case(lib, renderer, outer_text, &outer);
    if (ok)
        ok = shadow_grew(&inner, &outer, min_x, min_y);
    if (!ok)
        fprintf(stderr, "%s did not grow the shadow silhouette\n", name);
    return ok;
}

static bool expect_shadow_shift(ASS_Library *lib, ASS_Renderer *renderer,
                                const char *baseline_text,
                                const char *shifted_text,
                                int dx, int dy, const char *name)
{
    ShadowSig baseline, shifted;
    bool ok = render_shadow_case(lib, renderer, baseline_text, &baseline) &&
              render_shadow_case(lib, renderer, shifted_text, &shifted);
    if (ok)
        ok = baseline.count == shifted.count &&
             baseline.coverage == shifted.coverage &&
             shifted.min_x == baseline.min_x + dx &&
             shifted.max_x == baseline.max_x + dx &&
             shifted.min_y == baseline.min_y + dy &&
             shifted.max_y == baseline.max_y + dy;
    if (!ok)
        fprintf(stderr, "%s changed the shadow mask or offset\n", name);
    return ok;
}

static bool expect_shadow_match(ASS_Library *lib, ASS_Renderer *renderer,
                                const char *single_text,
                                const char *multi_text, const char *name)
{
    ShadowSig single = {0}, multi = {0};
    bool ok = render_shadow_case(lib, renderer, single_text, &single) &&
              render_shadow_case(lib, renderer, multi_text, &multi);
    if (ok)
        ok = same_shadow(&single, &multi);
    if (!ok) {
        fprintf(stderr, "%s shadow differs from cumulative single border\n", name);
        fprintf(stderr,
                "single: count=%d coverage=%llu bounds=(%d,%d)-(%d,%d) hash=%llu visible=%llu\n"
                "multi:  count=%d coverage=%llu bounds=(%d,%d)-(%d,%d) hash=%llu visible=%llu\n",
                single.count, (unsigned long long) single.coverage,
                single.min_x, single.min_y, single.max_x, single.max_y,
                (unsigned long long) single.hash,
                (unsigned long long) single.visible_hash,
                multi.count, (unsigned long long) multi.coverage,
                multi.min_x, multi.min_y, multi.max_x, multi.max_y,
                (unsigned long long) multi.hash,
                (unsigned long long) multi.visible_hash);
    }
    return ok;
}

static bool render_rgba_case(ASS_Library *lib, ASS_Renderer *renderer,
                             const char *text, RgbaSig *sig)
{
    ASS_Track *track = read_case_track(lib, text);
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
                uint8_t r = row[4 * x + 0];
                uint8_t b = row[4 * x + 2];
                uint8_t a = row[4 * x + 3];
                sig->alpha_coverage += a;
                for (int c = 0; c < 4; c++)
                    hash_u8(&sig->hash, row[4 * x + c]);
                if (cur->type == IMAGE_TYPE_OUTLINE && a) {
                    sig->outline_red |= r > b && r > 0;
                    sig->outline_blue |= b > r && b > 0;
                }
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
           a->n_colors == b->n_colors &&
           !memcmp(a->colors, b->colors, sizeof(a->colors));
}

static bool same_coverage_bounds(const RenderSig *a, const RenderSig *b)
{
    return a->coverage == b->coverage &&
           a->min_x == b->min_x && a->min_y == b->min_y &&
           a->max_x == b->max_x && a->max_y == b->max_y;
}

static bool same_rgba_sig(const RgbaSig *a, const RgbaSig *b)
{
    return a->count == b->count &&
           a->outline_count == b->outline_count &&
           a->alpha_coverage == b->alpha_coverage &&
           a->hash == b->hash &&
           a->needs_rgba == b->needs_rgba &&
           a->outline_red == b->outline_red &&
           a->outline_blue == b->outline_blue;
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

    RenderSig legacy, numbered, multi, invalid, expected;
    RenderSig equal_size, small_outer, three_layers, anisotropic;
    RenderSig bs5, style_bs5, malformed;
    RenderSig box_base, box_border, box_multi, box_numbered, bs_ignore;
    RenderSig box_reference, box_small_outer, box_large_outer, box_three_layers;
    RgbaSig rgba_legacy, rgba_numbered, rgba_multi, rgba_flat;
    bool ok = true;

    ok &= expect_shadow_growth(lib, renderer,
                               "{\\bord2\\2bs0\\shad4}Shadow",
                               "{\\bord2\\2bs8\\shad4}Shadow",
                               4, 4, "outer native border");
    ok &= expect_shadow_match(lib, renderer,
                              "{\\bord10\\shad4}Shadow",
                              "{\\bord2\\2bs8\\shad4}Shadow",
                              "outer native border");
    ok &= expect_shadow_growth(lib, renderer,
                               "{\\bord2\\2bs4\\shad5}TripleShadow",
                               "{\\bord2\\2bs4\\3bs6\\shad5}TripleShadow",
                               3, 3, "third native border");
    ok &= expect_shadow_growth(lib, renderer,
                               "{\\bord2\\shad5}SkipLayer",
                               "{\\bord2\\3bs6\\shad5}SkipLayer",
                               3, 3, "missing intermediate border");
    ok &= expect_shadow_growth(lib, renderer,
                               "{\\bord2\\xshad-6\\yshad3}NegativeShadow",
                               "{\\bord2\\2bs5\\xshad-6\\yshad3}NegativeShadow",
                               2, 2, "negative-offset border");
    ok &= expect_shadow_shift(lib, renderer,
                              "{\\bord2\\2bs5\\xshad0\\yshad3}NegativeShadow",
                              "{\\bord2\\2bs5\\xshad-6\\yshad3}NegativeShadow",
                              -6, 0, "negative x shadow offset");
    ok &= expect_shadow_shift(lib, renderer,
                              "{\\bord2\\2bs6\\xshad5\\yshad0}NegativeY",
                              "{\\bord2\\2bs6\\xshad5\\yshad-5}NegativeY",
                              0, -5, "negative y shadow offset");
    ok &= expect_shadow_growth(lib, renderer,
                               "{\\xbord3\\ybord2\\shad4}AnisoShadow",
                               "{\\xbord3\\ybord2\\2bsx7\\2bsy4\\shad4}AnisoShadow",
                               3, 2, "anisotropic outer border");
    ok &= expect_shadow_growth(lib, renderer,
                               "{\\bord2\\blur3\\shad5}BlurShadow",
                               "{\\bord2\\2bs6\\blur3\\shad5}BlurShadow",
                               3, 3, "blurred outer border");
    ok &= expect_shadow_match(lib, renderer,
                              "{\\bord8\\blur3\\shad5}BlurShadow",
                              "{\\bord2\\2bs6\\blur3\\shad5}BlurShadow",
                              "blurred cumulative border");
    ok &= expect_shadow_growth(lib, renderer,
                               "{\\bord2\\be2\\shad5}BeShadow",
                               "{\\bord2\\2bs6\\be2\\shad5}BeShadow",
                               3, 3, "BE outer border");
    ok &= expect_shadow_match(lib, renderer,
                              "{\\bord8\\be2\\shad5}BeShadow",
                              "{\\bord2\\2bs6\\be2\\shad5}BeShadow",
                              "BE cumulative border");
    ok &= expect_shadow_growth(lib, renderer,
                               "{\\bs5\\bord2\\2bs5\\shad4}BS5Shadow",
                               "{\\bs5\\bord2\\2bs5\\3bs3\\shad4}BS5Shadow",
                               1, 1, "geometric third border");
    ok &= expect_shadow_growth(lib, renderer,
                               "{\\furi1\\furipos(0,20)\\bord2\\shad4}<Base|ruby>",
                               "{\\furi1\\furipos(0,20)\\bord2\\2bs6\\shad4}<Base|ruby>",
                               2, 2, "furigana outer border");

    ShadowSig legacy_shadow, alias_shadow, zero_layer_shadow;
    ok &= render_shadow_case(lib, renderer,
                             "{\\bord3\\shad4}LegacyShadow", &legacy_shadow);
    ok &= render_shadow_case(lib, renderer,
                             "{\\bord3\\xshad4\\yshad4}LegacyShadow", &alias_shadow);
    ok &= render_shadow_case(lib, renderer,
                             "{\\bord3\\2bs0\\shad4}LegacyShadow", &zero_layer_shadow);
    if (ok && (!same_shadow(&legacy_shadow, &alias_shadow) ||
               !same_shadow(&legacy_shadow, &zero_layer_shadow))) {
        fprintf(stderr, "single-border shadow aliases changed rendering\n");
        ok = false;
    }

    const uint32_t red = 0xFF000000u;
    const uint32_t green = 0x00FF0000u;
    ok &= expect_border_filter(lib, renderer,
        "{\\blur2\\bord2\\1bc&H0000FF&\\2bs5\\2bc&H00FF00&}Inherit",
        "{\\blur2\\bord2\\1bc&H0000FF&\\2bs5\\2bc&H00FF00&\\1bblur2\\2bblur2}Inherit",
        red, true, "global blur inheritance, layer 1");
    ok &= expect_border_filter(lib, renderer,
        "{\\blur2\\bord2\\1bc&H0000FF&\\2bs5\\2bc&H00FF00&}Inherit",
        "{\\blur2\\bord2\\1bc&H0000FF&\\2bs5\\2bc&H00FF00&\\1bblur2\\2bblur2}Inherit",
        green, true, "global blur inheritance, layer 2");
    ok &= expect_border_filter(lib, renderer,
        "{\\be2\\bord2\\2bs5\\2bc&H00FF00&}InheritBE",
        "{\\be2\\bord2\\2bs5\\2bc&H00FF00&\\1bbe2\\2bbe2}InheritBE",
        green, true, "global BE inheritance");
    ok &= expect_border_filter(lib, renderer,
        "{\\blur1\\bord2\\2bs6\\2bc&H00FF00&\\2bblur1}Override",
        "{\\blur1\\bord2\\2bs6\\2bc&H00FF00&\\2bblur8}Override",
        green, false, "layer-2 Gaussian override");
    ok &= expect_border_spread(lib, renderer,
        "{\\blur1\\bord2\\2bs6\\2bc&H00FF00&\\2bblur1}Override",
        "{\\blur1\\bord2\\2bs6\\2bc&H00FF00&\\2bblur8}Override",
        green, 3, "layer-2 Gaussian radius");
    ok &= expect_border_filter(lib, renderer,
        "{\\blur6\\bord2\\1bc&H0000FF&\\2bs6\\2bc&H00FF00&}Zero",
        "{\\blur6\\bord2\\1bc&H0000FF&\\2bs6\\2bc&H00FF00&\\2bblur0}Zero",
        green, false, "explicit zero Gaussian override");
    ok &= expect_border_filter(lib, renderer,
        "{\\blur6\\bord2\\1bc&H0000FF&\\2bs6\\2bc&H00FF00&}Zero",
        "{\\blur6\\bord2\\1bc&H0000FF&\\2bs6\\2bc&H00FF00&\\2bblur0}Zero",
        red, true, "layer-1 blur independence");
    ok &= expect_border_spread(lib, renderer,
        "{\\blur6\\bord2\\1bc&H0000FF&\\2bs6\\2bc&H00FF00&\\2bblur0}Zero",
        "{\\blur6\\bord2\\1bc&H0000FF&\\2bs6\\2bc&H00FF00&}Zero",
        green, 2, "explicit zero keeps the outer border sharp");
    ok &= expect_border_filter(lib, renderer,
        "{\\blur1\\bord4\\1bc&H0000FF&\\2bs5\\2bc&H00FF00&}LayerOne",
        "{\\blur1\\bord4\\1bc&H0000FF&\\1bblur6\\2bs5\\2bc&H00FF00&}LayerOne",
        red, false, "layer-1 Gaussian override");
    ok &= expect_border_filter(lib, renderer,
        "{\\blur1\\bord4\\1bc&H0000FF&\\2bs5\\2bc&H00FF00&}LayerOne",
        "{\\blur1\\bord4\\1bc&H0000FF&\\1bblur6\\2bs5\\2bc&H00FF00&}LayerOne",
        green, true, "layer-2 blur independence");
    ok &= expect_border_filter(lib, renderer,
        "{\\blur1\\bord4\\1bc&H0000FF&}SoloLayerOne",
        "{\\blur1\\bord4\\1bc&H0000FF&\\1bblur6}SoloLayerOne",
        red, false, "single native border override");
    ok &= expect_border_filter(lib, renderer,
        "{\\blur1\\bord4\\1bc&H0000FF&}SoloLayerOne",
        "{\\blur1\\bord4\\1bc&H0000FF&\\1bblur1}SoloLayerOne",
        red, true, "equal layer-1 blur preserves legacy rendering");
    ok &= expect_border_filter(lib, renderer,
        "{\\be3\\bord4\\1bc&H0000FF&}SoloEdge",
        "{\\be3\\bord4\\1bc&H0000FF&\\1bbe0}SoloEdge",
        red, false, "single native border BE override");
    ok &= expect_border_filter(lib, renderer,
        "{\\bord3\\1bc&H0000FF&\\1bblur8\\2bs4\\2bc&H00FF00&\\2bblur1}Inverse",
        "{\\bord3\\1bc&H0000FF&\\1bblur1\\2bs4\\2bc&H00FF00&\\2bblur1}Inverse",
        green, true, "inverse blur ring geometry");
    ok &= expect_border_filter(lib, renderer,
        "{\\bord3\\1bc&H0000FF&\\1bblur1\\2bs4\\2bc&H00FF00&\\2bblur1}Opposite",
        "{\\bord3\\1bc&H0000FF&\\1bblur1\\2bs4\\2bc&H00FF00&\\2bblur8}Opposite",
        green, false, "outer soft blur");
    ok &= expect_border_filter(lib, renderer,
        "{\\be1\\bord2\\2bs5\\2bc&H00FF00&}Edge",
        "{\\be1\\bord2\\2bs5\\2bc&H00FF00&\\2bbe3}Edge",
        green, false, "layer-2 BE override");
    ok &= expect_border_filter(lib, renderer,
        "{\\be3\\bord2\\2bs5\\2bc&H00FF00&}EdgeZero",
        "{\\be3\\bord2\\2bs5\\2bc&H00FF00&\\2bbe0}EdgeZero",
        green, false, "explicit zero BE override");
    ok &= expect_border_visible_same(lib, renderer,
        "{\\bord3\\1bbe4\\2bs4\\2bc&H00FF00&\\2bbe1}EdgeInverse",
        "{\\bord3\\1bbe0\\2bs4\\2bc&H00FF00&\\2bbe1}EdgeInverse",
        green, "BE ring geometry independent of inner filter");
    ok &= expect_border_filter(lib, renderer,
        "{\\blur2\\be1\\bord2\\2bs5\\2bc&H00FF00&}Both",
        "{\\blur2\\be1\\bord2\\2bs5\\2bc&H00FF00&\\2bblur5\\2bbe3}Both",
        green, false, "combined Gaussian and BE override");
    ok &= expect_border_filter(lib, renderer,
        "{\\xbord3\\ybord2\\2bsx7\\2bsy4\\2bc&H00FF00&}AnisoFilter",
        "{\\xbord3\\ybord2\\2bsx7\\2bsy4\\2bc&H00FF00&\\2bblur5}AnisoFilter",
        green, false, "anisotropic border blur");
    ok &= expect_border_filter(lib, renderer,
        "{\\bs5\\bord2\\2bs5\\2bc&H00FF00&\\3bs4\\3bc&H0000FF&}GeoFilter",
        "{\\bs5\\bord2\\2bs5\\2bc&H00FF00&\\2bblur6\\3bs4\\3bc&H0000FF&\\3bbe2}GeoFilter",
        green, false, "geometric border blur");
    ok &= expect_border_filter(lib, renderer,
        "{\\bord2\\10bs4\\10bc&H00FF00&}Tenth",
        "{\\bord2\\10bs4\\10bc&H00FF00&\\10bblur5}Tenth",
        green, false, "tenth native border blur");
    ok &= expect_border_filter(lib, renderer,
        "{\\bord2\\2bs5\\2bc&H00FF00&}InvalidFilter",
        "{\\bord2\\2bs5\\2bc&H00FF00&\\11bblur8\\2bblurbad}InvalidFilter",
        green, true, "malformed numbered blur tags");

    RgbaSig reset_a, reset_b;
    ok &= render_rgba_case(lib, renderer,
        "{\\blur2\\bord2\\2bs5\\2bblur7}A{\\2bblur}B", &reset_a);
    ok &= render_rgba_case(lib, renderer,
        "{\\blur2\\bord2\\2bs5\\2bblur7}A{\\2bblur2}B", &reset_b);
    if (ok && !same_rgba_sig(&reset_a, &reset_b)) {
        fprintf(stderr, "bare numbered blur did not restore global inheritance\n");
        ok = false;
    }
    ok &= render_rgba_case(lib, renderer,
        "{\\be2\\bord2\\2bs5\\2bbe4}A{\\2bbe}B", &reset_a);
    ok &= render_rgba_case(lib, renderer,
        "{\\be2\\bord2\\2bs5\\2bbe4}A{\\2bbe2}B", &reset_b);
    if (ok && !same_rgba_sig(&reset_a, &reset_b)) {
        fprintf(stderr, "bare numbered BE did not restore global inheritance\n");
        ok = false;
    }
    ok &= render_rgba_case(lib, renderer,
        "{\\blur2\\bord2\\2bs5\\2bblur7}A{\\r\\blur2\\bord2\\2bs5}B", &reset_a);
    ok &= render_rgba_case(lib, renderer,
        "{\\blur2\\bord2\\2bs5\\2bblur7}A{\\r\\blur2\\bord2\\2bs5\\2bblur2}B", &reset_b);
    if (ok && !same_rgba_sig(&reset_a, &reset_b)) {
        fprintf(stderr, "\\r did not clear numbered blur override\n");
        ok = false;
    }
    ok &= render_rgba_case(lib, renderer,
        "{\\be2\\bord2\\2bs5\\2bbe7}A{\\r\\be2\\bord2\\2bs5}B", &reset_a);
    ok &= render_rgba_case(lib, renderer,
        "{\\be2\\bord2\\2bs5\\2bbe7}A{\\r\\be2\\bord2\\2bs5\\2bbe2}B", &reset_b);
    if (ok && !same_rgba_sig(&reset_a, &reset_b)) {
        fprintf(stderr, "\\r did not clear numbered BE override\n");
        ok = false;
    }
    ok &= expect_border_filter(lib, renderer,
        "{\\bord2\\2bs5\\2bc&H00FF00&\\2bblur2\\2bblur+1}Relative",
        "{\\bord2\\2bs5\\2bc&H00FF00&\\2bblur3}Relative",
        green, true, "relative numbered blur");
    ok &= expect_border_filter(lib, renderer,
        "{\\bord2\\2bs5\\2bc&H00FF00&\\2bblur2\\2bblur-0.5}Relative",
        "{\\bord2\\2bs5\\2bc&H00FF00&\\2bblur1.5}Relative",
        green, true, "negative relative numbered blur");

    BorderSig animated, midpoint;
    ok &= render_border_case_at(lib, renderer,
        "{\\bord2\\2bs5\\2bc&H00FF00&\\t(0,1000,\\2bblur8)}Animate",
        500, green, &animated);
    ok &= render_border_case_at(lib, renderer,
        "{\\bord2\\2bs5\\2bc&H00FF00&\\2bblur4}Animate",
        500, green, &midpoint);
    if (ok && !same_border(&animated, &midpoint)) {
        fprintf(stderr, "numbered blur transform did not interpolate\n");
        ok = false;
    }
    ok &= render_border_case_at(lib, renderer,
        "{\\bord2\\2bs5\\2bc&H00FF00&\\2bblur8\\t(0,1000,\\2bblur0)}Animate",
        500, green, &animated);
    if (ok && !same_border(&animated, &midpoint)) {
        fprintf(stderr, "numbered blur transform did not animate toward zero\n");
        ok = false;
    }
    ok &= render_border_case_at(lib, renderer,
        "{\\bord2\\2bs5\\2bc&H00FF00&\\t(0,1000,\\2bbe4)}Animate",
        500, green, &animated);
    ok &= render_border_case_at(lib, renderer,
        "{\\bord2\\2bs5\\2bc&H00FF00&\\2bbe2}Animate",
        500, green, &midpoint);
    if (ok && !same_border(&animated, &midpoint)) {
        fprintf(stderr, "numbered BE transform did not interpolate\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
        "{\\blur1\\bord2\\1bc&H0000FF&\\1bblur2"
        "\\2bs4\\2bc&H00FF00&\\2bblur6"
        "\\3bs3\\3bc&HFF0000&\\3bblur0}ThreeFilters", &multi);
    if (ok && (multi.outline_count < 3 ||
               !has_color(&multi, red) || !has_color(&multi, green) ||
               !has_color(&multi, 0x0000FF00u))) {
        fprintf(stderr, "three independently filtered borders disappeared\n");
        ok = false;
    }
    ok &= expect_border_filter(lib, renderer,
        "{\\bord2\\2bs6\\2bc&H00FF00&\\2ba&H80&}AlphaFilter",
        "{\\bord2\\2bs6\\2bc&H00FF00&\\2ba&H80&\\2bblur5}AlphaFilter",
        0x00FF0080u, false, "semitransparent outer blur");
    ok &= expect_border_filter(lib, renderer,
        "{\\bord2\\2bs4\\2bc&H00FF00&}<Base|ruby>",
        "{\\bord2\\2bs4\\2bc&H00FF00&\\2bblur5}<Base|ruby>",
        green, false, "furigana border blur");

    ShadowSig global_shadow, per_border_shadow;
    ok &= render_shadow_case(lib, renderer,
        "{\\blur1\\bord2\\2bs6\\3bs4\\shad5}Caster", &global_shadow);
    ok &= render_shadow_case(lib, renderer,
        "{\\blur1\\bord2\\2bs6\\2bblur8\\3bs4\\3bblur0\\shad5}Caster",
        &per_border_shadow);
    if (ok && !same_shadow(&global_shadow, &per_border_shadow)) {
        fprintf(stderr, "numbered border blur changed the shadow caster\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bord2\\3c&HFFFFFF&\\3a&H80&}Alias",
                      &legacy);
    ok &= render_case(lib, renderer,
                      "{\\1bs2\\1bc&HFFFFFF&\\1ba&H80&}Alias",
                      &numbered);
    if (ok && !same_sig(&legacy, &numbered)) {
        fprintf(stderr, "layer-1 numbered tags differ from legacy tags\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\xbord3\\ybord4}Axes",
                      &legacy);
    ok &= render_case(lib, renderer,
                      "{\\1bsx3\\1bsy4}Axes",
                      &numbered);
    if (ok && !same_sig(&legacy, &numbered)) {
        fprintf(stderr, "layer-1 numbered x/y tags differ from legacy tags\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bord2\\3c&HFFFFFF&\\3a&H00&"
                      "\\2bs6\\2bc&H000000&\\2ba&H80&}Two",
                      &multi);
    if (ok && (multi.outline_count < 2 ||
               !has_color(&multi, 0xFFFFFF00u) ||
               !has_color(&multi, 0x00000080u))) {
        fprintf(stderr, "two-border render did not expose both outline colors\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bord5\\2bs5\\2bc&H7161DF&}Equal",
                      &equal_size);
    if (ok && (equal_size.outline_count < 2 ||
               !has_color(&equal_size, 0xDF617100u))) {
        fprintf(stderr, "equal native border thickness did not render layer 2\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bord5\\2bs1\\2bc&H7161DF&}Small",
                      &small_outer);
    if (ok && (small_outer.outline_count < 2 ||
               !has_color(&small_outer, 0xDF617100u))) {
        fprintf(stderr, "small native border thickness did not render outside layer 1\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\1bs2\\1bc&HFFFFFF&\\2bs5\\2bc&H000000&"
                      "\\3bs4\\3bc&H202020&}Triple",
                      &three_layers);
    if (ok && (three_layers.outline_count < 3 ||
               !has_color(&three_layers, 0xFFFFFF00u) ||
               !has_color(&three_layers, 0x00000000u) ||
               !has_color(&three_layers, 0x20202000u))) {
        fprintf(stderr, "cumulative native border layers were not all visible\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\xbord5\\ybord3\\2bsx2\\2bsy4"
                      "\\2bc&H7161DF&}AnisoAdd",
                      &anisotropic);
    if (ok && (anisotropic.outline_count < 2 ||
               !has_color(&anisotropic, 0xDF617100u))) {
        fprintf(stderr, "x/y native border thickness did not render layer 2\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bord2\\2bs8\\3a&H80&}AllAlpha",
                      &legacy);
    ok &= render_case(lib, renderer,
                      "{\\bord2\\2bs8\\1ba&H80&\\2ba&H80&}AllAlpha",
                      &expected);
    if (ok && !same_sig(&legacy, &expected)) {
        fprintf(stderr, "\\3a did not apply to every enabled border layer\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bord2\\3a&H80&\\2bs8}DeferredAlpha",
                      &legacy);
    ok &= render_case(lib, renderer,
                      "{\\bord2\\1ba&H80&\\2ba&H80&\\2bs8}DeferredAlpha",
                      &expected);
    if (ok && !same_sig(&legacy, &expected)) {
        fprintf(stderr, "\\3a before \\2bs was not remembered by layer 2\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bord2\\2bs8\\3a&H80&\\2ba&H20&}LayerAlpha",
                      &legacy);
    ok &= render_case(lib, renderer,
                      "{\\bord2\\2bs8\\1ba&H80&\\2ba&H20&}LayerAlpha",
                      &expected);
    if (ok && !same_sig(&legacy, &expected)) {
        fprintf(stderr, "later \\2ba did not override prior \\3a\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bord2\\2bs8\\2ba&H20&\\3a&H80&}LayerAlpha",
                      &legacy);
    ok &= render_case(lib, renderer,
                      "{\\bord2\\2bs8\\1ba&H80&\\2ba&H80&}LayerAlpha",
                      &expected);
    if (ok && !same_sig(&legacy, &expected)) {
        fprintf(stderr, "later \\3a did not override prior \\2ba\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bord2\\2bs8\\1ba&H80&}LayerOneAlpha",
                      &legacy);
    if (ok && (!has_color(&legacy, 0x00000080u) ||
               !has_color(&legacy, 0x00000000u))) {
        fprintf(stderr, "\\1ba changed an extra border layer alpha\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\10bs20\\10bc&H202020&\\10ba&HAA&}Ten",
                      &multi);
    ok &= render_case(lib, renderer,
                      "{\\11bs20\\0bs20\\2bsbad\\2bcINVALID\\2baINVALID"
                      "\\2bvcINVALID\\2bvaINVALID}Invalid",
                      &invalid);
    ok &= render_case(lib, renderer,
                      "{\\1bs6\\2bs2\\2bc&H000000&}Small",
                      &invalid);
    ok &= render_case(lib, renderer,
                      "{\\bord2\\2bsx8\\2bsy4\\2bc&H000000&\\2ba&H80&}Aniso",
                      &invalid);
    ok &= render_case(lib, renderer,
                      "{\\2bs6}Before {\\r}After",
                      &invalid);

    ok &= render_case(lib, renderer,
                      "{\\bord20\\p1}m 0 0 l 200 0 200 100 0 100{\\p0}",
                      &legacy);
    ok &= render_case(lib, renderer,
                      "{\\bs5\\bord20\\p1}m 0 0 l 200 0 200 100 0 100{\\p0}",
                      &bs5);
    if (ok && same_sig(&legacy, &bs5)) {
        fprintf(stderr, "\\bs5 geometric border did not differ from legacy border\n");
        ok = false;
    }

    ok &= render_case_with_border_style(
        lib, renderer, 5,
        "{\\bord20\\p1}m 0 0 l 200 0 200 100 0 100{\\p0}",
        &style_bs5);
    if (ok && !same_sig(&bs5, &style_bs5)) {
        fprintf(stderr, "style BorderStyle=5 did not match inline \\bs5\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bord8}Amanon",
                      &legacy);
    ok &= render_case(lib, renderer,
                      "{\\bs5\\bord8}Amanon",
                      &bs5);
    if (ok && (sig_width(&bs5) > sig_width(&legacy) + 64 ||
               sig_height(&bs5) > sig_height(&legacy) + 64)) {
        fprintf(stderr, "\\bs5 text border produced excessive miter bounds\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bs5\\bord8}A{\\bs1}B",
                      &bs5);
    ok &= render_case(lib, renderer,
                      "{\\bs5\\bord8}AB",
                      &expected);
    if (ok && !same_sig(&bs5, &expected)) {
        fprintf(stderr, "later \\bs tag was not ignored\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bord8}A{\\bs5}B",
                      &bs5);
    if (ok && !same_sig(&bs5, &expected)) {
        fprintf(stderr, "later first valid \\bs5 did not apply to whole line\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bs2\\bord8}A{\\bs5}B",
                      &bs5);
    if (ok && !same_sig(&bs5, &expected)) {
        fprintf(stderr, "invalid \\bs consumed the first valid later \\bs\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bsbad}Bad",
                      &malformed);
    ok &= render_case(lib, renderer,
                      "Bad",
                      &expected);
    if (ok && !same_sig(&malformed, &expected)) {
        fprintf(stderr, "malformed \\bs changed rendering\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bs5\\bord2\\2bs8\\2bc&H000000&}GeoMulti",
                      &multi);
    if (ok && (multi.outline_count < 2 ||
               !has_color(&multi, 0x00000000u))) {
        fprintf(stderr, "\\bs5 multi-border render did not expose the extra layer\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bs5\\bord5\\2bs5\\2bc&H7161DF&}GeoAdd",
                      &multi);
    if (ok && (multi.outline_count < 2 ||
               !has_color(&multi, 0xDF617100u))) {
        fprintf(stderr, "\\bs5 equal native border thickness did not render layer 2\n");
        ok = false;
    }

    ok &= render_case_with_border_style(lib, renderer, 3,
                                        "{\\bord3\\shad4}Opaque", &legacy);
    ok &= render_case_with_border_style(lib, renderer, 3,
                                        "{\\bord3\\shad4\\1bblur6\\2bbe3}Opaque",
                                        &expected);
    if (ok && (!same_sig(&legacy, &expected) ||
               !same_coverage_bounds(&legacy, &expected))) {
        fprintf(stderr, "numbered native blur changed BorderStyle=3 box\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bs4\\boxp12\\2bbs4}BoxFilter", &legacy);
    ok &= render_case(lib, renderer,
                      "{\\bs4\\boxp12\\2bbs4\\2bblur6\\2bbe3}BoxFilter",
                      &expected);
    if (ok && (!same_sig(&legacy, &expected) ||
               !same_coverage_bounds(&legacy, &expected))) {
        fprintf(stderr, "native border blur changed box-border rendering\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bs4\\boxp12}Box",
                      &box_base);
    ok &= render_case(lib, renderer,
                      "{\\bs4\\boxp12\\bbs4\\bbc&H00FF00&\\bba&H00&}Box",
                      &box_border);
    if (ok && (same_sig(&box_base, &box_border) ||
               !has_color(&box_border, 0x00FF0000u))) {
        fprintf(stderr, "layer-1 box border did not render with explicit color\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bs4\\boxp12\\1bbs4\\1bbc&H00FF00&\\1bba&H00&"
                      "\\2bbs3\\2bbc&H0000FF&\\2bba&H00&}Box",
                      &box_multi);
    ok &= render_case(lib, renderer,
                      "{\\bs4\\boxp12\\bbs7\\bba&H00&}Box",
                      &box_reference);
    if (ok && (box_multi.outline_count < 2 ||
               !has_color(&box_multi, 0x00FF0000u) ||
               !has_color(&box_multi, 0xFF000000u) ||
               !same_coverage_bounds(&box_multi, &box_reference))) {
        fprintf(stderr, "\\bbs4\\2bbs3 did not produce cumulative "
                        "4 px + 3 px box borders\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bs4\\boxp12\\1bbs4\\1bbc&H00FF00&\\1bba&H00&"
                      "\\2bbs1\\2bbc&H0000FF&\\2bba&H00&}Box",
                      &box_small_outer);
    ok &= render_case(lib, renderer,
                      "{\\bs4\\boxp12\\bbs5\\bba&H00&}Box",
                      &box_reference);
    if (ok && (box_small_outer.outline_count < 2 ||
               !has_color(&box_small_outer, 0xFF000000u) ||
               !same_coverage_bounds(&box_small_outer, &box_reference))) {
        fprintf(stderr, "\\bbs4\\2bbs1 did not render a 1 px outer "
                        "box border\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bs4\\boxp12\\1bbs1\\1bbc&H00FF00&\\1bba&H00&"
                      "\\2bbs4\\2bbc&H0000FF&\\2bba&H00&}Box",
                      &box_large_outer);
    if (ok && (box_large_outer.outline_count < 2 ||
               !has_color(&box_large_outer, 0xFF000000u) ||
               !same_coverage_bounds(&box_large_outer, &box_reference))) {
        fprintf(stderr, "\\bbs1\\2bbs4 did not render a 4 px outer "
                        "box border\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bs4\\boxp12\\1bbs4\\1bbc&H00FF00&\\1bba&H00&"
                      "\\2bbs3\\2bbc&H0000FF&\\2bba&H00&"
                      "\\3bbs2\\3bbc&HFF0000&\\3bba&H00&}Box",
                      &box_three_layers);
    ok &= render_case(lib, renderer,
                      "{\\bs4\\boxp12\\bbs9\\bba&H00&}Box",
                      &box_reference);
    if (ok && (box_three_layers.outline_count < 3 ||
               !has_color(&box_three_layers, 0x00FF0000u) ||
               !has_color(&box_three_layers, 0xFF000000u) ||
               !has_color(&box_three_layers, 0x0000FF00u) ||
               !same_coverage_bounds(&box_three_layers, &box_reference))) {
        fprintf(stderr, "three box-border thicknesses did not accumulate "
                        "to 4 px + 3 px + 2 px\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bs4\\boxp12\\3bbs6\\3bbc&H00FF00&\\3bba&H00&}Box",
                      &box_numbered);
    if (ok && !has_color(&box_numbered, 0x00FF0000u)) {
        fprintf(stderr, "\\3bbc was not parsed as box border layer 3 color\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bs5\\bbs10\\bbc&H0000FF&}Ignore",
                      &bs_ignore);
    ok &= render_case(lib, renderer,
                      "{\\bs5}Ignore",
                      &expected);
    if (ok && !same_sig(&bs_ignore, &expected)) {
        fprintf(stderr, "box-border tags changed BorderStyle=5 rendering\n");
        ok = false;
    }

    ok &= render_case(lib, renderer,
                      "{\\bs1\\bbs10\\bbc&H0000FF&}Ignore",
                      &bs_ignore);
    ok &= render_case(lib, renderer,
                      "{\\bs1}Ignore",
                      &expected);
    if (ok && !same_sig(&bs_ignore, &expected)) {
        fprintf(stderr, "box-border tags changed BorderStyle=1 rendering\n");
        ok = false;
    }

    ok &= render_rgba_case(lib, renderer,
                           "{\\bord5\\3vc(&H0000FF&,&HFF0000&,"
                           "&H0000FF&,&HFF0000&)}Grad",
                           &rgba_legacy);
    ok &= render_rgba_case(lib, renderer,
                           "{\\1bs5\\1bvc(&H0000FF&,&HFF0000&,"
                           "&H0000FF&,&HFF0000&)}Grad",
                           &rgba_numbered);
    if (ok && !same_rgba_sig(&rgba_legacy, &rgba_numbered)) {
        fprintf(stderr, "layer-1 border gradient differs from legacy outline gradient\n");
        ok = false;
    }
    if (ok && (!rgba_numbered.needs_rgba ||
               !rgba_numbered.outline_red ||
               !rgba_numbered.outline_blue)) {
        fprintf(stderr, "layer-1 numbered gradient did not produce RGBA outline colors\n");
        ok = false;
    }

    ok &= render_rgba_case(lib, renderer,
                           "{\\bord2\\2bs8\\2bvc(&H0000FF&,&HFF0000&,"
                           "&H0000FF&,&HFF0000&)\\2bva(&H00&,&H80&,"
                           "&H00&,&H80&)}OuterGrad",
                           &rgba_multi);
    if (ok && (!rgba_multi.needs_rgba ||
               rgba_multi.outline_count < 2 ||
               !rgba_multi.outline_red ||
               !rgba_multi.outline_blue)) {
        fprintf(stderr, "extra border gradient did not render as RGBA outline\n");
        ok = false;
    }

    ok &= render_rgba_case(lib, renderer,
                           "{\\bord2\\2bs8\\2bvc(&H0000FF&,&HFF0000&,"
                           "&H0000FF&,&HFF0000&)\\2bva(&H00&,&H80&,"
                           "&H00&,&H80&)\\2bblur5}OuterGrad",
                           &rgba_numbered);
    if (ok && (!rgba_numbered.needs_rgba ||
               !rgba_numbered.outline_red ||
               !rgba_numbered.outline_blue ||
               rgba_numbered.hash == rgba_multi.hash)) {
        fprintf(stderr, "numbered blur broke or ignored border gradient\n");
        ok = false;
    }
    ok &= render_rgba_case(lib, renderer,
                           "{\\polc&HFF80C0&\\pols8\\zpol\\bord2\\2bs5"
                           "\\2bpc&H0000FF&\\2bps3\\2bsp10}Pattern",
                           &rgba_legacy);
    ok &= render_rgba_case(lib, renderer,
                           "{\\polc&HFF80C0&\\pols8\\zpol\\bord2\\2bs5"
                           "\\2bpc&H0000FF&\\2bps3\\2bsp10"
                           "\\2bblur5}Pattern",
                           &rgba_numbered);
    if (ok && (!rgba_numbered.needs_rgba ||
               rgba_numbered.outline_count < 2 ||
               rgba_numbered.hash == rgba_legacy.hash)) {
        fprintf(stderr, "numbered blur broke or ignored border pattern paint\n");
        ok = false;
    }
    ok &= render_rgba_case(lib, renderer,
                           "{\\bord2\\2bs5\\2bcyc(1,&H0000FF&,&HFF0000&)}AB",
                           &rgba_legacy);
    ok &= render_rgba_case(lib, renderer,
                           "{\\bord2\\2bs5\\2bcyc(1,&H0000FF&,&HFF0000&)"
                           "\\2bblur5}AB",
                           &rgba_numbered);
    if (ok && (!rgba_numbered.outline_red ||
               !rgba_numbered.outline_blue ||
               rgba_numbered.hash == rgba_legacy.hash)) {
        fprintf(stderr, "numbered blur broke or ignored border color cycling\n");
        ok = false;
    }

    ok &= render_rgba_case(lib, renderer,
                           "{\\2bs8\\2bvc(&H0000FF&,&HFF0000&,"
                           "&H0000FF&,&HFF0000&)\\2bc&H000000&}Flat",
                           &rgba_flat);
    if (ok && rgba_flat.needs_rgba) {
        fprintf(stderr, "flat border color did not disable extra-layer gradient\n");
        ok = false;
    }

    ass_renderer_done(renderer);
    ass_library_done(lib);
    return ok ? 0 : 1;
}
