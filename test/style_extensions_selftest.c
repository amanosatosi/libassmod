#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>

#include "ass.h"

#define RUBY "<က|စူဇူကီ>"
#define LONG_TEXT "က က က က က က က က က က က က က က က က က က က က က က က က က က က က က က"
#define VERTICAL_TEXT "<日本語|にほんご>Latin<漢字||ဆာတို့>"
#define VERTICAL_LONG VERTICAL_TEXT VERTICAL_TEXT VERTICAL_TEXT
#define META(style, tags) \
    "Comment: 0,8:00:00.00,0:00:00.00," style ",,0,0,0,mangetsu-style,{" tags "}\n"
#define ACTOR(name, tags) \
    "Comment: 0,0:00:00.00,0:00:00.00,Default," name ",0,0,0,mangetsu-colorcoding,{" tags "}\n"
#define WHITELIST \
    ACTOR("mangetsu-colorcode-applied-styles", "Default}{Other")
#define F70 META("Default", "\\furis70")
#define RED ACTOR("Nene", "\\1c&H0000FF&")
#define BLUE ACTOR("Blue", "\\1c&HFF0000&")

static unsigned missing_glyphs;

static void message(int level, const char *fmt, va_list args, void *data)
{
    (void) level;
    (void) args;
    (void) data;
    if (strstr(fmt, "failed to find any fallback with glyph") ||
            strstr(fmt, "not found in font for"))
        missing_glyphs++;
}

static char *script(const char *metadata, const char *style,
                    const char *actor, const char *text)
{
    static const char header[] =
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n"
        "ScaledBorderAndShadow: yes\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Noto Sans Myanmar,42,&H00FFFFFF,&H00FFFFFF,&H80000000,&H80000000,0,0,0,0,100,100,0,0,1,2,0,5,20,20,20,1\n"
        "Style: Other,Noto Sans Myanmar,42,&H0000FF00,&H00FFFFFF,&H80000000,&H80000000,0,0,0,0,100,100,0,0,1,3,0,5,20,20,20,1\n"
        "Style: Box,Noto Sans Myanmar,42,&H00FFFFFF,&H00FFFFFF,&H80000000,&H80000000,0,0,0,0,100,100,0,0,4,2,0,5,20,20,20,1\n"
        "Style: Vertical,Native Vertical Furi,42,&H00FFFFFF,&H00FFFFFF,&H80000000,&H80000000,0,0,0,0,100,100,0,0,1,2,0,5,20,20,20,1\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
    size_t size = sizeof(header) + strlen(metadata) + strlen(style) +
                  strlen(actor) + strlen(text) + 100;
    char *data = malloc(size);
    if (data)
        snprintf(data, size, "%s%sDialogue: 0,0:00:00.00,0:00:10.00,%s,%s,0,0,0,,%s\n",
                 header, metadata, style, actor, text);
    return data;
}

static ASS_Track *load(ASS_Library *lib, const char *metadata,
                       const char *style, const char *actor, const char *text)
{
    char *data = script(metadata, style, actor, text);
    ASS_Track *track = data ? ass_read_memory(lib, data, strlen(data), NULL) : NULL;
    free(data);
    return track;
}

static void hash_byte(uint64_t *hash, uint8_t value)
{
    *hash = (*hash ^ value) * UINT64_C(1099511628211);
}

static void hash_number(uint64_t *hash, uint32_t value)
{
    for (int i = 0; i < 4; i++)
        hash_byte(hash, value >> (8 * i));
}

/* Include geometry, colors and actual raster pixels in both output backends.
 * References use the existing inline implementation, not a metadata parser
 * mock. A nonempty frame and the separate 50-vs-70 check prevent vacuous passes. */
static bool snapshot_at(ASS_Renderer *renderer, ASS_Track *track, bool rgba,
                         uint64_t *hash, long long time)
{
    *hash = UINT64_C(1469598103934665603);
    uint64_t coverage = 0;
    int change;
    if (rgba) {
        ASS_ImageRGBA *images = ass_render_frame_rgba(renderer, track, time, &change);
        for (ASS_ImageRGBA *im = images; im; im = im->next) {
            hash_number(hash, im->type);
            hash_number(hash, im->dst_x);
            hash_number(hash, im->dst_y);
            hash_number(hash, im->w);
            hash_number(hash, im->h);
            for (int y = 0; y < im->h; y++)
                for (int x = 0; x < im->w; x++) {
                    const uint8_t *pixel = im->rgba + y * im->stride + 4 * x;
                    for (int c = 0; c < 4; c++) hash_byte(hash, pixel[c]);
                    coverage += pixel[3];
                }
        }
        ass_free_images_rgba(images);
    } else {
        ASS_Image *images = ass_render_frame(renderer, track, time, &change);
        for (ASS_Image *im = images; im; im = im->next) {
            hash_number(hash, im->type);
            hash_number(hash, im->dst_x);
            hash_number(hash, im->dst_y);
            hash_number(hash, im->w);
            hash_number(hash, im->h);
            hash_number(hash, im->color);
            for (int y = 0; y < im->h; y++)
                for (int x = 0; x < im->w; x++) {
                    uint8_t value = im->bitmap[y * im->stride + x];
                    hash_byte(hash, value);
                    coverage += value * (255 - (im->color & 255));
                }
        }
    }
    return coverage > 0;
}

static bool snapshot(ASS_Renderer *renderer, ASS_Track *track, bool rgba, uint64_t *hash)
{
    return snapshot_at(renderer, track, rgba, hash, 500);
}

typedef struct {
    const char *label, *metadata, *style, *actor, *text;
    const char *reference_metadata, *reference;
} Case;

static int compare_at(ASS_Library *lib, ASS_Renderer *renderer, const Case *c, long long time)
{
    ASS_Track *actual = load(lib, c->metadata, c->style, c->actor, c->text);
    ASS_Track *reference = load(lib, c->reference_metadata, c->style, c->actor, c->reference);
    int failed = !actual || !reference;
    for (int rgba = 0; !failed && rgba < 2; rgba++) {
        uint64_t a, b;
        if (!snapshot_at(renderer, actual, rgba, &a, time) ||
                !snapshot_at(renderer, reference, rgba, &b, time) || a != b) {
            fprintf(stderr, "%s (%s)\n", c->label, rgba ? "RGBA" : "legacy");
            failed = 1;
        }
    }
    if (actual && reference && actual->n_events != reference->n_events) {
        fprintf(stderr, "%s: metadata created an event\n", c->label);
        failed = 1;
    }
    ass_free_track(actual);
    ass_free_track(reference);
    return failed;
}

static int compare(ASS_Library *lib, ASS_Renderer *renderer, const Case *c)
{
    return compare_at(lib, renderer, c, 500);
}

static bool add_font(ASS_Library *lib, const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool ok = fseek(file, 0, SEEK_END) == 0;
    long size = ok ? ftell(file) : -1;
    ok = size > 0 && size <= INT_MAX && fseek(file, 0, SEEK_SET) == 0;
    char *data = ok ? malloc(size) : NULL;
    ok = data && fread(data, 1, size, file) == (size_t) size;
    fclose(file);
    if (ok) ass_add_font(lib, path, data, size);
    free(data);
    return ok;
}

