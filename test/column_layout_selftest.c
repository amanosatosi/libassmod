#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ass.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

enum { WIDTH = 640, HEIGHT = 640, ROW_HEIGHT = 30, GROUPS = 10, ROWS = 20 };

typedef struct {
    int first, marker;
} Row;

typedef struct {
    Row rows[ROWS];
    uint64_t hash;
} Frame;

typedef struct {
    char script[8192];
    size_t length;
} Fixture;

static Fixture fixture(void)
{
    Fixture f = {
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 640\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,sans-serif,20,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,"
        "0,0,0,0,100,100,0,0,1,0,0,7,10,10,10,1\n"
        "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
    };
    f.length = strlen(f.script);
    return f;
}

static void add_line(Fixture *f, int row, int start, int end, const char *tags,
                     const char *text, const char *between)
{
    size_t remaining = sizeof(f->script) - f->length;
    int length = snprintf(f->script + f->length, remaining,
                          "Dialogue: 0,0:00:%02d.00,0:00:%02d.00,Default,,0,0,0,,"
                          "{\\an7\\pos(20,%d)%s}%s%s|{\\1c&H0000FF&}X\n",
                          start, end, row * ROW_HEIGHT + 2, tags, text, between);
    CHECK(length > 0 && (size_t) length < remaining);
    f->length += length;
}

static void add_group_line(Fixture *f, int row, int start, int end, int group,
                           const char *text)
{
    char tag[16];
    snprintf(tag, sizeof(tag), "\\col%d", group);
    add_line(f, row, start, end, tag, text, "");
}

static ASS_Track *read_fixture(ASS_Library *lib, Fixture *f)
{
    ASS_Track *track = ass_read_memory(lib, f->script, f->length, NULL);
    CHECK(track);
    return track;
}

static void record_pixel(Frame *frame, int x, int y, unsigned coverage, bool marker)
{
    CHECK(y >= 0 && y < ROWS * ROW_HEIGHT);
    Row *row = &frame->rows[y / ROW_HEIGHT];
    int *left = marker ? &row->marker : &row->first;
    if (x < *left)
        *left = x;
    frame->hash ^= ((uint64_t) x << 24) | ((uint64_t) y << 12) | (coverage << 1) | marker;
    frame->hash *= UINT64_C(1099511628211);
}

static Frame render(ASS_Renderer *renderer, ASS_Track *track, long long now, bool rgba)
{
    Frame frame = {.hash = UINT64_C(1469598103934665603)};
    for (int row = 0; row < ROWS; row++)
        frame.rows[row] = (Row) {INT_MAX, INT_MAX};

    if (rgba) {
        ASS_ImageRGBA *images = ass_render_frame_rgba(renderer, track, now, NULL);
        for (ASS_ImageRGBA *img = images; img; img = img->next) {
            for (int y = 0; y < img->h; y++) {
                const uint8_t *p = img->rgba + (size_t) y * img->stride;
                for (int x = 0; x < img->w; x++, p += 4) {
                    if (p[3])
                        record_pixel(&frame, img->dst_x + x, img->dst_y + y, p[3], p[0] > p[1]);
                }
            }
        }
        ass_free_images_rgba(images);
    } else {
        for (ASS_Image *img = ass_render_frame(renderer, track, now, NULL); img; img = img->next) {
            bool marker = (img->color >> 8) == 0xFF0000;
            CHECK((img->color & 0xFF) == 0);
            for (int y = 0; y < img->h; y++) {
                const uint8_t *p = img->bitmap + (size_t) y * img->stride;
                for (int x = 0; x < img->w; x++) {
                    if (p[x])
                        record_pixel(&frame, img->dst_x + x, img->dst_y + y, p[x], marker);
                }
            }
        }
    }
    CHECK(frame.rows[0].marker < WIDTH);
    return frame;
}

static Frame render_fixture(ASS_Library *lib, ASS_Renderer *renderer, Fixture *f, bool rgba)
{
    ASS_Track *track = read_fixture(lib, f);
    Frame frame = render(renderer, track, 0, rgba);
    ass_free_track(track);
    return frame;
}

static void test_tags(ASS_Library *lib, ASS_Renderer *renderer, bool rgba)
{
    Fixture f = fixture();
    add_line(&f, 0, 0, 4, "", "A", "");
    Frame ordinary = render_fixture(lib, renderer, &f, rgba);

    f = fixture();
    add_group_line(&f, 0, 0, 4, 0, "A");
    CHECK(ordinary.hash == render_fixture(lib, renderer, &f, rgba).hash);

    f = fixture();
    add_group_line(&f, 0, 0, 4, 1, "A");
    Frame grouped = render_fixture(lib, renderer, &f, rgba);
    CHECK(ordinary.hash != grouped.hash);

    f = fixture();
    add_line(&f, 0, 0, 4, "\\col1\\col0", "A", "");
    CHECK(ordinary.hash == render_fixture(lib, renderer, &f, rgba).hash);

    static const char *const invalid[] = {
        "\\col-1", "\\col11", "\\col100", "\\col2.0", "\\col10x", "\\col",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        f = fixture();
        add_line(&f, 0, 0, 4, invalid[i], "A", "");
        CHECK(ordinary.hash == render_fixture(lib, renderer, &f, rgba).hash);
    }
    f = fixture();
    add_line(&f, 0, 0, 4, "\\col1\\col-1\\col11\\col100\\col2.0\\col10x\\col", "A", "");
    add_group_line(&f, 1, 0, 4, 1, "AAAAAAA");
    Frame frame = render_fixture(lib, renderer, &f, rgba);
    CHECK(frame.rows[0].marker > grouped.rows[0].marker);
    CHECK(frame.rows[0].marker == frame.rows[1].marker);
}

