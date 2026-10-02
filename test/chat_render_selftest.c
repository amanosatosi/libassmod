#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ass.h"
#include "ass_render.h"

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

typedef struct {
    Box box;
    uint32_t color;
    bool found;
} ImageSample;

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

static ASS_Track *make_actor_track(ASS_Library *lib, const char *body,
                                  const char *metadata, const char *actor)
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
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
    size_t length = strlen(prefix) + strlen(body) + strlen(metadata) + strlen(actor) + 80;
    char *script = malloc(length);
    if (!script)
        return NULL;
    snprintf(script, length, "%s%sDialogue: 0,0:00:00.00,0:00:10.00,Default,%s,0,0,0,,%s\n",
             prefix, metadata, actor, body);
    ASS_Track *track = ass_read_memory(lib, script, strlen(script), NULL);
    free(script);
    return track;
}

static ASS_Track *make_track(ASS_Library *lib, const char *body)
{
    return make_actor_track(lib, body, "", "");
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

static uint64_t hash_image(uint64_t hash, const ASS_Image *image)
{
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
    return hash;
}

static uint64_t render_hash(ASS_Renderer *renderer, ASS_Track *track)
{
    int changed = 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (ASS_Image *image = ass_render_frame(renderer, track, 0, &changed);
         image; image = image->next)
        hash = hash_image(hash, image);
    return hash;
}

static ImageSample widest_image(ASS_Renderer *renderer, ASS_Track *track,
                                int type, int rank)
{
    int changed = 0;
    ASS_Image *images = ass_render_frame(renderer, track, 0, &changed);
    ImageSample result = {0};
    int width_limit = INT_MAX;
    for (int pass = 0; pass <= rank; pass++) {
        result = (ImageSample) {0};
        for (ASS_Image *image = images; image; image = image->next) {
            if (image->type != type || image->w >= width_limit ||
                image->w <= result.box.w)
                continue;
            result.found = true;
            result.color = image->color;
            result.box = (Box) {image->dst_x, image->dst_y,
                                image->w, image->h};
        }
        width_limit = result.box.w;
    }
    return result;
}

static ImageSample image_by_order(ASS_Renderer *renderer, ASS_Track *track,
                                  int type, int index)
{
    int changed = 0;
    ImageSample result = {0};
    for (ASS_Image *image = ass_render_frame(renderer, track, 0, &changed);
         image; image = image->next) {
        if (image->type != type)
            continue;
        if (index-- != 0)
            continue;
        result.found = true;
        result.color = image->color;
        result.box = (Box) {image->dst_x, image->dst_y,
                            image->w, image->h};
        break;
    }
    return result;
}

static bool has_image_color(ASS_Renderer *renderer, ASS_Track *track,
                            int type, uint32_t color)
{
    int changed = 0;
    for (ASS_Image *image = ass_render_frame(renderer, track, 0, &changed);
         image; image = image->next)
        if (image->type == type && image->color == color)
            return true;
    return false;
}

static uint32_t bubble_color(ASS_Library *lib, ASS_Renderer *renderer,
                             const char *source)
{
    ASS_Track *track = make_track(lib, source);
    assert(track);
    ImageSample bubble = widest_image(renderer, track,
                                       IMAGE_TYPE_SHADOW, 1);
    assert(bubble.found);
    ass_free_track(track);
    return bubble.color;
}

static void expect_title_colors(ASS_Renderer *renderer, ASS_Track *track,
                                uint32_t text, uint32_t background,
                                uint32_t speaker, bool named)
{
    ImageSample header = image_by_order(renderer, track, IMAGE_TYPE_SHADOW, 1);
    assert(header.found && header.color == background);
    bool saw_title = false, saw_speaker = false, saw_body = false;
    int changed = 0;
    for (ASS_Image *image = ass_render_frame(renderer, track, 0, &changed);
         image; image = image->next) {
        if (image->type != IMAGE_TYPE_CHARACTER || !image->w || !image->h)
            continue;
        if (image->dst_y < header.box.y + header.box.h) {
            assert(image->color == text);
            saw_title = true;
        } else if (named && image->color == speaker) {
            saw_speaker = true;
        } else {
            assert(image->color == UINT32_C(0x66554400));
            saw_body = true;
        }
    }
    assert(saw_title && saw_body && (!named || saw_speaker));
}

static void test_title_colors(ASS_Library *lib, ASS_Renderer *renderer)
{
    const char *payloads[] = {
        "{\\msg(Miku)}Hi", "|Miku:\\NHi|", "{\\ta7}Hi",
    };
    const struct {
        const char *tags;
        uint32_t text, background, speaker;
    } cases[] = {
        /* The yellow style SecondaryColour is not an explicit title color. */
        {"", 0x00000000u, 0xFFFFFF00u, 0xFFFF0000u},
        {"\\4c&HFFFFFF&", 0xFFFFFF00u, 0x00000000u, 0xFFFF0000u},
        {"\\msgtitlec&H39C5BB&", 0xBBC53900u, 0xFFFFFF00u, 0xFFFF0000u},
        {"\\msgtitlegbc&H223344&", 0x00000000u, 0x44332200u, 0xFFFF0000u},
        {"\\2c&H39C5BB&", 0xBBC53900u, 0xFFFFFF00u, 0xBBC53900u},
        {"\\2c&HFFFFFF&\\msgtitlec&H39C5BB&",
         0xBBC53900u, 0xFFFFFF00u, 0xFFFFFF00u},
        {"\\msgtitlec&H39C5BB&\\2c&HFFFFFF&",
         0xFFFFFF00u, 0xFFFFFF00u, 0xFFFFFF00u},
        {"\\msgtitlec&H39C5BB&\\msgtitlegbc&H223344&",
         0xBBC53900u, 0x44332200u, 0xFFFF0000u},
        /* A background override leaves the existing panel-based text fallback. */
        {"\\4c&HFFFFFF&\\msgtitlegbc&H223344&",
         0xFFFFFF00u, 0x44332200u, 0xFFFF0000u},
        {"\\2a&H40&", 0x00000000u, 0xFFFFFF00u, 0xFFFF0040u},
        {"\\2c&HFFFFFF&\\2c", 0xFFFF0000u, 0xFFFFFF00u, 0xFFFF0000u},
        {"\\2c&HFFFFFF&\\msgtitlec&H39C5BB&\\msgtitlegbc&H223344&"
         "\\r\\c&H445566&", 0x00000000u, 0xFFFFFF00u, 0xFFFF0000u},
        {"\\2c&H39C5BB&\\msgtitlec", 0x00000000u, 0xFFFFFF00u, 0xBBC53900u},
        {"\\msgtitlegbc&H223344&\\msgtitlegbc",
         0x00000000u, 0xFFFFFF00u, 0xFFFF0000u},
    };
    for (size_t mode = 0; mode < sizeof(payloads) / sizeof(*payloads); mode++) {
        uint64_t legacy_hash = 0;
        char source[512];
        for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
            snprintf(source, sizeof(source),
                "{\\an9\\pos(1850,80)\\chatmode%d\\msgtitle(Miku)"
                "\\c&H445566&%s}%s", (int) mode + 1,
                cases[i].tags, payloads[mode]);
            ASS_Track *track = make_track(lib, source);
            assert(track);
            expect_title_colors(renderer, track, cases[i].text,
                                cases[i].background, cases[i].speaker, mode < 2);
            if (!i)
                legacy_hash = render_hash(renderer, track);
            ass_free_track(track);
        }
        /* Legacy lines match explicitly selecting the old automatic colors,
         * including every output image's geometry and bitmap bytes. */
        snprintf(source, sizeof(source),
            "{\\an9\\pos(1850,80)\\chatmode%d\\msgtitle(Miku)"
            "\\c&H445566&\\msgtitlec&H000000&\\msgtitlegbc&HFFFFFF&}%s",
            (int) mode + 1, payloads[mode]);
        ASS_Track *track = make_track(lib, source);
        assert(track);
        assert(legacy_hash == render_hash(renderer, track));
        ass_free_track(track);
    }

    /* Explicit colors in message overrides use the same header state and
     * source order as prefix overrides; speaker colors stay sequential. */
    ASS_Track *track = make_track(lib,
        "{\\chatmode2\\msgtitle(Miku)\\msgtitlec&H39C5BB&\\c&H445566&}"
        "|{\\2c&HFFFFFF&\\msgtitlegbc&H223344&}Miku:\\NHi|");
    assert(track);
    expect_title_colors(renderer, track, 0xFFFFFF00u, 0x44332200u,
                        0xFFFFFF00u, true);
    ass_free_track(track);
    track = make_track(lib,
        "{\\chatmode2\\msgtitle(Miku)\\2c&HFFFFFF&\\c&H445566&}"
        "|{\\msgtitlec&H39C5BB&}Miku:\\NHi|");
    assert(track);
    expect_title_colors(renderer, track, 0xBBC53900u, 0xFFFFFF00u,
                        0xFFFFFF00u, true);
    ass_free_track(track);

    track = make_track(lib,
        "{\\chatmode2\\msgtitle(Miku)\\1c&H445566&\\2c&HFFFFFF&"
        "\\msgtitlec&H39C5BB&\\msgtitlegbc&H223344&"
        "\\3c&H303030&\\4c&H181818&\\bc&H112233&\\bs2"
        "\\bubbc&HFF55CC&\\bubbs4}|Miku:\\NHi|");
    assert(track);
    expect_title_colors(renderer, track, 0xBBC53900u, 0x44332200u,
                        0xFFFFFF00u, true);
    assert(image_by_order(renderer, track, IMAGE_TYPE_SHADOW, 0).color ==
           UINT32_C(0x18181800));
    assert(image_by_order(renderer, track, IMAGE_TYPE_SHADOW, 2).color ==
           UINT32_C(0x30303000));
    assert(has_image_color(renderer, track, IMAGE_TYPE_OUTLINE,
                           UINT32_C(0x33221100)));
    assert(has_image_color(renderer, track, IMAGE_TYPE_OUTLINE,
                           UINT32_C(0xCC55FF00)));
    ass_free_track(track);
}

