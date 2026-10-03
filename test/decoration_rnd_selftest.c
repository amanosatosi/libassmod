/* Exercise owned rnd bitmaps, including stack-local custom decorations. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ass.h"

static void quiet(int level, const char *fmt, va_list va, void *data)
{
    (void) level; (void) fmt; (void) va; (void) data;
}

static const char *fixtures[] = {
    "{\\rnd30\\rnds123\\t(0,1000,\\rnd100\\rndsabc)}randomized text",
    "{\\rnd30\\bord3\\shad4\\frx20\\t(0,1000,\\rndz80)}border and shadow",
    "{\\rnd30\\bord2\\2bs4\\3bs3\\shad4\\t(0,1000,\\rndx100)}multi border",
    "{\\u1\\s1\\5c&H0000FF&\\6c&H00FF00&\\rnd30\\bord2\\2bs3\\t(0,1000,\\rnds123)}decorated rnd lifetime",
    "{\\u1\\5c&H0000FF&\\rnd30\\bord2\\1vc(&H0000FF&,&H00FF00&,&HFF0000&,&HFFFFFF&)\\t(0,1000,\\rnd60)}gradient decoration",
    "{\\furi1\\furipos(0,20)\\rnd30\\bord2\\2bs3\\t(0,1000,\\rnds42)}<Base|ruby>",
    "{\\an7\\pos(120,90)\\p1\\rnd30\\bord3\\2bs2\\t(0,1000,\\rndy100)}m 0 0 l 180 0 180 100 90 145 0 100",
    "{\\rnd30\\rnds1\\bord2\\distort(1,-0.1,1.1,1.1,-0.1,1,0.1,0.05)\\t(0,1000,\\rnd100)}first line\\Nsecond longer line",
    "{\\rnd30}active{\\rnd-1}inactive{\\rndx100\\rndy\\rndz}X{\\r}plain",
    "{\\rnd-10\\t(0,1000,\\rnd30)\\bord2}A\xCC\x81\xCC\xA3" " linked cluster"
};

static ASS_Track *read_track(ASS_Library *lib, const char *text)
{
    char script[4096];
    int n = snprintf(script,sizeof(script),
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n"
        "ScaledBorderAndShadow: yes\n"
        "[V4+ Styles]\n"
        "Format: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,OutlineColour,BackColour,"
        "Bold,Italic,Underline,StrikeOut,ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,"
        "Alignment,MarginL,MarginR,MarginV,Encoding\n"
        "Style: Default,Arial,32,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,0,0,5,10,10,10,1\n"
        "[Events]\nFormat: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text\n"
        "Dialogue: 0,0:00:00.00,0:00:02.00,Default,,0,0,0,,%s\n",text);
    if (n < 0 || n >= (int) sizeof(script)) return NULL;
    return ass_read_memory(lib,script,n,NULL);
}

static uint64_t hash_rgba(const ASS_ImageRGBA *images)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const ASS_ImageRGBA *img = images; img; img = img->next) {
        const int values[] = {img->w,img->h,img->dst_x,img->dst_y};
        for (int i = 0; i < 4; i++) {
            hash ^= (uint32_t) values[i]; hash *= UINT64_C(1099511628211);
        }
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w * 4; x++) {
                hash ^= img->rgba[y * img->stride + x];
                hash *= UINT64_C(1099511628211);
            }
    }
    return hash;
}

int main(int argc, char **argv)
{
    int extended = argc == 2 && !strcmp(argv[1],"--extended");
    int cycles = extended ? 12 : 2;
    int frames = extended ? 80 : 16;
    for (int cycle = 0; cycle < cycles; cycle++) {
        ASS_Library *lib = ass_library_init();
        ASS_Renderer *renderer = lib ? ass_renderer_init(lib) : NULL;
        ASS_Track *track = NULL;
        ASS_ImageRGBA *held = NULL;
        uint64_t held_hash = 0;
        if (!lib || !renderer) goto fail;
        ass_set_message_cb(lib,quiet,NULL);
        ass_set_frame_size(renderer,640,360);
        ass_set_fonts(renderer,getenv("RND_TEST_FONT"),NULL,1,NULL,1);
        for (unsigned f = 0; f < sizeof(fixtures) / sizeof(fixtures[0]); f++) {
            track = read_track(lib,fixtures[f]);
            if (!track) goto fail;
            int change;
            ASS_ImageRGBA *reference = ass_render_frame_rgba(renderer,track,500,&change);
            if (!reference) goto fail;
            uint64_t expected = hash_rgba(reference);
            ass_free_images_rgba(reference);
            for (int frame = 0; frame < frames; frame++) {
                long long time = (frame * 733) % 1900;
                if (!ass_render_frame(renderer,track,time,&change)) goto fail;
                held = ass_render_frame_rgba(renderer,track,time,&change);
                if (!held) goto fail;
                if (!hash_rgba(held)) goto fail; // read every byte under sanitizers
                ass_free_images_rgba(held);
                held = ass_render_frame_rgba(renderer,track,500,&change);
                if (!held || hash_rgba(held) != expected) goto fail;
                ass_free_images_rgba(held);
                held = NULL;
            }
            if (f + 1 == sizeof(fixtures) / sizeof(fixtures[0])) {
                held = ass_render_frame_rgba(renderer,track,500,&change);
                if (!held) goto fail;
                held_hash = hash_rgba(held);
            }
            ass_free_track(track);
            track = NULL;
        }
        ass_renderer_done(renderer);
        renderer = NULL;
        ass_library_done(lib);
        lib = NULL;
        /* Returned RGBA images must remain valid after all source outlines,
         * caches, stack-local decoration glyphs and renderer are destroyed. */
        if (hash_rgba(held) != held_hash) goto fail;
        ass_free_images_rgba(held);
        continue;
fail:
        ass_free_images_rgba(held);
        if (track) ass_free_track(track);
        if (renderer) ass_renderer_done(renderer);
        if (lib) ass_library_done(lib);
        fprintf(stderr,"rnd lifetime/stress failed in cycle %d\n",cycle);
        return 1;
    }
    return 0;
}
