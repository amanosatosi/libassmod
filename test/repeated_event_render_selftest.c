/* SPDX-License-Identifier: ISC */
#include "ass.h"
#include "ass_render.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EVENTS 576
static int failures;
static double fast_seconds, reference_seconds;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "repeated event line %d: %s\n", __LINE__, #c); \
    failures++; } } while (0)

static void quiet(int level, const char *fmt, va_list args, void *data)
{
    (void) level; (void) fmt; (void) args; (void) data;
}

static bool add_font(ASS_Library *lib, const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    char data[4096];
    size_t size = fread(data, 1, sizeof(data), file);
    bool ok = size && size < sizeof(data) && !ferror(file);
    fclose(file);
    if (ok) ass_add_font(lib, (char *) path, data, (int) size);
    return ok;
}

static ASS_Track *new_track(ASS_Library *lib)
{
    char script[] =
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n"
        "ScaledBorderAndShadow: yes\nWrapStyle: 0\nKerning: yes\n"
        "[V4+ Styles]\n"
        "Format: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,OutlineColour,BackColour,"
        "Bold,Italic,Underline,StrikeOut,ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,"
        "Alignment,MarginL,MarginR,MarginV,Encoding\n"
        "Style: Default,Deco Primary A,32,&H00FFFFFF,&H0000FFFF,&H00000000,&H00000000,"
        "0,0,0,0,100,100,0,0,1,2,2,7,20,20,20,1\n"
        "[Events]\n"
        "Format: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text\n";
    return ass_read_memory(lib, script, sizeof(script) - 1, NULL);
}

static bool add_event(ASS_Track *track, const char *tags, const char *text)
{
    int index = ass_alloc_event(track);
    if (index < 0) return false;
    ASS_Event *event = track->events + index;
    event->Style = track->default_style;
    event->Start = 0;
    event->Duration = 2000;
    event->ReadOrder = index;
    size_t len = strlen(tags) + strlen(text) + 64;
    event->Text = malloc(len);
    if (!event->Text) return false;
    snprintf(event->Text, len, "{\\an7\\pos(80,80)%s}%s", tags, text);
    return true;
}

static unsigned legacy_count(ASS_Image *img, bool visible)
{
    unsigned count = 0;
    for (; img; img = img->next)
        count += !visible || (img->color & 255) != 255;
    return count;
}

static ASS_Image *visible_legacy(ASS_Image *img)
{
    while (img && (img->color & 255) == 255) img = img->next;
    return img;
}

/* Compare ordered visible tiles and every alpha byte, rather than a union of
 * coverage. This also catches changes to overlapping painter composition. */
static bool equal_legacy(ASS_Image *a, ASS_Image *b)
{
    for (;;) {
        a = visible_legacy(a); b = visible_legacy(b);
        if (!a || !b) return a == b;
        if (a->w != b->w || a->h != b->h || a->dst_x != b->dst_x ||
            a->dst_y != b->dst_y || a->color != b->color || a->type != b->type)
            return false;
        for (int y = 0; y < a->h; y++)
            if (memcmp(a->bitmap + (ptrdiff_t) y * a->stride,
                       b->bitmap + (ptrdiff_t) y * b->stride, a->w)) return false;
        a = a->next; b = b->next;
    }
}

static bool rgba_visible(ASS_ImageRGBA *img)
{
    for (int y = 0; y < img->h; y++)
        for (int x = 0; x < img->w; x++)
            if (img->rgba[(ptrdiff_t) y * img->stride + 4 * x + 3]) return true;
    return false;
}

static bool equal_rgba(ASS_ImageRGBA *a, ASS_ImageRGBA *b)
{
    for (;;) {
        while (a && !rgba_visible(a)) a = a->next;
        while (b && !rgba_visible(b)) b = b->next;
        if (!a || !b) return a == b;
        if (a->w != b->w || a->h != b->h || a->dst_x != b->dst_x ||
            a->dst_y != b->dst_y || a->type != b->type) return false;
        for (int y = 0; y < a->h; y++)
            if (memcmp(a->rgba + (ptrdiff_t) y * a->stride,
                       b->rgba + (ptrdiff_t) y * b->stride, 4 * a->w)) return false;
        a = a->next; b = b->next;
    }
}