#define CHAT_RIGHT_ARGS "&HFFFFFF&,&H11&,&H39C5BB&,&H22&,&H332244&,&H33&," \
                        "&HFF55CC&,&H44&,4,&H000000&,&H55&,2"
#define CHAT_LEFT_ARGS "&H0000FF&,&H66&,&H00FF00&,&H77&,&HFF0000&,&H88&," \
                       "&HFFFF00&,&H99&,3,&H111111&,&HAA&,1"

static bool message_has_image_color(ASS_Renderer *renderer, ASS_Track *track,
                                     int message, int type, uint32_t color)
{
    /* These fixtures have no header: the first shadow is the panel, followed
     * by one fill per message. Glyph state has already been released when
     * ass_render_frame returns, so inspect the returned images instead. */
    ImageSample bubble = image_by_order(renderer, track, IMAGE_TYPE_SHADOW,
                                        message + 1);
    assert(bubble.found);
    int changed = 0;
    for (ASS_Image *image = ass_render_frame(renderer, track, 0, &changed);
         image; image = image->next)
        if (image->type == type && image->color == color && image->w && image->h &&
            image->dst_x < bubble.box.x + bubble.box.w &&
            image->dst_x + image->w > bubble.box.x &&
            image->dst_y < bubble.box.y + bubble.box.h &&
            image->dst_y + image->h > bubble.box.y)
            return true;
    return false;
}

