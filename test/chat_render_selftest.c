#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ass.h"

#undef assert
#define assert(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        abort(); \
    } \
} while (0)

enum { FRAME_W = 1920, FRAME_H = 1080, MAX_BOXES = 256 };

typedef struct {
    int x, y, w, h;
} Box;

typedef struct {
    Box boxes[MAX_BOXES];
    int count;
} Frame;

static unsigned char *font_bytes;

static bool add_test_font(ASS_Library *lib)
{
    const char *environment = getenv("FURI_TEST_FONT");
    const char *paths[] = {environment, "compare/test/font1.ttf",
                           "../compare/test/font1.ttf"};
    FILE *file = NULL;
    for (size_t i = 0; i < sizeof(paths) / sizeof(*paths); i++) {
        if (paths[i] && (file = fopen(paths[i], "rb")))
            break;
    }
    if (!file)
        return false;
    if (fseek(file, 0, SEEK_END)) {
        fclose(file);
        return false;
    }
    long size = ftell(file);
    if (size <= 0 || size > INT_MAX || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return false;
    }
    font_bytes = malloc(size);
    bool loaded = font_bytes && fread(font_bytes, 1, size, file) == (size_t) size;
    fclose(file);
    if (!loaded) {
        free(font_bytes);
        font_bytes = NULL;
        return false;
    }
    ass_add_font(lib, "font1.ttf", (const char *) font_bytes, (int) size);
    return true;
}

static ASS_Track *make_track(ASS_Library *lib, const char *body)
{
    const char *prefix =
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: 1920\n"
        "PlayResY: 1080\n"
        "ScaledBorderAndShadow: yes\n\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,sans-serif,42,&H00FFFFFF,&H0000FFFF,&H0000AA00,&H00AA0000,"
        "0,0,0,0,100,100,0,0,1,0,0,9,20,20,20,1\n\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,";
    size_t length = strlen(prefix) + strlen(body) + 2;
    char *script = malloc(length);
    if (!script)
        return NULL;
    snprintf(script, length, "%s%s\n", prefix, body);
    ASS_Track *track = ass_read_memory(lib, script, strlen(script), NULL);
    free(script);
    return track;
}

static Frame render(ASS_Renderer *renderer, ASS_Track *track, long long time)
{
    Frame frame = {0};
    int changed = 0;
    for (ASS_Image *image = ass_render_frame(renderer, track, time, &changed);
         image; image = image->next) {
        if (image->type != IMAGE_TYPE_SHADOW ||
            !image->w || !image->h || frame.count == MAX_BOXES)
            continue;
        frame.boxes[frame.count++] = (Box) {
            image->dst_x, image->dst_y, image->w, image->h
        };
    }
    return frame;
}

static Box widest(const Frame *frame, int rank)
{
    Box result = {0};
    int width = INT_MAX;
    for (int r = 0; r <= rank; r++) {
        Box candidate = {0};
        for (int i = 0; i < frame->count; i++) {
            Box box = frame->boxes[i];
            if (box.w > candidate.w && box.w < width)
                candidate = box;
        }
        result = candidate;
        width = candidate.w;
    }
    return result;
}

static bool same_box(Box a, Box b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

static uint64_t render_hash(ASS_Renderer *renderer, ASS_Track *track)
{
    int changed = 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (ASS_Image *image = ass_render_frame(renderer, track, 0, &changed);
         image; image = image->next) {
        const uint32_t fields[] = {
            image->color, (uint32_t) image->dst_x,
            (uint32_t) image->dst_y, (uint32_t) image->w,
            (uint32_t) image->h, (uint32_t) image->type,
        };
        for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); i++) {
            hash ^= fields[i];
            hash *= UINT64_C(1099511628211);
        }
        for (int y = 0; y < image->h; y++)
            for (int x = 0; x < image->w; x++) {
                hash ^= image->bitmap[y * image->stride + x];
                hash *= UINT64_C(1099511628211);
            }
    }
    return hash;
}