static uint64_t mask_hash(ASS_Image *img)
{
    uint64_t h = UINT64_C(1469598103934665603);
    for (; img; img = img->next)
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w; x++) {
                h ^= img->bitmap[(ptrdiff_t) y * img->stride + x];
                h *= UINT64_C(1099511628211);
            }
    return h;
}

static void compare_frame(ASS_Renderer *fast, ASS_Renderer *reference,
                          ASS_Track *track, long long time, bool rgba)
{
    int changed;
    clock_t begin = clock();
    if (rgba) {
        ASS_ImageRGBA *a = ass_render_frame_rgba(fast, track, time, &changed);
        fast_seconds = (double) (clock() - begin) / CLOCKS_PER_SEC;
        begin = clock();
        ASS_ImageRGBA *b = ass_render_frame_rgba(reference, track, time, &changed);
        reference_seconds = (double) (clock() - begin) / CLOCKS_PER_SEC;
        CHECK(a && b && equal_rgba(a, b));
        ass_free_images_rgba(a); ass_free_images_rgba(b);
    } else {
        ASS_Image *a = ass_render_frame(fast, track, time, &changed);
        fast_seconds = (double) (clock() - begin) / CLOCKS_PER_SEC;
        begin = clock();
        ASS_Image *b = ass_render_frame(reference, track, time, &changed);
        reference_seconds = (double) (clock() - begin) / CLOCKS_PER_SEC;
        CHECK(a && b && equal_legacy(a, b));
        CHECK(legacy_count(a, false) == legacy_count(a, true));
    }
}