static void expect_side_images(ASS_Renderer *renderer, ASS_Track *track,
                                int message, bool right, uint32_t bubble)
{
    assert(image_by_order(renderer, track, IMAGE_TYPE_SHADOW, 0).color ==
           0x0000AA00u);
    assert(image_by_order(renderer, track, IMAGE_TYPE_SHADOW, message + 1).color ==
           bubble);
    assert(message_has_image_color(renderer, track, message, IMAGE_TYPE_CHARACTER,
                                   right ? 0xFFFFFF11u : 0xFF000066u));
    assert(message_has_image_color(renderer, track, message, IMAGE_TYPE_OUTLINE,
                                   right ? 0x00000055u : 0x111111AAu));
    assert(message_has_image_color(renderer, track, message, IMAGE_TYPE_OUTLINE,
                                   right ? 0xCC55FF44u : 0x00FFFF99u));
}

static void test_side_styles(ASS_Library *lib, ASS_Renderer *renderer)
{
    ASS_Track *legacy = make_track(lib,
        "{\\chatmode2\\msgm(Miku)\\msgshowname0}"
        "|{\\bubc&H112233&}Yurf:\\NA||Miku:\\NB||C|");
    assert(legacy);
    uint64_t legacy_hash = render_hash(renderer, legacy);
    ass_free_track(legacy);
    legacy = make_track(lib,
        "{\\chatmode2\\msgm(Miku)\\msgshowname0\\msgleft()\\msgright()}"
        "|{\\bubc&H112233&}Yurf:\\NA||Miku:\\NB||C|");
    assert(legacy && legacy_hash == render_hash(renderer, legacy));
    assert(image_by_order(renderer, legacy, IMAGE_TYPE_SHADOW, 3).color ==
           0x33221100u);
    ass_free_track(legacy);
    const char *payloads[] = {
        "{\\msg(Yurf)}A{\\msg(Miku)\\bubc&H0000FF&}B{\\msg(Miku)}C"
        "{\\msg(Yurf)}D{\\msg(Yurf,right)}E",
        "|Yurf:\\NA||{\\bubc&H0000FF&}Miku:\\NB||C||Yurf:\\ND||Miku:\\NE|",
        "{\\ta7}A\\N{\\ta9\\bubc&H0000FF&}B\\N{|}C\\N{\\ta4}D\\N{\\ta3}E",
    };
    for (size_t i = 0; i < sizeof(payloads) / sizeof(*payloads); i++) {
        char source[1024];
        snprintf(source, sizeof(source), "{\\chatmode%d\\msgm(Miku)\\msgshowname0"
                 "\\msgleft(" CHAT_LEFT_ARGS ")\\msgright(" CHAT_RIGHT_ARGS ")}%s",
                 (int) i + 1, payloads[i]);
        ASS_Track *track = make_track(lib, source);
        assert(track && render(renderer, track, 0).count == 6);
        const ChatSideStyle *left = &renderer->state.chat_side[0];
        const ChatSideStyle *right = &renderer->state.chat_side[1];
        assert(left->enabled && right->enabled);
        assert(left->c[0] == 0xFF000066u && left->c[1] == 0x00FF0077u &&
               left->c[2] == 0x111111AAu);
        assert(left->bubble.fill == 0x0000FF88u &&
               left->bubble.border == 0x00FFFF99u &&
               left->bubble.border_size == 3 && left->outline_size == 1);
        assert(right->c[0] == 0xFFFFFF11u && right->c[1] == 0xBBC53922u &&
               right->c[2] == 0x00000055u);
        assert(right->bubble.fill == 0x44223333u &&
               right->bubble.border == 0xCC55FF44u &&
               right->bubble.border_size == 4 && right->outline_size == 2);
        expect_side_images(renderer, track, 0, false, 0x0000FF88u);
        expect_side_images(renderer, track, 1, true, 0xFF000033u);
        expect_side_images(renderer, track, 2, true, 0x44223333u);
        expect_side_images(renderer, track, 3, false, 0x0000FF88u);
        expect_side_images(renderer, track, 4, true, 0x44223333u);
        assert(!renderer->state.chat_title.has_text);
        ass_free_track(track);
    }

    /* Same paint, layout and bitmaps as the individual tags, with no receipts. */
    ASS_Track *track = make_track(lib,
        "{\\chatmode2\\msgm(Miku)\\msgright(" CHAT_RIGHT_ARGS ")}|Miku:\\NA|");
    assert(track);
    uint64_t preset_hash = render_hash(renderer, track);
    ass_free_track(track);
    track = make_track(lib,
        "{\\chatmode2\\msgm(Miku)}|{\\c&HFFFFFF&\\1a&H11&\\2c&H39C5BB&\\2a&H22&"
        "\\3c&H332244&\\3a&H33&\\bubbc&HFF55CC&\\bubba&H44&\\bubbs4"
        "\\bc&H000000&\\ba&H55&\\bs2}Miku:\\NA|");
    assert(track && preset_hash == render_hash(renderer, track));
    ass_free_track(track);

    /* The other side also matches every individual appearance tag, including
     * name paint and both outline sizes in the output geometry and bitmaps. */
    track = make_track(lib,
        "{\\chatmode2\\msgm(Miku)\\msgleft(" CHAT_LEFT_ARGS ")}|Yurf:\\NA|");
    assert(track);
    uint64_t left_hash = render_hash(renderer, track);
    ass_free_track(track);
    track = make_track(lib,
        "{\\chatmode2\\msgm(Miku)}|{\\c&H0000FF&\\1a&H66&\\2c&H00FF00&\\2a&H77&"
        "\\3c&HFF0000&\\3a&H88&\\bubbc&HFFFF00&\\bubba&H99&\\bubbs3"
        "\\bc&H111111&\\ba&HAA&\\bs1}Yurf:\\NA|");
    assert(track && left_hash == render_hash(renderer, track));
    ass_free_track(track);

    /* An absent left preset sees the inherited defaults, not right-side paint.
     * A preset authored in a message's leading block applies to that message. */
    track = make_track(lib,
        "{\\chatmode2\\msgm(Miku)\\msgshowname0\\c&H445566&}"
        "|{\\msgright(" CHAT_RIGHT_ARGS ")}Miku:\\NA||Yurf:\\NB||Miku:\\NC|");
    assert(track);
    render(renderer, track, 0);
    expect_side_images(renderer, track, 0, true, 0x44223333u);
    assert(message_has_image_color(renderer, track, 1, IMAGE_TYPE_CHARACTER,
                                   0x66554400u));
    assert(image_by_order(renderer, track, IMAGE_TYPE_SHADOW, 2).color ==
           0x00AA0000u);
    expect_side_images(renderer, track, 2, true, 0x44223333u);
    ass_free_track(track);

    track = make_track(lib,
        "{\\chatmode2\\msgtitle(Miku)\\msgtitlec&H123456&\\msgtitlegbc&H223344&"
        "\\msgm(Miku)\\msgright(" CHAT_RIGHT_ARGS ")}|Miku:\\NA|");
    assert(track);
    assert(image_by_order(renderer, track, IMAGE_TYPE_SHADOW, 1).color == 0x44332200u);
    assert(image_by_order(renderer, track, IMAGE_TYPE_CHARACTER, 0).color ==
           0x56341200u);
    ass_free_track(track);

    const char *invalid[] = {
        "\\msgright(1,2)", "\\msgright(" CHAT_RIGHT_ARGS ",1)",
        "\\msgright(&HFFFFFF&,,&H39C5BB&,&H22&,&H332244&,&H33&,"
        "&HFF55CC&,&H44&,4,&H000000&,&H55&,2)",
        "\\msgright(&HFFFFFF&,&H100&,&H39C5BB&,&H22&,&H332244&,&H33&,"
        "&HFF55CC&,&H44&,4,&H000000&,&H55&,2)",
        "\\msgright(&HFFFFFF&,&H11&,&H39C5BB&,&H22&,&H332244&,&H33&,"
        "&HFF55CC&,&H44&,nan,&H000000&,&H55&,2)",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        char source[768];
        snprintf(source, sizeof(source), "{\\chatmode2\\msgm(Miku)"
                 "\\msgright(" CHAT_RIGHT_ARGS ")%s}|Miku:\\NA|", invalid[i]);
        track = make_track(lib, source);
        assert(track && preset_hash == render_hash(renderer, track));
        ass_free_track(track);
    }

    /* Existing actor defaults provide the baseline; actor side presets are
     * inherited and an event preset takes precedence without changing actors. */
    const char *metadata =
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,"
        "{\\c&H445566&\\msgright(" CHAT_RIGHT_ARGS ")}\n";
    track = make_actor_track(lib,
        "{\\chatmode2\\msgm(Miku)\\msgshowname0}|Yurf:\\NA||Miku:\\NB|", metadata, "Nene");
    assert(track);
    render(renderer, track, 0);
    assert(message_has_image_color(renderer, track, 0, IMAGE_TYPE_CHARACTER,
                                   0x66554400u));
    expect_side_images(renderer, track, 1, true, 0x44223333u);
    ass_free_track(track);
    track = make_actor_track(lib,
        "{\\chatmode2\\msgm(Miku)\\msgshowname0\\msgright(" CHAT_LEFT_ARGS ")}"
        "|Miku:\\NA||{\\r}Miku:\\NB||C|", metadata, "Nene");
    assert(track);
    render(renderer, track, 0);
    expect_side_images(renderer, track, 0, false, 0x0000FF88u);
    /* The reset reapplies actor defaults, including its next-message preset. */
    assert(renderer->state.chat_side[1].bubble.fill == 0x44223333u);
    assert(message_has_image_color(renderer, track, 1, IMAGE_TYPE_CHARACTER,
                                   0x66554400u));
    expect_side_images(renderer, track, 2, true, 0x44223333u);
    ass_free_track(track);

    /* Explicit style resets keep the existing actor whitelist rules. */
    track = make_actor_track(lib,
        "{\\chatmode2\\msgm(Miku)\\msgshowname0}|{\\rDefault}Miku:\\NA||B|",
        metadata, "Nene");
    assert(track);
    render(renderer, track, 0);
    assert(!renderer->state.chat_side[1].enabled);
    assert(message_has_image_color(renderer, track, 0, IMAGE_TYPE_CHARACTER,
                                   0xFFFFFF00u));
    assert(message_has_image_color(renderer, track, 1, IMAGE_TYPE_CHARACTER,
                                   0xFFFFFF00u));
    ass_free_track(track);

    track = make_track(lib,
        "{\\chatmode1\\msgm(Miku)\\msgshowname0\\msgright(" CHAT_RIGHT_ARGS ")}"
        "{\\msg(Miku)}{\\msg(Miku)}A");
    assert(track);
    assert(image_by_order(renderer, track, IMAGE_TYPE_SHADOW, 1).color == 0x44223333u);
    ass_free_track(track);
}

