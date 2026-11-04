/* Public-API harness, deliberately buildable against the pre-feature branch.
 * Time only warmed rendering with the same attached fonts in both binaries. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "ass.h"

static void quiet(int level, const char *format, va_list args, void *data)
{ (void)level; (void)format; (void)args; (void)data; }

static int font(ASS_Library *lib, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int ok = !fseek(f, 0, SEEK_END);
    long n = ok ? ftell(f) : -1;
    ok = n > 0 && n < 100000000 && !fseek(f, 0, SEEK_SET);
    char *data = ok ? malloc(n) : NULL;
    ok = data && fread(data, 1, n, f) == (size_t)n;
    if (ok) ass_add_font(lib, path, data, (int)n);
    free(data); fclose(f); return ok;
}

int main(int argc, char **argv)
{
    if (argc != 5) return 1;
    int frames = atoi(argv[3]);
    const char *workload = argv[4];
    int ruby = !strncmp(workload, "furi-", 5);
    int metadata = !strcmp(workload, "furi-metadata");
    if (frames <= 0 || frames > 1000000) return 1;
    ASS_Library *lib = ass_library_init();
    if (!lib) return 1;
    ass_set_message_cb(lib, quiet, NULL);
    if (!font(lib, argv[1]) || !font(lib, argv[2])) { ass_library_done(lib); return 1; }
    ASS_Renderer *renderer = ass_renderer_init(lib);
    if (!renderer) { ass_library_done(lib); return 1; }
    ass_set_frame_size(renderer, 1920, 1080);
    ass_set_storage_size(renderer, 1920, 1080);
    ass_set_fonts(renderer, NULL, "Noto Sans Myanmar", ASS_FONTPROVIDER_NONE, NULL, 1);
    char script[16384];
    int length = snprintf(script, sizeof(script),
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 1920\nPlayResY: 1080\n"
        "ScaledBorderAndShadow: yes\n[V4+ Styles]\n"
        "Format: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,OutlineColour,BackColour,"
        "Bold,Italic,Underline,StrikeOut,ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,"
        "Alignment,MarginL,MarginR,MarginV,Encoding\n"
        "Style: Default,%s,28,&H00FFFFFF,&H00FFFFFF,&H00000000,&H80000000,"
        "0,0,0,0,100,100,0,0,1,2,0,2,20,20,20,1\n"
        "[Events]\nFormat: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text\n%s",
        ruby ? "Noto Sans Myanmar" : "Pixel Operator Mono",
        metadata ? "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\furis70}\n" : "");
    for (int i = 0; i < 12; i++) {
        const char *tags = ruby ? (metadata ? "" : "{\\furis70}") :
            !strcmp(workload, "ordinary-overrides") ? "{\\bord2\\blur0.5\\1c&H40A0FF&}" : "";
        length += snprintf(script + length, sizeof(script) - length,
            "Dialogue: 0,0:00:00.00,9:00:00.00,Default,,0,0,0,,%s%s\n", tags,
            ruby ? "<က|စူဇူကီ> <က||ဆာတို့>" : "Ordinary ASS subtitle text with no Mangetsu metadata.");
    }
    ASS_Track *track = ass_read_memory(lib, script, length, NULL);
    if (!track) { ass_renderer_done(renderer); ass_library_done(lib); return 1; }
    for (int i = 0; i < 100; i++) ass_render_frame(renderer, track, i, NULL);
    uint64_t checksum = UINT64_C(1469598103934665603);
    unsigned long images = 0;
    clock_t start = clock();
    for (int i = 0; i < frames; i++) {
        ASS_Image *im = ass_render_frame(renderer, track, i, NULL);
        for (; im; im = im->next) {
            uint32_t geometry[] = {im->dst_x, im->dst_y, im->w, im->h, im->color};
            for (int j = 0; j < 5; j++)
                checksum = (checksum ^ geometry[j]) * UINT64_C(1099511628211);
            images++;
        }
    }
    double seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
    printf("{\"workload\":\"%s\",\"frames\":%d,\"cpu_seconds\":%.9f,"
           "\"checksum\":\"%016" PRIx64 "\",\"images\":%lu}\n",
           workload, frames, seconds, checksum, images);
    ass_free_track(track); ass_renderer_done(renderer); ass_library_done(lib);
    return seconds > 0 && images > 0 ? 0 : 1;
}