static void test_all_groups(ASS_Library *lib, ASS_Renderer *renderer, bool rgba)
{
    Fixture f = fixture();
    for (int group = 1; group <= GROUPS; group++) {
        char wide[GROUPS + 2];
        memset(wide, 'A', group + 1);
        wide[group + 1] = 0;
        add_group_line(&f, (group - 1) * 2, 0, 4, group, "A");
        add_group_line(&f, (group - 1) * 2 + 1, 0, 4, group, wide);
    }
    Frame frame = render_fixture(lib, renderer, &f, rgba);
    for (int row = 0; row < ROWS; row += 2) {
        CHECK(frame.rows[row].marker < WIDTH);
        CHECK(frame.rows[row].marker == frame.rows[row + 1].marker);
        if (row)
            CHECK(frame.rows[row].marker > frame.rows[row - 2].marker);
    }
}

static void test_timing_and_options(ASS_Library *lib, ASS_Renderer *renderer, bool rgba)
{
    Fixture f = fixture();
    add_line(&f, 0, 0, 4, "\\col1\\colsp20\\colan6", "A", "{\\r}");
    add_line(&f, 1, 1, 2, "\\col1\\colsp20", "AAAAAAAA", "");
    add_group_line(&f, 2, 0, 4, 10, "A");
    add_group_line(&f, 3, 2, 3, 10, "AAAA");
    ASS_Track *track = read_fixture(lib, &f);
    Frame before = render(renderer, track, 999, rgba);
    Frame first = render(renderer, track, 1000, rgba);
    CHECK(first.hash == render(renderer, track, 1999, rgba).hash);
    CHECK(first.rows[0].first > before.rows[0].first);
    CHECK(first.rows[0].marker > before.rows[0].marker);
    CHECK(first.rows[0].marker == first.rows[1].marker);
    CHECK(first.rows[2].marker == before.rows[2].marker);

    Frame second = render(renderer, track, 2000, rgba);
    CHECK(second.rows[1].marker == INT_MAX);
    CHECK(second.rows[0].marker == before.rows[0].marker);
    CHECK(second.rows[2].marker > before.rows[2].marker);
    CHECK(second.rows[2].marker == second.rows[3].marker);
    CHECK(before.hash == render(renderer, track, 3000, rgba).hash);
    CHECK(first.hash == render(renderer, track, 1000, rgba).hash);
    CHECK(before.hash == render(renderer, track, 999, rgba).hash);
    ass_free_track(track);

    f = fixture();
    add_line(&f, 0, 0, 4, "\\col10\\colsp20", "A", "{\\r}");
    Frame wider_gap = render_fixture(lib, renderer, &f, rgba);
    CHECK(wider_gap.rows[0].marker == before.rows[0].marker);
    CHECK(wider_gap.rows[0].marker - before.rows[2].marker == 19);
}

static void benchmark_groups(ASS_Library *lib, ASS_Renderer *renderer)
{
    enum { WARMUP = 12, FRAMES = 180 };
    static const char *const scenarios[] = {
        "disabled", "single column event", "single shared group", "multiple shared groups",
    };
    for (int mode = 0; mode < 4; mode++) {
        Fixture f = fixture();
        for (int row = 0; row < ROWS; row++) {
            int group = 0;
            if (mode == 2 || (mode == 1 && row == 0))
                group = 1;
            else if (mode == 3)
                group = row / 2 + 1;
            add_group_line(&f, row, 0, 4, group, row % 2 ? "AAAAAAAA" : "A");
        }
        ASS_Track *track = read_fixture(lib, &f);
        for (int i = 0; i < WARMUP; i++)
            CHECK(ass_render_frame(renderer, track, i * 16, NULL));
        clock_t start = clock();
        for (int i = 0; i < FRAMES; i++)
            CHECK(ass_render_frame(renderer, track, (WARMUP + i) * 16, NULL));
        clock_t end = clock();
        CHECK(start != (clock_t) -1 && end != (clock_t) -1);
        printf("%s: %.3f ms/frame (%d frames, %d active events)\n", scenarios[mode],
               1000.0 * (end - start) / CLOCKS_PER_SEC / FRAMES, FRAMES, ROWS);
        ass_free_track(track);
    }
}

int main(int argc, char **argv)
{
    CHECK(argc == 2 || (argc == 3 && !strcmp(argv[2], "--bench")));
    ASS_Library *lib = ass_library_init();
    CHECK(lib);
    ASS_Renderer *renderer = ass_renderer_init(lib);
    CHECK(renderer);
    ass_set_frame_size(renderer, WIDTH, HEIGHT);
    ass_set_storage_size(renderer, WIDTH, HEIGHT);
    ass_set_fonts(renderer, argv[1], NULL, ASS_FONTPROVIDER_NONE, NULL, 0);

    if (argc == 3) {
        benchmark_groups(lib, renderer);
    } else {
        for (int rgba = 0; rgba < 2; rgba++) {
            test_tags(lib, renderer, rgba);
            test_all_groups(lib, renderer, rgba);
            test_timing_and_options(lib, renderer, rgba);
        }
    }
    ass_renderer_done(renderer);
    ass_library_done(lib);
    return 0;
}