static int receipt_strokes(ASS_Renderer *renderer, ASS_Track *track,
                            long long time, int message)
{
    int changed = 0;
    ASS_Image *images = ass_render_frame(renderer, track, time, &changed);
    Box bubble = {0};
    int index = 0;
    for (ASS_Image *image = images; image; image = image->next)
        if (image->type == IMAGE_TYPE_SHADOW && index++ == message + 1) {
            bubble = (Box) {image->dst_x, image->dst_y, image->w, image->h};
            break;
        }
    if (!bubble.w)
        return 0;
    int strokes = 0;
    for (ASS_Image *image = images; image; image = image->next)
        if (image->type == IMAGE_TYPE_CHARACTER && image->w && image->h &&
            image->dst_y >= bubble.y + bubble.h - 12 &&
            image->dst_y < bubble.y + bubble.h &&
            image->dst_x >= bubble.x && image->dst_x < bubble.x + bubble.w)
            strokes++;
    return strokes;
}

static uint64_t message_text_hash(ASS_Renderer *renderer, ASS_Track *track)
{
    int changed = 0;
    ASS_Image *images = ass_render_frame(renderer, track, 0, &changed);
    uint64_t hash = UINT64_C(1469598103934665603);
    bool saw_text = false;
    for (ASS_Image *image = images; image; image = image->next) {
        if (image->type != IMAGE_TYPE_CHARACTER || !image->w || !image->h)
            continue;
        bool receipt = false, panel = true;
        for (ASS_Image *fill = images; fill; fill = fill->next) {
            if (fill->type != IMAGE_TYPE_SHADOW)
                continue;
            if (panel) {
                panel = false;
                continue;
            }
            /* The receipt fixtures have no header, names or descenders;
             * metadata occupies the last 12 pixels of the bubble padding. */
            if (image->dst_y >= fill->dst_y + fill->h - 12 &&
                image->dst_y < fill->dst_y + fill->h &&
                image->dst_x >= fill->dst_x &&
                image->dst_x < fill->dst_x + fill->w) {
                receipt = true;
                break;
            }
        }
        if (!receipt) {
            saw_text = true;
            hash = hash_image(hash, image);
        }
    }
    assert(saw_text);
    return hash;
}

