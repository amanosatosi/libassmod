#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ass.h"

typedef struct ClipCase {
    const char *name;
    const char *text;
    bool expect_image;
    /* 0: existing parse/render smoke test, 1: visible, -1: hidden */
    int preview_visibility;
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

    if (tc->preview_visibility) {
        bool visible_legacy = false;
        for (ASS_Image *cur = img2; cur; cur = cur->next) {
            if (!cur->bitmap)
                continue;
            for (int y = 0; y < cur->h && !visible_legacy; y++)
                for (int x = 0; x < cur->w; x++)
                    if (cur->bitmap[(size_t) y * cur->stride + x]) {
                        visible_legacy = true;
                        break;
                    }
        }

        int change_rgba = 0;
        ASS_ImageRGBA *rgba = ass_render_frame_rgba(renderer, track, 0,
                                                   &change_rgba);
        bool visible_rgba = false;
        for (ASS_ImageRGBA *cur = rgba; cur; cur = cur->next) {
            if (!cur->rgba)
                continue;
            for (int y = 0; y < cur->h && !visible_rgba; y++)
                for (int x = 0; x < cur->w; x++)
                    if (cur->rgba[(size_t) y * cur->stride + 4 * x + 3]) {
                        visible_rgba = true;
                        break;
                    }
        }
        ass_free_images_rgba(rgba);

        bool expected_visible = tc->preview_visibility > 0;
        if (visible_legacy != expected_visible ||
                visible_rgba != expected_visible) {
            fprintf(stderr,
                    "%s preview: expected visible=%d, legacy=%d, rgba=%d\n",
                    tc->name, expected_visible, visible_legacy, visible_rgba);
            ok = false;
        }
    }

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
        /*
         * Aegisub polygon clip creation: a move-only path or a line between
         * two points cannot enclose any area. Normal clips must suppress
         * rendering until a third non-collinear point is added; inverse
         * clips must leave the subtitle intact while the path is empty.
         */
        {
            "vector-preview-one-point",
            "{\\pos(320,180)\\clip(m 100 100)}clip",
            true, -1,
        },
        {
            "vector-preview-two-points",
            "{\\pos(320,180)\\clip(m 100 100 l 600 500)}clip",
            true, -1,
        },
        {
            "vector-preview-two-moves",
            "{\\pos(320,180)\\clip(m 100 100 m 600 500)}clip",
            true, -1,
        },
        {
            "vector-preview-three-points",
            "{\\pos(320,180)\\clip(m 0 0 l 900 0 0 900)}clip",
            true, 1,
        },
        {
            "vector-preview-scaled-two-points",
            "{\\pos(320,180)\\clip(1,m 100 100 l 600 500)}clip",
            true, -1,
        },
        {
            "inverse-preview-one-point",
            "{\\pos(320,180)\\iclip(m 100 100)}clip",
            true, 1,
        },
        {
            "inverse-preview-two-points",
            "{\\pos(320,180)\\iclip(m 100 100 l 600 500)}clip",
            true, 1,
        },
        {
            "inverse-preview-two-moves",
            "{\\pos(320,180)\\iclip(m 100 100 m 600 500)}clip",
            true, 1,
        },
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
