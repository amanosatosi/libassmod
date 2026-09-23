#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ass.h"

static void quiet(int level, const char *fmt, va_list args, void *data)
{
    (void) level; (void) fmt; (void) args; (void) data;
}

static ASS_Track *track_for(ASS_Library *lib, const char *text)
{
    char script[8192];
    int n = snprintf(script, sizeof(script),
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,48,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,"
        "0,0,0,0,100,100,0,0,1,3,0,5,10,10,10,1\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,%s\n", text);
    if (n < 0 || n >= (int) sizeof(script))
        return NULL;
    return ass_read_memory(lib, script, n, NULL);
}

typedef struct {
    uint32_t face[128], outline[128];
    int n_face, n_outline;
} Colors;

static bool render_colors(ASS_Library *lib, ASS_Renderer *renderer,
                          const char *text, Colors *result)
{
    ASS_Track *track = track_for(lib, text);
    if (!track) return false;
    memset(result, 0, sizeof(*result));
    int change = 0;
    ASS_Image *images = ass_render_frame(renderer, track, 0, &change);
    for (ASS_Image *img = images; img; img = img->next) {
        uint32_t rgb = img->color & 0xFFFFFF00u;
        if (img->type == IMAGE_TYPE_CHARACTER && result->n_face < 128)
            result->face[result->n_face++] = rgb;
        if (img->type == IMAGE_TYPE_OUTLINE && result->n_outline < 128)
            result->outline[result->n_outline++] = rgb;
    }
    ass_free_track(track);
    return result->n_face > 0;
}

static bool begins(const uint32_t *actual, int length,
                   const uint32_t *expected, int count)
{
    return length >= count && !memcmp(actual, expected,
                                      count * sizeof(uint32_t));
}

static uint64_t rgba_hash(ASS_Library *lib, ASS_Renderer *renderer,
                          const char *text, bool *needs_rgba)
{
    ASS_Track *track = track_for(lib, text);
    if (!track) return 0;
    int change = 0;
    ASS_ImageRGBA *images = ass_render_frame_rgba(renderer, track, 0, &change);
    *needs_rgba = ass_frame_needs_rgba(renderer) != 0;
    uint64_t hash = 1469598103934665603ULL;
    for (ASS_ImageRGBA *img = images; img; img = img->next) {
        hash ^= img->type; hash *= 1099511628211ULL;
        for (int y = 0; y < img->h; y++) {
            const uint8_t *row = img->rgba + y * img->stride;
            for (int x = 0; x < img->w * 4; x++) {
                hash ^= row[x]; hash *= 1099511628211ULL;
            }
        }
    }
    ass_free_images_rgba(images);
    ass_free_track(track);
    return hash;
}