static void test_receipts(ASS_Library *lib, ASS_Renderer *renderer)
{
    const char *payloads[] = {
        "{\\msg(Miku)}A{\\msg(Yurf)}B{\\msg(Miku)}C{\\msg(Miku)}D",
        "|Miku:\\NA||Yurf:\\NB||Miku:\\NC||D|",
        "{\\ta9}A\\N{\\ta7}B\\N{\\ta6}C\\N{|}D",
    };
    const struct { const char *tags; int strokes; } cases[] = {
        {"", 0}, {"\\readtime0", 4}, {"\\readmark0", 0},
        {"\\readmark1", 2}, {"\\readmark2", 4},
        {"\\readmark2\\readtime0", 4}, {"\\readmark1\\readtime1245", 2},
        {"\\readmark0\\readtime1245", 0},
        {"\\readmark9\\readtime-1", 0}, {"\\readmark(2)\\readtime(1245)", 0},
    };
    for (size_t mode = 0; mode < sizeof(payloads) / sizeof(*payloads); mode++) {
        Frame legacy = {0};
        uint64_t legacy_hash = 0;
        uint64_t legacy_text_hash = 0;
        char source[768];
        for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
            snprintf(source, sizeof(source), "{\\chatmode%d\\msgm(Miku)\\msgshowname0%s}%s",
                     (int) mode + 1, cases[i].tags, payloads[mode]);
            ASS_Track *track = make_track(lib, source);
            assert(track);
            Frame frame = render(renderer, track, 0);
            if (!i) {
                legacy = frame;
                legacy_text_hash = message_text_hash(renderer, track);
                legacy_hash = render_hash(renderer, track);
            }
            assert(frame.count == legacy.count);
            /* Receipts add only native metadata: text pixels, positions and
             * wrapping remain identical to the receipt-free output. */
            assert(message_text_hash(renderer, track) == legacy_text_hash);
            for (int j = 0; j < frame.count; j++)
                assert(same_box(frame.boxes[j], legacy.boxes[j]));
            assert(receipt_strokes(renderer, track, 0, 0) == cases[i].strokes);
            assert(receipt_strokes(renderer, track, 0, 1) == 0);
            assert(receipt_strokes(renderer, track, 0, 2) == cases[i].strokes);
            assert(receipt_strokes(renderer, track, 0, 3) == cases[i].strokes);
            assert(receipt_strokes(renderer, track, 2000, 0) == cases[i].strokes);
            if (!cases[i].strokes)
                assert(legacy_hash == render_hash(renderer, track));
            ass_free_track(track);
        }
        snprintf(source, sizeof(source), "{\\chatmode%d\\msgm(Miku)\\msgshowname0"
                 "\\readmark2\\readtime1245\\msgstartcount(1)\\msgtime(1000,3000,5000)"
                 "\\msganim(0)}%s", (int) mode + 1, payloads[mode]);
        ASS_Track *track = make_track(lib, source);
        assert(track);
        assert(receipt_strokes(renderer, track, 1244, 0) == 2);
        assert(receipt_strokes(renderer, track, 1245, 0) == 4);
        assert(receipt_strokes(renderer, track, 3000, 2) == 2);
        assert(receipt_strokes(renderer, track, 4244, 2) == 2);
        assert(receipt_strokes(renderer, track, 4245, 2) == 4);
        assert(receipt_strokes(renderer, track, 5000, 3) == 2);
        assert(receipt_strokes(renderer, track, 6244, 3) == 2);
        assert(receipt_strokes(renderer, track, 6245, 3) == 4);
        assert(receipt_strokes(renderer, track, 6245, 1) == 0);
        ass_free_track(track);
    }

    ASS_Track *track = make_track(lib,
        "{\\chatmode2\\msgm(Miku)\\msgshowname0}"
        "|{\\readmark1}Miku:\\NA||Yurf:\\NB||Miku:\\NC|"
        "|{\\readmark2\\readtime1245}D||Yurf:\\NE||Miku:\\NF|");
    assert(track);
    assert(receipt_strokes(renderer, track, 1245, 0) == 2);
    assert(receipt_strokes(renderer, track, 1245, 1) == 0);
    assert(receipt_strokes(renderer, track, 1245, 2) == 2);
    assert(receipt_strokes(renderer, track, 1244, 3) == 2);
    assert(receipt_strokes(renderer, track, 1245, 3) == 4);
    assert(receipt_strokes(renderer, track, 1245, 4) == 0);
    assert(receipt_strokes(renderer, track, 1244, 5) == 2);
    assert(receipt_strokes(renderer, track, 1245, 5) == 4);
    ass_free_track(track);

    track = make_track(lib,
        "{\\chatmode1\\msgm(Miku)\\msgshowname0\\readmark2}"
        "{\\msg(Yurf,right)}A{\\msg(Miku,left)}B{\\msg(Miku)}C");
    assert(track);
    assert(receipt_strokes(renderer, track, 0, 0) == 0);
    assert(receipt_strokes(renderer, track, 0, 1) == 0);
    assert(receipt_strokes(renderer, track, 0, 2) == 4);
    ass_free_track(track);

    track = make_track(lib,
        "{\\chatmode3\\readtime1245\\msgstartcount(0)\\msgtime(1000)\\msganim(250)}"
        "{\\ta9}A");
    assert(track);
    assert(receipt_strokes(renderer, track, 1000, 0) == 0);
    assert(receipt_strokes(renderer, track, 1250, 0) == 2);
    assert(receipt_strokes(renderer, track, 2244, 0) == 2);
    assert(receipt_strokes(renderer, track, 2245, 0) == 4);
    ass_free_track(track);

    /* Receipt defaults can come from the existing actor metadata. */
    track = make_actor_track(lib,
        "{\\chatmode2\\msgm(Miku)\\msgshowname0}|Miku:\\NA||Yurf:\\NB||Miku:\\NC|",
        "Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,"
        "{\\readtime1245}\n", "Nene");
    assert(track);
    assert(receipt_strokes(renderer, track, 1244, 0) == 2);
    assert(receipt_strokes(renderer, track, 1245, 0) == 4);
    assert(receipt_strokes(renderer, track, 1244, 1) == 0);
    assert(receipt_strokes(renderer, track, 1244, 2) == 2);
    assert(receipt_strokes(renderer, track, 1245, 2) == 4);
    ass_free_track(track);

    track = make_track(lib, "{\\an9\\pos(1850,80)\\chatmode3\\readmark2"
                           "\\clip(0,0,1800,1080)}{\\ta9}A");
    assert(track);
    int changed = 0;
    for (ASS_Image *image = ass_render_frame(renderer, track, 0, &changed);
         image; image = image->next)
        assert(image->dst_x + image->w <= 1800);
    ass_free_track(track);

    track = make_track(lib, "{\\chatmode3\\readtime0}{\\ta9}A");
    assert(track);
    ImageSample bubble = image_by_order(renderer, track, IMAGE_TYPE_SHADOW, 1);
    assert(bubble.found);
    int strokes = 0;
    ASS_ImageRGBA *rgba = ass_render_frame_rgba(renderer, track, 0, &changed);
    assert(rgba);
    for (ASS_ImageRGBA *image = rgba;
         image; image = image->next)
        if (image->type == IMAGE_TYPE_CHARACTER && image->w && image->h &&
            image->dst_y >= bubble.box.y + bubble.box.h - 12 &&
            image->dst_y < bubble.box.y + bubble.box.h)
            strokes++;
    assert(strokes == 4);
    ass_free_images_rgba(rgba);
    ass_free_track(track);
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

    test_title_colors(lib, renderer);
    test_side_styles(lib, renderer);
    test_receipts(lib, renderer);

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
        "{\\an9\\pos(1850,80)\\chatmode2\\msgm(Miku)}"
        "|{\\1c&HFFFFFF&\\2c&H39C5BB&\\3c&H303030&\\4c&H181818&"
        "\\1a&H10&\\2a&H20&\\3a&H30&\\4a&H40&}Miku:\\NHello|");
    assert(track);
    ImageSample old_panel = widest_image(renderer, track, IMAGE_TYPE_SHADOW, 0);
    ImageSample old_bubble = widest_image(renderer, track, IMAGE_TYPE_SHADOW, 1);
    assert(old_panel.found && old_panel.color == UINT32_C(0x18181840));
    assert(old_bubble.found && old_bubble.color == UINT32_C(0x30303030));
    assert(has_image_color(renderer, track, IMAGE_TYPE_CHARACTER,
                           UINT32_C(0xFFFFFF10)));
    assert(has_image_color(renderer, track, IMAGE_TYPE_CHARACTER,
                           UINT32_C(0xBBC53920)));
    ass_free_track(track);

    assert(bubble_color(lib, renderer,
        "{\\chatmode2\\msgshowname0}|{\\3c&H111111&"
        "\\bubc&H222222&}Miku:\\NHi|") == UINT32_C(0x22222200));
    assert(bubble_color(lib, renderer,
        "{\\chatmode2\\msgshowname0}|{\\bubc&H222222&"
        "\\3c&H111111&}Miku:\\NHi|") == UINT32_C(0x11111100));
    assert((bubble_color(lib, renderer,
        "{\\chatmode2\\msgshowname0}|{\\3a&H80&"
        "\\buba&H20&}Miku:\\NHi|") & 0xFFu) == 0x20u);
    assert((bubble_color(lib, renderer,
        "{\\chatmode2\\msgshowname0}|{\\buba128"
        "\\3a&H40&}Miku:\\NHi|") & 0xFFu) == 0x40u);

    track = make_track(lib,
        "{\\chatmode2\\msgshowname0}|{\\bubbs3\\alpha&H40&}Hi|");
    assert(track);
    assert((widest_image(renderer, track, IMAGE_TYPE_SHADOW, 1).color &
            0xFFu) == 0x40u);
    assert((widest_image(renderer, track, IMAGE_TYPE_OUTLINE, 0).color &
            0xFFu) == 0x40u);
    ass_free_track(track);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode2\\msgshowname0}"
        "|{\\bubc&H303030&\\bubbc&H39C5BB&\\bubba&H40&"
        "\\bubbs4}Miku:\\NHello|");
    assert(track);
    ImageSample border_panel = widest_image(renderer, track,
                                             IMAGE_TYPE_SHADOW, 0);
    ImageSample border_fill = widest_image(renderer, track,
                                            IMAGE_TYPE_SHADOW, 1);
    ImageSample border_ring = widest_image(renderer, track,
                                            IMAGE_TYPE_OUTLINE, 0);
    assert(border_panel.found && border_fill.found && border_ring.found);
    assert(border_ring.color == UINT32_C(0xBBC53940));
    assert(border_ring.box.w > border_fill.box.w);
    assert(border_ring.box.x >= border_panel.box.x);
    assert(border_ring.box.x + border_ring.box.w <=
           border_panel.box.x + border_panel.box.w);
    ass_free_track(track);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode2\\msgshowname0}"
        "|{\\bubbc&H39C5BB&\\bubbs0}Miku:\\NHello|");
    assert(track);
    assert(!widest_image(renderer, track, IMAGE_TYPE_OUTLINE, 0).found);
    ass_free_track(track);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode2\\msgshowname0}"
        "|{\\bubbc&H39C5BB&\\bubbs-5}Miku:\\NHello|");
    assert(track);
    assert(!widest_image(renderer, track, IMAGE_TYPE_OUTLINE, 0).found);
    ass_free_track(track);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode2\\msgshowname0}"
        "|{\\bubc&H303030&\\buba&HFF&\\bubbc&H39C5BB&"
        "\\bubbs8}Miku:\\NHello|");
    assert(track);
    assert(widest_image(renderer, track, IMAGE_TYPE_OUTLINE, 0).found);
    assert(!has_image_color(renderer, track, IMAGE_TYPE_SHADOW,
                            UINT32_C(0x303030FF)));
    ass_free_track(track);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode2\\msgshowname0}"
        "|{\\c&HFFFFFF&\\bc&HFF55CC&\\ba&H40&\\bs3"
        "\\3c&H303030&\\bubbc&H00FF00&\\bubbs4}Miku:\\NHello|");
    assert(track);
    assert(has_image_color(renderer, track, IMAGE_TYPE_OUTLINE,
                           UINT32_C(0xCC55FF40)));
    assert(has_image_color(renderer, track, IMAGE_TYPE_OUTLINE,
                           UINT32_C(0x00FF0000)));
    assert(widest_image(renderer, track, IMAGE_TYPE_SHADOW, 1).color ==
           UINT32_C(0x30303000));
    ass_free_track(track);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode2\\msgshowname0}"
        "|{\\bubbc&HFFFFFF&\\bubbs80}Miku:\\NHi|");
    assert(track);
    ImageSample thick_panel = widest_image(renderer, track,
                                            IMAGE_TYPE_SHADOW, 0);
    ImageSample thick_fill = widest_image(renderer, track,
                                           IMAGE_TYPE_SHADOW, 1);
    ImageSample thick_ring = widest_image(renderer, track,
                                           IMAGE_TYPE_OUTLINE, 0);
    assert(thick_panel.found && thick_fill.found && thick_ring.found);
    assert(thick_ring.box.w - thick_fill.box.w >
           border_ring.box.w - border_fill.box.w);
    assert(thick_ring.box.x >= thick_panel.box.x &&
           thick_ring.box.x + thick_ring.box.w <=
           thick_panel.box.x + thick_panel.box.w);
    assert(thick_ring.box.y >= thick_panel.box.y &&
           thick_ring.box.y + thick_ring.box.h <=
           thick_panel.box.y + thick_panel.box.h);
    ass_free_track(track);

    const char *inherited_styles =
        "{\\an9\\pos(1850,80)\\chatmode2\\msgm(Miku)\\msgshowname0}"
        "|{\\bubc&H111111&\\bubbc&H222222&\\bubba&H20&"
        "\\bubbs2\\bc&H333333&\\ba&H40&\\bs1}Miku:\\NA|"
        "|B|"
        "|{\\bubc&H444444&\\bubbs5}Yurf:\\NC|"
        "|D|";
    track = make_track(lib, inherited_styles);
    assert(track);
    ImageSample inherited_a = image_by_order(renderer, track,
                                              IMAGE_TYPE_SHADOW, 1);
    ImageSample inherited_b = image_by_order(renderer, track,
                                              IMAGE_TYPE_SHADOW, 2);
    ImageSample inherited_c = image_by_order(renderer, track,
                                              IMAGE_TYPE_SHADOW, 3);
    ImageSample inherited_d = image_by_order(renderer, track,
                                              IMAGE_TYPE_SHADOW, 4);
    assert(inherited_a.found && inherited_b.found &&
           inherited_c.found && inherited_d.found);
    assert(inherited_a.color == UINT32_C(0x11111100));
    assert(inherited_b.color == inherited_a.color);
    assert(inherited_c.color == UINT32_C(0x44444400));
    assert(inherited_d.color == inherited_c.color);
    uint64_t inherited_hash = render_hash(renderer, track);
    ass_free_track(track);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode2\\msgm(Miku)\\msgshowname0}"
        "|{\\bubc&H111111&\\bubbc&H222222&\\bubba&H20&"
        "\\bubbs2\\bc&H333333&\\ba&H40&\\bs1}Miku:\\NA|"
        "|{\\bubc&H111111&\\bubbc&H222222&\\bubba&H20&"
        "\\bubbs2\\bc&H333333&\\ba&H40&\\bs1}B|"
        "|{\\bubc&H444444&\\bubbs5}Yurf:\\NC|"
        "|{\\bubc&H444444&\\bubbc&H222222&\\bubba&H20&"
        "\\bubbs5\\bc&H333333&\\ba&H40&\\bs1}D|");
    assert(track);
    assert(inherited_hash == render_hash(renderer, track));
    ass_free_track(track);

    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode2\\msgshowname0}"
        "|{\\bubc&H123456&\\bubbc&HFFFFFF&\\bubbs4"
        "\\bc&HFF55CC&\\ba&H40&\\bs3\\r}Hi|");
    assert(track);
    uint64_t reset_hash = render_hash(renderer, track);
    ass_free_track(track);
    track = make_track(lib,
        "{\\an9\\pos(1850,80)\\chatmode2\\msgshowname0}|Hi|");
    assert(track);
    assert(reset_hash == render_hash(renderer, track));
    ass_free_track(track);

    const char *all_modes[] = {
        "{\\an9\\pos(1850,80)\\chatmode1\\msgtitle(Miku)"
        "\\msgshowname0}{\\msg(Miku)\\bubc&H303030&"
        "\\bubbc&H00FF00&\\bubbs3\\bc&HFF55CC&\\bs2}Hi",
        "{\\an9\\pos(1850,80)\\chatmode2\\msgtitle(Miku)"
        "\\msgshowname0}|{\\bubc&H303030&\\bubbc&H00FF00&"
        "\\bubbs3\\bc&HFF55CC&\\bs2}Miku:\\NHi|",
        "{\\an9\\pos(1850,80)\\chatmode3\\msgtitle(Miku)}"
        "{\\ta7\\bubc&H303030&\\bubbc&H00FF00&\\bubbs3"
        "\\bc&HFF55CC&\\bs2}Hi",
    };
    for (size_t i = 0; i < sizeof(all_modes) / sizeof(*all_modes); i++) {
        track = make_track(lib, all_modes[i]);
        assert(track);
        assert(render(renderer, track, 0).count >= 3);
        assert(has_image_color(renderer, track, IMAGE_TYPE_OUTLINE,
                               UINT32_C(0x00FF0000)));
        assert(has_image_color(renderer, track, IMAGE_TYPE_OUTLINE,
                               UINT32_C(0xCC55FF00)));
        assert(has_image_color(renderer, track, IMAGE_TYPE_SHADOW,
                               UINT32_C(0x30303000)));
        ass_free_track(track);
    }

    track = make_track(lib,
        "{\\chatmode2\\msgshowname0}|{\\t(0,1000,"
        "\\bubc&H303030&\\bubbc&H00FF00&\\bubbs4"
        "\\bc&HFF55CC&\\bs2)}Hi|");
    assert(track);
    assert(render(renderer, track, 500).count >= 2);
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
                            "\\3c&H00FF00&\\4c&HFF0000&"
                            "\\msgtitlec&H39C5BB&\\msgtitlegbc&H223344&"
                            "\\msgright(" CHAT_RIGHT_ARGS ")\\readmark2\\readtime1245}A\\NB{|}");
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
