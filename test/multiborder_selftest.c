#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ass.h"

typedef struct {
    int count;
    int outline_count;
    uint64_t coverage;
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

static bool render_case(ASS_Library *lib, ASS_Renderer *renderer,
                        const char *text, RenderSig *sig)
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
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,{\\pos(320,180)}%s\n",
        text);
    if (n < 0 || n >= (int) sizeof(script))
        return false;

    ASS_Track *track = ass_read_memory(lib, script, strlen(script), NULL);
    if (!track)
        return false;

    int change = 0;
    ASS_Image *img = ass_render_frame(renderer, track, 0, &change);
    (void) change;

    memset(sig, 0, sizeof(*sig));
    for (ASS_Image *cur = img; cur; cur = cur->next) {
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

static bool same_sig(const RenderSig *a, const RenderSig *b)
{
    return a->count == b->count &&
           a->outline_count == b->outline_count &&
           a->coverage == b->coverage &&
           a->n_colors == b->n_colors &&
           !memcmp(a->colors, b->colors, sizeof(a->colors));
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

    RenderSig legacy, numbered, multi, invalid;
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
                      "{\\10bs20\\10bc&H202020&\\10ba&HAA&}Ten",
                      &multi);
    ok &= render_case(lib, renderer,
                      "{\\11bs20\\0bs20\\2bsbad\\2bcINVALID\\2baINVALID}Invalid",
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

    ass_renderer_done(renderer);
    ass_library_done(lib);
    return ok ? 0 : 1;
}