int main(void)
{
    ASS_Library *lib = ass_library_init();
    if (!lib) return 1;
    ass_set_message_cb(lib, quiet, NULL);
    ASS_Renderer *renderer = ass_renderer_init(lib);
    if (!renderer) { ass_library_done(lib); return 1; }
    ass_set_frame_size(renderer, 640, 360);
    ass_set_fonts(renderer, NULL, "sans-serif", ASS_FONTPROVIDER_AUTODETECT,
                  NULL, 1);

    bool ok = true, rgba = false;
    Colors colors, reference;
    const uint32_t rgb[] = {0xFF000000u, 0x00FF0000u, 0x0000FF00u};
    ok &= render_colors(lib, renderer,
        "{\\cyc(1,&H0000FF&,&H00FF00&,&HFF0000&)}ABCDEF", &colors);
    ok &= begins(colors.face, colors.n_face, rgb, 3);
    ok &= render_colors(lib, renderer,
        "{\\cyc(2,&H0000FF&,&H00FF00&,&HFF0000&)}ABC", &colors);
    ok &= begins(colors.face, colors.n_face, rgb, 3);
    ok &= render_colors(lib, renderer,
        "{\\1cyc(1,&H0000FF&,&H00FF00&)}AB", &colors);
    ok &= begins(colors.face, colors.n_face, rgb, 2);
    ok &= render_colors(lib, renderer,
        "{\\cyc(1,&H0000FF&,&H00FF00&)}A B\\NC", &colors);
    ok &= begins(colors.face, colors.n_face,
                 (uint32_t[]) {rgb[0], rgb[1], rgb[0]}, 3);

    /* A later base paint source terminates only its own layer's cycle. */
    const uint32_t white = 0xFFFFFF00u;
    const uint32_t black = 0x00000000u;
    ok &= render_colors(lib, renderer,
        "{\\cyc(1,&H0000FF&,&H00FF00&,&HFF0000&)}ABC"
        "{\\c&HFFFFFF&}DEF", &colors);
    ok &= begins(colors.face, colors.n_face,
                 (uint32_t[]) {rgb[0], rgb[1], rgb[2], white}, 4);
    for (int i = 3; i < colors.n_face; i++)
        ok &= colors.face[i] == white;
    ok &= render_colors(lib, renderer,
        "{\\c&HFFFFFF&}ABC"
        "{\\cyc(1,&H0000FF&,&H00FF00&,&HFF0000&)}DEF", &colors);
    ok &= colors.n_face >= 4 && colors.face[0] == white &&
          begins(colors.face + 1, colors.n_face - 1, rgb, 3);
    ok &= render_colors(lib, renderer,
        "{\\cyc(1,&H0000FF&,&H00FF00&,&HFF0000&)}AB"
        "{\\c&HFFFFFF&}CD"
        "{\\cyc(1,&H0000FF&,&H00FF00&,&HFF0000&)}EF", &colors);
    ok &= begins(colors.face, colors.n_face,
                 (uint32_t[]) {rgb[0], rgb[1], white, rgb[0], rgb[1]}, 5);
    ok &= render_colors(lib, renderer,
        "{\\cyc(1,&H0000FF&,&H00FF00&,&HFF0000&)"
        "\\3cyc(1,&HFFFFFF&,&H000000&)}ABC"
        "{\\c&HFFFFFF&}DEF", &colors);
    ok &= begins(colors.face, colors.n_face,
                 (uint32_t[]) {rgb[0], rgb[1], rgb[2], white, white, white}, 6);
    ok &= begins(colors.outline, colors.n_outline,
                 (uint32_t[]) {white, black, white, black, white, black}, 6);
    ok &= render_colors(lib, renderer,
        "{\\cyc(1,&H0000FF&,&H00FF00&,&HFF0000&)"
        "\\3cyc(1,&HFFFFFF&,&H000000&)}ABC"
        "{\\3c&HFF0000&}DEF", &colors);
    ok &= begins(colors.face, colors.n_face,
                 (uint32_t[]) {rgb[0], rgb[1], rgb[2], rgb[0], rgb[1], rgb[2]}, 6);
    ok &= begins(colors.outline, colors.n_outline,
                 (uint32_t[]) {white, black, white, rgb[2], rgb[2], rgb[2]}, 6);
    ok &= render_colors(lib, renderer,
        "{\\cyc(1,&H0000FF&,&H00FF00&)\\3cyc(2,&HFFFFFF&,&H000000&)}AB",
        &colors);
    ok &= colors.n_outline >= 2 && colors.outline[0] != colors.outline[1];
    ok &= render_colors(lib, renderer,
        "{\\bord2\\2bs5\\2bcyc(1,&H0000FF&,&H00FF00&)}AB", &colors);
    bool border_red = false, border_green = false;
    for (int i = 0; i < colors.n_outline; i++) {
        border_red |= colors.outline[i] == rgb[0];
        border_green |= colors.outline[i] == rgb[1];
    }
    ok &= border_red && border_green;
    ok &= render_colors(lib, renderer,
        "{\\cyc(1,&H0000FF&,&H00FF00&)\\bord2\\2bs5"
        "\\2bcyc(1,&H0000FF&,&H00FF00&)}AB"
        "{\\2bc&HFFFFFF&}CD", &colors);
    ok &= begins(colors.face, colors.n_face,
                 (uint32_t[]) {rgb[0], rgb[1], rgb[0], rgb[1]}, 4);
    bool outer_white = false;
    for (int i = 0; i < colors.n_outline; i++)
        outer_white |= colors.outline[i] == white;
    ok &= outer_white;
    /* Each Myanmar orthographic syllable may emit multiple glyphs, but its
     * marks must retain the same color until the next syllable. */
    ok &= render_colors(lib, renderer,
        "{\\cyc(1,&H0000FF&,&H00FF00&,&HFF0000&)}အေ အော ကြှေ", &colors);
    int changes = 0;
    for (int i = 1; i < colors.n_face; i++)
        changes += colors.face[i] != colors.face[i - 1];
    ok &= changes == 2 && colors.face[0] == rgb[0] &&
          colors.face[colors.n_face - 1] == rgb[2];
    ok &= render_colors(lib, renderer,
        "{\\cyc(1,&H0000FF&,&H00FF00&)}<漢|かん><字|じ>", &colors);
    /* Base and reading ink may be interleaved by the compositor, but every
     * face tile must use one of the two associated base colors. */
    bool ruby_red = false, ruby_green = false;
    for (int i = 0; i < colors.n_face; i++) {
        ruby_red |= colors.face[i] == rgb[0];
        ruby_green |= colors.face[i] == rgb[1];
        ok &= colors.face[i] == rgb[0] || colors.face[i] == rgb[1];
    }
    ok &= ruby_red && ruby_green;

    ok &= render_colors(lib, renderer, "Plain", &reference);
    ok &= render_colors(lib, renderer, "{\\cyc(9,bad)}Plain", &colors);
    ok &= reference.n_face == colors.n_face &&
          !memcmp(reference.face, colors.face,
                  reference.n_face * sizeof(uint32_t));
    uint64_t plain = rgba_hash(lib, renderer, "{\\c&HFFFFFF&}Polka", &rgba);
    ok &= !rgba;
    uint64_t dotted = rgba_hash(lib, renderer,
        "{\\c&HFFFFFF&\\polc&HFF80C0&\\pols8\\polsp20}Polka", &rgba);
    ok &= rgba && dotted != plain;
    uint64_t disabled = rgba_hash(lib, renderer,
        "{\\c&HFFFFFF&\\polc&HFF80C0&\\pols0}Polka", &rgba);
    ok &= disabled == plain;
    uint64_t malformed = rgba_hash(lib, renderer,
        "{\\c&HFFFFFF&\\polc&HZZZZZZ&\\pols8}Polka", &rgba);
    ok &= malformed == plain;
    uint64_t cycle_dots = rgba_hash(lib, renderer,
        "{\\cyc(1,&H0000FF&,&H00FF00&)\\polc&HFF80C0&\\pols8}Polka",
        &rgba);
    ok &= rgba && cycle_dots != dotted;
    const char *gradient =
        "{\\grd(0,&H0000FF&,&H00FF00&)}TEXT";
    const char *cycle_then_gradient =
        "{\\cyc(1,&H0000FF&,&H00FF00&)"
        "\\grd(0,&H0000FF&,&H00FF00&)}TEXT";
    const char *cycle_only =
        "{\\cyc(1,&H0000FF&,&H00FF00&)}TEXT";
    const char *gradient_then_cycle =
        "{\\1grd(0,&H0000FF&,&H00FF00&)"
        "\\cyc(1,&H0000FF&,&H00FF00&)}TEXT";
    uint64_t gradient_hash = rgba_hash(lib, renderer, gradient, &rgba);
    ok &= rgba;
    ok &= rgba_hash(lib, renderer, cycle_then_gradient, &rgba) == gradient_hash;
    uint64_t cycle_hash = rgba_hash(lib, renderer, cycle_only, &rgba);
    ok &= rgba_hash(lib, renderer, gradient_then_cycle, &rgba) == cycle_hash;
    ok &= render_colors(lib, renderer,
        "{\\cyc(1,&H0000FF&,&H00FF00&)\\1grd()}AB", &colors);
    ok &= colors.n_face > 0 && colors.face[0] == white;
    const char *vector =
        "{\\vc(&H0000FF&,&H00FF00&,&HFF0000&,&HFFFFFF&)}TEXT";
    const char *cycle_then_vector =
        "{\\cyc(1,&H0000FF&,&H00FF00&)"
        "\\vc(&H0000FF&,&H00FF00&,&HFF0000&,&HFFFFFF&)}TEXT";
    uint64_t vector_hash = rgba_hash(lib, renderer, vector, &rgba);
    ok &= rgba;
    ok &= rgba_hash(lib, renderer, cycle_then_vector, &rgba) == vector_hash;
    uint64_t outline_plain = rgba_hash(lib, renderer,
        "{\\bord5}Outline", &rgba);
    uint64_t outline_dots = rgba_hash(lib, renderer,
        "{\\bord5\\3polc&HFFFFFF&\\3pols3\\3polsp8}Outline", &rgba);
    ok &= rgba && outline_dots != outline_plain;
    uint64_t animated_dots = rgba_hash(lib, renderer,
        "{\\polc&H0000FF&\\pols3\\t(0,1000,\\polc&HFF0000&\\pols9)}Polka",
        &rgba);
    ok &= rgba && animated_dots != dotted;
    uint64_t ruby_plain = rgba_hash(lib, renderer,
        "{\\polc&HFF80C0&\\pols8}<漢字|かんじ>", &rgba);
    uint64_t ruby_zpol = rgba_hash(lib, renderer,
        "{\\polc&HFF80C0&\\pols8\\zpol}<漢字|かんじ>", &rgba);
    ok &= ruby_zpol != ruby_plain;
    uint64_t border_zpol = rgba_hash(lib, renderer,
        "{\\polc&HFF80C0&\\pols8\\zpol\\2bs6}TEXT", &rgba);
    uint64_t border_override = rgba_hash(lib, renderer,
        "{\\polc&HFF80C0&\\pols8\\zpol\\2bs6"
        "\\2bpc&HFFFFFF&\\2bps3\\2bsp10}TEXT", &rgba);
    ok &= border_override != border_zpol;

    ass_renderer_done(renderer);
    ass_library_done(lib);
    if (!ok) fprintf(stderr, "pattern paint regression\n");
    return ok ? 0 : 1;
}
