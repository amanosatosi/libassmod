#include <stdio.h>
#include <string.h>
#include "ass.h"

static void msg_cb(int level, const char *fmt, va_list va, void *data)
{
    (void) level;
    (void) data;
    vfprintf(stdout, fmt, va);
    fputc('\n', stdout);
}

int main(void)
{
    const char *script =
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
        "Style: Default,Arial,40,&H00FFFFFF,&H00FFFFFF,&H00000000,&H64000000,0,0,0,0,100,100,0,0,1,2,2,2,20,20,20,1\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,{\\rndx10}rndx10\n"
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,{\\rndy10}rndy10\n"
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,{\\rnd10}rnd10\n";

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
    ass_set_frame_size(renderer, 640, 360);
    ass_set_fonts(renderer, NULL, NULL, 1, NULL, 1);

    ASS_Track *track = ass_read_memory(lib, script, strlen(script), NULL);
    if (!track) {
        fprintf(stderr, "failed to load script\n");
        ass_renderer_done(renderer);
        ass_library_done(lib);
        return 1;
    }

    int change = 0;
    ASS_Image *img = ass_render_frame(renderer, track, 0, &change);
    (void) img;

    ass_free_track(track);
    ass_renderer_done(renderer);
    ass_library_done(lib);
    return 0;
}
