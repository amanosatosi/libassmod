/* SPDX-License-Identifier: ISC */

#include "ass.h"
#include "ass_render.h"

enum { WIDTH = 640, HEIGHT = 360 };

#define BASE "{\\an5\\pos(320,180)"
#define PIN "\\mpersp(0.12,0.02,1,0.06,0.8,1,0.21,0.94)"
#define TEXT "}{\\fnNative Vertical Furi}Short text"

static int failures;

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "perspective render:%d: %s\n", __LINE__, #condition); \
    failures++; } } while (0)

static bool add_font(ASS_Library *library, const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    bool ok = false;
    if (fseek(file, 0, SEEK_END) == 0) {
        long size = ftell(file);
        if (size > 0 && size <= INT_MAX && fseek(file, 0, SEEK_SET) == 0) {
            char *data = malloc(size);
            if (data && fread(data, 1, size, file) == (size_t) size) {
                ass_add_font(library, path, data, size);
                ok = true;
            }
            free(data);
        }
    }
    fclose(file);
    return ok;
}

static ASS_Track *make_track(ASS_Library *lib, const char *text)
{
    char data[4096];
    int n = snprintf(data, sizeof(data),
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\nWrapStyle: 0\n"
        "ScaledBorderAndShadow: yes\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
        "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
        "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Native Vertical Furi,44,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,"
        "0,0,0,0,100,100,0,0,1,2,3,5,42,42,16,1\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:04.00,Default,,0,0,0,,%s\n", text);
    if (n <= 0 || n >= sizeof(data))
        return NULL;
    return ass_read_memory(lib, data, n, NULL);
}

static uint64_t hash(uint64_t before, uint32_t value)
{
    return (before ^ value) * UINT64_C(1099511628211);
}

static uint64_t render(ASS_Renderer *r, ASS_Track *track, long long now, bool rgba)
{
    if (!track) {
        CHECK(track);
        return 0;
    }
    uint64_t checksum = 0;
    size_t visible = 0;
    if (rgba) {
        ASS_ImageRGBA *head = ass_render_frame_rgba(r, track, now, NULL);
        for (ASS_ImageRGBA *im = head; im; im = im->next) {
            CHECK(im->w >= 0 && im->h >= 0 && im->dst_x >= 0 && im->dst_y >= 0 &&
                  im->dst_x + im->w <= WIDTH && im->dst_y + im->h <= HEIGHT);
            checksum = hash(hash(checksum, im->dst_x), im->dst_y);
            for (int y = 0; y < im->h; y++)
                for (int x = 0; x < im->w; x++) {
                    const uint8_t *pixel = im->rgba + y * im->stride + 4 * x;
                    visible += pixel[3] != 0;
                    for (int c = 0; c < 4; c++)
                        checksum = hash(checksum, pixel[c]);
                }
        }
        ass_free_images_rgba(head);
    } else {
        for (ASS_Image *im = ass_render_frame(r, track, now, NULL); im; im = im->next) {
            CHECK(im->w >= 0 && im->h >= 0 && im->dst_x >= 0 && im->dst_y >= 0 &&
                  im->dst_x + im->w <= WIDTH && im->dst_y + im->h <= HEIGHT);
            checksum = hash(hash(hash(checksum, im->dst_x), im->dst_y), im->color);
            for (int y = 0; y < im->h; y++)
                for (int x = 0; x < im->w; x++) {
                    int pixel = im->bitmap[y * im->stride + x];
                    visible += pixel != 0;
                    checksum = hash(checksum, pixel);
                }
        }
    }
    CHECK(visible > 0);
    return checksum;
}

static double check_layout(ASS_Renderer *renderer)
{
    int event_index = -1;
    double rect[4], pins[8];
    if (ass_get_perspective_layout(renderer, 0, &event_index, rect, pins) != 1) {
        CHECK(false);
        return 0;
    }
    CHECK(event_index == 0 && rect[0] < rect[2] && rect[1] < rect[3]);
    CHECK(ass_get_perspective_layout(renderer, 1, &event_index, rect, pins) == -1);
    double width = rect[2] - rect[0], height = rect[3] - rect[1];
    CHECK(fabs((pins[0] - rect[0]) / width - 0.12) < 1e-9);
    CHECK(fabs((pins[1] - rect[1]) / height - 0.02) < 1e-9);
    return width;
}

