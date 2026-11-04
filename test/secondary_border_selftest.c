/* Pixel contracts for numbered waiting border paint in both public outputs. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ass.h"

enum { W = 640, H = 480 };
typedef struct { uint32_t *p; } Frame;
#define META(style, tags) "Comment: 0,0:00:00.00,0:00:00.00," style ",,0,0,0,mangetsu-style,{" tags "}\n"
#define PREFIX "\\an7\\pos(150,160)\\bord0"
#define ACTIVE "&HFF0000&"
#define WAITING "&H0000FF&"
static int failures;

static bool attach(ASS_Library *lib, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = !fseek(f, 0, SEEK_END);
    long n = ok ? ftell(f) : -1;
    ok = n > 0 && n < 100000000 && !fseek(f, 0, SEEK_SET);
    char *data = ok ? malloc(n) : NULL;
    ok = data && fread(data, 1, n, f) == (size_t)n;
    if (ok) ass_add_font(lib, path, data, n);
    free(data); fclose(f); return ok;
}

static bool render(ASS_Library *lib, ASS_Renderer *r, const char *metadata,
                    const char *text, long long time, bool rgba, Frame *out)
{
    out->p = calloc(W * H * 4, sizeof(*out->p));
    if (!out->p) return false;
    char data[16384];
    int n = snprintf(data, sizeof(data),
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 480\n"
        "ScaledBorderAndShadow: yes\n[V4+ Styles]\n"
        "Format: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,OutlineColour,BackColour,"
        "Bold,Italic,Underline,StrikeOut,ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,"
        "Alignment,MarginL,MarginR,MarginV,Encoding\n"
        "Style: Default,Karaoke Border,48,&HFFFFFFFF,&HFFFFFFFF,&H00000000,&HFF000000,0,0,0,0,100,100,0,0,1,0,0,7,20,20,20,1\n"
        "Style: Other,Karaoke Border,48,&HFFFFFFFF,&HFFFFFFFF,&H00000000,&HFF000000,0,0,0,0,100,100,0,0,1,0,0,7,20,20,20,1\n"
        "[Events]\nFormat: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text\n"
        "%sDialogue: 0,0:00:00.00,0:00:10.00,Default,Nene,0,0,0,,%s\n", metadata, text);
    ASS_Track *track = n > 0 && n < sizeof(data) ? ass_read_memory(lib, data, n, NULL) : NULL;
    if (!track) return false;
    if (rgba) {
        ASS_ImageRGBA *images = ass_render_frame_rgba(r, track, time, NULL);
        for (ASS_ImageRGBA *im = images; im; im = im->next)
            for (int y = 0; y < im->h; y++) for (int x = 0; x < im->w; x++) {
                int xx = x + im->dst_x, yy = y + im->dst_y;
                if (xx < 0 || xx >= W || yy < 0 || yy >= H) continue;
                const uint8_t *pixel = im->rgba + y * im->stride + 4 * x;
                for (int c = 0; c < 4; c++) out->p[4 * (yy * W + xx) + c] += pixel[c];
            }
        ass_free_images_rgba(images);
    } else {
        ASS_Image *images = ass_render_frame(r, track, time, NULL);
        for (ASS_Image *im = images; im; im = im->next)
            for (int y = 0; y < im->h; y++) for (int x = 0; x < im->w; x++) {
                int xx = x + im->dst_x, yy = y + im->dst_y;
                if (xx < 0 || xx >= W || yy < 0 || yy >= H) continue;
                unsigned a = im->bitmap[y * im->stride + x] * (255 - (im->color & 255)) / 255;
                uint32_t *pixel = out->p + 4 * (yy * W + xx);
                for (int c = 0; c < 3; c++) pixel[c] += a * ((im->color >> (24 - 8 * c)) & 255) / 255;
                pixel[3] += a;
            }
    }
    ass_free_track(track);
    return true;
}

static void pair(ASS_Library *lib, ASS_Renderer *r, const char *meta,
                 const char *actual, const char *expected, long long time,
                 bool rgba, const char *label)
{
    Frame a = {0}, b = {0};
    bool ok = render(lib, r, meta, actual, time, rgba, &a) &&
              render(lib, r, "", expected, time, rgba, &b) &&
              !memcmp(a.p, b.p, W * H * 4 * sizeof(*a.p));
    if (!ok) { fprintf(stderr, "%s (%s, %lldms)\n", label, rgba ? "RGBA" : "legacy", time); failures++; }
    free(a.p); free(b.p);
}

/* Ring masks differ from layer 1's full outline. Compare their actual paint
 * at shared visible pixels against the established \3sc karaoke frontier.
 * Require both waiting and active pixels so parser-only support cannot pass. */
