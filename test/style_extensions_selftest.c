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
static bool snapshot(ASS_Renderer *renderer, ASS_Track *track, bool rgba,
                      uint64_t *hash)
{
    *hash = UINT64_C(1469598103934665603);
    uint64_t coverage = 0;
    int change;
    if (rgba) {
        ASS_ImageRGBA *images = ass_render_frame_rgba(renderer, track, 500, &change);
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
        ASS_Image *images = ass_render_frame(renderer, track, 500, &change);
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

typedef struct {
    const char *label, *metadata, *style, *actor, *text;
    const char *reference_metadata, *reference;
} Case;

static int compare(ASS_Library *lib, ASS_Renderer *renderer, const Case *c)
{
    ASS_Track *actual = load(lib, c->metadata, c->style, c->actor, c->text);
    ASS_Track *reference = load(lib, c->reference_metadata, c->style, c->actor, c->reference);
    int failed = !actual || !reference;
    for (int rgba = 0; !failed && rgba < 2; rgba++) {
        uint64_t a, b;
        if (!snapshot(renderer, actual, rgba, &a) ||
                !snapshot(renderer, reference, rgba, &b) || a != b) {
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
    char *owned = copy_string("{\\furis70}");
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
    ASS_Track *reference = load(lib, F70 RED BLUE, "Default", "Nene", text);
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
    reference = load(lib, META("Default", "\\furis80") RED BLUE,
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

    static const Case cases[] = {
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
