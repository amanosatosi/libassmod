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
    RgbaSig rgba_legacy, rgba_numbered, rgba_multi, rgba_flat;
    bool ok = true;

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
                      "{\\bs4\\boxp12\\1bbs3\\1bbc&H00FF00&\\1bba&H00&"
                      "\\2bbs8\\2bbc&H0000FF&\\2bba&H00&}Box",
                      &box_multi);
    if (ok && (!has_color(&box_multi, 0x00FF0000u) ||
               !has_color(&box_multi, 0xFF000000u))) {
        fprintf(stderr, "two-layer box border did not expose both colors\n");
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
