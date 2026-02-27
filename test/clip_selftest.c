#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ass.h"

typedef struct ClipCase {
    const char *name;
    const char *text;
    bool expect_image;
} ClipCase;

static void msg_cb(int level, const char *fmt, va_list va, void *data)
{
    (void) level;
    (void) fmt;
    (void) va;
    (void) data;
}

static bool render_case(ASS_Library *lib, ASS_Renderer *renderer, const ClipCase *tc)
{
    char script[8192];
    int n = snprintf(
        script, sizeof(script),
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: 1920\n"
        "PlayResY: 1080\n"
        "ScaledBorderAndShadow: yes\n"
        "\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,40,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,%s\n",
        tc->text
    );
    if (n < 0 || n >= (int) sizeof(script))
        return false;

    ASS_Track *track = ass_read_memory(lib, script, strlen(script), NULL);
    if (!track)
        return false;

    int change1 = 0;
    int change2 = 0;
    ASS_Image *img1 = ass_render_frame(renderer, track, 0, &change1);
    ASS_Image *img2 = ass_render_frame(renderer, track, 0, &change2);
    (void) change1;
    (void) change2;

    bool ok = true;
    if (tc->expect_image && (!img1 || !img2))
        ok = false;

    ass_free_track(track);
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

    ass_set_storage_size(renderer, 1920, 1080);
    ass_set_frame_size(renderer, 1920, 1080);
    ass_set_fonts(renderer, NULL, "sans-serif",
                  ASS_FONTPROVIDER_AUTODETECT, NULL, 1);

    static const ClipCase cases[] = {
        {
            "rect-valid-integers",
            "{\\pos(320,180)\\clip(0,0,640,360)}clip",
            true,
        },
        {
            "rect-valid-whitespace",
            "{\\pos(320,180)\\clip(  0 ,\t0 , 640 , 360  )}clip",
            true,
        },
        {
            "rect-valid-decimals",
            "{\\pos(320,180)\\clip(0.9, 0.1, 640.999999999999, 360.1234567890123)}clip",
            true,
        },
        {
            "rect-valid-negative",
            "{\\pos(320,180)\\clip(-40.75,-20.5,640.1,360.9)}clip",
            true,
        },
        {
            "rect-right-side-1080p",
            "{\\pos(1750,540)\\clip(1200.5,300.25,1919.9,900.75)}clip",
            true,
        },
        {
            "rect-huge-overflow",
            "{\\pos(320,180)\\clip(99999999999999999999,0,640,360)}clip",
            true,
        },
        {
            "rect-missing-args",
            "{\\pos(320,180)\\clip(0,0,640)}clip",
            true,
        },
        {
            "rect-extra-comma",
            "{\\pos(320,180)\\clip(0, 0,, 640, 360)}clip",
            true,
        },
        {
            "rect-malformed-token",
            "{\\pos(320,180)\\clip(0,abc,640,360)}clip",
            true,
        },
        {
            "vector-valid",
            "{\\pos(320,180)\\clip(m 0 0 l 640 0 640 360 0 360)}clip",
            true,
        },
        {
            "vector-valid-scale",
            "{\\pos(320,180)\\clip(1,m 0 0 l 640 0 640 360 0 360)}clip",
            true,
        },
        {
            "vector-malformed",
            "{\\pos(320,180)\\clip(1,2)}clip",
            true,
        },
        {
            "vector-malformed-scale",
            "{\\pos(320,180)\\clip(1.5,m 0 0 l 640 0 640 360 0 360)}clip",
            true,
        },
        {
            "vector-huge-scale",
            "{\\pos(320,180)\\clip(1000,m 0 0 l 640 0 640 360 0 360)}clip",
            true,
        },
        {
            "vector-huge-point",
            "{\\pos(320,180)\\clip(m 0 0 l 99999999999999999999 0 10 10)}clip",
            true,
        },
        {
            "iclip-rect-valid",
            "{\\pos(320,180)\\iclip(0,0,20,20)}clip",
            true,
        },
        {
            "iclip-rect-decimals",
            "{\\pos(320,180)\\iclip(-10.5,-10.5,20.5,20.5)}clip",
            true,
        },
        {
            "iclip-rect-malformed",
            "{\\pos(320,180)\\iclip(0,,20,20)}clip",
            true,
        },
        {
            "iclip-vector-valid",
            "{\\pos(320,180)\\iclip(m 0 0 l 20 0 20 20 0 20)}clip",
            true,
        },
        {
            "iclip-vector-valid-scale",
            "{\\pos(320,180)\\iclip(1,m 0 0 l 20 0 20 20 0 20)}clip",
            true,
        },
        {
            "iclip-vector-malformed",
            "{\\pos(320,180)\\iclip(1,2)}clip",
            true,
        },
        {
            "iclip-vector-huge-scale",
            "{\\pos(320,180)\\iclip(1000,m 0 0 l 20 0 20 20 0 20)}clip",
            true,
        },
        {
            "iclip-vector-huge-point",
            "{\\pos(320,180)\\iclip(m 0 0 l 99999999999999999999 0 10 10)}clip",
            true,
        },
    };

    bool ok = true;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (!render_case(lib, renderer, &cases[i])) {
            fprintf(stderr, "clip test failed: %s\n", cases[i].name);
            ok = false;
            break;
        }
    }

    ass_renderer_done(renderer);
    ass_library_done(lib);
    return ok ? 0 : 1;
}