static void test_output(ASS_Library *lib, ASS_Renderer *r, bool rgba)
{
    ASS_Track *ordinary = make_track(lib, BASE TEXT);
    ASS_Track *identity = make_track(lib, BASE "\\mpersp(0,0,1,0,1,1,0,1)" TEXT);
    ASS_Track *invalid = make_track(lib, BASE "\\mpersp(0,0,1,1,0,1,1,0)" TEXT);
    ASS_Track *oversized = make_track(lib, BASE "\\mpersp(0,0,512,0,512,512,0,512)" TEXT);
    ASS_Track *pins = make_track(lib, BASE PIN TEXT);
    ASS_Track *edited = make_track(lib, BASE PIN "\\fs52\\fsp2\\fscx107"
                                  "}Short edit\\NA long second line");
    ASS_Track *animation = make_track(lib, BASE "\\t(0,1000," PIN ")" TEXT);
    ASS_Track *crossing = make_track(lib, BASE "\\t(0,1000,\\mpersp(1,0,0,0,0,1,1,1))" TEXT);

    uint64_t plain = render(r, ordinary, 0, rgba);
    CHECK(render(r, identity, 0, rgba) == plain);
    CHECK(render(r, invalid, 0, rgba) == plain);
    CHECK(render(r, oversized, 0, rgba) == plain);
    CHECK(render(r, crossing, 500, rgba) == plain);
    CHECK(render(r, animation, 0, rgba) == plain);

    uint64_t projected = render(r, pins, 0, rgba);
    CHECK(projected != plain);
    double width = check_layout(r);
    CHECK(render(r, pins, 500, rgba) == projected);
    printf("%s warm perspective frame: %llu bitmap and %llu composite constructions\n",
           rgba ? "RGBA" : "legacy",
           (unsigned long long) r->repeated_event_stats.bitmap_constructions,
           (unsigned long long) r->repeated_event_stats.composite_constructions);
    CHECK(!r->repeated_event_stats.bitmap_constructions);
    CHECK(!r->repeated_event_stats.composite_constructions);

    CHECK(render(r, animation, 500, rgba) != projected);
    CHECK(render(r, animation, 1000, rgba) == projected);
    CHECK(render(r, edited, 0, rgba) != projected && check_layout(r) > width);

    ass_free_track(crossing);
    ass_free_track(animation);
    ass_free_track(edited);
    ass_free_track(pins);
    ass_free_track(oversized);
    ass_free_track(invalid);
    ass_free_track(identity);
    ass_free_track(ordinary);
}

static void test_mixed_geometry(ASS_Library *lib, ASS_Renderer *r, bool rgba)
{
    const char tags[] = BASE "\\distort(1,0,1.02,1,0,1)\\frx12\\fry-18\\frz6"
        "\\fscx110\\fscy96\\bord3\\2bs2\\shad4\\clip(40,30,600,330)"
        "\\1grd(0,&H3355EE&,&HDDFFEE&)";
    const char body[] = "}<漢字|かんじ>\\N{\\fnNoto Sans Myanmar}မြန်မာ ဆာတို့";
    char text[1024];
    snprintf(text, sizeof(text), "%s%s", tags, body);
    ASS_Track *source = make_track(lib, text);
    snprintf(text, sizeof(text), "%s%s%s", tags, PIN, body);
    ASS_Track *target = make_track(lib, text);
    uint64_t plain = render(r, source, 0, rgba);
    CHECK(render(r, target, 0, rgba) != plain);
    CHECK(check_layout(r) > 0);
    ass_free_track(target);
    ass_free_track(source);
}

int main(int argc, char *argv[])
{
    if (argc != 3)
        return 1;
    ASS_Library *lib = ass_library_init();
    if (!lib)
        return 1;
    ASS_Renderer *r = ass_renderer_init(lib);
    if (!r || !add_font(lib, argv[1]) || !add_font(lib, argv[2])) {
        ass_renderer_done(r);
        ass_library_done(lib);
        return 1;
    }

    ass_set_frame_size(r, WIDTH, HEIGHT);
    ass_set_storage_size(r, WIDTH, HEIGHT);
    ass_set_fonts(r, NULL, "Native Vertical Furi", ASS_FONTPROVIDER_NONE, NULL, 1);
    for (int rgba = 0; rgba <= 1; rgba++) {
        test_output(lib, r, rgba);
        test_mixed_geometry(lib, r, rgba);
    }
    ass_renderer_done(r);
    ass_library_done(lib);
    return failures != 0;
}