static void stress(ASS_Library *lib, ASS_Renderer *fast, ASS_Renderer *reference)
{
    /* Earlier lifetime controls deliberately evict only one renderer. Start
     * each comparison with identical cache temperatures for build counters. */
    ass_set_frame_size(fast, 640, 361);
    ass_set_frame_size(reference, 640, 361);
    ass_set_frame_size(fast, 640, 360);
    ass_set_frame_size(reference, 640, 360);
    ASS_Track *track = new_track(lib);
    CHECK(track);
    if (!track) return;
    for (int i = 0; i < EVENTS; i++) {
        char tags[256];
        snprintf(tags, sizeof(tags),
            "\\1c&H%06X&\\1a&H%02X&\\2a&H80&\\4a&HFF&"
            "\\clip(%d,50,%d,180)\\t(0,1500,\\clip(%d,50,%d,180))",
            (i * 7919) & 0xffffff, 64 + i % 128,
            70 + i % 8, 330 + i % 8, 100 + i % 16, 350 + i % 16);
        CHECK(add_event(track, tags, "ABC ABC ABC ABC"));
    }
    ASS_Image *held = NULL;
    uint64_t held_hash = 0;
    for (int backend = 0; backend < 2; backend++) {
        for (int frame = 0; frame < 3; frame++) {
            compare_frame(fast, reference, track, frame * 500, backend != 0);
            CHECK(reference->repeated_event_stats.shapes == EVENTS);
            CHECK(fast->repeated_event_stats.shapes == 1);
            CHECK(fast->repeated_event_stats.geometry == 1);
            CHECK(fast->repeated_event_stats.reuse_hits == EVENTS - 1);
            CHECK(fast->repeated_event_stats.glyph_bitmap_requests > 0);
            CHECK(reference->repeated_event_stats.glyph_bitmap_requests ==
                  fast->repeated_event_stats.glyph_bitmap_requests * EVENTS);
            CHECK(reference->repeated_event_stats.composite_lookups ==
                  fast->repeated_event_stats.composite_lookups * EVENTS);
            CHECK(reference->repeated_event_stats.bitmap_constructions ==
                  fast->repeated_event_stats.bitmap_constructions);
            CHECK(reference->repeated_event_stats.composite_constructions ==
                  fast->repeated_event_stats.composite_constructions);
            CHECK(fast->repeated_event_stats.memo_bytes <= 2 * MEGABYTE);
            CHECK(fast->repeated_event_stats.transparent_skips > 0);
            CHECK(fast->repeated_event_stats.images < reference->repeated_event_stats.images);
            printf("backend=%d frame=%d shapes=%llu/%llu glyph_bitmaps=%llu/%llu "
                   "composites=%llu/%llu images=%llu/%llu memo=%zu seconds=%.6f/%.6f "
                   "bitmap_builds=%llu/%llu composite_builds=%llu/%llu\n",
                   backend, frame,
                   (unsigned long long) reference->repeated_event_stats.shapes,
                   (unsigned long long) fast->repeated_event_stats.shapes,
                   (unsigned long long) reference->repeated_event_stats.glyph_bitmap_requests,
                   (unsigned long long) fast->repeated_event_stats.glyph_bitmap_requests,
                   (unsigned long long) reference->repeated_event_stats.composite_lookups,
                   (unsigned long long) fast->repeated_event_stats.composite_lookups,
                   (unsigned long long) reference->repeated_event_stats.images,
                   (unsigned long long) fast->repeated_event_stats.images,
                   fast->repeated_event_stats.memo_bytes, reference_seconds, fast_seconds,
                   (unsigned long long) reference->repeated_event_stats.bitmap_constructions,
                   (unsigned long long) fast->repeated_event_stats.bitmap_constructions,
                   (unsigned long long) reference->repeated_event_stats.composite_constructions,
                   (unsigned long long) fast->repeated_event_stats.composite_constructions);
            if (!backend && !frame) {
                held = fast->images_root;
                ass_frame_ref(held);
                held_hash = mask_hash(held);
            }
            if (held) CHECK(mask_hash(held) == held_hash);
        }
    }
    CHECK(held && ((ASS_ImagePriv *) held)->source);
    /* Cropped legacy images reference existing composite storage directly. */
    if (held && held->next)
        CHECK(((ASS_ImagePriv *) held)->source == ((ASS_ImagePriv *) held->next)->source);
    ass_set_frame_size(fast, 640, 361);
    CHECK(!fast->repeated_geometry);
    CHECK(mask_hash(held) == held_hash);
    ass_frame_unref(held);
    ass_set_frame_size(fast, 640, 360);
    int changed;
    ass_render_frame(fast, track, 3000, &changed);
    CHECK(!fast->repeated_geometry);
    ass_free_track(track);
}