static void frontier(ASS_Library *lib, ASS_Renderer *r, int layer,
                     const char *layout, const char *text, long long time, bool rgba)
{
    char actual[2048], reference[2048];
    snprintf(actual, sizeof(actual), "{" PREFIX "%s\\%dbs6\\%dbc" ACTIVE "\\%dbsc" WAITING "}%s",
             layout, layer, layer, layer, text);
    snprintf(reference, sizeof(reference), "{" PREFIX "%s\\bord6\\3c" ACTIVE "\\3sc" WAITING "}%s", layout, text);
    Frame a = {0}, b = {0};
    bool ok = render(lib, r, "", actual, time, rgba, &a) && render(lib, r, "", reference, time, rgba, &b);
    unsigned red = 0, blue = 0;
    if (ok) for (int i = 0; i < W * H; i++) {
        uint32_t *x = a.p + 4 * i, *y = b.p + 4 * i;
        if (!x[3] || !y[3] || (y[0] && y[2])) continue;
        if (y[0] == y[3] && !y[2]) { red++; if (x[0] != x[3] || x[2]) ok = false; }
        if (y[2] == y[3] && !y[0]) { blue++; if (x[2] != x[3] || x[0]) ok = false; }
    }
    if (!ok || !red || !blue) {
        fprintf(stderr, "layer %d frontier (%s, %lldms, %s): red=%u blue=%u\n", layer, rgba ? "RGBA" : "legacy", time, text, red, blue);
        failures++;
    }
    free(a.p); free(b.p);
}