static char *copy_string(const char *text)
{
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

static int host_metadata(ASS_Library *lib, ASS_Renderer *renderer)
{
    char *header = script("", "Default", "", "");
    if (!header) return 1;
    char *events = strstr(header, "Dialogue:");
    if (!events) { free(header); return 1; }
    *events = 0;
    ASS_Track *track = ass_new_track(lib);
    if (!track) { free(header); return 1; }
    ass_process_codec_private(track, header, strlen(header));
    free(header);
    char *owned = copy_string("{\\furis70\\grd(0,&HFF0000&,&H0000FF&)\\pgrdalpha(0,0,640,360,0,&H00&,&HAA&)}");
    int failed = !owned;
    if (owned) {
        failed |= ass_process_mangetsu_metadata_line(track, "Missing", "",
            "mangetsu-style", "{\\furis10}", 1, 1) != 1;
        failed |= ass_process_mangetsu_metadata_line(track, "Default", "Nene",
            "mangetsu-colorcoding", "{\\1c&H0000FF&}", 1, 1) != 1;
        failed |= ass_process_mangetsu_metadata_line(track, "***default", "",
            "mangetsu-style", owned, 1, 1) != 1;
        memset(owned, 'x', strlen(owned));
        free(owned); // no compiled value may borrow the caller's text
        failed |= ass_process_mangetsu_colorcoding_line(track, "Blue",
            "mangetsu-colorcoding", "{\\1c&HFF0000&}", 1, 1) != 1;
        failed |= ass_process_mangetsu_metadata_line(track, "Default", "",
            "mangetsu-style", "{\\furis10}", 0, 1) != 0;
        failed |= ass_process_mangetsu_metadata_line(track, "Default", "",
            "mangetsu-style", "{\\furis10}", 1, 0) != 0;
        failed |= ass_process_mangetsu_metadata_line(track, "Default", "",
            "other-metadata", "{\\furis10}", 1, 1) != 0;
    }
    static const char first[] = "0,0,Default,Nene,0,0,0,,{\\pos(160,180)}" RUBY;
    static const char second[] = "1,0,Default,Blue,0,0,0,,{\\pos(480,180)}" RUBY;
    ass_process_chunk(track, first, sizeof(first) - 1, 0, 10000);
    ass_process_chunk(track, second, sizeof(second) - 1, 0, 10000);
    const char *text = "{\\pos(160,180)}" RUBY "\n"
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,Blue,0,0,0,,{\\pos(480,180)}" RUBY;
    ASS_Track *reference = load(lib, META("Default", "\\furis70\\grd(0,&HFF0000&,&H0000FF&)\\pgrdalpha(0,0,640,360,0,&H00&,&HAA&)") RED BLUE, "Default", "Nene", text);
    for (int rgba = 0; reference && rgba < 2; rgba++) {
        uint64_t a, b;
        failed |= !snapshot(renderer, track, rgba, &a) ||
                  !snapshot(renderer, reference, rgba, &b) || a != b;
    }
    failed |= !reference || track->n_events != 2;
    ass_free_track(reference);

    // Recompilation between render passes must replace owned defaults safely.
    failed |= ass_process_mangetsu_metadata_line(track, "Default", "",
        "mangetsu-style", "{\\furis80}", 1, 1) != 1;
    reference = load(lib, META("Default", "\\furis80\\grd(0,&HFF0000&,&H0000FF&)\\pgrdalpha(0,0,640,360,0,&H00&,&HAA&)") RED BLUE,
                     "Default", "Nene", text);
    uint64_t a, b;
    failed |= !reference || !snapshot(renderer, track, false, &a) ||
              !snapshot(renderer, reference, false, &b) || a != b;
    ass_free_track(reference);
    ass_free_track(track);
    if (failed) fprintf(stderr, "embedded metadata ingestion/ownership failed\n");
    return failed;
}

int main(int argc, char **argv)
{
    if (argc != 4) return 1;
    ASS_Library *lib = ass_library_init();
    if (!lib) return 1;
    ass_set_message_cb(lib, message, NULL);
    if (!add_font(lib, argv[1]) || !add_font(lib, argv[2]) || !add_font(lib, argv[3])) {
        ass_library_done(lib);
        return 1;
    }
    ASS_Renderer *renderer = ass_renderer_init(lib);
    if (!renderer) { ass_library_done(lib); return 1; }
    ass_set_frame_size(renderer, 640, 360);
    ass_set_storage_size(renderer, 640, 360);
    // Myanmar is the explicit style font; the bundled Latin fixture supplies
    // punctuation when \furi0 makes the angle brackets and pipe visible.
    ass_set_fonts(renderer, NULL, "Pixel Operator Mono", ASS_FONTPROVIDER_NONE, NULL, 1);

    const uint8_t image[] = {0, 255, 0, 255};
    if (ass_set_tag_image_rgba(renderer, "style-test.png", ASS_TAG_IMAGE_FORMAT_PNG, 1, 1, 4, image)) return 1;

    static const Case cases[] = {
        {"rich defaults avoid ordinary event memoization", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\grd(0,&HFF0000&,&H0000FF&)\\pgrdalpha(0,0,640,360,0,&H00&,&HAA&)}\n", "Default", "", "{\\pos(320,180)}က က\nDialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,{\\pos(320,180)}က က", "", "{\\pos(320,180)\\grd(0,&HFF0000&,&H0000FF&)\\pgrdalpha(0,0,640,360,0,&H00&,&HAA&)}က က\nDialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,{\\pos(320,180)\\grd(0,&HFF0000&,&H0000FF&)\\pgrdalpha(0,0,640,360,0,&H00&,&HAA&)}က က"},
        {"fshp inherited 0", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\fshp0}\n", "Default", "", "က\\Nက", "", "{\\fshp0}က\\Nက"},
        {"fshp inherited 5", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\fshp5}\n", "Default", "", "က\\Nက", "", "{\\fshp5}က\\Nက"},
        {"fshp inherited -5", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\fshp-5}\n", "Default", "", "က\\Nက", "", "{\\fshp-5}က\\Nက"},
        {"fshp relative merge", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\fshp5}\nComment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\fshp~+2}\n", "Default", "", "က\\Nက", "", "{\\fshp7}က\\Nက"},
        {"fshp relative inline override", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\fshp5}\n", "Default", "", "{\\fshp~+2}က\\Nက", "", "{\\fshp7}က\\Nက"},
        {"fshp empty reset", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\fshp5}\n", "Default", "", "{\\fshp-3\\fshp}က\\Nက", "", "{\\fshp5}က\\Nက"},
        {"fshp bare reset", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\fshp5}\n", "Default", "", "{\\fshp-3\\r}က\\Nက", "", "{\\r\\fshp5}က\\Nက"},
        {"fshp named reset", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\fshp5}\nComment: 0,0:00:00.00,0:00:00.00,Other,,0,0,0,mangetsu-style,{\\fshp-4}\n", "Default", "", "{\\rOther\\fshp10\\fshp}က\\Nက", "", "{\\rOther\\fshp-4}က\\Nက"},
        {"ortho inherited 0", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\ortho0}\n", "Default", "", "{\\frx35\\fry20\\frz5\\z10\\distort(1,0,1,1,0,1,0,0)}က က", "", "{\\ortho0\\frx35\\fry20\\frz5\\z10\\distort(1,0,1,1,0,1,0,0)}က က"},
        {"ortho inherited 1", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\ortho1}\n", "Default", "", "{\\frx35\\fry20\\frz5\\z10\\distort(1,0,1,1,0,1,0,0)}က က", "", "{\\ortho1\\frx35\\fry20\\frz5\\z10\\distort(1,0,1,1,0,1,0,0)}က က"},
        {"ortho explicit override", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\ortho1}\n", "Default", "", "{\\ortho0\\frx35\\fry20\\frz5\\z10\\distort(1,0,1,1,0,1,0,0)}က က", "", "{\\ortho0\\frx35\\fry20\\frz5\\z10\\distort(1,0,1,1,0,1,0,0)}က က"},
        {"ortho empty reset", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\ortho1}\n", "Default", "", "{\\ortho0\\ortho\\frx35\\fry20\\frz5\\z10\\distort(1,0,1,1,0,1,0,0)}က က", "", "{\\ortho1\\frx35\\fry20\\frz5\\z10\\distort(1,0,1,1,0,1,0,0)}က က"},
        {"ortho bare reset", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\ortho1}\n", "Default", "", "{\\ortho0\\r\\frx35\\fry20\\frz5\\z10\\distort(1,0,1,1,0,1,0,0)}က က", "", "{\\r\\ortho1\\frx35\\fry20\\frz5\\z10\\distort(1,0,1,1,0,1,0,0)}က က"},
        {"ortho named reset", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\ortho0}\nComment: 0,0:00:00.00,0:00:00.00,Other,,0,0,0,mangetsu-style,{\\ortho1}\n", "Default", "", "{\\rOther\\ortho0\\ortho\\frx35\\fry20\\frz5\\z10\\distort(1,0,1,1,0,1,0,0)}က က", "", "{\\rOther\\ortho1\\frx35\\fry20\\frz5\\z10\\distort(1,0,1,1,0,1,0,0)}က က"},
        {"strict scalar metadata", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\ortho2\\ortho1junk\\ortho(1,0)\\fshp(nan)\\fshp(1,2)}\n", "Default", "", "က က", "", "က က"},
        {"fill family 1grd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1grd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{}က က", "", "{\\1grd(45,&HFF0000&,&H0000FF&)}က က"},
        {"fill family reset 1grd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1grd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\1vc(&H00FF00&)\\1grd()}က က", "", "{\\1grd(45,&HFF0000&,&H0000FF&)}က က"},
        {"fill family 1gra", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1gra(0,&H00&,&HAA&)}\n", "Default", "", "{}က က", "", "{\\1gra(0,&H00&,&HAA&)}က က"},
        {"fill family reset 1gra", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1gra(0,&H00&,&HAA&)}\n", "Default", "", "{\\1va(&H80&)\\1gra()}က က", "", "{\\1gra(0,&H00&,&HAA&)}က က"},
        {"fill family 1pgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{}က က", "", "{\\1pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"fill family reset 1pgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\1pgrd(0,0,640,360,90,&H00FF00&,&HFFFFFF&)\\1pgrd()}က က", "", "{\\1pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"fill family 1vc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1vc(&HFF0000&,&H0000FF&,&HFF0000&,&H0000FF&)}\n", "Default", "", "{}က က", "", "{\\1vc(&HFF0000&,&H0000FF&,&HFF0000&,&H0000FF&)}က က"},
        {"fill family reset 1vc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1vc(&HFF0000&,&H0000FF&,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\1vc(&H00FF00&)\\1vc}က က", "", "{\\1vc(&HFF0000&,&H0000FF&,&HFF0000&,&H0000FF&)}က က"},
        {"fill family 1va", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1va(&H00&,&HAA&,&H00&,&HAA&)}\n", "Default", "", "{}က က", "", "{\\1va(&H00&,&HAA&,&H00&,&HAA&)}က က"},
        {"fill family reset 1va", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1va(&H00&,&HAA&,&H00&,&HAA&)}\n", "Default", "", "{\\1va(&H80&)\\1va}က က", "", "{\\1va(&H00&,&HAA&,&H00&,&HAA&)}က က"},
        {"fill family 2grd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2grd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\kt100\\k100}က က", "", "{\\2grd(45,&HFF0000&,&H0000FF&)\\kt100\\k100}က က"},
        {"fill family reset 2grd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2grd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\2vc(&H00FF00&)\\2grd()\\kt100\\k100}က က", "", "{\\2grd(45,&HFF0000&,&H0000FF&)\\kt100\\k100}က က"},
        {"fill family 2gra", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2gra(0,&H00&,&HAA&)}\n", "Default", "", "{\\kt100\\k100}က က", "", "{\\2gra(0,&H00&,&HAA&)\\kt100\\k100}က က"},
        {"fill family reset 2gra", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2gra(0,&H00&,&HAA&)}\n", "Default", "", "{\\2va(&H80&)\\2gra()\\kt100\\k100}က က", "", "{\\2gra(0,&H00&,&HAA&)\\kt100\\k100}က က"},
        {"fill family 2pgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\kt100\\k100}က က", "", "{\\2pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)\\kt100\\k100}က က"},
        {"fill family reset 2pgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\2pgrd(0,0,640,360,90,&H00FF00&,&HFFFFFF&)\\2pgrd()}က က", "", "{\\2pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)\\kt100\\k100}က က"},
        {"fill family 2vc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2vc(&HFF0000&,&H0000FF&,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\kt100\\k100}က က", "", "{\\2vc(&HFF0000&,&H0000FF&,&HFF0000&,&H0000FF&)\\kt100\\k100}က က"},
        {"fill family reset 2vc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2vc(&HFF0000&,&H0000FF&,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\2vc(&H00FF00&)\\2vc\\kt100\\k100}က က", "", "{\\2vc(&HFF0000&,&H0000FF&,&HFF0000&,&H0000FF&)\\kt100\\k100}က က"},
        {"fill family 2va", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2va(&H00&,&HAA&,&H00&,&HAA&)}\n", "Default", "", "{\\kt100\\k100}က က", "", "{\\2va(&H00&,&HAA&,&H00&,&HAA&)\\kt100\\k100}က က"},
        {"fill family reset 2va", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2va(&H00&,&HAA&,&H00&,&HAA&)}\n", "Default", "", "{\\2va(&H80&)\\2va\\kt100\\k100}က က", "", "{\\2va(&H00&,&HAA&,&H00&,&HAA&)\\kt100\\k100}က က"},
        {"fill family 3grd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3grd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{}က က", "", "{\\3grd(45,&HFF0000&,&H0000FF&)}က က"},
        {"fill family reset 3grd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3grd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\3vc(&H00FF00&)\\3grd()}က က", "", "{\\3grd(45,&HFF0000&,&H0000FF&)}က က"},
        {"fill family 3gra", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3gra(0,&H00&,&HAA&)}\n", "Default", "", "{}က က", "", "{\\3gra(0,&H00&,&HAA&)}က က"},
        {"fill family reset 3gra", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3gra(0,&H00&,&HAA&)}\n", "Default", "", "{\\3va(&H80&)\\3gra()}က က", "", "{\\3gra(0,&H00&,&HAA&)}က က"},
        {"fill family 3pgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{}က က", "", "{\\3pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"fill family reset 3pgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\3pgrd(0,0,640,360,90,&H00FF00&,&HFFFFFF&)\\3pgrd()}က က", "", "{\\3pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"fill family 3vc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3vc(&HFF0000&,&H0000FF&,&HFF0000&,&H0000FF&)}\n", "Default", "", "{}က က", "", "{\\3vc(&HFF0000&,&H0000FF&,&HFF0000&,&H0000FF&)}က က"},
        {"fill family reset 3vc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3vc(&HFF0000&,&H0000FF&,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\3vc(&H00FF00&)\\3vc}က က", "", "{\\3vc(&HFF0000&,&H0000FF&,&HFF0000&,&H0000FF&)}က က"},
        {"fill family 3va", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3va(&H00&,&HAA&,&H00&,&HAA&)}\n", "Default", "", "{}က က", "", "{\\3va(&H00&,&HAA&,&H00&,&HAA&)}က က"},
        {"fill family reset 3va", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3va(&H00&,&HAA&,&H00&,&HAA&)}\n", "Default", "", "{\\3va(&H80&)\\3va}က က", "", "{\\3va(&H00&,&HAA&,&H00&,&HAA&)}က က"},
        {"fill family 4grd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\4grd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\shad5}က က", "", "{\\4grd(45,&HFF0000&,&H0000FF&)\\shad5}က က"},
        {"fill family reset 4grd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\4grd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\4vc(&H00FF00&)\\4grd()\\shad5}က က", "", "{\\4grd(45,&HFF0000&,&H0000FF&)\\shad5}က က"},
        {"fill family 4gra", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\4gra(0,&H00&,&HAA&)}\n", "Default", "", "{\\shad5}က က", "", "{\\4gra(0,&H00&,&HAA&)\\shad5}က က"},
        {"fill family reset 4gra", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\4gra(0,&H00&,&HAA&)}\n", "Default", "", "{\\4va(&H80&)\\4gra()\\shad5}က က", "", "{\\4gra(0,&H00&,&HAA&)\\shad5}က က"},
        {"fill family 4pgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\4pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\shad5}က က", "", "{\\4pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)\\shad5}က က"},
        {"fill family reset 4pgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\4pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\4pgrd(0,0,640,360,90,&H00FF00&,&HFFFFFF&)\\4pgrd()}က က", "", "{\\4pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)\\shad5}က က"},
        {"fill family 4vc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\4vc(&HFF0000&,&H0000FF&,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\shad5}က က", "", "{\\4vc(&HFF0000&,&H0000FF&,&HFF0000&,&H0000FF&)\\shad5}က က"},
        {"fill family reset 4vc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\4vc(&HFF0000&,&H0000FF&,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\4vc(&H00FF00&)\\4vc\\shad5}က က", "", "{\\4vc(&HFF0000&,&H0000FF&,&HFF0000&,&H0000FF&)\\shad5}က က"},
        {"fill family 4va", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\4va(&H00&,&HAA&,&H00&,&HAA&)}\n", "Default", "", "{\\shad5}က က", "", "{\\4va(&H00&,&HAA&,&H00&,&HAA&)\\shad5}က က"},
        {"fill family reset 4va", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\4va(&H00&,&HAA&,&H00&,&HAA&)}\n", "Default", "", "{\\4va(&H80&)\\4va\\shad5}က က", "", "{\\4va(&H00&,&HAA&,&H00&,&HAA&)\\shad5}က က"},
        {"fill family 5grd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\5grd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\u1}က က", "", "{\\5grd(45,&HFF0000&,&H0000FF&)\\u1}က က"},
        {"fill family reset 5grd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\5grd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\5vc(&H00FF00&)\\5grd()\\u1}က က", "", "{\\5grd(45,&HFF0000&,&H0000FF&)\\u1}က က"},
        {"fill family 5gra", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\5gra(0,&H00&,&HAA&)}\n", "Default", "", "{\\u1}က က", "", "{\\5gra(0,&H00&,&HAA&)\\u1}က က"},
        {"fill family reset 5gra", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\5gra(0,&H00&,&HAA&)}\n", "Default", "", "{\\5va(&H80&)\\5gra()\\u1}က က", "", "{\\5gra(0,&H00&,&HAA&)\\u1}က က"},
        {"fill family 5pgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\5pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\u1}က က", "", "{\\5pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)\\u1}က က"},
        {"fill family reset 5pgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\5pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\5pgrd(0,0,640,360,90,&H00FF00&,&HFFFFFF&)\\5pgrd()}က က", "", "{\\5pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)\\u1}က က"},
        {"ring family 1bgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1bs4\\1bgrd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\1bs4\\1bgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 1bgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1bs4\\1bgrd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\1bvc(&H00FF00&)\\1bgrd()}က က", "", "{\\1bs4\\1bgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 1bga", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1bs4\\1bga(0,&H00&,&HAA&)}\n", "Default", "", "က က", "", "{\\1bs4\\1bga(0,&H00&,&HAA&)}က က"},
        {"ring reset 1bga", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1bs4\\1bga(0,&H00&,&HAA&)}\n", "Default", "", "{\\1bva(&H80&)\\1bga()}က က", "", "{\\1bs4\\1bga(0,&H00&,&HAA&)}က က"},
        {"ring family 1bvc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1bs4\\1bvc(&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\1bs4\\1bvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 1bvc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1bs4\\1bvc(&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\1bvc(&H00FF00&)\\1bvc}က က", "", "{\\1bs4\\1bvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring family 1bva", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1bs4\\1bva(&H00&,&HAA&)}\n", "Default", "", "က က", "", "{\\1bs4\\1bva(&H00&,&HAA&)}က က"},
        {"ring reset 1bva", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1bs4\\1bva(&H00&,&HAA&)}\n", "Default", "", "{\\1bva(&H80&)\\1bva}က က", "", "{\\1bs4\\1bva(&H00&,&HAA&)}က က"},
        {"ring family 1bpgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1bs4\\1bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\1bs4\\1bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 1bpgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1bs4\\1bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\1bpgrd(0,0,640,360,90,&H00FF00&,&HFFFFFF&)\\1bpgrd()}က က", "", "{\\1bs4\\1bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 1bbgrd", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\1bbs4\\1bbgrd(45,&HFF0000&,&H0000FF&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\1bbs4\\1bbgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 1bbgrd", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\1bbs4\\1bbgrd(45,&HFF0000&,&H0000FF&)}\n", "Box", "", "{\\1bbvc(&H00FF00&)\\1bbgrd()}က က", "", "{\\box1\\boxp3\\1bbs4\\1bbgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 1bbga", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\1bbs4\\1bbga(0,&H00&,&HAA&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\1bbs4\\1bbga(0,&H00&,&HAA&)}က က"},
        {"ring reset 1bbga", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\1bbs4\\1bbga(0,&H00&,&HAA&)}\n", "Box", "", "{\\1bbva(&H80&)\\1bbga()}က က", "", "{\\box1\\boxp3\\1bbs4\\1bbga(0,&H00&,&HAA&)}က က"},
        {"ring family 1bbvc", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\1bbs4\\1bbvc(&HFF0000&,&H0000FF&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\1bbs4\\1bbvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 1bbvc", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\1bbs4\\1bbvc(&HFF0000&,&H0000FF&)}\n", "Box", "", "{\\1bbvc(&H00FF00&)\\1bbvc}က က", "", "{\\box1\\boxp3\\1bbs4\\1bbvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring family 1bbva", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\1bbs4\\1bbva(&H00&,&HAA&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\1bbs4\\1bbva(&H00&,&HAA&)}က က"},
        {"ring reset 1bbva", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\1bbs4\\1bbva(&H00&,&HAA&)}\n", "Box", "", "{\\1bbva(&H80&)\\1bbva}က က", "", "{\\box1\\boxp3\\1bbs4\\1bbva(&H00&,&HAA&)}က က"},
        {"ring family 2bgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2bs4\\2bgrd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\2bs4\\2bgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 2bgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2bs4\\2bgrd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\2bvc(&H00FF00&)\\2bgrd()}က က", "", "{\\2bs4\\2bgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 2bga", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2bs4\\2bga(0,&H00&,&HAA&)}\n", "Default", "", "က က", "", "{\\2bs4\\2bga(0,&H00&,&HAA&)}က က"},
        {"ring reset 2bga", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2bs4\\2bga(0,&H00&,&HAA&)}\n", "Default", "", "{\\2bva(&H80&)\\2bga()}က က", "", "{\\2bs4\\2bga(0,&H00&,&HAA&)}က က"},
        {"ring family 2bvc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2bs4\\2bvc(&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\2bs4\\2bvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 2bvc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2bs4\\2bvc(&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\2bvc(&H00FF00&)\\2bvc}က က", "", "{\\2bs4\\2bvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring family 2bva", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2bs4\\2bva(&H00&,&HAA&)}\n", "Default", "", "က က", "", "{\\2bs4\\2bva(&H00&,&HAA&)}က က"},
        {"ring reset 2bva", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2bs4\\2bva(&H00&,&HAA&)}\n", "Default", "", "{\\2bva(&H80&)\\2bva}က က", "", "{\\2bs4\\2bva(&H00&,&HAA&)}က က"},
        {"ring family 2bpgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2bs4\\2bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\2bs4\\2bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 2bpgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2bs4\\2bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\2bpgrd(0,0,640,360,90,&H00FF00&,&HFFFFFF&)\\2bpgrd()}က က", "", "{\\2bs4\\2bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 2bbgrd", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\2bbs4\\2bbgrd(45,&HFF0000&,&H0000FF&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\2bbs4\\2bbgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 2bbgrd", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\2bbs4\\2bbgrd(45,&HFF0000&,&H0000FF&)}\n", "Box", "", "{\\2bbvc(&H00FF00&)\\2bbgrd()}က က", "", "{\\box1\\boxp3\\2bbs4\\2bbgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 2bbga", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\2bbs4\\2bbga(0,&H00&,&HAA&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\2bbs4\\2bbga(0,&H00&,&HAA&)}က က"},
        {"ring reset 2bbga", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\2bbs4\\2bbga(0,&H00&,&HAA&)}\n", "Box", "", "{\\2bbva(&H80&)\\2bbga()}က က", "", "{\\box1\\boxp3\\2bbs4\\2bbga(0,&H00&,&HAA&)}က က"},
        {"ring family 2bbvc", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\2bbs4\\2bbvc(&HFF0000&,&H0000FF&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\2bbs4\\2bbvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 2bbvc", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\2bbs4\\2bbvc(&HFF0000&,&H0000FF&)}\n", "Box", "", "{\\2bbvc(&H00FF00&)\\2bbvc}က က", "", "{\\box1\\boxp3\\2bbs4\\2bbvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring family 2bbva", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\2bbs4\\2bbva(&H00&,&HAA&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\2bbs4\\2bbva(&H00&,&HAA&)}က က"},
        {"ring reset 2bbva", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\2bbs4\\2bbva(&H00&,&HAA&)}\n", "Box", "", "{\\2bbva(&H80&)\\2bbva}က က", "", "{\\box1\\boxp3\\2bbs4\\2bbva(&H00&,&HAA&)}က က"},
        {"ring family 3bgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3bs4\\3bgrd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\3bs4\\3bgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 3bgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3bs4\\3bgrd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\3bvc(&H00FF00&)\\3bgrd()}က က", "", "{\\3bs4\\3bgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 3bga", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3bs4\\3bga(0,&H00&,&HAA&)}\n", "Default", "", "က က", "", "{\\3bs4\\3bga(0,&H00&,&HAA&)}က က"},
        {"ring reset 3bga", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3bs4\\3bga(0,&H00&,&HAA&)}\n", "Default", "", "{\\3bva(&H80&)\\3bga()}က က", "", "{\\3bs4\\3bga(0,&H00&,&HAA&)}က က"},
        {"ring family 3bvc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3bs4\\3bvc(&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\3bs4\\3bvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 3bvc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3bs4\\3bvc(&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\3bvc(&H00FF00&)\\3bvc}က က", "", "{\\3bs4\\3bvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring family 3bva", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3bs4\\3bva(&H00&,&HAA&)}\n", "Default", "", "က က", "", "{\\3bs4\\3bva(&H00&,&HAA&)}က က"},
        {"ring reset 3bva", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3bs4\\3bva(&H00&,&HAA&)}\n", "Default", "", "{\\3bva(&H80&)\\3bva}က က", "", "{\\3bs4\\3bva(&H00&,&HAA&)}က က"},
        {"ring family 3bpgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3bs4\\3bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\3bs4\\3bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 3bpgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3bs4\\3bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\3bpgrd(0,0,640,360,90,&H00FF00&,&HFFFFFF&)\\3bpgrd()}က က", "", "{\\3bs4\\3bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 3bbgrd", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\3bbs4\\3bbgrd(45,&HFF0000&,&H0000FF&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\3bbs4\\3bbgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 3bbgrd", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\3bbs4\\3bbgrd(45,&HFF0000&,&H0000FF&)}\n", "Box", "", "{\\3bbvc(&H00FF00&)\\3bbgrd()}က က", "", "{\\box1\\boxp3\\3bbs4\\3bbgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 3bbga", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\3bbs4\\3bbga(0,&H00&,&HAA&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\3bbs4\\3bbga(0,&H00&,&HAA&)}က က"},
        {"ring reset 3bbga", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\3bbs4\\3bbga(0,&H00&,&HAA&)}\n", "Box", "", "{\\3bbva(&H80&)\\3bbga()}က က", "", "{\\box1\\boxp3\\3bbs4\\3bbga(0,&H00&,&HAA&)}က က"},
        {"ring family 3bbvc", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\3bbs4\\3bbvc(&HFF0000&,&H0000FF&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\3bbs4\\3bbvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 3bbvc", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\3bbs4\\3bbvc(&HFF0000&,&H0000FF&)}\n", "Box", "", "{\\3bbvc(&H00FF00&)\\3bbvc}က က", "", "{\\box1\\boxp3\\3bbs4\\3bbvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring family 3bbva", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\3bbs4\\3bbva(&H00&,&HAA&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\3bbs4\\3bbva(&H00&,&HAA&)}က က"},
        {"ring reset 3bbva", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\3bbs4\\3bbva(&H00&,&HAA&)}\n", "Box", "", "{\\3bbva(&H80&)\\3bbva}က က", "", "{\\box1\\boxp3\\3bbs4\\3bbva(&H00&,&HAA&)}က က"},
        {"ring family 4bgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\4bs4\\4bgrd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\4bs4\\4bgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 4bgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\4bs4\\4bgrd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\4bvc(&H00FF00&)\\4bgrd()}က က", "", "{\\4bs4\\4bgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 4bga", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\4bs4\\4bga(0,&H00&,&HAA&)}\n", "Default", "", "က က", "", "{\\4bs4\\4bga(0,&H00&,&HAA&)}က က"},
        {"ring reset 4bga", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\4bs4\\4bga(0,&H00&,&HAA&)}\n", "Default", "", "{\\4bva(&H80&)\\4bga()}က က", "", "{\\4bs4\\4bga(0,&H00&,&HAA&)}က က"},
        {"ring family 4bvc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\4bs4\\4bvc(&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\4bs4\\4bvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 4bvc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\4bs4\\4bvc(&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\4bvc(&H00FF00&)\\4bvc}က က", "", "{\\4bs4\\4bvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring family 4bva", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\4bs4\\4bva(&H00&,&HAA&)}\n", "Default", "", "က က", "", "{\\4bs4\\4bva(&H00&,&HAA&)}က က"},
        {"ring reset 4bva", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\4bs4\\4bva(&H00&,&HAA&)}\n", "Default", "", "{\\4bva(&H80&)\\4bva}က က", "", "{\\4bs4\\4bva(&H00&,&HAA&)}က က"},
        {"ring family 4bpgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\4bs4\\4bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\4bs4\\4bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 4bpgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\4bs4\\4bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\4bpgrd(0,0,640,360,90,&H00FF00&,&HFFFFFF&)\\4bpgrd()}က က", "", "{\\4bs4\\4bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 4bbgrd", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\4bbs4\\4bbgrd(45,&HFF0000&,&H0000FF&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\4bbs4\\4bbgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 4bbgrd", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\4bbs4\\4bbgrd(45,&HFF0000&,&H0000FF&)}\n", "Box", "", "{\\4bbvc(&H00FF00&)\\4bbgrd()}က က", "", "{\\box1\\boxp3\\4bbs4\\4bbgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 4bbga", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\4bbs4\\4bbga(0,&H00&,&HAA&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\4bbs4\\4bbga(0,&H00&,&HAA&)}က က"},
        {"ring reset 4bbga", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\4bbs4\\4bbga(0,&H00&,&HAA&)}\n", "Box", "", "{\\4bbva(&H80&)\\4bbga()}က က", "", "{\\box1\\boxp3\\4bbs4\\4bbga(0,&H00&,&HAA&)}က က"},
        {"ring family 4bbvc", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\4bbs4\\4bbvc(&HFF0000&,&H0000FF&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\4bbs4\\4bbvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 4bbvc", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\4bbs4\\4bbvc(&HFF0000&,&H0000FF&)}\n", "Box", "", "{\\4bbvc(&H00FF00&)\\4bbvc}က က", "", "{\\box1\\boxp3\\4bbs4\\4bbvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring family 4bbva", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\4bbs4\\4bbva(&H00&,&HAA&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\4bbs4\\4bbva(&H00&,&HAA&)}က က"},
        {"ring reset 4bbva", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\4bbs4\\4bbva(&H00&,&HAA&)}\n", "Box", "", "{\\4bbva(&H80&)\\4bbva}က က", "", "{\\box1\\boxp3\\4bbs4\\4bbva(&H00&,&HAA&)}က က"},
        {"ring family 5bgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\5bs4\\5bgrd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\5bs4\\5bgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 5bgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\5bs4\\5bgrd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\5bvc(&H00FF00&)\\5bgrd()}က က", "", "{\\5bs4\\5bgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 5bga", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\5bs4\\5bga(0,&H00&,&HAA&)}\n", "Default", "", "က က", "", "{\\5bs4\\5bga(0,&H00&,&HAA&)}က က"},
        {"ring reset 5bga", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\5bs4\\5bga(0,&H00&,&HAA&)}\n", "Default", "", "{\\5bva(&H80&)\\5bga()}က က", "", "{\\5bs4\\5bga(0,&H00&,&HAA&)}က က"},
        {"ring family 5bvc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\5bs4\\5bvc(&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\5bs4\\5bvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 5bvc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\5bs4\\5bvc(&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\5bvc(&H00FF00&)\\5bvc}က က", "", "{\\5bs4\\5bvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring family 5bva", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\5bs4\\5bva(&H00&,&HAA&)}\n", "Default", "", "က က", "", "{\\5bs4\\5bva(&H00&,&HAA&)}က က"},
        {"ring reset 5bva", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\5bs4\\5bva(&H00&,&HAA&)}\n", "Default", "", "{\\5bva(&H80&)\\5bva}က က", "", "{\\5bs4\\5bva(&H00&,&HAA&)}က က"},
        {"ring family 5bpgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\5bs4\\5bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\5bs4\\5bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 5bpgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\5bs4\\5bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\5bpgrd(0,0,640,360,90,&H00FF00&,&HFFFFFF&)\\5bpgrd()}က က", "", "{\\5bs4\\5bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 5bbgrd", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\5bbs4\\5bbgrd(45,&HFF0000&,&H0000FF&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\5bbs4\\5bbgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 5bbgrd", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\5bbs4\\5bbgrd(45,&HFF0000&,&H0000FF&)}\n", "Box", "", "{\\5bbvc(&H00FF00&)\\5bbgrd()}က က", "", "{\\box1\\boxp3\\5bbs4\\5bbgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 5bbga", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\5bbs4\\5bbga(0,&H00&,&HAA&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\5bbs4\\5bbga(0,&H00&,&HAA&)}က က"},
        {"ring reset 5bbga", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\5bbs4\\5bbga(0,&H00&,&HAA&)}\n", "Box", "", "{\\5bbva(&H80&)\\5bbga()}က က", "", "{\\box1\\boxp3\\5bbs4\\5bbga(0,&H00&,&HAA&)}က က"},
        {"ring family 5bbvc", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\5bbs4\\5bbvc(&HFF0000&,&H0000FF&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\5bbs4\\5bbvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 5bbvc", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\5bbs4\\5bbvc(&HFF0000&,&H0000FF&)}\n", "Box", "", "{\\5bbvc(&H00FF00&)\\5bbvc}က က", "", "{\\box1\\boxp3\\5bbs4\\5bbvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring family 5bbva", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\5bbs4\\5bbva(&H00&,&HAA&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\5bbs4\\5bbva(&H00&,&HAA&)}က က"},
        {"ring reset 5bbva", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\5bbs4\\5bbva(&H00&,&HAA&)}\n", "Box", "", "{\\5bbva(&H80&)\\5bbva}က က", "", "{\\box1\\boxp3\\5bbs4\\5bbva(&H00&,&HAA&)}က က"},
        {"ring family 6bgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\6bs4\\6bgrd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\6bs4\\6bgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 6bgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\6bs4\\6bgrd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\6bvc(&H00FF00&)\\6bgrd()}က က", "", "{\\6bs4\\6bgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 6bga", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\6bs4\\6bga(0,&H00&,&HAA&)}\n", "Default", "", "က က", "", "{\\6bs4\\6bga(0,&H00&,&HAA&)}က က"},
        {"ring reset 6bga", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\6bs4\\6bga(0,&H00&,&HAA&)}\n", "Default", "", "{\\6bva(&H80&)\\6bga()}က က", "", "{\\6bs4\\6bga(0,&H00&,&HAA&)}က က"},
        {"ring family 6bvc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\6bs4\\6bvc(&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\6bs4\\6bvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 6bvc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\6bs4\\6bvc(&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\6bvc(&H00FF00&)\\6bvc}က က", "", "{\\6bs4\\6bvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring family 6bva", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\6bs4\\6bva(&H00&,&HAA&)}\n", "Default", "", "က က", "", "{\\6bs4\\6bva(&H00&,&HAA&)}က က"},
        {"ring reset 6bva", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\6bs4\\6bva(&H00&,&HAA&)}\n", "Default", "", "{\\6bva(&H80&)\\6bva}က က", "", "{\\6bs4\\6bva(&H00&,&HAA&)}က က"},
        {"ring family 6bpgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\6bs4\\6bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\6bs4\\6bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 6bpgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\6bs4\\6bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\6bpgrd(0,0,640,360,90,&H00FF00&,&HFFFFFF&)\\6bpgrd()}က က", "", "{\\6bs4\\6bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 6bbgrd", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\6bbs4\\6bbgrd(45,&HFF0000&,&H0000FF&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\6bbs4\\6bbgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 6bbgrd", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\6bbs4\\6bbgrd(45,&HFF0000&,&H0000FF&)}\n", "Box", "", "{\\6bbvc(&H00FF00&)\\6bbgrd()}က က", "", "{\\box1\\boxp3\\6bbs4\\6bbgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 6bbga", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\6bbs4\\6bbga(0,&H00&,&HAA&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\6bbs4\\6bbga(0,&H00&,&HAA&)}က က"},
        {"ring reset 6bbga", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\6bbs4\\6bbga(0,&H00&,&HAA&)}\n", "Box", "", "{\\6bbva(&H80&)\\6bbga()}က က", "", "{\\box1\\boxp3\\6bbs4\\6bbga(0,&H00&,&HAA&)}က က"},
        {"ring family 6bbvc", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\6bbs4\\6bbvc(&HFF0000&,&H0000FF&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\6bbs4\\6bbvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 6bbvc", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\6bbs4\\6bbvc(&HFF0000&,&H0000FF&)}\n", "Box", "", "{\\6bbvc(&H00FF00&)\\6bbvc}က က", "", "{\\box1\\boxp3\\6bbs4\\6bbvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring family 6bbva", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\6bbs4\\6bbva(&H00&,&HAA&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\6bbs4\\6bbva(&H00&,&HAA&)}က က"},
        {"ring reset 6bbva", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\6bbs4\\6bbva(&H00&,&HAA&)}\n", "Box", "", "{\\6bbva(&H80&)\\6bbva}က က", "", "{\\box1\\boxp3\\6bbs4\\6bbva(&H00&,&HAA&)}က က"},
        {"ring family 7bgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\7bs4\\7bgrd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\7bs4\\7bgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 7bgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\7bs4\\7bgrd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\7bvc(&H00FF00&)\\7bgrd()}က က", "", "{\\7bs4\\7bgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 7bga", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\7bs4\\7bga(0,&H00&,&HAA&)}\n", "Default", "", "က က", "", "{\\7bs4\\7bga(0,&H00&,&HAA&)}က က"},
        {"ring reset 7bga", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\7bs4\\7bga(0,&H00&,&HAA&)}\n", "Default", "", "{\\7bva(&H80&)\\7bga()}က က", "", "{\\7bs4\\7bga(0,&H00&,&HAA&)}က က"},
        {"ring family 7bvc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\7bs4\\7bvc(&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\7bs4\\7bvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 7bvc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\7bs4\\7bvc(&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\7bvc(&H00FF00&)\\7bvc}က က", "", "{\\7bs4\\7bvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring family 7bva", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\7bs4\\7bva(&H00&,&HAA&)}\n", "Default", "", "က က", "", "{\\7bs4\\7bva(&H00&,&HAA&)}က က"},
        {"ring reset 7bva", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\7bs4\\7bva(&H00&,&HAA&)}\n", "Default", "", "{\\7bva(&H80&)\\7bva}က က", "", "{\\7bs4\\7bva(&H00&,&HAA&)}က က"},
        {"ring family 7bpgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\7bs4\\7bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\7bs4\\7bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 7bpgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\7bs4\\7bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\7bpgrd(0,0,640,360,90,&H00FF00&,&HFFFFFF&)\\7bpgrd()}က က", "", "{\\7bs4\\7bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 7bbgrd", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\7bbs4\\7bbgrd(45,&HFF0000&,&H0000FF&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\7bbs4\\7bbgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 7bbgrd", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\7bbs4\\7bbgrd(45,&HFF0000&,&H0000FF&)}\n", "Box", "", "{\\7bbvc(&H00FF00&)\\7bbgrd()}က က", "", "{\\box1\\boxp3\\7bbs4\\7bbgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 7bbga", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\7bbs4\\7bbga(0,&H00&,&HAA&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\7bbs4\\7bbga(0,&H00&,&HAA&)}က က"},
        {"ring reset 7bbga", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\7bbs4\\7bbga(0,&H00&,&HAA&)}\n", "Box", "", "{\\7bbva(&H80&)\\7bbga()}က က", "", "{\\box1\\boxp3\\7bbs4\\7bbga(0,&H00&,&HAA&)}က က"},
        {"ring family 7bbvc", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\7bbs4\\7bbvc(&HFF0000&,&H0000FF&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\7bbs4\\7bbvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 7bbvc", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\7bbs4\\7bbvc(&HFF0000&,&H0000FF&)}\n", "Box", "", "{\\7bbvc(&H00FF00&)\\7bbvc}က က", "", "{\\box1\\boxp3\\7bbs4\\7bbvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring family 7bbva", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\7bbs4\\7bbva(&H00&,&HAA&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\7bbs4\\7bbva(&H00&,&HAA&)}က က"},
        {"ring reset 7bbva", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\7bbs4\\7bbva(&H00&,&HAA&)}\n", "Box", "", "{\\7bbva(&H80&)\\7bbva}က က", "", "{\\box1\\boxp3\\7bbs4\\7bbva(&H00&,&HAA&)}က က"},
        {"ring family 8bgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\8bs4\\8bgrd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\8bs4\\8bgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 8bgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\8bs4\\8bgrd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\8bvc(&H00FF00&)\\8bgrd()}က က", "", "{\\8bs4\\8bgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 8bga", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\8bs4\\8bga(0,&H00&,&HAA&)}\n", "Default", "", "က က", "", "{\\8bs4\\8bga(0,&H00&,&HAA&)}က က"},
        {"ring reset 8bga", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\8bs4\\8bga(0,&H00&,&HAA&)}\n", "Default", "", "{\\8bva(&H80&)\\8bga()}က က", "", "{\\8bs4\\8bga(0,&H00&,&HAA&)}က က"},
        {"ring family 8bvc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\8bs4\\8bvc(&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\8bs4\\8bvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 8bvc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\8bs4\\8bvc(&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\8bvc(&H00FF00&)\\8bvc}က က", "", "{\\8bs4\\8bvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring family 8bva", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\8bs4\\8bva(&H00&,&HAA&)}\n", "Default", "", "က က", "", "{\\8bs4\\8bva(&H00&,&HAA&)}က က"},
        {"ring reset 8bva", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\8bs4\\8bva(&H00&,&HAA&)}\n", "Default", "", "{\\8bva(&H80&)\\8bva}က က", "", "{\\8bs4\\8bva(&H00&,&HAA&)}က က"},
        {"ring family 8bpgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\8bs4\\8bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\8bs4\\8bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 8bpgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\8bs4\\8bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\8bpgrd(0,0,640,360,90,&H00FF00&,&HFFFFFF&)\\8bpgrd()}က က", "", "{\\8bs4\\8bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 8bbgrd", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\8bbs4\\8bbgrd(45,&HFF0000&,&H0000FF&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\8bbs4\\8bbgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 8bbgrd", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\8bbs4\\8bbgrd(45,&HFF0000&,&H0000FF&)}\n", "Box", "", "{\\8bbvc(&H00FF00&)\\8bbgrd()}က က", "", "{\\box1\\boxp3\\8bbs4\\8bbgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 8bbga", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\8bbs4\\8bbga(0,&H00&,&HAA&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\8bbs4\\8bbga(0,&H00&,&HAA&)}က က"},
        {"ring reset 8bbga", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\8bbs4\\8bbga(0,&H00&,&HAA&)}\n", "Box", "", "{\\8bbva(&H80&)\\8bbga()}က က", "", "{\\box1\\boxp3\\8bbs4\\8bbga(0,&H00&,&HAA&)}က က"},
        {"ring family 8bbvc", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\8bbs4\\8bbvc(&HFF0000&,&H0000FF&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\8bbs4\\8bbvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 8bbvc", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\8bbs4\\8bbvc(&HFF0000&,&H0000FF&)}\n", "Box", "", "{\\8bbvc(&H00FF00&)\\8bbvc}က က", "", "{\\box1\\boxp3\\8bbs4\\8bbvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring family 8bbva", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\8bbs4\\8bbva(&H00&,&HAA&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\8bbs4\\8bbva(&H00&,&HAA&)}က က"},
        {"ring reset 8bbva", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\8bbs4\\8bbva(&H00&,&HAA&)}\n", "Box", "", "{\\8bbva(&H80&)\\8bbva}က က", "", "{\\box1\\boxp3\\8bbs4\\8bbva(&H00&,&HAA&)}က က"},
        {"ring family 9bgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\9bs4\\9bgrd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\9bs4\\9bgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 9bgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\9bs4\\9bgrd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\9bvc(&H00FF00&)\\9bgrd()}က က", "", "{\\9bs4\\9bgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 9bga", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\9bs4\\9bga(0,&H00&,&HAA&)}\n", "Default", "", "က က", "", "{\\9bs4\\9bga(0,&H00&,&HAA&)}က က"},
        {"ring reset 9bga", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\9bs4\\9bga(0,&H00&,&HAA&)}\n", "Default", "", "{\\9bva(&H80&)\\9bga()}က က", "", "{\\9bs4\\9bga(0,&H00&,&HAA&)}က က"},
        {"ring family 9bvc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\9bs4\\9bvc(&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\9bs4\\9bvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 9bvc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\9bs4\\9bvc(&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\9bvc(&H00FF00&)\\9bvc}က က", "", "{\\9bs4\\9bvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring family 9bva", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\9bs4\\9bva(&H00&,&HAA&)}\n", "Default", "", "က က", "", "{\\9bs4\\9bva(&H00&,&HAA&)}က က"},
        {"ring reset 9bva", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\9bs4\\9bva(&H00&,&HAA&)}\n", "Default", "", "{\\9bva(&H80&)\\9bva}က က", "", "{\\9bs4\\9bva(&H00&,&HAA&)}က က"},
        {"ring family 9bpgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\9bs4\\9bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\9bs4\\9bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 9bpgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\9bs4\\9bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\9bpgrd(0,0,640,360,90,&H00FF00&,&HFFFFFF&)\\9bpgrd()}က က", "", "{\\9bs4\\9bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 9bbgrd", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\9bbs4\\9bbgrd(45,&HFF0000&,&H0000FF&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\9bbs4\\9bbgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 9bbgrd", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\9bbs4\\9bbgrd(45,&HFF0000&,&H0000FF&)}\n", "Box", "", "{\\9bbvc(&H00FF00&)\\9bbgrd()}က က", "", "{\\box1\\boxp3\\9bbs4\\9bbgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 9bbga", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\9bbs4\\9bbga(0,&H00&,&HAA&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\9bbs4\\9bbga(0,&H00&,&HAA&)}က က"},
        {"ring reset 9bbga", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\9bbs4\\9bbga(0,&H00&,&HAA&)}\n", "Box", "", "{\\9bbva(&H80&)\\9bbga()}က က", "", "{\\box1\\boxp3\\9bbs4\\9bbga(0,&H00&,&HAA&)}က က"},
        {"ring family 9bbvc", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\9bbs4\\9bbvc(&HFF0000&,&H0000FF&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\9bbs4\\9bbvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 9bbvc", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\9bbs4\\9bbvc(&HFF0000&,&H0000FF&)}\n", "Box", "", "{\\9bbvc(&H00FF00&)\\9bbvc}က က", "", "{\\box1\\boxp3\\9bbs4\\9bbvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring family 9bbva", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\9bbs4\\9bbva(&H00&,&HAA&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\9bbs4\\9bbva(&H00&,&HAA&)}က က"},
        {"ring reset 9bbva", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\9bbs4\\9bbva(&H00&,&HAA&)}\n", "Box", "", "{\\9bbva(&H80&)\\9bbva}က က", "", "{\\box1\\boxp3\\9bbs4\\9bbva(&H00&,&HAA&)}က က"},
        {"ring family 10bgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\10bs4\\10bgrd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\10bs4\\10bgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 10bgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\10bs4\\10bgrd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\10bvc(&H00FF00&)\\10bgrd()}က က", "", "{\\10bs4\\10bgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 10bga", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\10bs4\\10bga(0,&H00&,&HAA&)}\n", "Default", "", "က က", "", "{\\10bs4\\10bga(0,&H00&,&HAA&)}က က"},
        {"ring reset 10bga", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\10bs4\\10bga(0,&H00&,&HAA&)}\n", "Default", "", "{\\10bva(&H80&)\\10bga()}က က", "", "{\\10bs4\\10bga(0,&H00&,&HAA&)}က က"},
        {"ring family 10bvc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\10bs4\\10bvc(&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\10bs4\\10bvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 10bvc", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\10bs4\\10bvc(&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\10bvc(&H00FF00&)\\10bvc}က က", "", "{\\10bs4\\10bvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring family 10bva", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\10bs4\\10bva(&H00&,&HAA&)}\n", "Default", "", "က က", "", "{\\10bs4\\10bva(&H00&,&HAA&)}က က"},
        {"ring reset 10bva", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\10bs4\\10bva(&H00&,&HAA&)}\n", "Default", "", "{\\10bva(&H80&)\\10bva}က က", "", "{\\10bs4\\10bva(&H00&,&HAA&)}က က"},
        {"ring family 10bpgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\10bs4\\10bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\10bs4\\10bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 10bpgrd", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\10bs4\\10bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\10bpgrd(0,0,640,360,90,&H00FF00&,&HFFFFFF&)\\10bpgrd()}က က", "", "{\\10bs4\\10bpgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 10bbgrd", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\10bbs4\\10bbgrd(45,&HFF0000&,&H0000FF&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\10bbs4\\10bbgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 10bbgrd", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\10bbs4\\10bbgrd(45,&HFF0000&,&H0000FF&)}\n", "Box", "", "{\\10bbvc(&H00FF00&)\\10bbgrd()}က က", "", "{\\box1\\boxp3\\10bbs4\\10bbgrd(45,&HFF0000&,&H0000FF&)}က က"},
        {"ring family 10bbga", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\10bbs4\\10bbga(0,&H00&,&HAA&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\10bbs4\\10bbga(0,&H00&,&HAA&)}က က"},
        {"ring reset 10bbga", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\10bbs4\\10bbga(0,&H00&,&HAA&)}\n", "Box", "", "{\\10bbva(&H80&)\\10bbga()}က က", "", "{\\box1\\boxp3\\10bbs4\\10bbga(0,&H00&,&HAA&)}က က"},
        {"ring family 10bbvc", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\10bbs4\\10bbvc(&HFF0000&,&H0000FF&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\10bbs4\\10bbvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring reset 10bbvc", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\10bbs4\\10bbvc(&HFF0000&,&H0000FF&)}\n", "Box", "", "{\\10bbvc(&H00FF00&)\\10bbvc}က က", "", "{\\box1\\boxp3\\10bbs4\\10bbvc(&HFF0000&,&H0000FF&)}က က"},
        {"ring family 10bbva", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\10bbs4\\10bbva(&H00&,&HAA&)}\n", "Box", "", "က က", "", "{\\box1\\boxp3\\10bbs4\\10bbva(&H00&,&HAA&)}က က"},
        {"ring reset 10bbva", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\boxp3\\10bbs4\\10bbva(&H00&,&HAA&)}\n", "Box", "", "{\\10bbva(&H80&)\\10bbva}က က", "", "{\\box1\\boxp3\\10bbs4\\10bbva(&H00&,&HAA&)}က က"},
        {"waiting family \\3svc(&HFF0000&,&H0000FF&)", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3svc(&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\kt100\\kf100}<က|စူဇူကီ><က||ဆာတို့>", "", "{\\3svc(&HFF0000&,&H0000FF&)\\kt100\\kf100}<က|စူဇူကီ><က||ဆာတို့>"},
        {"waiting inherited reset \\3svc(&HFF0000&,&H0000FF&)", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3svc(&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\3sc&H00FF00&\\3svc\\kt100\\kf100}<က|စူဇူကီ><က||ဆာတို့>", "", "{\\3svc(&HFF0000&,&H0000FF&)\\kt100\\kf100}<က|စူဇူကီ><က||ဆာတို့>"},
        {"waiting family \\3sgrd(0,&HFF0000&,&H0000FF&)", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3sgrd(0,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\kt100\\kf100}<က|စူဇူကီ><က||ဆာတို့>", "", "{\\3sgrd(0,&HFF0000&,&H0000FF&)\\kt100\\kf100}<က|စူဇူကီ><က||ဆာတို့>"},
        {"waiting inherited reset \\3sgrd(0,&HFF0000&,&H0000FF&)", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3sgrd(0,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\3sc&H00FF00&\\3sgrd()\\kt100\\kf100}<က|စူဇူကီ><က||ဆာတို့>", "", "{\\3sgrd(0,&HFF0000&,&H0000FF&)\\kt100\\kf100}<က|စူဇူကီ><က||ဆာတို့>"},
        {"global alpha inherited", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\pgrdalpha(0,0,640,360,0,&H00&,&HAA&)}\n", "Default", "", "က က", "", "{\\pgrdalpha(0,0,640,360,0,&H00&,&HAA&)}က က"},
        {"global alpha empty restore", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\pgrdalpha(0,0,640,360,0,&H00&,&HAA&)}\n", "Default", "", "{\\pgrdalpha(0,0,640,360,90,&HFF&,&H00&)\\pgrdalpha()}က က", "", "{\\pgrdalpha(0,0,640,360,0,&H00&,&HAA&)}က က"},
        {"color first wins single comment", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\grd(45,&HFF0000&,&H0000FF&)\\vc(&HFFFFFF&,&H000000&,&HFFFFFF&,&H000000&)\\1grd(90,&H00FF00&,&HFFFFFF&)}\n", "Default", "", "က က", "", "{\\grd(45,&HFF0000&,&H0000FF&)}က က"},
        {"color first wins split comments", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\grd(45,&HFF0000&,&H0000FF&)}\nComment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\vc(&HFFFFFF&,&H000000&,&HFFFFFF&,&H000000&)}\nComment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1grd(90,&H00FF00&,&HFFFFFF&)}\n", "Default", "", "က က", "", "{\\grd(45,&HFF0000&,&H0000FF&)}က က"},
        {"vector first wins split comments", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\vc(&HFFFFFF&,&H000000&,&HFFFFFF&,&H000000&)}\nComment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\grd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\vc(&HFFFFFF&,&H000000&,&HFFFFFF&,&H000000&)}က က"},
        {"attached repeated family first wins", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\grd(45,&HFF0000&,&H0000FF&)\\1grd(90,&H00FF00&,&HFFFFFF&)}\n", "Default", "", "က က", "", "{\\grd(45,&HFF0000&,&H0000FF&)}က က"},
        {"positioned first wins", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)\\grd(45,&HFF0000&,&H0000FF&)\\vc(&HFFFFFF&,&H000000&,&HFFFFFF&,&H000000&)}\n", "Default", "", "က က", "", "{\\pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}က က"},
        {"alpha first wins independent", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1gra(0,&H00&,&HAA&)\\va(&H00&,&HAA&,&H00&,&HAA&)\\grd(45,&HFF0000&,&H0000FF&)\\vc(&HFFFFFF&,&H000000&,&HFFFFFF&,&H000000&)}\n", "Default", "", "က က", "", "{\\1gra(0,&H00&,&HAA&)\\grd(45,&HFF0000&,&H0000FF&)}က က"},
        {"vector alpha first wins", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\va(&H00&,&HAA&,&H00&,&HAA&)\\1gra(0,&H00&,&HAA&)}\n", "Default", "", "က က", "", "{\\va(&H00&,&HAA&,&H00&,&HAA&)}က က"},
        {"global alpha first wins independent", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\pgrdalpha(0,0,640,360,0,&H00&,&HAA&)}\nComment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\pgrdalpha(0,0,640,360,90,&HFF&,&H00&)\\1gra(0,&H00&,&HAA&)\\grd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\pgrdalpha(0,0,640,360,0,&H00&,&HAA&)\\1gra(0,&H00&,&HAA&)\\grd(45,&HFF0000&,&H0000FF&)}က က"},
        {"invalid gradient leaves slot free", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\grd(nan,&HFF0000&,&H0000FF&)\\vc(bad)\\pgrd(0,0,640,360,0,bad)\\grd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\grd(45,&HFF0000&,&H0000FF&)}က က"},
        {"empty reset leaves slot free", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\grd()\\vc\\grd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "က က", "", "{\\grd(45,&HFF0000&,&H0000FF&)}က က"},
        {"outline aliases share source", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3grd(0,&HFF0000&,&H0000FF&)\\1bvc(&H00FF00&)}\n", "Default", "", "က က", "", "{\\3grd(0,&HFF0000&,&H0000FF&)}က က"},
        {"outline reverse aliases share source", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1bpgrd(0,0,640,360,0,&HFF0000&,&H0000FF&)\\3grd(90,&H00FF00&,&HFFFFFF&)}\n", "Default", "", "က က", "", "{\\1bpgrd(0,0,640,360,0,&HFF0000&,&H0000FF&)}က က"},
        {"box aliases share source", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\bbs4\\bbgrd(0,&HFF0000&,&H0000FF&)\\1bbvc(&H00FF00&)}\n", "Box", "", "က က", "", "{\\box1\\bbs4\\bbgrd(0,&HFF0000&,&H0000FF&)}က က"},
        {"waiting first source wins", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\3sgrd(0,&HFF0000&,&H0000FF&)\\3svc(&H00FF00&)}\n", "Default", "", "{\\kt100\\kf100}<က|စူဇူကီ><က||ဆာတို့>", "", "{\\3sgrd(0,&HFF0000&,&H0000FF&)\\kt100\\kf100}<က|စူဇူကီ><က||ဆာတို့>"},
        {"solid remains fallback not gradient replacement", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\grd(45,&HFF0000&,&H0000FF&)}\nComment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2bs3\\2bc&H00FF00&}\n", "Default", "", "က က", "", "{\\2bs3\\2bc&H00FF00&\\grd(45,&HFF0000&,&H0000FF&)}က က"},
        {"native solid fallback independent of gradient", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2bs4\\2bgrd(0,&HFF0000&,&H0000FF&)}\nComment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\2bc&H00FF00&}\n", "Default", "", "က က", "", "{\\2bs4\\2bc&H00FF00&\\2bgrd(0,&HFF0000&,&H0000FF&)}က က"},
        {"inline vector replaces inherited attached", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\grd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\vc(&HFFFFFF&,&H000000&,&HFFFFFF&,&H000000&)}က က", "", "{\\vc(&HFFFFFF&,&H000000&,&HFFFFFF&,&H000000&)}က က"},
        {"inline attached replaces inherited vector", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\vc(&HFFFFFF&,&H000000&,&HFFFFFF&,&H000000&)}\n", "Default", "", "{\\grd(45,&HFF0000&,&H0000FF&)}က က", "", "{\\grd(45,&HFF0000&,&H0000FF&)}က က"},
        {"inline latest same family remains unchanged", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\grd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\1grd(90,&H00FF00&,&HFFFFFF&)\\grd(45,&HFF0000&,&H0000FF&)}က က", "", "{\\grd(45,&HFF0000&,&H0000FF&)}က က"},
        {"inline image replaces inherited gradient", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\grd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\img(style-test.png)}က က", "", "{\\img(style-test.png)}က က"},
        {"image rejected as style property", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\img(style-test.png)}\n", "Default", "", "က က", "", "က က"},
        {"bare reset inherited paint", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\grd(45,&HFF0000&,&H0000FF&)\\1gra(0,&H00&,&HAA&)}\n", "Default", "", "{\\1grd(90,&H00FF00&,&HFFFFFF&)\\r}က က", "", "{\\grd(45,&HFF0000&,&H0000FF&)\\1gra(0,&H00&,&HAA&)}က က"},
        {"named style gradient defaults", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\grd(45,&HFF0000&,&H0000FF&)}\nComment: 0,0:00:00.00,0:00:00.00,Other,,0,0,0,mangetsu-style,{\\vc(&HFFFFFF&,&H000000&,&HFFFFFF&,&H000000&)}\n", "Default", "", "{\\rOther}က က", "", "{\\rOther\\vc(&HFFFFFF&,&H000000&,&HFFFFFF&,&H000000&)}က က"},
        {"named style no defaults", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\grd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\rOther}က က", "", "{\\rOther}က က"},
        {"empty scalar color restores inherited paint", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\grd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\1c&H00FF00&\\1c}က က", "", "{\\grd(45,&HFF0000&,&H0000FF&)}က က"},
        {"empty scalar alpha restores only alpha", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\1gra(0,&H00&,&HAA&)\\grd(45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\1a&H80&\\1a}က က", "", "{\\1gra(0,&H00&,&HAA&)\\grd(45,&HFF0000&,&H0000FF&)}က က"},
        {"actor solid overrides inherited gradient", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\grd(45,&HFF0000&,&H0000FF&)}\nComment: 0,0:00:00.00,0:00:00.00,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1c&H0000FF&}\n", "Default", "Nene", "က က", "", "{\\1c&H0000FF&}က က"},
        {"actor gradient overrides inherited gradient", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\vc(&HFFFFFF&,&H000000&,&HFFFFFF&,&H000000&)}\nComment: 0,0:00:00.00,0:00:00.00,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1grd(90,&H00FF00&,&HFFFFFF&)}\n", "Default", "Nene", "က က", "", "{\\1grd(90,&H00FF00&,&HFFFFFF&)}က က"},
        {"actor effective gradient reset", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\grd(45,&HFF0000&,&H0000FF&)}\nComment: 0,0:00:00.00,0:00:00.00,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1grd(90,&H00FF00&,&HFFFFFF&)}\n", "Default", "Nene", "{\\vc(&HFFFFFF&,&H000000&,&HFFFFFF&,&H000000&)\\vc}က က", "", "{\\1grd(90,&H00FF00&,&HFFFFFF&)}က က"},
        {"actor effective solid reset", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\grd(45,&HFF0000&,&H0000FF&)}\nComment: 0,0:00:00.00,0:00:00.00,Default,Nene,0,0,0,mangetsu-colorcoding,{\\1c&H0000FF&}\n", "Default", "Nene", "{\\vc(&HFFFFFF&,&H000000&,&HFFFFFF&,&H000000&)\\vc}က က", "", "{\\1c&H0000FF&}က က"},
        {"inherited native vertical furigana paint", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\vert1\\vtype3\\furis70\\grd(45,&HFF0000&,&H0000FF&)\\1gra(0,&H00&,&HAA&)}\n", "Default", "", "<က|စူဇူကီ><က||ဆာတို့>", "", "{\\vert1\\vtype3\\furis70\\grd(45,&HFF0000&,&H0000FF&)\\1gra(0,&H00&,&HAA&)}<က|စူဇူကီ><က||ဆာတို့>"},
        {"positioned movement clipped and faded", "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)}\n", "Default", "", "{\\move(100,100,400,200)\\clip(0,0,600,350)\\fad(1000,1000)}က က", "", "{\\pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)\\move(100,100,400,200)\\clip(0,0,600,350)\\fad(1000,1000)}က က"},
        {"box gradients transformed", "Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\\box1\\bbs4\\1bbgrd(0,&HFF0000&,&H0000FF&)\\4gra(90,&H00&,&H80&)}\n", "Box", "", "{\\frx20\\fry15\\frz10}က က", "", "{\\box1\\bbs4\\1bbgrd(0,&HFF0000&,&H0000FF&)\\4gra(90,&H00&,&H80&)\\frx20\\fry15\\frz10}က က"},
        {"wrapping q0", META("Default", "\\q0"), "Default", "", LONG_TEXT, "", "{\\q0}" LONG_TEXT},
        {"wrapping q1", META("Default", "\\q1"), "Default", "", LONG_TEXT, "", "{\\q1}" LONG_TEXT},
        {"wrapping q2", META("Default", "\\q2"), "Default", "", LONG_TEXT, "", "{\\q2}" LONG_TEXT},
        {"wrapping q3", META("Default", "\\q3"), "Default", "", LONG_TEXT, "", "{\\q3}" LONG_TEXT},
        {"inline wrapping overrides", META("Default", "\\q2"), "Default", "", "{\\q0}" LONG_TEXT, "", "{\\q0}" LONG_TEXT},
        {"bare q restores initial style", META("Default", "\\q2"), "Default", "", "{\\q0\\q}" LONG_TEXT, "", "{\\q2}" LONG_TEXT},
        {"bare q without metadata", "", "Default", "", "{\\q2\\q}" LONG_TEXT, "", "{\\q0}" LONG_TEXT},
        {"appearance reset does not reflow wrap", META("Default", "\\q2"), "Default", "", "{\\q1\\r}" LONG_TEXT, "", "{\\q1}" LONG_TEXT},
        {"named appearance reset ignores target layout", META("Default", "\\q2") META("Other", "\\q1"), "Default", "", "{\\rOther}" LONG_TEXT, "", "{\\rOther\\q2}" LONG_TEXT},
        {"bare q after named reset uses initial layout", META("Default", "\\q2") META("Other", "\\q1"), "Default", "", "{\\rOther\\q0\\q}" LONG_TEXT, "", "{\\rOther\\q2}" LONG_TEXT},
        {"vertical defaults", META("Vertical", "\\vert1\\vtype3\\vdir2\\vsp2\\vcolsp8"), "Vertical", "", VERTICAL_TEXT, "", "{\\vert1\\vtype3\\vdir2\\vsp2\\vcolsp8}" VERTICAL_TEXT},
        {"vertical profile automatic", META("Vertical", "\\vert1\\vtype0"), "Vertical", "", VERTICAL_TEXT, "", "{\\vert1\\vtype0}" VERTICAL_TEXT},
        {"vertical profile CJK", META("Vertical", "\\vert1\\vtype1"), "Vertical", "", VERTICAL_TEXT, "", "{\\vert1\\vtype1}" VERTICAL_TEXT},
        {"vertical profile Latin", META("Vertical", "\\vert1\\vtype2"), "Vertical", "", VERTICAL_TEXT, "", "{\\vert1\\vtype2}" VERTICAL_TEXT},
        {"vertical leftward columns", META("Vertical", "\\vert1\\vdir1\\vsp3\\vcolsp12"), "Vertical", "", VERTICAL_LONG, "", "{\\vert1\\vdir1\\vsp3\\vcolsp12}" VERTICAL_LONG},
        {"vertical actor color coexists", RED META("Default", "\\vert1\\vtype3\\furis70"), "Default", "Nene", RUBY, RED, "{\\vert1\\vtype3\\furis70}" RUBY},
        {"vertical Myanmar shaping", META("Default", "\\vert1\\vtype3\\vsp2"), "Default", "", RUBY, "", "{\\vert1\\vtype3\\vsp2}" RUBY},
        {"vertical q2 wrapping", META("Vertical", "\\vert1\\q2"), "Vertical", "", VERTICAL_LONG, "", "{\\vert1\\q2}" VERTICAL_LONG},
        {"vertical q0 wrapping", META("Vertical", "\\vert1\\q0"), "Vertical", "", VERTICAL_LONG, "", "{\\vert1\\q0}" VERTICAL_LONG},
        {"early inline vertical override", META("Vertical", "\\vert1\\vtype3\\vdir2\\vsp2\\vcolsp8"), "Vertical", "", "{\\vert0\\vtype1\\vdir1\\vsp4\\vcolsp3}" VERTICAL_TEXT, "", "{\\vert0\\vtype1\\vdir1\\vsp4\\vcolsp3}" VERTICAL_TEXT},
        {"late vertical controls ignored", META("Vertical", "\\vert1"), "Vertical", "", "漢{\\vert0\\vtype3\\vdir2\\vsp10\\vcolsp10}字", "", "{\\vert1}漢字"},
        {"transformed vertical controls ignored", META("Vertical", "\\vert1"), "Vertical", "", "{\\t(0,1000,\\vert0\\vtype3\\vdir2\\vsp10\\vcolsp10)}漢字", "", "{\\vert1}漢字"},
        {"named reset preserves vertical selection", META("Vertical", "\\vert1\\vtype3") META("Other", "\\vert0"), "Vertical", "", "{\\rOther}" RUBY, "", "{\\vert1\\vtype3\\rOther}" RUBY},
        {"layout metadata merges properties", META("Vertical", "\\vert1\\vdir2\\q2") META("Vertical", "\\vtype3\\vsp2\\q1\\vcolsp8"), "Vertical", "", VERTICAL_TEXT, "", "{\\vert1\\vdir2\\vtype3\\vsp2\\q1\\vcolsp8}" VERTICAL_TEXT},
        {"invalid layout metadata ignored", META("Default", "\\q4\\vert2\\vtype4\\vdir3\\vsp(nan)\\vcolsp(inf)\\vertex1"), "Default", "", LONG_TEXT, "", LONG_TEXT},
        {"inherited Burmese furigana", F70, "Default", "", RUBY, "", "{\\furis70}" RUBY},
        {"native defaults", "", "Default", "", RUBY, "", "{\\furis50}" RUBY},
        {"inline override", F70, "Default", "", "{\\furis50}" RUBY, "", "{\\furis50}" RUBY},
        {"parameterless scale restores inheritance", F70, "Default", "",
         "{\\furis50}" RUBY "{\\furis}" RUBY,
         "", "{\\furis50}" RUBY "{\\furis70}" RUBY},
        {"parameterless X preserves current Y", META("Default", "\\furisx70\\furisy60"),
         "Default", "", "{\\furis40\\furisx}" RUBY,
         "", "{\\furisx70\\furisy40}" RUBY},
        {"parameterless Y preserves current X", META("Default", "\\furisx70\\furisy60"),
         "Default", "", "{\\furis40\\furisy}" RUBY,
         "", "{\\furisx40\\furisy60}" RUBY},
        {"parameterless asymmetric scale", META("Default", "\\furisx70\\furisy60"),
         "Default", "", "{\\furis40\\furis}" RUBY,
         "", "{\\furisx70\\furisy60}" RUBY},
        {"native parameterless scale unchanged", "", "Default", "",
         "{\\furis80\\furis}" RUBY, "", "{\\furis50}" RUBY},
        {"all parameterless furigana controls", META("Default", "\\furifsp2\\furistyle1"
         "\\furipos(1,-2)\\furiap0\\furiplaceauto1\\furichangepos2"), "Default", "",
         "{\\furifsp0\\furistyle0\\furipos(9,9)\\furiap1\\furiplaceauto0\\furichangepos0}"
         "{\\furifsp\\furistyle\\furipos\\furiap\\furiplaceauto\\furichangepos}" RUBY,
         "", "{\\furifsp2\\furistyle1\\furipos(1,-2)\\furiap0\\furiplaceauto1\\furichangepos2}" RUBY},
        {"parameterless enable restores disabled style", META("Default", "\\furi0"),
         "Default", "", "{\\furi1}" RUBY "{\\furi}" RUBY,
         "", "{\\furi1}" RUBY "{\\furi0}" RUBY},
        {"bare reset", F70, "Default", "", "{\\furis30}" RUBY "{\\r}" RUBY,
         "", "{\\furis30}" RUBY "{\\r\\furis70}" RUBY},
        {"named reset and return", F70 META("Other", "\\furis40"), "Default", "",
         RUBY "{\\rOther}" RUBY "{\\r}" RUBY, "",
         "{\\furis70}" RUBY "{\\rOther\\furis40}" RUBY "{\\r\\furis70}" RUBY},
        {"named reset to unextended style", F70, "Default", "", "{\\rOther}" RUBY,
         "", "{\\rOther}" RUBY},
        {"named reset updates parameterless defaults", F70 META("Other", "\\furis40"),
         "Default", "", "{\\rOther\\furis20\\furis}" RUBY,
         "", "{\\rOther\\furis40}" RUBY},
        {"merge declarations", F70 META("Default", "\\2bs3\\2bc&HFFAA00&"), "Default", "",
         RUBY, "", "{\\furis70\\2bs3\\2bc&HFFAA00&}" RUBY},
        {"last property wins", F70 META("Default", "\\furis60\\2bs4") META("Default", "\\furis50\\2bc&H0000FF&"),
         "Default", "", RUBY, "", "{\\furis50\\2bs4\\2bc&H0000FF&}" RUBY},
        {"duplicate within one block", META("Default", "\\furis80\\furis70\\2bs4\\2bs2"),
         "Default", "", RUBY, "", "{\\furis70\\2bs2}" RUBY},
        {"individual axes merge", F70 META("Default", "\\furisx40"),
         "Default", "", RUBY, "", "{\\furisx40\\furisy70}" RUBY},
        {"native value remains an explicit assignment", META("Default", "\\furis50\\2bs0"),
         "Default", "", RUBY, "", "{\\furis50\\2bs0}" RUBY},
        {"relative defaults resolve once", F70 META("Default", "\\furis~+10\\2bs3\\2bs~+2"),
         "Default", "", RUBY, "", "{\\furis80\\2bs5}" RUBY},
        {"styles independent", F70 META("Default", "\\2bs5") META("Other", "\\furis40"),
         "Other", "", RUBY, "", "{\\furis40}" RUBY},
        {"normal style-name rules", META("***default", "\\furis70"),
         "Default", "", RUBY, "", "{\\furis70}" RUBY},
        {"unknown style ignored", META("Missing", "\\furis70"), "Default", "", RUBY, "", RUBY},
        {"unknown style does not close actor block", META("Missing", "\\furis70") RED,
         "Default", "Nene", RUBY, RED, RUBY},
        {"malformed and forbidden tags", F70 META("Default",
         "\\furis(nan)\\furistyle9\\2bs(inf)\\2bc&HGG&\\pos(1,1)\\move(0,0,5,5)"
         "\\t(\\furis10\\2bs9)\\fad(1000,1000)\\rOther\\fnMissing\\unknown70"
         "\\furisextra(10)\\furiposx(9,9)\\boxpadding100"),
         "Default", "", RUBY, "", "{\\furis70}" RUBY},
        {"unclosed block ignored", F70
         "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\furis20\n",
         "Default", "", RUBY, "", "{\\furis70}" RUBY},
        {"ordinary comment ends block",
         "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,notes,hello\n" F70 RED,
         "Default", "Nene", RUBY, "", RUBY},
        {"exact effect identifier",
         "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,{mangetsu-style},{\\furis70}\n",
         "Default", "", RUBY, "", RUBY},
        {"late metadata ignored", "", "Default", "", RUBY "\n" F70 RED, "", RUBY},
        {"later format does not reopen block", "", "Default", "", RUBY
         "\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n" F70,
         "", RUBY},
        {"animation starts from inheritance", F70, "Default", "", "{\\t(0,1000,\\furis50)}" RUBY,
         "", "{\\furis70\\t(0,1000,\\furis50)}" RUBY},
        {"gyaku-furi inherits", F70, "Default", "", "<က||ဆာတို့>", "", "{\\furis70}<က||ဆာတို့>"},
        {"both readings inherit", F70, "Default", "", "<က|စူဇူကီ|ဆာတို့>",
         "", "{\\furis70}<က|စူဇူကီ|ဆာတို့>"},
        {"native vertical readings inherit", F70, "Default", "",
         "{\\vert1}<က|စူဇူကီ|ဆာတို့>", "", "{\\vert1\\furis70}<က|စူဇူကီ|ဆာတို့>"},
        {"reading karaoke inherits", F70, "Default", "", "<က|{\\kf30}စူ{\\kf30}ဇူကီ>",
         "", "{\\furis70}<က|{\\kf30}စူ{\\kf30}ဇူကီ>"},
        {"all furigana controls", META("Default", "\\furisx70\\furisy60\\furifsp2\\furipos(1,-2)"
         "\\furistyle1\\furiap0\\furiplaceauto1\\furichangepos2"), "Default", "", RUBY, "",
         "{\\furisx70\\furisy60\\furifsp2\\furipos(1,-2)\\furistyle1\\furiap0\\furiplaceauto1\\furichangepos2}" RUBY},
        {"furigana disabled", META("Default", "\\furi0"), "Default", "", RUBY, "", "{\\furi0}" RUBY},
        {"flat border and blur", META("Default", "\\1bsx4\\1bsy3\\1bc&HFF0000&\\1ba&H40&"
         "\\2bs2\\2bblur1\\2bbe1\\2bc&H00FF00&\\2ba&H80&"), "Default", "", RUBY, "",
         "{\\1bsx4\\1bsy3\\1bc&HFF0000&\\1ba&H40&\\2bs2\\2bblur1\\2bbe1\\2bc&H00FF00&\\2ba&H80&}" RUBY},
        {"box defaults", META("Box", "\\box1\\boxpx10\\boxpy5\\boxr3\\bbs2\\bbc&HFF0000&\\bba&H20&"
         "\\2bbs1\\2bbc&H00FF00&\\2bba&H60&"), "Box", "", RUBY, "",
         "{\\box1\\boxpx10\\boxpy5\\boxr3\\bbs2\\bbc&HFF0000&\\bba&H20&\\2bbs1\\2bbc&H00FF00&\\2bba&H60&}" RUBY},
        {"boxp uses existing single-value semantics", META("Box", "\\boxp(10,20,5,5)"),
         "Box", "", RUBY, "", "{\\boxp(10,20,5,5)}" RUBY},
        {"metadata boxes do not enter ordinary repeated-event reuse",
         META("Default", "\\box1\\boxpx10\\boxpy5\\boxr3\\bbs2"), "Default", "",
         "{\\pos(320,180)}က\nDialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,{\\pos(320,180)}က",
         "", "{\\pos(320,180)\\box1\\boxpx10\\boxpy5\\boxr3\\bbs2}က\n"
         "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,{\\pos(320,180)\\box1\\boxpx10\\boxpy5\\boxr3\\bbs2}က"},
        {"actor alone", RED, "Default", "Nene", RUBY, "", "{\\1c&H0000FF&}" RUBY},
        {"style before actor", F70 RED, "Default", "Nene", RUBY, RED, "{\\furis70}" RUBY},
        {"actor before style", RED F70, "Default", "Nene", RUBY, RED, "{\\furis70}" RUBY},
        {"second actor independent", RED F70 BLUE, "Default", "Blue", RUBY,
         "", "{\\furis70\\1c&HFF0000&}" RUBY},
        {"simultaneous actors independent", RED F70 BLUE, "Default", "Nene",
         "{\\pos(160,180)}" RUBY
         "\nDialogue: 0,0:00:00.00,0:00:10.00,Default,Blue,0,0,0,,{\\pos(480,180)}" RUBY,
         "", "{\\pos(160,180)\\furis70\\1c&H0000FF&}" RUBY
         "\nDialogue: 0,0:00:00.00,0:00:10.00,Default,Blue,0,0,0,,{\\pos(480,180)\\furis70\\1c&HFF0000&}" RUBY},
        {"furigana does not recolor ordinary text", RED F70, "Default", "Nene", "က", RED, "က"},
        {"actor wins shared border color", META("Default", "\\2bs3\\2bc&HFF0000&")
         ACTOR("Nene", "\\2bc&H0000FF&"), "Default", "Nene", RUBY, "", "{\\2bs3\\2bc&H0000FF&}" RUBY},
        {"explicit color keeps priority", RED F70, "Default", "Nene", "{\\1c&H00FF00&}" RUBY,
         RED, "{\\furis70\\1c&H00FF00&}" RUBY},
        {"bare reset keeps actor and style", RED F70, "Default", "Nene",
         "{\\1c&H00FF00&\\furis30}" RUBY "{\\r}" RUBY, RED,
         "{\\1c&H00FF00&\\furis30}" RUBY "{\\r\\furis70}" RUBY},
        {"named reset preserves actor escape hatch", RED F70 META("Other", "\\furis40"),
         "Default", "Nene", "{\\rOther}" RUBY, RED, "{\\rOther\\furis40}" RUBY},
        {"whitelist reset reapplies actor", RED F70 WHITELIST META("Other", "\\furis40"),
         "Default", "Nene", "{\\rOther}" RUBY, RED WHITELIST, "{\\rOther\\furis40}" RUBY},
        {"blank border reset uses effective defaults", META("Default", "\\2bs3\\2bc&HFF0000&")
         ACTOR("Nene", "\\2bc&H0000FF&"), "Default", "Nene", "{\\2bs8\\2bc&H00FF00&\\2bs\\2bc}" RUBY,
         "", "{\\2bs3\\2bc&H0000FF&}" RUBY},
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++)
        failed |= compare(lib, renderer, &cases[i]);
    failed |= host_metadata(lib, renderer);
    static const Case moving = {
        "positioned inherited fields across seeking",
        META("Default", "\\pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)\\pgrdalpha(0,0,640,360,0,&H00&,&HAA&)"),
        "Default", "", "{\\move(100,100,500,250,0,1000)}က က", "",
        "{\\pgrd(0,0,640,360,45,&HFF0000&,&H0000FF&)\\pgrdalpha(0,0,640,360,0,&H00&,&HAA&)\\move(100,100,500,250,0,1000)}က က"};
    const long long seek_times[] = {900, 100, 500, 900};
    for (int i = 0; i < 4; i++) failed |= compare_at(lib, renderer, &moving, seek_times[i]);


    // A script-level nonzero WrapStyle remains the fallback without metadata.
    ASS_Track *script_wrap = load(lib, "", "Default", "", "{\\q2\\q}" LONG_TEXT);
    ASS_Track *script_reference = load(lib, "", "Default", "", "{\\q3}" LONG_TEXT);
    if (script_wrap && script_reference) {
        script_wrap->WrapStyle = script_reference->WrapStyle = 3;
        for (int rgba = 0; rgba < 2; rgba++) {
            uint64_t a, b;
            if (!snapshot(renderer, script_wrap, rgba, &a) ||
                    !snapshot(renderer, script_reference, rgba, &b) || a != b)
                failed = 1;
        }
    } else failed = 1;
    ass_free_track(script_wrap); ass_free_track(script_reference);

    // Prove the wrapping probes overflow, rather than comparing two identical
    // unwrapped lines. Include both public outputs and native vertical layout.
    for (int vertical = 0; vertical < 2; vertical++) {
        ASS_Track *wrap = load(lib, vertical ? META("Vertical", "\\vert1\\q0") : META("Default", "\\q0"),
                              vertical ? "Vertical" : "Default", "", vertical ? VERTICAL_LONG : LONG_TEXT);
        ASS_Track *nowrap = load(lib, vertical ? META("Vertical", "\\vert1\\q2") : META("Default", "\\q2"),
                                vertical ? "Vertical" : "Default", "", vertical ? VERTICAL_LONG : LONG_TEXT);
        for (int rgba = 0; rgba < 2; rgba++) {
            uint64_t a, b;
            if (!wrap || !nowrap || !snapshot(renderer, wrap, rgba, &a) ||
                    !snapshot(renderer, nowrap, rgba, &b) || a == b) {
                fprintf(stderr, "wrapping probes must differ (vertical=%d RGBA=%d)\n", vertical, rgba);
                failed = 1;
            }
        }
        ass_free_track(wrap); ass_free_track(nowrap);
    }

    ASS_Track *scaled = load(lib, F70, "Default", "", RUBY);
    ASS_Track *native = load(lib, "", "Default", "", RUBY);
    uint64_t a, b;
    bool baselines = scaled && native && snapshot(renderer, scaled, false, &a) &&
                     snapshot(renderer, native, false, &b);
    if (!baselines || a == b) {
        fprintf(stderr, "70 and 50 must produce different real Burmese ruby pixels\n");
        failed = 1;
    }
    // Flushing packets retains track defaults. Replacing/freeing the track must
    // release them; exercise both lifetimes repeatedly under CI sanitizers.
    if (baselines) {
        for (int i = 0; i < 8; i++) {
            ass_flush_events(scaled);
            static const char packet[] = "0,0,Default,,0,0,0,," RUBY;
            ass_process_chunk(scaled, packet, sizeof(packet) - 1, 0, 10000);
            uint64_t again;
            if (!snapshot(renderer, scaled, false, &again) || again != a ||
                    !snapshot(renderer, native, false, &again) || again != b)
                failed = 1;
            failed |= compare(lib, renderer, &cases[0]);
        }
        int sid = scaled->events[0].Style;
        ASS_Style replacement = scaled->styles[sid];
        replacement.Name = copy_string(replacement.Name);
        replacement.FontName = copy_string(replacement.FontName);
        if (replacement.Name && replacement.FontName) {
            ass_free_style(scaled, sid);
            scaled->styles[sid] = replacement;
            uint64_t again;
            if (!snapshot(renderer, scaled, false, &again) || again != b) {
                fprintf(stderr, "freeing/replacing a style retained its extension defaults\n");
                failed = 1;
            }
        } else {
            free(replacement.Name);
            free(replacement.FontName);
            failed = 1;
        }
    }
    if (baselines) for (int i = 0; i < 8; i++) {
        ASS_Track *owned = load(lib, META("Default", "\\grd(0,&HFF0000&,&H0000FF&)\\pgrdalpha(0,0,640,360,0,&H00&,&HAA&)"), "Default", "", RUBY);
        if (!owned) { failed = 1; continue; }
        uint64_t before = 0, after = 0;
        failed |= !snapshot(renderer, owned, true, &before);
        for (int j = 0; j < 4; j++)
            failed |= ass_process_mangetsu_metadata_line(owned, "Default", "", "mangetsu-style",
                "{\\grd(90,&H00FF00&,&HFFFFFF&)\\pgrdalpha(0,0,640,360,90,&HFF&,&H00&)}", 1, 1) != 1;
        failed |= !snapshot(renderer, owned, true, &after) || before != after;
        int sid = owned->events[0].Style;
        ASS_Style replacement = owned->styles[sid];
        replacement.Name = copy_string(replacement.Name);
        replacement.FontName = copy_string(replacement.FontName);
        if (replacement.Name && replacement.FontName) {
            ass_free_style(owned, sid);
            owned->styles[sid] = replacement;
            failed |= !snapshot(renderer, owned, false, &after) || after != b;
        } else { free(replacement.Name); free(replacement.FontName); failed = 1; }
        ass_free_track(owned);
        failed |= !snapshot(renderer, native, false, &after) || after != b;
    }
    ass_free_track(scaled);
    ass_free_track(native);
    if (missing_glyphs) {
        fprintf(stderr, "bundled Myanmar font is missing required glyphs\n");
        failed = 1;
    }
    ass_renderer_done(renderer);
    ass_library_done(lib);
    return failed;
}
