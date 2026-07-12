#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ass.h"

static void msg_cb(int level, const char *fmt, va_list va, void *data)
{
    (void) level;
    (void) fmt;
    (void) va;
    (void) data;
}

int main(void)
{
    const char *script =
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: 640\n"
        "PlayResY: 360\n"
        "\n"
        "[V4+ Styles]\n"
        "Format: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,OutlineColour,BackColour,"
        "Bold,Italic,Underline,StrikeOut,ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,"
        "Alignment,MarginL,MarginR,MarginV,Encoding\n"
        "Style: Default,Arial,42,&H00FFFFFF,&H00FFFFFF,&H80000000,&H80000000,0,0,1,0,100,100,0,0,1,2,1,2,10,10,10,1\n"
        "\n"
        "[Events]\n"
        "Format: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text\n"
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,{\\5c&H0000FF&\\rnd12\\bord2}decorated rnd lifetime\n";

    ASS_Library *lib = ass_library_init();
    ASS_Renderer *renderer = lib ? ass_renderer_init(lib) : NULL;
    ASS_Track *track = NULL;
    if (!lib || !renderer)
        goto fail;
    ass_set_message_cb(lib, msg_cb, NULL);
    ass_set_frame_size(renderer, 640, 360);
    ass_set_fonts(renderer, NULL, NULL, 1, NULL, 1);
    track = ass_read_memory(lib, script, strlen(script), NULL);
    if (!track)
        goto fail;

    for (int i = 0; i < 200; i++) {
        int change = 0;
        if (!ass_render_frame(renderer, track, (i % 10) * 1000, &change))
            goto fail;
    }

    ass_free_track(track);
    ass_renderer_done(renderer);
    ass_library_done(lib);
    return 0;

fail:
    if (track)
        ass_free_track(track);
    if (renderer)
        ass_renderer_done(renderer);
    if (lib)
        ass_library_done(lib);
    fprintf(stderr, "custom-decoration rnd lifetime render failed\n");
    return 1;
}