int main(int argc, char **argv)
{
    if (argc != 2) return 1;
    ASS_Library *lib = ass_library_init();
    if (!lib) return 1;
    if (!attach(lib, argv[1])) { ass_library_done(lib); return 1; }
    ASS_Renderer *r = ass_renderer_init(lib);
    if (!r) { ass_library_done(lib); return 1; }
    ass_set_storage_size(r, W, H); ass_set_frame_size(r, W, H);
    ass_set_fonts(r, NULL, "Karaoke Border", ASS_FONTPROVIDER_NONE, NULL, 1);
    for (int rgba = 0; rgba < 2; rgba++) {
        for (int layer = 1; layer <= 10; layer++) {
            char actual[1024], expected[1024], meta[1024];
            snprintf(actual, sizeof(actual), "{" PREFIX "\\%dbs6\\%dbc" ACTIVE "\\%dbsc" WAITING "\\kt50\\k100}WW", layer, layer, layer);
            snprintf(expected, sizeof(expected), "{" PREFIX "\\%dbs6\\%dbc" WAITING "\\kt50\\k100}WW", layer, layer);
            pair(lib, r, "", actual, expected, 499, rgba, "waiting paint");
            snprintf(expected, sizeof(expected), "{" PREFIX "\\%dbs6\\%dbc" ACTIVE "\\kt50\\k100}WW", layer, layer);
            pair(lib, r, "", actual, expected, 500, rgba, "active paint preserved");
            snprintf(actual, sizeof(actual), "{" PREFIX "\\%dbs6\\%dbc" ACTIVE "\\%dbsc" WAITING "\\%dbsc\\kt50\\k100}WW", layer, layer, layer, layer);
            pair(lib, r, "", actual, expected, 499, rgba, "unset parameterless waiting paint");
            snprintf(meta, sizeof(meta), META("Default", "\\%dbs6\\%dbc" ACTIVE "\\%dbsc" WAITING), layer, layer, layer);
            snprintf(actual, sizeof(actual), "{\\an7\\pos(150,160)\\%dbsc&H00FFFF&\\%dbsc\\kt50\\k100}WW", layer, layer);
            snprintf(expected, sizeof(expected), "{" PREFIX "\\%dbs6\\%dbc" WAITING "\\kt50\\k100}WW", layer, layer);
            pair(lib, r, meta, actual, expected, 499, rgba, "inherited secondary restored");
        }
        pair(lib, r, "", "{" PREFIX "\\bord6\\3c" ACTIVE "\\1bsc" WAITING "\\kf100}WW",
             "{" PREFIX "\\bord6\\3c" ACTIVE "\\3sc" WAITING "\\kf100}WW", 500, rgba, "layer 1 alias");
        const long long times[] = {200, 500, 700};
        for (int layer = 2; layer <= 3; layer++) for (int t = 0; t < 3; t++) {
            frontier(lib, r, layer, "", "{\\kf100}WW", times[t], rgba);
            frontier(lib, r, layer, "", "{\\kf100}אב", times[t], rgba);
            frontier(lib, r, layer, "\\vert1\\vtype2", "{\\kf100}WW", times[t], rgba);
            frontier(lib, r, layer, "", "<WW|{\\kf100}AB>", times[t], rgba);
            frontier(lib, r, layer, "", "<WW||{\\kf100}AB>", times[t], rgba);
            frontier(lib, r, layer, "\\vert1", "<漢字|{\\kf100}日本語>", times[t], rgba);
        }
        pair(lib, r, "", "{" PREFIX "\\2bs6\\2bc" ACTIVE "\\2bsc" WAITING "\\kt50\\k100}W{\\2bsc&H00FF00&}W",
             "{" PREFIX "\\2bs6\\2bc" WAITING "\\kt50\\k100}W{\\2bc&H00FF00&}W", 499, rgba, "inline waiting scopes split paint runs");
        pair(lib, r, META("Default", "\\2bsc&H00FF00&\\3bsc&H00FFFF&")
             META("Default", "\\2bs6\\3bs3\\2bc" ACTIVE "\\3bc" ACTIVE "\\2bsc" WAITING),
             "{\\an7\\pos(150,160)\\kt50\\k100}WW",
             "{" PREFIX "\\2bs6\\3bs3\\2bc" WAITING "\\3bc&H00FFFF&\\kt50\\k100}WW", 499, rgba, "waiting declarations merge individual layers");
        pair(lib, r, "", "{" PREFIX "\\bord3\\3c" ACTIVE "\\2bsc" WAITING "\\kf100}WW",
             "{" PREFIX "\\bord3\\3c" ACTIVE "\\kf100}WW", 500, rgba, "disabled layer stays disabled");
        pair(lib, r, "", "{" PREFIX "\\2bs6\\2bc" ACTIVE "\\2ba&H80&\\2bsc" WAITING "\\fad(1000,1000)\\kt50\\k100}WW",
             "{" PREFIX "\\2bs6\\2bc" WAITING "\\2ba&H80&\\fad(1000,1000)\\kt50\\k100}WW", 499, rgba, "active alpha and fade inherited");
        pair(lib, r, "", "{" PREFIX "\\2bs6\\2bc" ACTIVE "\\2bsc" WAITING "\\2bva(&H00&,&H80&,&H00&,&H80&)\\kt50\\k100}WW",
             "{" PREFIX "\\2bs6\\2bc" WAITING "\\2bva(&H00&,&H80&,&H00&,&H80&)\\kt50\\k100}WW", 499, rgba, "waiting alpha gradient inherited");
        pair(lib, r, "", "{" PREFIX "\\bord2\\3c" ACTIVE "\\1bsc" WAITING "\\2bs3\\2bc" ACTIVE "\\2bsc&H00FF00&\\3bs2\\3bc" ACTIVE "\\3bsc&H00FFFF&\\kt50\\k100}WW",
             "{" PREFIX "\\bord2\\3c" WAITING "\\2bs3\\2bc&H00FF00&\\3bs2\\3bc&H00FFFF&\\kt50\\k100}WW", 499, rgba, "independent waiting ring colors");
        static const char defaults[] = META("Default", "\\2bs6\\2bc" ACTIVE "\\2bsc" WAITING)
                                      META("Other", "\\2bs6\\2bc" ACTIVE "\\2bsc&H00FF00&");
        pair(lib, r, defaults, "{\\an7\\pos(150,160)\\2bsc&H00FFFF&\\r\\kt50\\k100}WW",
             "{\\an7\\pos(150,160)\\r\\2bs6\\2bc" WAITING "\\kt50\\k100}WW", 499, rgba, "bare reset secondary defaults");
        pair(lib, r, defaults, "{\\an7\\pos(150,160)\\rOther\\kt50\\k100}WW",
             "{\\an7\\pos(150,160)\\rOther\\2bs6\\2bc&H00FF00&\\kt50\\k100}WW", 499, rgba, "named secondary defaults");
        const char *actor = META("Default", "\\2bs6\\2bc" ACTIVE "\\2bsc" WAITING)
            "Comment: 0,0:00:00.00,0:00:00.00,Default,Nene,0,0,0,mangetsu-colorcoding,{\\2bc&H00FF00&\\2bsc&H00FFFF&}\n";
        pair(lib, r, actor, "{\\an7\\pos(150,160)\\kt50\\k100}WW",
             "{" PREFIX "\\2bs6\\2bc" WAITING "\\kt50\\k100}WW", 499, rgba, "actor whitelist does not expand to waiting colors");
        pair(lib, r, actor, "{\\an7\\pos(150,160)\\kt50\\k100}WW",
             "{" PREFIX "\\2bs6\\2bc&H00FF00&\\kt50\\k100}WW", 500, rgba, "actor active color priority");
        const char *effects[] = {"ko", "kO"};
        for (int e = 0; e < 2; e++) for (int time = 499; time <= 500; time++) {
            char actual[1024], expected[1024];
            snprintf(actual, sizeof(actual), "{" PREFIX "\\2bs6\\2bc" ACTIVE "\\2bsc" WAITING "\\kt50\\%s100}WW", effects[e]);
            snprintf(expected, sizeof(expected), "{" PREFIX "\\2bs6\\2bc" ACTIVE "\\kt50\\%s100}WW", effects[e]);
            pair(lib, r, "", actual, expected, time, rgba, "suppression/reveal unchanged");
        }
    }
    ass_renderer_done(r); ass_library_done(lib);
    return failures ? 1 : 0;
}