static void controls(ASS_Library *lib, ASS_Renderer *fast, ASS_Renderer *reference)
{
    const struct { const char *a, *b, *text; bool reuse; } cases[] = {
        {"\\1c&H112233&", "\\1c&H332211&", "ABC", true},
        {"\\1a&H40&", "\\1a&H80&", "ABC", true},
        {"\\4a&H00&", "\\4a&HFF&", "ABC", true},
        {"\\clip(70,50,150,180)", "\\clip(100,50,200,180)", "ABC", true},
        {"\\clip(70,50,150,180)", "\\iclip(90,50,110,100)", "ABC", true},
        {"", "", "ACC", false},
        {"", "\\fnDeco Primary B", "ABC", false},
        {"", "\\fs48", "ABC", false},
        {"", "\\b1", "ABC", false},
        {"", "\\i1", "ABC", false},
        {"", "\\fsp3", "ABC", false},
        {"", "\\fscx120", "ABC", false},
        {"", "\\fscy120", "ABC", false},
        {"", "\\bord4", "ABC", false},
        {"", "\\frz15", "ABC", false},
        {"", "\\t(0,1500,\\fs48)", "ABC", false},
        {"\\1a&H00&", "\\1a&H80&", "ABC", false},
        {"\\1a&H80&", "\\1a&HFF&", "ABC", false},
        {"", "\\clip(m 70 50 l 150 50 150 180 70 180)", "ABC", false},
        {"\\kf100", "\\kf100", "ABC", false},
        {"\\rnd10\\rnds42", "\\rnd10\\rnds42", "ABC", false},
        {"\\distort(0.2,0.1,0.8,0.2,0.9,0.9,0.1,0.8)",
         "\\distort(0.2,0.1,0.8,0.2,0.9,0.9,0.1,0.8)", "ABC", false},
        {"\\vert1", "\\vert1", "ABC", false},
        {"\\furi1", "\\furi1", "<A|B>", false},
        {"\\ct(m 0 0 b 60 -30 120 30 180 0)",
         "\\ct(m 0 0 b 60 -30 120 30 180 0)", "ABC", false},
        {"\\clip(70,50,150,180)\\clippos(5,5)",
         "\\clip(70,50,150,180)\\clippos(5,5)", "ABC", false},
        {"\\u1", "\\u1", "ABC", false},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ASS_Track *track = new_track(lib);
        CHECK(track);
        if (!track) continue;
        CHECK(add_event(track, cases[i].a, "ABC"));
        CHECK(add_event(track, cases[i].b, cases[i].text));
        for (int backend = 0; backend < 2; backend++) {
            compare_frame(fast, reference, track, 500, backend != 0);
            CHECK(fast->repeated_event_stats.reuse_hits == (unsigned) cases[i].reuse);
            CHECK(fast->repeated_event_stats.shapes == (cases[i].reuse ? 1u : 2u));
        }
        ass_free_track(track);
    }
    ASS_Track *track = new_track(lib);
    CHECK(track);
    if (!track) return;
    CHECK(add_event(track, "", "ABC")); CHECK(add_event(track, "", "ABC"));
    size_t limit = fast->cache.composite_max_size;
    fast->cache.composite_max_size = 0;
    compare_frame(fast, reference, track, 500, false);
    CHECK(fast->repeated_event_stats.shapes == 2 && !fast->repeated_geometry);
    fast->cache.composite_max_size = limit;
    compare_frame(fast, reference, track, 500, false);
    CHECK(fast->repeated_event_stats.reuse_hits == 1);
    ass_set_fonts(fast, NULL, "Deco Primary A", ASS_FONTPROVIDER_NONE, NULL, 1);
    CHECK(!fast->repeated_geometry);
    compare_frame(fast, reference, track, 500, false);
    CHECK(fast->repeated_event_stats.reuse_hits == 1);
    ass_set_cache_limits(fast, 1, 1);
    CHECK(!fast->repeated_geometry);
    compare_frame(fast, reference, track, 500, false);
    CHECK(fast->repeated_event_stats.memo_bytes <= fast->cache.composite_max_size / 16);
    ass_set_cache_limits(fast, 0, 0);
    ass_free_track(track);
}

int main(int argc, char **argv)
{
    if (argc != 4) return 1;
    ASS_Library *lib = ass_library_init();
    if (!lib) return 1;
    ass_set_message_cb(lib, quiet, NULL);
    for (int i = 1; i < argc; i++) CHECK(add_font(lib, argv[i]));
    ASS_Renderer *fast = ass_renderer_init(lib);
    ASS_Renderer *reference = ass_renderer_init(lib);
    if (!fast || !reference) {
        ass_renderer_done(fast); ass_renderer_done(reference); ass_library_done(lib);
        return 1;
    }
    ASS_Renderer *renderers[] = {fast, reference};
    for (unsigned i = 0; i < 2; i++) {
        ass_set_frame_size(renderers[i], 640, 360);
        ass_set_fonts(renderers[i], NULL, "Deco Primary A", ASS_FONTPROVIDER_NONE, NULL, 1);
    }
    reference->debug_disable_event_reuse = true;
    reference->debug_keep_transparent_images = true;
    for (int shaping = 0; shaping < 2; shaping++) {
        for (unsigned i = 0; i < 2; i++)
            ass_set_shaper(renderers[i], shaping ? ASS_SHAPING_COMPLEX : ASS_SHAPING_SIMPLE);
        stress(lib, fast, reference);
        controls(lib, fast, reference);
    }
    ass_renderer_done(fast); ass_renderer_done(reference); ass_library_done(lib);
    return !!failures;
}