int main(void)
{
    ASS_Library *lib = ass_library_init();
    assert(lib);
    assert(add_test_font(lib));
    ASS_Renderer *renderer = ass_renderer_init(lib);
    assert(renderer);
    ass_set_storage_size(renderer, FRAME_W, FRAME_H);
    ass_set_frame_size(renderer, FRAME_W, FRAME_H);
    ass_set_fonts(renderer, NULL, "sans-serif",
                  ASS_FONTPROVIDER_AUTODETECT, NULL, 1);

    const char *shorthand =
        "{\\an9\\pos(1850,80)\\chatmode3\\msgtitle(Miku)"
        "\\msgstartcount(1)\\msgtime(1200,2400,3000,3900)\\msganim(250)}"
        "{\\ta7}Hey\\N{\\ta9}What\\N{\\ta7}Look at this"
        "\\N{|}This shit crazy\\N{\\ta9}💀";
    ASS_Track *track = make_track(lib, shorthand);
    assert(track);
    Frame at_zero = render(renderer, track, 0);
    /* The new bubble starts below the clipped viewport at its reveal time. */
    Frame at_reveal = render(renderer, track, 1200);
    Frame after_transition = render(renderer, track, 1450);
    Frame at_end = render(renderer, track, 4000);
    assert(at_zero.count >= 3);
    assert(at_reveal.count >= 3);
    assert(after_transition.count >= 4);
    Box panel = widest(&at_zero, 0);
    Box header = widest(&at_zero, 1);
    assert(panel.w > header.w && header.w > 0);
    assert(panel.x + panel.w <= 1852);
    assert(same_box(panel, widest(&at_reveal, 0)));
    assert(same_box(panel, widest(&after_transition, 0)));
    assert(same_box(panel, widest(&at_end, 0)));
    assert(same_box(header, widest(&at_reveal, 1)));
    assert(same_box(header, widest(&after_transition, 1)));
    assert(same_box(header, widest(&at_end, 1)));
    for (int i = 0; i < at_end.count; i++) {
        Box box = at_end.boxes[i];
        if (box.w >= header.w)
            continue;
        assert(box.w < header.w * 0.80 + 3);
        assert(box.y >= header.y + header.h - 2);
    }
    ass_free_track(track);

    track = make_track(lib,
        "{\\an9\\move(1600,80,1850,80,0,1000)\\chatmode3}"
        "{\\ta7}Moving");
    assert(track);
    Frame move_start = render(renderer, track, 0);
    Frame move_end = render(renderer, track, 1000);
    assert(widest(&move_end, 0).x > widest(&move_start, 0).x + 200);
    assert(widest(&move_end, 0).w == widest(&move_start, 0).w);
    ass_free_track(track);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode1\\msgm(Miku)"
        "\\msgshowname0\\msgtitle(Miku)\\msgstartcount(1)"
        "\\msgtime(1200,2400,3000,3900)\\msganim(250)}"
        "{\\msg(Yurf)}Hey{\\msg(Miku)}What"
        "{\\msg(Yurf)}Look at this{\\msg(Yurf)}This shit crazy"
        "{\\msg(Miku)}💀");
    assert(track);
    Frame clean = render(renderer, track, 1200);
    assert(same_box(panel, widest(&clean, 0)));
    assert(same_box(header, widest(&clean, 1)));
    ass_free_track(track);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode2\\msgtitle(Miku)\\msgm(Miku)"
        "\\msgshowname1\\msgstartcount(1)\\msgtime(1200)\\msganim(250)}"
        "|Miku:\\NHello|\\N\\N|Yurf:\\NYo|");
    assert(track);
    Frame named_start = render(renderer, track, 0);
    Frame named_complete = render(renderer, track, 1450);
    assert(named_start.count >= 3 && named_complete.count >= 4);
    assert(same_box(widest(&named_start, 0), widest(&named_complete, 0)));
    assert(same_box(widest(&named_start, 1), widest(&named_complete, 1)));
    uint64_t names_visible = render_hash(renderer, track);
    ass_free_track(track);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode2\\msgtitle(Miku)\\msgm(Miku)"
        "\\msgshowname0\\msgstartcount(1)\\msgtime(1200)\\msganim(250)}"
        "|Miku:\\NHello|\\N\\N|Yurf:\\NYo|");
    assert(track);
    Frame names_hidden = render(renderer, track, 0);
    assert(names_hidden.count >= 3);
    assert(names_visible != render_hash(renderer, track));
    ass_free_track(track);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode2\\msgm(Miku)}"
        "|Miku:\\NHello|");
    assert(track);
    uint64_t default_colors = render_hash(renderer, track);
    ass_free_track(track);
    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode2\\msgm(Miku)}"
        "|{\\c&HFFFFFF&\\2c&H39C5BB&\\3c&H303030&}Miku:\\NHello|");
    assert(track);
    assert(default_colors != render_hash(renderer, track));
    ass_free_track(track);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode2\\msgshowname0}"
        "|Miku:\\N{\\fs20}Hi|");
    assert(track);
    Frame named_small = render(renderer, track, 0);
    ass_free_track(track);
    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode2\\msgshowname0}"
        "|Miku:\\N{\\fs60}Hi|");
    assert(track);
    Frame named_large = render(renderer, track, 0);
    ass_free_track(track);
    assert(named_small.count >= 2 && named_large.count >= 2);
    assert(widest(&named_large, 1).h > widest(&named_small, 1).h);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode2\\msgm(Miku)}"
        "|Extremely Long Messaging Account Person:\\NHello|");
    assert(track);
    Frame named_long = render(renderer, track, 0);
    assert(named_long.count >= 2);
    assert(widest(&named_long, 1).w < widest(&named_long, 0).w * 0.78);
    ass_free_track(track);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode2\\msgm(Miku)"
        "\\msgshowname0\\furi0}"
        "|Miku:\\N今日はどう？|");
    assert(track);
    Frame named_plain = render(renderer, track, 0);
    ass_free_track(track);
    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode2\\msgm(Miku)"
        "\\msgshowname0\\furi1}"
        "|Miku:\\N<今日|きょう>はどう？|");
    assert(track);
    Frame named_ruby = render(renderer, track, 0);
    assert(named_plain.count >= 2 && named_ruby.count >= 2);
    assert(widest(&named_ruby, 1).h > widest(&named_plain, 1).h);
    ass_free_track(track);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode2}{\\ta7}A\\N{\\ta9}B");
    assert(track);
    assert(render(renderer, track, 0).count == 0);
    ass_free_track(track);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode3\\msgtitle(Miku)}"
        "{\\ta7}A\\N{\\ta9}B\\N{|}C");
    assert(track);
    Frame static_frame = render(renderer, track, 0);
    assert(static_frame.count >= 5); /* panel, header, three bubbles */
    ass_free_track(track);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode3\\msgtitle(A very long title "
        "that must stay within the phone width)}"
        "{\\ta7}Supercalifragilisticexpialidocious"
        "Supercalifragilisticexpialidocious");
    assert(track);
    Frame long_frame = render(renderer, track, 0);
    assert(long_frame.count >= 3);
    Box long_panel = widest(&long_frame, 0);
    assert(long_panel.w <= FRAME_W * 0.56 + 4);
    for (int i = 0; i < long_frame.count; i++) {
        Box box = long_frame.boxes[i];
        if (box.w < widest(&long_frame, 1).w)
            assert(box.w < long_panel.w * 0.78);
    }
    ass_free_track(track);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode3}{\\ta7\\fs20}Small");
    assert(track);
    Frame small = render(renderer, track, 0);
    ass_free_track(track);
    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode3}{\\ta7\\fs60}Large");
    assert(track);
    Frame large = render(renderer, track, 0);
    ass_free_track(track);
    assert(small.count >= 2 && large.count >= 2);
    assert(widest(&large, 1).h > widest(&small, 1).h);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode3}{\\ta7}Hi");
    assert(track);
    Frame base = render(renderer, track, 0);
    ass_free_track(track);
    assert(base.count == 2); /* no title means no blank header shape */
    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode3}{\\ta7\\fscx200}Hi");
    assert(track);
    Frame wide = render(renderer, track, 0);
    ass_free_track(track);
    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode3}{\\ta7\\fscy200}Hi");
    assert(track);
    Frame tall = render(renderer, track, 0);
    ass_free_track(track);
    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode3}{\\ta7\\fsp10}Hi");
    assert(track);
    Frame spaced = render(renderer, track, 0);
    ass_free_track(track);
    assert(widest(&wide, 1).w > widest(&base, 1).w);
    assert(widest(&tall, 1).h > widest(&base, 1).h);
    assert(widest(&spaced, 1).w > widest(&base, 1).w);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode3}{\\ta7}First\\Nsecond");
    assert(track);
    Frame multiline = render(renderer, track, 0);
    ass_free_track(track);
    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode3}{\\ta7}First");
    assert(track);
    Frame one_line = render(renderer, track, 0);
    ass_free_track(track);
    assert(widest(&multiline, 1).h > widest(&one_line, 1).h);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode3\\furi0}{\\ta7}今日");
    assert(track);
    Frame plain = render(renderer, track, 0);
    ass_free_track(track);
    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode3\\furi1}"
        "{\\ta7}<今日|きょう>");
    assert(track);
    Frame ruby = render(renderer, track, 0);
    ass_free_track(track);
    assert(plain.count >= 2 && ruby.count >= 2);
    assert(widest(&ruby, 1).h > widest(&plain, 1).h);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\clip(1300,0,1920,1080)"
        "\\chatmode3\\msgtitle(Miku)}{\\ta7}A");
    assert(track);
    Frame clipped = render(renderer, track, 0);
    for (int i = 0; i < clipped.count; i++)
        assert(clipped.boxes[i].x >= 1300);
    ass_free_track(track);

    /* Unknown marker and color tags outside chat keep the ordinary path. */
    track = make_track(lib, "{\\ta7\\c&HFFFFFF&\\2c&H00FFFF&"
                            "\\3c&H00FF00&\\4c&HFF0000&}A\\NB{|}");
    assert(track);
    uint64_t ordinary = render_hash(renderer, track);
    ass_free_track(track);
    track = make_track(lib, "{\\ta7\\c&HFFFFFF&\\2c&H00FFFF&"
                            "\\3c&H00FF00&\\4c&HFF0000&}A\\NB");
    assert(track);
    assert(ordinary == render_hash(renderer, track));
    ass_free_track(track);

    track = make_track(lib, "|Miku:\\NHello|");
    assert(track);
    uint64_t ordinary_pipe = render_hash(renderer, track);
    ass_free_track(track);
    track = make_track(lib, "Miku:\\NHello");
    assert(track);
    assert(ordinary_pipe != render_hash(renderer, track));
    ass_free_track(track);

    ass_renderer_done(renderer);
    ass_library_done(lib);
    free(font_bytes);
    puts("native chat render tests passed");
    return 0;
}
