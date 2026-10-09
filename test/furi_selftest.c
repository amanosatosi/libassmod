#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ass.h"

#define FRAME_W 384
#define FRAME_H 216

typedef struct {
    uint8_t *alpha;
    int x0, y0, x1, y1;
    bool empty;
} Mask;

typedef struct {
    uint64_t primary;
    uint64_t secondary;
} KaraokeCounts;

typedef struct {
    uint32_t *primary;
    uint32_t *secondary;
    uint32_t *outline;
} KaraokeFrame;

static unsigned char *test_font_data;
static int test_font_size;

static int expect_same(const char *a, const char *b);
static int expect_different(const char *a, const char *b);

static bool load_test_font(void)
{
    const char *env = getenv("FURI_TEST_FONT");
    const char *paths[] = {
        env,
        "compare/test/font1.ttf",
        "../compare/test/font1.ttf",
        "../../compare/test/font1.ttf",
    };
    FILE *file = NULL;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (!paths[i])
            continue;
        file = fopen(paths[i], "rb");
        if (file)
            break;
    }
    if (!file)
        return false;

    bool ok = fseek(file, 0, SEEK_END) == 0;
    long size = ok ? ftell(file) : -1;
    ok = size > 0 && size <= INT_MAX && fseek(file, 0, SEEK_SET) == 0;
    unsigned char *data = ok ? malloc(size) : NULL;
    ok = data && fread(data, 1, size, file) == (size_t) size;
    fclose(file);
    if (!ok) {
        free(data);
        return false;
    }
    test_font_data = data;
    test_font_size = size;
    return true;
}

static void test_message(int level, const char *format, va_list args, void *data)
{
    (void) data;
    if (level <= 2) {
        vfprintf(stderr, format, args);
        fputc('\n', stderr);
    }
}

static void add_test_font(ASS_Library *lib)
{
    ass_set_message_cb(lib, test_message, NULL);
    ass_add_font(lib, "font1.ttf", (const char *) test_font_data,
                 test_font_size);
}

static char *make_script(const char *text)
{
    const char *prefix =
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: 384\n"
        "PlayResY: 216\n"
        "ScaledBorderAndShadow: yes\n"
        "\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,48,&H00FFFFFF,&H00FFFFFF,&H00000000,&H64000000,"
        "0,0,0,0,100,100,0,0,1,1,0,5,20,20,20,1\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,";
    const char *family = getenv("FURI_TEST_FAMILY");
    if (!family)
        family = "Arial";
    size_t len = strlen(prefix) + strlen(family) + strlen(text) + 8;
    char *script = malloc(len);
    if (!script)
        return NULL;
    snprintf(script, len, "%s{\\fn%s}%s\n", prefix, family, text);
    return script;
}

static char *make_karaoke_script_with_outline(const char *text, bool outline)
{
    const char *prefix_no_outline =
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: 384\n"
        "PlayResY: 216\n"
        "ScaledBorderAndShadow: yes\n"
        "\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,48,&H000000FF,&H00FF0000,&H00000000,&H00000000,"
        "0,0,0,0,100,100,0,0,1,0,0,5,20,20,20,1\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,";
    const char *prefix_outline =
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: 384\n"
        "PlayResY: 216\n"
        "ScaledBorderAndShadow: yes\n"
        "\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,48,&H000000FF,&H00FF0000,&H00000000,&H00000000,"
        "0,0,0,0,100,100,0,0,1,2,0,5,20,20,20,1\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,";
    const char *prefix = outline ? prefix_outline : prefix_no_outline;
    size_t len = strlen(prefix) + strlen(text) + 2;
    char *script = malloc(len);
    if (!script)
        return NULL;
    snprintf(script, len, "%s%s\n", prefix, text);
    return script;
}

static char *make_karaoke_script(const char *text)
{
    return make_karaoke_script_with_outline(text, false);
}

static void free_karaoke_frame(KaraokeFrame *frame)
{
    free(frame->primary);
    free(frame->secondary);
    free(frame->outline);
    *frame = (KaraokeFrame) {0};
}

static int render_karaoke_frame(const char *text, long long now,
                                bool outline, KaraokeFrame *frame)
{
    *frame = (KaraokeFrame) {0};
    frame->primary = calloc(FRAME_W * FRAME_H, sizeof(*frame->primary));
    frame->secondary = calloc(FRAME_W * FRAME_H, sizeof(*frame->secondary));
    frame->outline = calloc(FRAME_W * FRAME_H, sizeof(*frame->outline));
    if (!frame->primary || !frame->secondary || !frame->outline)
        goto fail;

    ASS_Library *lib = ass_library_init();
    ASS_Renderer *renderer = NULL;
    ASS_Track *track = NULL;
    char *script = NULL;
    int ret = 1;
    if (!lib)
        goto done;
    add_test_font(lib);
    renderer = ass_renderer_init(lib);
    if (!renderer)
        goto done;
    ass_set_storage_size(renderer, FRAME_W, FRAME_H);
    ass_set_frame_size(renderer, FRAME_W, FRAME_H);
    ass_set_fonts(renderer, NULL, "Arial",
                  ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    script = make_karaoke_script_with_outline(text, outline);
    if (!script)
        goto done;
    track = ass_read_memory(lib, script, strlen(script), NULL);
    if (!track)
        goto done;

    int change = 0;
    for (ASS_Image *img = ass_render_frame(renderer, track, now, &change);
         img; img = img->next) {
        uint32_t rgb = img->color & 0xFFFFFF00u;
        uint32_t *plane = rgb == 0xFF000000u ? frame->primary :
                          rgb == 0x0000FF00u ? frame->secondary :
                          rgb == 0x00000000u ? frame->outline : NULL;
        if (!plane)
            continue;
        int opacity = 255 - (img->color & 0xFF);
        for (int y = 0; y < img->h; y++) {
            int yy = img->dst_y + y;
            if (yy < 0 || yy >= FRAME_H)
                continue;
            for (int x = 0; x < img->w; x++) {
                int xx = img->dst_x + x;
                if (xx < 0 || xx >= FRAME_W)
                    continue;
                plane[yy * FRAME_W + xx] +=
                    img->bitmap[y * img->stride + x] * opacity;
            }
        }
    }
    ret = 0;

done:
    free(script);
    if (track)
        ass_free_track(track);
    if (renderer)
        ass_renderer_done(renderer);
    if (lib)
        ass_library_done(lib);
    if (!ret)
        return 0;
fail:
    free_karaoke_frame(frame);
    return 1;
}

static uint64_t plane_coverage(const uint32_t *plane,
                               int x0, int y0, int x1, int y1)
{
    uint64_t total = 0;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > FRAME_W ? FRAME_W : x1;
    y1 = y1 > FRAME_H ? FRAME_H : y1;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            total += plane[y * FRAME_W + x];
    return total;
}

static uint64_t positive_plane_delta(const uint32_t *before,
                                     const uint32_t *after,
                                     int x0, int y0, int x1, int y1)
{
    uint64_t total = 0;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > FRAME_W ? FRAME_W : x1;
    y1 = y1 > FRAME_H ? FRAME_H : y1;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            int off = y * FRAME_W + x;
            if (after[off] > before[off])
                total += after[off] - before[off];
        }
    return total;
}

static double plane_centroid_x(const uint32_t *plane,
                               int x0, int y0, int x1, int y1,
                               uint64_t *coverage)
{
    uint64_t total = 0;
    long double weighted = 0.0;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > FRAME_W ? FRAME_W : x1;
    y1 = y1 > FRAME_H ? FRAME_H : y1;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            uint32_t value = plane[y * FRAME_W + x];
            total += value;
            weighted += (x + 0.5) * value;
        }
    *coverage = total;
    return total ? (double) (weighted / total) : 0.0;
}

static double positive_delta_centroid_x(const uint32_t *before,
                                        const uint32_t *after,
                                        int x0, int y0, int x1, int y1,
                                        uint64_t *coverage)
{
    uint64_t total = 0;
    long double weighted = 0.0;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > FRAME_W ? FRAME_W : x1;
    y1 = y1 > FRAME_H ? FRAME_H : y1;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            int off = y * FRAME_W + x;
            uint32_t value = after[off] > before[off] ?
                after[off] - before[off] : 0;
            total += value;
            weighted += (x + 0.5) * value;
        }
    *coverage = total;
    return total ? (double) (weighted / total) : 0.0;
}

static void count_karaoke_images(ASS_Image *img, KaraokeCounts *counts)
{
    *counts = (KaraokeCounts) {0};
    for (; img; img = img->next) {
        uint32_t rgb = img->color & 0xFFFFFF00u;
        if (rgb != 0xFF000000u && rgb != 0x0000FF00u)
            continue;
        uint64_t coverage = 0;
        int opacity = 255 - (img->color & 0xFF);
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w; x++)
                coverage += img->bitmap[y * img->stride + x] * opacity;
        if (rgb == 0xFF000000u)
            counts->primary += coverage;
        else
            counts->secondary += coverage;
    }
}

static int render_karaoke_counts(const char *text, long long now,
                                  KaraokeCounts *counts)
{
    *counts = (KaraokeCounts) {0};
    ASS_Library *lib = ass_library_init();
    ASS_Renderer *renderer = NULL;
    ASS_Track *track = NULL;
    char *script = NULL;
    int ret = 1;

    if (!lib)
        goto done;
    add_test_font(lib);
    renderer = ass_renderer_init(lib);
    if (!renderer)
        goto done;
    ass_set_storage_size(renderer, FRAME_W, FRAME_H);
    ass_set_frame_size(renderer, FRAME_W, FRAME_H);
    ass_set_fonts(renderer, NULL, "Arial",
                  ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    script = make_karaoke_script(text);
    if (!script)
        goto done;
    track = ass_read_memory(lib, script, strlen(script), NULL);
    if (!track)
        goto done;

    int change = 0;
    count_karaoke_images(ass_render_frame(renderer, track, now, &change),
                          counts);
    ret = 0;

done:
    free(script);
    if (track)
        ass_free_track(track);
    if (renderer)
        ass_renderer_done(renderer);
    if (lib)
        ass_library_done(lib);
    return ret;
}

static int render_karaoke_sequence(const char *text, const long long *times,
                                    int count, KaraokeCounts *counts)
{
    ASS_Library *lib = ass_library_init();
    ASS_Renderer *renderer = NULL;
    ASS_Track *track = NULL;
    char *script = NULL;
    int ret = 1;
    if (!lib)
        goto done;
    add_test_font(lib);
    renderer = ass_renderer_init(lib);
    if (!renderer)
        goto done;
    ass_set_storage_size(renderer, FRAME_W, FRAME_H);
    ass_set_frame_size(renderer, FRAME_W, FRAME_H);
    ass_set_fonts(renderer, NULL, "Arial",
                  ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    script = make_karaoke_script(text);
    if (!script)
        goto done;
    track = ass_read_memory(lib, script, strlen(script), NULL);
    if (!track)
        goto done;

    for (int i = 0; i < count; i++) {
        int change = 0;
        ASS_Image *images = ass_render_frame(renderer, track, times[i], &change);
        count_karaoke_images(images, &counts[i]);
    }
    ret = 0;

done:
    free(script);
    if (track)
        ass_free_track(track);
    if (renderer)
        ass_renderer_done(renderer);
    if (lib)
        ass_library_done(lib);
    return ret;
}

static bool same_counts(KaraokeCounts a, KaraokeCounts b)
{
    return a.primary == b.primary && a.secondary == b.secondary;
}

static int expect_karaoke_same_at(const char *a, const char *b, long long now)
{
    KaraokeCounts ca, cb;
    int err = render_karaoke_counts(a, now, &ca);
    if (!err)
        err = render_karaoke_counts(b, now, &cb);
    bool ok = !err && same_counts(ca, cb);
    if (!ok)
        fprintf(stderr, "expected same karaoke render at %lld: `%s` vs `%s`\n",
                now, a, b);
    return ok ? 0 : 1;
}

static int expect_karaoke_steps(const char *text, const long long *times,
                                int count, bool strictly_increasing)
{
    KaraokeCounts previous = {0};
    for (int i = 0; i < count; i++) {
        KaraokeCounts current;
        if (render_karaoke_counts(text, times[i], &current))
            return 1;
        if (!current.primary && !current.secondary)
            return 1;
        if (i && (current.primary < previous.primary ||
                  current.secondary > previous.secondary ||
                  (strictly_increasing &&
                   current.primary == previous.primary))) {
            fprintf(stderr, "unexpected karaoke progression at %lld: `%s`\n",
                    times[i], text);
            return 1;
        }
        previous = current;
    }
    return 0;
}

static int render_mask_color(const char *text, uint32_t color, Mask *mask)
{
    memset(mask, 0, sizeof(*mask));
    mask->alpha = calloc(FRAME_W * FRAME_H, 1);
    if (!mask->alpha)
        return 1;

    ASS_Library *lib = ass_library_init();
    ASS_Renderer *renderer = NULL;
    ASS_Track *track = NULL;
    char *script = NULL;
    int ret = 1;

    if (!lib)
        goto done;
    add_test_font(lib);
    renderer = ass_renderer_init(lib);
    if (!renderer)
        goto done;
    ass_set_storage_size(renderer, FRAME_W, FRAME_H);
    ass_set_frame_size(renderer, FRAME_W, FRAME_H);
    ass_set_fonts(renderer, NULL, "Arial",
                  ASS_FONTPROVIDER_AUTODETECT, NULL, 1);

    script = make_script(text);
    if (!script)
        goto done;
    track = ass_read_memory(lib, script, strlen(script), NULL);
    if (!track)
        goto done;

    int change = 0;
    ASS_Image *img = ass_render_frame(renderer, track, 0, &change);
    while (img) {
        if (color != UINT32_MAX &&
                (img->color & 0xFFFFFF00u) != color) {
            img = img->next;
            continue;
        }
        int a = 255 - (int) (img->color & 0xFF);
        for (int y = 0; y < img->h; y++) {
            int yy = img->dst_y + y;
            if (yy < 0 || yy >= FRAME_H)
                continue;
            for (int x = 0; x < img->w; x++) {
                int xx = img->dst_x + x;
                if (xx < 0 || xx >= FRAME_W)
                    continue;
                int v = img->bitmap[y * img->stride + x] * a / 255;
                int off = yy * FRAME_W + xx;
                int sum = mask->alpha[off] + v;
                mask->alpha[off] = sum > 255 ? 255 : sum;
            }
        }
        img = img->next;
    }

    mask->x0 = FRAME_W;
    mask->y0 = FRAME_H;
    mask->x1 = 0;
    mask->y1 = 0;
    mask->empty = true;
    for (int y = 0; y < FRAME_H; y++) {
        for (int x = 0; x < FRAME_W; x++) {
            if (!mask->alpha[y * FRAME_W + x])
                continue;
            mask->empty = false;
            if (x < mask->x0) mask->x0 = x;
            if (y < mask->y0) mask->y0 = y;
            if (x + 1 > mask->x1) mask->x1 = x + 1;
            if (y + 1 > mask->y1) mask->y1 = y + 1;
        }
    }
    ret = 0;

done:
    free(script);
    if (track)
        ass_free_track(track);
    if (renderer)
        ass_renderer_done(renderer);
    if (lib)
        ass_library_done(lib);
    return ret;
}

static int render_mask(const char *text, Mask *mask)
{
    return render_mask_color(text, UINT32_MAX, mask);
}

static void free_mask(Mask *mask)
{
    free(mask->alpha);
    mask->alpha = NULL;
}

static bool same_mask(const Mask *a, const Mask *b)
{
    return !memcmp(a->alpha, b->alpha, FRAME_W * FRAME_H);
}

static bool mask_horizontal_bounds(const Mask *mask, int y0, int y1,
                                   int *left, int *right)
{
    *left = FRAME_W;
    *right = 0;
    y0 = y0 < 0 ? 0 : y0;
    y1 = y1 > FRAME_H ? FRAME_H : y1;
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < FRAME_W; x++) {
            if (!mask->alpha[y * FRAME_W + x])
                continue;
            *left = x < *left ? x : *left;
            *right = x + 1 > *right ? x + 1 : *right;
        }
    }
    return *right > *left;
}

/* Test cases use an upward manual offset to leave distinct ruby and base
 * bands.  Comparing band widths makes the assertions independent of the
 * event's final screen translation. */
static int furi_band_span(const Mask *mask, bool base)
{
    if (mask->empty)
        return 0;
    int y0 = base && mask->y1 - 12 > mask->y0 ?
        mask->y1 - 12 : mask->y0;
    int y1 = !base && mask->y0 + 12 < mask->y1 ?
        mask->y0 + 12 : mask->y1;
    int left, right;
    return mask_horizontal_bounds(mask, y0, y1, &left, &right) ?
        right - left : 0;
}

static bool furi_top_bounds(const Mask *mask, int *left, int *right)
{
    if (mask->empty)
        return false;
    int y1 = mask->y0 + 12 < mask->y1 ? mask->y0 + 12 : mask->y1;
    return mask_horizontal_bounds(mask, mask->y0, y1, left, right);
}

static int expect_same_base_span(const char *a, const char *b)
{
    Mask ma = {0}, mb = {0};
    int err = render_mask(a, &ma);
    if (!err)
        err = render_mask(b, &mb);
    int wa = !err ? furi_band_span(&ma, true) : 0;
    int wb = !err ? furi_band_span(&mb, true) : 0;
    bool ok = !err && wa > 0 && wb > 0 && abs(wa - wb) <= 1;
    if (!ok)
        fprintf(stderr,
                "::error title=furigana base advance::expected same base "
                "span: `%s` (%d) vs `%s` (%d)\n",
                a, wa, b, wb);
    free_mask(&ma);
    free_mask(&mb);
    return ok ? 0 : 1;
}

/* Character images retain base-run order followed by sidecar-run order. Use
 * separate masks so inward ruby in a multiline case cannot hide a displaced
 * base glyph in the union of all rendered pixels. These cases use no clipping,
 * border or shadow. Outer zero-duration karaoke also retains the run order. */
typedef struct {
    Mask runs[32];
    uint32_t colors[32];
    int image_x[32], image_width[32];
    int count;
} RubySnapshot;

static void free_ruby_snapshot(RubySnapshot *snapshot)
{
    for (int i = 0; i < snapshot->count; i++)
        free_mask(&snapshot->runs[i]);
}

static int render_ruby_snapshot(const char *text, RubySnapshot *snapshot)
{
    *snapshot = (RubySnapshot) {0};
    ASS_Library *lib = ass_library_init();
    ASS_Renderer *renderer = NULL;
    ASS_Track *track = NULL;
    char *script = NULL;
    int ret = 1;
    if (!lib)
        goto done;
    add_test_font(lib);
    renderer = ass_renderer_init(lib);
    if (!renderer)
        goto done;
    ass_set_storage_size(renderer, FRAME_W, FRAME_H);
    ass_set_frame_size(renderer, FRAME_W, FRAME_H);
    ass_set_fonts(renderer, NULL, "Arial",
                  ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    script = make_script(text);
    if (!script)
        goto done;
    track = ass_read_memory(lib, script, strlen(script), NULL);
    if (!track)
        goto done;
    int change = 0;
    for (ASS_Image *img = ass_render_frame(renderer, track, 0, &change);
         img; img = img->next) {
        if (img->type != IMAGE_TYPE_CHARACTER || !img->w || !img->h)
            continue;
        if (snapshot->count == 32)
            goto done;
        Mask *mask = &snapshot->runs[snapshot->count];
        snapshot->image_x[snapshot->count] = img->dst_x;
        snapshot->image_width[snapshot->count] = img->w;
        snapshot->colors[snapshot->count++] = img->color;
        mask->alpha = calloc(FRAME_W * FRAME_H, 1);
        if (!mask->alpha)
            goto done;
        mask->x0 = FRAME_W;
        mask->y0 = FRAME_H;
        mask->empty = true;
        for (int y = 0; y < img->h; y++) {
            int yy = img->dst_y + y;
            if (yy < 0 || yy >= FRAME_H)
                continue;
            for (int x = 0; x < img->w; x++) {
                int xx = img->dst_x + x;
                int value = img->bitmap[y * img->stride + x] *
                            (255 - (img->color & 255)) / 255;
                if (xx < 0 || xx >= FRAME_W || !value)
                    continue;
                mask->alpha[yy * FRAME_W + xx] = value;
                mask->empty = false;
                if (xx < mask->x0) mask->x0 = xx;
                if (yy < mask->y0) mask->y0 = yy;
                if (xx + 1 > mask->x1) mask->x1 = xx + 1;
                if (yy + 1 > mask->y1) mask->y1 = yy + 1;
            }
        }
    }
    ret = 0;
done:
    free(script);
    if (track) ass_free_track(track);
    if (renderer) ass_renderer_done(renderer);
    if (lib) ass_library_done(lib);
    return ret;
}

/* Three distinct contracts: unchanged base runs; deliberate base movement;
 * or a centered block that may expand symmetrically without moving its anchor.
 * These must not be conflated when internal ruby clearance is enabled. */
enum {
    RUBY_BASE_MOVED = 0,
    RUBY_BASE_STABLE = 1,
    RUBY_BASE_CENTERED = 2,
};

static int expect_ruby_geometry(const char *control, const char *ruby,
                                int bases, int annotations,
                                const int *base_index, const int *side,
                                int base_policy)
{
    RubySnapshot plain = {0}, annotated = {0};
    int err = render_ruby_snapshot(control, &plain);
    if (!err)
        err = render_ruby_snapshot(ruby, &annotated);
    bool ok = !err && plain.count == bases &&
              annotated.count == bases + annotations;
    bool changed = false;
    for (int i = 0; ok && i < bases; i++) {
        // Ruby accommodation may expand advances and change horizontal
        // alignment. Only vertical base placement is authoritative in mode 0.
        bool same = !plain.runs[i].empty && !annotated.runs[i].empty &&
                    plain.runs[i].y0 == annotated.runs[i].y0 &&
                    plain.runs[i].y1 == annotated.runs[i].y1;
        changed |= !same;
        if (base_policy == RUBY_BASE_STABLE && !same)
            ok = false;
    }
    if (base_policy == RUBY_BASE_MOVED)
        ok = ok && changed;
    if (ok && base_policy == RUBY_BASE_CENTERED) {
        // \an5 is anchored to the middle of the *base* block. Ruby may
        // spread the first/last lines apart, but their center remains fixed.
        const Mask *top0 = &plain.runs[0];
        const Mask *bottom0 = &plain.runs[bases - 1];
        const Mask *top1 = &annotated.runs[0];
        const Mask *bottom1 = &annotated.runs[bases - 1];
        int original_center2 = top0->y0 + bottom0->y1;
        int actual_center2 = top1->y0 + bottom1->y1;
        ok = !top0->empty && !bottom0->empty &&
             !top1->empty && !bottom1->empty &&
             abs(original_center2 - actual_center2) <= 2;
    }
    for (int i = 0; ok && i < annotations; i++) {
        Mask *base = &annotated.runs[base_index[i]];
        Mask *reading = &annotated.runs[bases + i];
        int gap = side[i] < 0 ? base->y0 - reading->y1 :
                               reading->y0 - base->y1;
        ok = !base->empty && !reading->empty && gap >= 1;
    }
    if (!ok) {
        fprintf(stderr, "::error title=ruby geometry::base Y stability/side/"
                "clearance failed (err=%d runs=%d/%d): `%s` vs `%s`\n",
                err, plain.count, annotated.count, control, ruby);
        fprintf(stderr, "  base policy=%d changed=%d\n", base_policy, changed);
        for (int i = 0; i < plain.count; i++) {
            Mask *m = &plain.runs[i];
            fprintf(stderr, "  control[%d]=[%d,%d)x[%d,%d)\n",
                    i, m->x0, m->x1, m->y0, m->y1);
        }
        for (int i = 0; i < annotated.count; i++) {
            Mask *m = &annotated.runs[i];
            fprintf(stderr, "  annotated[%d]=[%d,%d)x[%d,%d)\n",
                    i, m->x0, m->x1, m->y0, m->y1);
        }
    }
    free_ruby_snapshot(&plain);
    free_ruby_snapshot(&annotated);
    return ok ? 0 : 1;
}

/* Compare every image's X geometry and occupied columns, including the base
 * run's internal spacing, following text and both ruby sides. Y is allowed to
 * change only in the reserve mode. The union also checks final visible width. */
static int expect_horizontal_ruby_modes(const char *text, const char *tags,
                                        int bases, bool vertical_moves)
{
    char disabled[1024], enabled[1024];
    snprintf(disabled, sizeof(disabled), "{%s\\furichangepos0}%s", tags, text);
    snprintf(enabled, sizeof(enabled), "{%s\\furichangepos1}%s", tags, text);
    RubySnapshot a = {0}, b = {0};
    int err = render_ruby_snapshot(disabled, &a);
    if (!err)
        err = render_ruby_snapshot(enabled, &b);
    bool ok = !err && a.count == b.count && a.count > bases;
    bool moved = false;
    int left_a = FRAME_W, left_b = FRAME_W, right_a = 0, right_b = 0;
    for (int i = 0; ok && i < a.count; i++) {
        Mask *ma = &a.runs[i], *mb = &b.runs[i];
        ok = !ma->empty && !mb->empty && a.colors[i] == b.colors[i] &&
             a.image_x[i] == b.image_x[i] &&
             a.image_width[i] == b.image_width[i] &&
             abs(ma->x0 - mb->x0) <= 1 &&
             abs(ma->x1 - mb->x1) <= 1;
        if (i < bases)
            moved |= ma->y0 != mb->y0 || ma->y1 != mb->y1;
        if (ma->x0 < left_a) left_a = ma->x0;
        if (mb->x0 < left_b) left_b = mb->x0;
        if (ma->x1 > right_a) right_a = ma->x1;
        if (mb->x1 > right_b) right_b = mb->x1;
        // Different vertical subpixel positions can change antialiased
        // edge columns. Exact column masks are only comparable without a
        // vertical movement; image X/width and visible bounds remain checked.
        if (ma->y0 == mb->y0 && ma->y1 == mb->y1)
            for (int x = 0; ok && x < FRAME_W; x++) {
                bool occupied_a = false, occupied_b = false;
                for (int y = 0; y < FRAME_H; y++) {
                    occupied_a |= ma->alpha[y * FRAME_W + x] != 0;
                    occupied_b |= mb->alpha[y * FRAME_W + x] != 0;
                }
                ok = occupied_a == occupied_b;
            }
    }
    ok = ok && right_a - left_a == right_b - left_b &&
         (!vertical_moves || moved);
    if (!ok) {
        fprintf(stderr, "::error title=ruby horizontal modes::X geometry/width "
                "or vertical reservation failed (err=%d runs=%d/%d Y moved=%d): "
                "`%s` vs `%s`\n", err, a.count, b.count, moved, disabled, enabled);
        for (int i = 0; i < a.count && i < b.count; i++)
            fprintf(stderr, "  run[%d] X=%d/%d width=%d/%d Y=%d/%d\n", i,
                    a.image_x[i], b.image_x[i],
                    a.image_width[i], b.image_width[i],
                    a.runs[i].y0, b.runs[i].y0);
    }
    free_ruby_snapshot(&a);
    free_ruby_snapshot(&b);
    return ok ? 0 : 1;
}

static int expect_ruby_gap(const char *text, int bases, int annotations)
{
    RubySnapshot snapshot = {0};
    int err = render_ruby_snapshot(text, &snapshot);
    bool ok = !err && snapshot.count == bases + annotations;
    for (int i = bases + 1; ok && i < snapshot.count; i++) {
        Mask *left = &snapshot.runs[i - 1], *right = &snapshot.runs[i];
        ok = !left->empty && !right->empty && right->x0 - left->x1 >= 1;
    }
    if (!ok) {
        fprintf(stderr, "::error title=ruby minimum gap::failed: `%s`\n", text);
        for (int i = 0; i < snapshot.count; i++) {
            Mask *m = &snapshot.runs[i];
            fprintf(stderr, "  run[%d]=[%d,%d)x[%d,%d)\n",
                    i, m->x0, m->x1, m->y0, m->y1);
        }
    }
    free_ruby_snapshot(&snapshot);
    return ok ? 0 : 1;
}

static int expect_literal_color(void)
{
    Mask candidate = {0}, ordinary = {0}, uncolored = {0};
    int err = render_mask_color("<{\\c&H3535C5&}test>", 0xC5353500u,
                                &candidate);
    if (!err)
        err = render_mask_color("{\\furi0}<{\\c&H3535C5&}test>",
                                 0xC5353500u, &ordinary);
    if (!err)
        err = render_mask_color("<test>", 0xC5353500u, &uncolored);
    bool ok = !err && !candidate.empty && uncolored.empty &&
              same_mask(&candidate, &ordinary);
    if (!ok)
        fprintf(stderr, "angle-bracket ASS color override did not execute\n");
    free_mask(&candidate);
    free_mask(&ordinary);
    free_mask(&uncolored);
    return ok ? 0 : 1;
}

static int expect_reading_style(const char *tags)
{
    char normal[256], gyaku[256];
    snprintf(normal, sizeof(normal), "{\\pos(192,90)\\bord0\\shad0%s}<W|MMM>", tags);
    snprintf(gyaku, sizeof(gyaku), "{\\pos(192,90)\\bord0\\shad0%s}<W||MMM>", tags);
    RubySnapshot a = {0}, b = {0};
    int err = render_ruby_snapshot(normal, &a);
    if (!err)
        err = render_ruby_snapshot(gyaku, &b);
    bool ok = !err && a.count == 2 && b.count == 2;
    if (ok) {
        Mask *ma = &a.runs[1], *mb = &b.runs[1];
        uint64_t ca = 0, cb = 0;
        for (int i = 0; i < FRAME_W * FRAME_H; i++) {
            ca += ma->alpha[i];
            cb += mb->alpha[i];
        }
        ok = !ma->empty && !mb->empty && a.colors[1] == b.colors[1] &&
             abs((ma->x1 - ma->x0) - (mb->x1 - mb->x0)) <= 1 &&
             abs((ma->y1 - ma->y0) - (mb->y1 - mb->y0)) <= 1 &&
             ca > 0 && cb > 0 && (ca > cb ? ca - cb : cb - ca) * 20 < ca;
    }
    if (!ok)
        fprintf(stderr, "gyaku reading lost inherited style: `%s`\n", tags);
    free_ruby_snapshot(&a);
    free_ruby_snapshot(&b);
    return ok ? 0 : 1;
}

/* Two-line ruby must make space *inside* the block, preserving its original
 * top/bottom base anchor even when the next line's ruby is very tall. */
static int expect_ruby_interline_anchor(int alignment)
{
    char plain[192], annotated[256];
    snprintf(plain, sizeof(plain),
             "{\\an%d\\pos(192,%d)\\bord0\\shad0\\fs32}W\\NW",
             alignment, alignment == 8 ? 70 : 170);
    snprintf(annotated, sizeof(annotated),
             "{\\an%d\\pos(192,%d)\\bord0\\shad0\\fs32"
             "\\furis140\\furichangepos0}<W|MMMM>\\N<W|MMMM>",
             alignment, alignment == 8 ? 70 : 170);
    RubySnapshot control = {0}, ruby = {0};
    int err = render_ruby_snapshot(plain, &control);
    if (!err)
        err = render_ruby_snapshot(annotated, &ruby);
    int anchor = alignment == 8 ? 0 : 1;
    bool same_anchor = !err && control.count == 2 && ruby.count == 4 &&
        control.runs[anchor].y0 == ruby.runs[anchor].y0 &&
        control.runs[anchor].y1 == ruby.runs[anchor].y1;
    bool separated = same_anchor && !ruby.runs[0].empty &&
        !ruby.runs[3].empty &&
        ruby.runs[0].y1 + 1 <= ruby.runs[3].y0;
    bool internal_shift = separated &&
        (alignment == 8 ? ruby.runs[1].y0 > control.runs[1].y0 :
                          ruby.runs[0].y0 < control.runs[0].y0);
    if (!internal_shift)
        fprintf(stderr, "::error title=furigana interline anchor::"
                "anchor/clearance failed for an%d (err=%d)\n",
                alignment, err);
    free_ruby_snapshot(&control);
    free_ruby_snapshot(&ruby);
    return internal_shift ? 0 : 1;
}

/* A and g have different ink bounds. Their relative tops in two separate
 * ruby readings must match glyphs sharing an ordinary typographic baseline.
 * A color boundary makes each control glyph a separate IMAGE_TYPE_CHARACTER
 * run: without it, ordinary "Ag" is rasterized as one run. */
static int expect_shared_ruby_baseline(void)
{
    RubySnapshot plain = {0}, ruby = {0};
    int err = render_ruby_snapshot(
        "{\\an8\\pos(192,75)\\bord0\\shad0\\fs36}"
        "A{\\c&H0000FF&}g", &plain);
    if (!err)
        err = render_ruby_snapshot(
            "{\\an8\\pos(192,75)\\bord0\\shad0\\fs40\\furis90}"
            "<W|A><W|g>", &ruby);

    // A single contiguous base text run can contain both W glyphs; the
    // two independent reading groups still produce two distinct sidecars.
    // Count the observed output, rather than assuming a run per base glyph.
    bool have_runs = !err && plain.count == 2 && ruby.count == 3;
    int expected = 0, actual = 0;
    if (have_runs) {
        expected = plain.runs[0].y0 - plain.runs[1].y0;
        actual = ruby.runs[1].y0 - ruby.runs[2].y0;
    }
    bool ok = have_runs && !plain.runs[0].empty && !plain.runs[1].empty &&
        !ruby.runs[1].empty && !ruby.runs[2].empty &&
        abs(actual - expected) <= 1;
    if (!ok)
        fprintf(stderr, "::error title=furigana shared baseline::"
                "reading top delta=%d, control delta=%d; runs=%d/%d err=%d\n",
                actual, expected, plain.count, ruby.count, err);
    free_ruby_snapshot(&plain);
    free_ruby_snapshot(&ruby);
    return ok ? 0 : 1;
}

static int test_ruby_geometry(void)
{
    int fail = 0;
    const int upper[] = {-1}, lower[] = {1}, both[] = {-1, 1};
    const int first[] = {0, 0};
    const int two_base[] = {0, 1}, outward[] = {-1, 1}, inward[] = {1, -1};
    const int two_upper[] = {-1, -1};
    const int four_base[] = {0, 0, 1, 1}, four_side[] = {-1, 1, 1, -1};
    const int three_base[] = {0, 1, 2}, three_upper[] = {-1, -1, -1};
    fail |= expect_ruby_interline_anchor(8);
    fail |= expect_ruby_interline_anchor(2);
    fail |= expect_shared_ruby_baseline();
    // Every ASS alignment keeps the base's Y placement in mode 0. Horizontal
    // accommodation and alignment are identical between modes, even with
    // overhanging readings on both sides.
    for (int alignment = 1; alignment <= 9; alignment++) {
        char control[128], ruby[160], dual[160];
        snprintf(control, sizeof(control),
                 "{\\an%d\\pos(192,108)\\bord0\\shad0}W", alignment);
        snprintf(ruby, sizeof(ruby),
                 "{\\an%d\\pos(192,108)\\bord0\\shad0\\furichangepos0}<W|MMMM>",
                 alignment);
        snprintf(dual, sizeof(dual),
                 "{\\an%d\\pos(192,108)\\bord0\\shad0}<W|MMMM|MMMM>", alignment);
        fail |= expect_ruby_geometry(control, ruby, 1, 1, first, upper, true);
        fail |= expect_ruby_geometry(control, dual, 1, 2, first, both, true);
        char tags[128];
        snprintf(tags, sizeof(tags),
                 "\\an%d\\pos(192,108)\\bord0\\shad0\\fs32\\q2", alignment);
        fail |= expect_horizontal_ruby_modes("<W|MMMM><W|MMMM>", tags, 1, false);
        fail |= expect_horizontal_ruby_modes("<W|MMMM|MMM><W|MMMM|MMM>", tags, 1,
                                             alignment <= 3 || alignment >= 7);
    }
    fail |= expect_ruby_geometry(
        "{\\pos(192,108)\\bord0\\shad0}W",
        "{\\pos(192,108)\\bord0\\shad0}<W||M>", 1, 1, first, lower, true);
    fail |= expect_ruby_geometry(
        "{\\an7\\pos(100,40)\\bord0\\shad0}W",
        "{\\an7\\pos(100,40)\\bord0\\shad0\\furipos(0,-80)}<W|M|M>",
        1, 2, first, (int[]) {1, -1}, true);
    for (int size = 30; size <= 80; size += 25) {
        char plain[128], ruby[160];
        snprintf(plain, sizeof(plain), "{\\pos(192,108)\\bord0\\shad0}W");
        snprintf(ruby, sizeof(ruby),
                 "{\\pos(192,108)\\bord0\\shad0\\furis%d\\furisx80}<W|MMM|MMM>", size);
        fail |= expect_ruby_geometry(plain, ruby, 1, 2, first, both, true);
    }
    const char *two_plain = "{\\pos(192,108)\\bord0\\shad0\\fs32}W\\NW";
    fail |= expect_ruby_geometry(two_plain,
        "{\\pos(192,108)\\bord0\\shad0\\fs32\\furiplaceauto0}<W|M>\\N<W|M>",
        2, 2, two_base, two_upper, RUBY_BASE_CENTERED);
    fail |= expect_ruby_geometry(two_plain,
        "{\\pos(192,108)\\bord0\\shad0\\fs32\\furiplaceauto1}<W|M>\\N<W|M>",
        2, 2, two_base, outward, RUBY_BASE_CENTERED);
    fail |= expect_ruby_geometry(two_plain,
        "{\\pos(192,108)\\bord0\\shad0\\fs32\\furiplaceauto1}<W||M>\\N<W||M>",
        2, 2, two_base, inward, RUBY_BASE_CENTERED);
    fail |= expect_ruby_geometry(two_plain,
        "{\\pos(192,108)\\bord0\\shad0\\fs32\\furiplaceauto1}<W| |M>\\N<W| |M>",
        2, 2, two_base, inward, RUBY_BASE_CENTERED);
    fail |= expect_ruby_geometry(two_plain,
        "{\\pos(192,108)\\bord0\\shad0\\fs32\\furiplaceauto1}<W|M|M>\\N<W|M|M>",
        2, 4, four_base, four_side, RUBY_BASE_CENTERED);
    fail |= expect_ruby_geometry(two_plain,
        "{\\pos(192,108)\\bord0\\shad0\\fs32\\furiplaceauto1\\furichangepos1}<W|M|M>\\N<W|M|M>",
        2, 4, four_base, four_side, false);
    fail |= expect_ruby_geometry(
        "{\\pos(192,108)\\bord0\\shad0\\fs32}W\\NW\\NW",
        "{\\pos(192,108)\\bord0\\shad0\\fs32\\furiplaceauto1}<W|M>\\N<W|M>\\N<W|M>",
        3, 3, three_base, three_upper, RUBY_BASE_CENTERED);
    fail |= expect_same("<W|M>", "{\\furiplaceauto1}<W|M>");
    fail |= expect_same("<W|M>\\N<W|M>\\N<W|M>",
                        "{\\furiplaceauto1}<W|M>\\N<W|M>\\N<W|M>");
    fail |= expect_same("<W|M>\\N<W|M>",
                        "{\\furiplaceauto0}<W|M>\\N<W|M>");
    fail |= expect_same("{\\furiplaceauto1}<W|M>\\N<W|M>",
                        "{\\furiplaceauto1\\furiap1}<W|M>\\N<W|M>");
    fail |= expect_same("{\\r}<W|M>\\N<W|M>",
                        "{\\furiplaceauto1\\r}<W|M>\\N<W|M>");
    fail |= expect_same("{\\r}<W|M>", "{\\furichangepos1\\r}<W|M>");
    // Wrapping, rather than a literal hard break, must also select two-line
    // auto placement. The long first word supplies exactly one soft break.
    fail |= expect_ruby_geometry(
        "{\\pos(192,108)\\bord0\\shad0\\fs32\\q1}WWWWWWWWWWWWWWWWWWWWW W",
        "{\\pos(192,108)\\bord0\\shad0\\fs32\\q1\\furiplaceauto1}WWWWWWWWWWWWWWWWWWWWW <W|M>",
        2, 1, (int[]) {1}, lower, true);
    // Horizontal accommodation may expand the base runs while their vertical
    // placement remains stable, even with three wide readings.
    fail |= expect_ruby_geometry(
        "{\\pos(192,108)\\bord0\\shad0}WWW",
        "{\\pos(192,108)\\bord0\\shad0}<W|MMMM><W|MMMM><W|MMMM>",
        1, 3, (int[]) {0, 0, 0}, three_upper, true);
    fail |= expect_ruby_gap(
        "{\\pos(192,108)\\bord0\\shad0}<W|M><W|M>", 1, 2);
    fail |= expect_ruby_gap(
        "{\\pos(192,108)\\bord0\\shad0}<W|MMMM><W|MMMM><W|MMMM>", 1, 3);
    fail |= expect_same(
        "{\\pos(192,108)\\bord0\\shad0}<W|MMMM><W|MMMM>",
        "{\\pos(192,108)\\bord0\\shad0\\furichangepos0}<W|MMMM><W|MMMM>");
    fail |= expect_ruby_gap(
        "{\\pos(192,80)\\bord0\\shad0\\furis80}<W||MMMM><W||MMMM>", 1, 2);
    fail |= expect_ruby_gap(
        "{\\pos(192,108)\\bord0\\shad0\\furis30}<W|MMMM>"
        "{\\furis80}<W|MMMM>{\\furis50}<W|MM>", 1, 3);
    // Unequal overhang can reverse the initial annotation left edges. The
    // collision solver must retain visual base order in both directions.
    fail |= expect_ruby_gap(
        "{\\pos(192,108)\\bord0\\shad0}<W|M><W|MMMMMM>", 1, 2);
    fail |= expect_ruby_gap(
        "{\\pos(192,108)\\bord0\\shad0}<W|MMMMMM><W|M>", 1, 2);
    fail |= expect_horizontal_ruby_modes(
        "<W|MMMM>{\\furis80}<W|MMMM>{\\furis30}<W|MM>",
        "\\an8\\pos(192,70)\\bord0\\shad0\\q2", 1, true);
    fail |= expect_horizontal_ruby_modes("<W||MMMM><W||MMMM>",
        "\\an2\\pos(192,150)\\bord0\\shad0\\q2", 1, true);
    fail |= expect_horizontal_ruby_modes(
        "<W|MMMM|MMM><W|MMMM|MMM>\\N<W|MMMM|MMM><W|MMMM|MMM>",
        "\\an8\\pos(192,70)\\bord0\\shad0\\fs32\\furiplaceauto1", 2, true);
    // An odd sidecar count forces reallocation while appending a gyaku run.
    fail |= expect_ruby_geometry(
        "{\\pos(192,108)\\bord0\\shad0}WWWWW",
        "{\\pos(192,108)\\bord0\\shad0}<W|M><W|M|M><W|M|M><W|M|M><W|M|M>",
        1, 9, (int[]) {0, 0, 0, 0, 0, 0, 0, 0, 0},
        (int[]) {-1, -1, 1, -1, 1, -1, 1, -1, 1}, true);
    fail |= expect_ruby_geometry(
        "{\\an8\\pos(192,70)\\bord0\\shad0}W",
        "{\\an8\\pos(192,70)\\bord0\\shad0\\furichangepos1}<W|M>",
        1, 1, first, upper, false);
    fail |= expect_ruby_geometry(
        "{\\an8\\pos(192,70)\\bord0\\shad0}W",
        "{\\an8\\pos(192,70)\\bord0\\shad0\\furichangepos1}<W|M|M>",
        1, 2, first, both, false);
    fail |= expect_reading_style("\\furis30");
    fail |= expect_reading_style("\\furis80\\furisx65\\furisy70\\furifsp2");
    fail |= expect_reading_style("\\b1\\i1\\c&H3535C5&\\alpha&H40&");
    fail |= expect_reading_style("\\fscx120\\fscy80\\t(0,1000,\\furis80)");
    fail |= expect_different("{\\bord0}<W||M>", "{\\bord4}<W||M>");
    fail |= expect_different("{\\shad0}<W||M>", "{\\shad4}<W||M>");
    // The original rectangle fixtures also cover these codepoints, so the
    // explicit UTF-8 probes do not depend on installed Japanese fonts.
    if (getenv("FURI_TEST_FAMILY")) {
        // Exact reported outer-karaoke regression:
        // {\k0}<認|みと>{\k0}めていた{\k0}<臆|おく>{\k0}<病|びょう>{\k0}な{\k0}<過|か>{\k0}<去|こ>
        const char *reported =
            "{\\k0}<\xE8\xAA\x8D|\xE3\x81\xBF\xE3\x81\xA8>"
            "{\\k0}\xE3\x82\x81\xE3\x81\xA6\xE3\x81\x84\xE3\x81\x9F"
            "{\\k0}<\xE8\x87\x86|\xE3\x81\x8A\xE3\x81\x8F>"
            "{\\k0}<\xE7\x97\x85|\xE3\x81\xB3\xE3\x82\x87\xE3\x81\x86>"
            "{\\k0}\xE3\x81\xAA"
            "{\\k0}<\xE9\x81\x8E|\xE3\x81\x8B>"
            "{\\k0}<\xE5\x8E\xBB|\xE3\x81\x93>";
        fail |= expect_horizontal_ruby_modes(reported,
            "\\an8\\pos(192,70)\\bord0\\shad0\\fs32\\q2", 7, true);
        const char *base = "\xE6\xBC\xA2\xE5\xAD\x97";
        const char *reading = "\xE3\x81\x8B\xE3\x82\x93\xE3\x81\x98";
        char control[160], ruby[256];
        snprintf(control, sizeof(control),
                 "{\\pos(192,108)\\bord0\\shad0}%s", base);
        snprintf(ruby, sizeof(ruby),
                 "{\\pos(192,108)\\bord0\\shad0\\furichangepos0}<%s|%s>", base, reading);
        fail |= expect_ruby_geometry(control, ruby, 1, 1, first, upper, true);
        snprintf(ruby, sizeof(ruby),
                 "{\\pos(192,108)\\bord0\\shad0\\furichangepos0}<%s|%s|M>", base, reading);
        fail |= expect_ruby_geometry(control, ruby, 1, 2, first, both, true);
        snprintf(control, sizeof(control),
                 "{\\pos(192,108)\\bord0\\shad0\\fs32}%s\\N%s", base, base);
        snprintf(ruby, sizeof(ruby),
                 "{\\pos(192,108)\\bord0\\shad0\\fs32\\furiplaceauto1}<%s|%s|M>\\N<%s|%s|M>",
                 base, reading, base, reading);
        fail |= expect_ruby_geometry(control, ruby, 2, 4, four_base, four_side, false);
    }
    return fail;
}

static int expect_overlap_spacing_bounded(const char *short_furi,
                                          const char *long_furi,
                                          const char *single_long_furi)
{
    Mask short_mask = {0}, long_mask = {0}, single_mask = {0};
    int err = render_mask(short_furi, &short_mask);
    if (!err)
        err = render_mask(long_furi, &long_mask);
    if (!err)
        err = render_mask(single_long_furi, &single_mask);
    int short_base = !err ? furi_band_span(&short_mask, true) : 0;
    int long_base = !err ? furi_band_span(&long_mask, true) : 0;
    int single_furi = !err ? furi_band_span(&single_mask, false) : 0;
    int added = long_base - short_base;
    bool ok = !err && short_base > 0 && single_furi > 0 &&
        added > 0 && added < single_furi;
    if (!ok)
        fprintf(stderr,
                "::error title=furigana collision spacing::expected bounded "
                "spacing: short=%d long=%d added=%d single-ruby=%d\n",
                short_base, long_base, added, single_furi);
    free_mask(&short_mask);
    free_mask(&long_mask);
    free_mask(&single_mask);
    return ok ? 0 : 1;
}

static int expect_colored_furi_separate(const char *text)
{
    static const uint32_t colors[] = {
        0xFF000000u, 0x00FF0000u, 0x0000FF00u,
    };
    Mask masks[3] = {{0}};
    int left[3] = {0}, right[3] = {0};
    int err = 0;
    bool ok = true;
    for (int i = 0; i < 3; i++) {
        if (!err)
            err = render_mask_color(text, colors[i], &masks[i]);
        ok = ok && !err && furi_top_bounds(&masks[i], &left[i], &right[i]);
    }
    ok = ok && right[0] + 1 <= left[1] && right[1] + 1 <= left[2];
    if (!ok)
        fprintf(stderr,
                "::error title=furigana collision chain::expected separated "
                "ruby: err=%d "
                "red=[%d,%d) green=[%d,%d) blue=[%d,%d): `%s`\n",
                err, left[0], right[0], left[1], right[1],
                left[2], right[2], text);
    for (int i = 0; i < 3; i++)
        free_mask(&masks[i]);
    return ok ? 0 : 1;
}

static int expect_same(const char *a, const char *b)
{
    Mask ma = {0}, mb = {0};
    int err = render_mask(a, &ma);
    if (!err)
        err = render_mask(b, &mb);
    if (err) {
        free_mask(&ma);
        free_mask(&mb);
        return 1;
    }
    bool ok = same_mask(&ma, &mb);
    free_mask(&ma);
    free_mask(&mb);
    if (!ok)
        fprintf(stderr, "expected same render: `%s` vs `%s`\n", a, b);
    return ok ? 0 : 1;
}

static int expect_different(const char *a, const char *b)
{
    Mask ma = {0}, mb = {0};
    int err = render_mask(a, &ma);
    if (!err)
        err = render_mask(b, &mb);
    if (err) {
        free_mask(&ma);
        free_mask(&mb);
        return 1;
    }
    bool ok = !same_mask(&ma, &mb);
    free_mask(&ma);
    free_mask(&mb);
    if (!ok)
        fprintf(stderr, "expected different render: `%s` vs `%s`\n", a, b);
    return ok ? 0 : 1;
}

static int expect_y_order(const char *up, const char *down)
{
    Mask mu = {0}, md = {0};
    int err = render_mask(up, &mu);
    if (!err)
        err = render_mask(down, &md);
    if (err) {
        free_mask(&mu);
        free_mask(&md);
        return 1;
    }
    bool ok = !mu.empty && !md.empty && mu.y0 < md.y0;
    if (!ok)
        fprintf(stderr, "expected `%s` above `%s` (%d >= %d)\n",
                up, down, mu.y0, md.y0);
    free_mask(&mu);
    free_mask(&md);
    return ok ? 0 : 1;
}

static int expect_relative_furi_directions(void)
{
    const char *operands[] = {"", "\\furipos(~+0,~+8)", "\\furipos(~+0,~-8)"};
    Mask masks[3] = {{0}};
    int err = 0;
    for (int i = 0; i < 3 && !err; i++) {
        char text[256];
        // Anchor the base at the bottom and keep ruby above it even after
        // moving down 8. The top edge then measures ruby; the bottom edge
        // checks that the base stayed fixed. Only karaoke overrides are
        // supported inside a reading, so do not try to color ruby separately.
        snprintf(text, sizeof(text), "{\\an2\\pos(192,170)\\bord0\\shad0"
                 "\\furipos(0,20)%s}<A|B>", operands[i]);
        err = render_mask(text, &masks[i]);
    }
    bool ok = !err && !masks[0].empty && !masks[1].empty && !masks[2].empty &&
        masks[1].y0 == masks[0].y0 - 8 && masks[2].y0 == masks[0].y0 + 8 &&
        masks[1].y1 == masks[0].y1 && masks[2].y1 == masks[0].y1;
    if (!ok) fprintf(stderr, "relative ruby Y direction failed: %d, %d, %d\n",
                     masks[0].y0, masks[1].y0, masks[2].y0);
    for (int i = 0; i < 3; i++)
        free_mask(&masks[i]);
    return ok ? 0 : 1;
}

static int expect_bottom_anchor_with_taller_block(const char *with_furi,
                                                  const char *without_furi)
{
    Mask mf = {0}, mn = {0};
    int err = render_mask(with_furi, &mf);
    if (!err)
        err = render_mask(without_furi, &mn);
    if (err) {
        free_mask(&mf);
        free_mask(&mn);
        return 1;
    }
    bool ok = !mf.empty && !mn.empty && abs(mf.y1 - mn.y1) <= 1 &&
        mf.y0 < mn.y0;
    if (!ok)
        fprintf(stderr, "expected bottom anchor and taller block: `%s` vs `%s`\n",
                with_furi, without_furi);
    free_mask(&mf);
    free_mask(&mn);
    return ok ? 0 : 1;
}

static int expect_top_anchor_with_taller_block(const char *taller,
                                               const char *shorter)
{
    Mask mf = {0}, mn = {0};
    int err = render_mask(taller, &mf);
    if (!err)
        err = render_mask(shorter, &mn);
    if (err) {
        free_mask(&mf);
        free_mask(&mn);
        return 1;
    }
    bool ok = !mf.empty && !mn.empty && abs(mf.y0 - mn.y0) <= 1 &&
        mf.y1 > mn.y1;
    if (!ok)
        fprintf(stderr, "expected top anchor and taller block: `%s` vs `%s`\n",
                taller, shorter);
    free_mask(&mf);
    free_mask(&mn);
    return ok ? 0 : 1;
}

static int expect_center_anchor_with_taller_block(const char *with_furi,
                                                  const char *without_furi)
{
    Mask mf = {0}, mn = {0};
    int err = render_mask(with_furi, &mf);
    if (!err)
        err = render_mask(without_furi, &mn);
    if (err) {
        free_mask(&mf);
        free_mask(&mn);
        return 1;
    }
    int cf = mf.y0 + mf.y1;
    int cn = mn.y0 + mn.y1;
    bool ok = !mf.empty && !mn.empty && abs(cf - cn) <= 2 &&
        (mf.y1 - mf.y0) > (mn.y1 - mn.y0);
    if (!ok)
        fprintf(stderr, "expected center anchor and taller block: `%s` vs `%s`\n",
                with_furi, without_furi);
    free_mask(&mf);
    free_mask(&mn);
    return ok ? 0 : 1;
}

static int expect_same_height(const char *a, const char *b)
{
    Mask ma = {0}, mb = {0};
    int err = render_mask(a, &ma);
    if (!err)
        err = render_mask(b, &mb);
    if (err) {
        free_mask(&ma);
        free_mask(&mb);
        return 1;
    }
    bool ok = !ma.empty && !mb.empty &&
        abs((ma.y1 - ma.y0) - (mb.y1 - mb.y0)) <= 1;
    if (!ok)
        fprintf(stderr, "expected same visual height: `%s` vs `%s`\n", a, b);
    free_mask(&ma);
    free_mask(&mb);
    return ok ? 0 : 1;
}

static int expect_partition_tops_aligned(const char *text, int parts)
{
    Mask mask = {0};
    int err = render_mask(text, &mask);
    if (err) {
        free_mask(&mask);
        return 1;
    }

    bool ok = !mask.empty && parts > 1;
    int expected_top = -1;
    for (int part = 0; ok && part < parts; part++) {
        int x0 = mask.x0 + (mask.x1 - mask.x0) * part / parts;
        int x1 = mask.x0 + (mask.x1 - mask.x0) * (part + 1) / parts;
        int top = FRAME_H;
        for (int y = 0; y < FRAME_H; y++) {
            for (int x = x0; x < x1; x++) {
                if (mask.alpha[y * FRAME_W + x]) {
                    top = y;
                    break;
                }
            }
            if (top != FRAME_H)
                break;
        }
        if (top == FRAME_H)
            ok = false;
        else if (expected_top < 0)
            expected_top = top;
        else if (abs(top - expected_top) > 1)
            ok = false;
    }

    if (!ok)
        fprintf(stderr, "expected aligned furigana tops: `%s`\n", text);
    free_mask(&mask);
    return ok ? 0 : 1;
}

static int expect_ko_outline_activation(const char *text,
                                        long long before,
                                        long long after)
{
    KaraokeFrame first = {0}, second = {0};
    int err = render_karaoke_frame(text, before, true, &first);
    if (!err)
        err = render_karaoke_frame(text, after, true, &second);
    uint64_t before_outline = err ? 0 :
        plane_coverage(first.outline, 0, 0, FRAME_W, FRAME_H);
    uint64_t after_outline = err ? 0 :
        plane_coverage(second.outline, 0, 0, FRAME_W, FRAME_H);
    bool ok = !err && !before_outline && after_outline;
    if (!ok)
        fprintf(stderr, "expected delayed ko outline activation: `%s`\n", text);
    free_karaoke_frame(&first);
    free_karaoke_frame(&second);
    return ok ? 0 : 1;
}

static int expect_furi_ko_base_outline_activation(const char *text,
                                                   const char *base,
                                                   long long before,
                                                   long long after)
{
    KaraokeFrame first = {0}, second = {0};
    Mask base_mask = {0};
    int err = render_karaoke_frame(text, before, true, &first);
    if (!err)
        err = render_karaoke_frame(text, after, true, &second);
    if (!err)
        err = render_mask(base, &base_mask);
    uint64_t before_outline = 0, after_outline = 0;
    if (!err && !base_mask.empty) {
        before_outline = plane_coverage(
            first.outline, base_mask.x0, base_mask.y0,
            base_mask.x1, base_mask.y1);
        after_outline = plane_coverage(
            second.outline, base_mask.x0, base_mask.y0,
            base_mask.x1, base_mask.y1);
    }
    bool ok = !err && !before_outline && after_outline;
    if (!ok)
        fprintf(stderr, "expected delayed ko outline on furigana base: `%s`\n",
                text);
    free_karaoke_frame(&first);
    free_karaoke_frame(&second);
    free_mask(&base_mask);
    return ok ? 0 : 1;
}

static int expect_cross_segment_spatial_ownership(void)
{
    const char *text =
        "{\\an1\\pos(40,180)}<\xE6\x8E\xB4|{\\k40}\xE3\x81\xA4"
        "{\\k60}\xE3\x81\x8B>\xE3\x82\x93{\\k70}\xE3\x81\xA0";
    const char *base = "{\\an1\\pos(40,180)}\xE6\x8E\xB4";
    KaraokeFrame before = {0}, after = {0};
    Mask base_mask = {0};
    int err = render_karaoke_frame(text, 399, false, &before);
    if (!err)
        err = render_karaoke_frame(text, 400, false, &after);
    if (!err)
        err = render_mask(base, &base_mask);

    uint64_t base_change = 0, following_change = 0;
    if (!err && !base_mask.empty) {
        int lower_y = (base_mask.y0 + base_mask.y1) / 2;
        base_change = positive_plane_delta(
            before.primary, after.primary, base_mask.x0, lower_y,
            base_mask.x1, base_mask.y1);
        following_change = positive_plane_delta(
            before.primary, after.primary, base_mask.x1, lower_y,
            FRAME_W, base_mask.y1);
    }
    bool ok = !err && base_change && following_change;
    if (!ok)
        fprintf(stderr,
                "cross-boundary karaoke did not activate both base region and following glyph\n");
    free_karaoke_frame(&before);
    free_karaoke_frame(&after);
    free_mask(&base_mask);
    return ok ? 0 : 1;
}

static int expect_unequal_width_base_regions(void)
{
    const char *text =
        "{\\an1\\pos(40,180)}<WWWW|{\\k30}W{\\k30}i>";
    const char *base = "{\\an1\\pos(40,180)}WWWW";
    KaraokeFrame frame = {0};
    Mask base_mask = {0};
    int err = render_karaoke_frame(text, 0, false, &frame);
    if (!err)
        err = render_mask(base, &base_mask);

    int last_primary = -1, first_secondary = FRAME_W;
    if (!err && !base_mask.empty) {
        int y0 = (base_mask.y0 + base_mask.y1) / 2;
        for (int x = base_mask.x0; x < base_mask.x1; x++) {
            if (plane_coverage(frame.primary, x, y0, x + 1, base_mask.y1))
                last_primary = x;
            if (first_secondary == FRAME_W &&
                    plane_coverage(frame.secondary, x, y0,
                                   x + 1, base_mask.y1))
                first_secondary = x;
        }
    }
    double split = last_primary >= 0 && first_secondary < FRAME_W ?
        (last_primary + first_secondary + 1) / 2.0 : -1.0;
    double ratio = split >= 0 ?
        (split - base_mask.x0) / (base_mask.x1 - base_mask.x0) : 0.0;
    bool ok = !err && ratio > 0.65 && ratio < 0.95;
    if (!ok)
        fprintf(stderr,
                "::error title=unequal-width base regions::unequal shaped reading widths did not own unequal base regions: err=%d empty=%d bounds=[%d,%d)x[%d,%d) last_primary=%d first_secondary=%d split=%.3f ratio=%.6f\n",
                err, base_mask.empty, base_mask.x0, base_mask.x1,
                base_mask.y0, base_mask.y1, last_primary,
                first_secondary, split, ratio);
    free_karaoke_frame(&frame);
    free_mask(&base_mask);
    return ok ? 0 : 1;
}

static int expect_bidi_visual_region_order(void)
{
    const char *text =
        "{\\an1\\pos(40,180)}<WW|{\\k30}\xD7\x90{\\k30}\xD7\x91>";
    const char *base = "{\\an1\\pos(40,180)}WW";
    KaraokeFrame frame = {0};
    Mask base_mask = {0};
    int err = render_karaoke_frame(text, 0, false, &frame);
    if (!err)
        err = render_mask(base, &base_mask);

    uint64_t p_left = 0, p_right = 0, s_left = 0, s_right = 0;
    uint64_t rp_left = 0, rp_right = 0, rs_left = 0, rs_right = 0;
    if (!err && !base_mask.empty) {
        int mid_x = (base_mask.x0 + base_mask.x1) / 2;
        int y0 = (base_mask.y0 + base_mask.y1) / 2;
        p_left = plane_coverage(frame.primary, base_mask.x0, y0,
                                mid_x, base_mask.y1);
        p_right = plane_coverage(frame.primary, mid_x, y0,
                                 base_mask.x1, base_mask.y1);
        s_left = plane_coverage(frame.secondary, base_mask.x0, y0,
                                mid_x, base_mask.y1);
        s_right = plane_coverage(frame.secondary, mid_x, y0,
                                 base_mask.x1, base_mask.y1);
        rp_left = plane_coverage(frame.primary, 0, 0,
                                 mid_x, base_mask.y0);
        rp_right = plane_coverage(frame.primary, mid_x, 0,
                                  FRAME_W, base_mask.y0);
        rs_left = plane_coverage(frame.secondary, 0, 0,
                                 mid_x, base_mask.y0);
        rs_right = plane_coverage(frame.secondary, mid_x, 0,
                                  FRAME_W, base_mask.y0);
    }
    bool ok = !err && p_right > 2 * p_left && s_left > 2 * s_right &&
        rp_right > 2 * rp_left && rs_left > 2 * rs_right;
    if (!ok)
        fprintf(stderr,
                "::error title=bidi visual-region order::bidi-reordered reading did not map to visual base regions: err=%d empty=%d bounds=[%d,%d)x[%d,%d) base primary(left=%llu right=%llu) secondary(left=%llu right=%llu) reading primary(left=%llu right=%llu) secondary(left=%llu right=%llu)\n",
                err, base_mask.empty, base_mask.x0, base_mask.x1,
                base_mask.y0, base_mask.y1,
                (unsigned long long) p_left, (unsigned long long) p_right,
                (unsigned long long) s_left, (unsigned long long) s_right,
                (unsigned long long) rp_left, (unsigned long long) rp_right,
                (unsigned long long) rs_left, (unsigned long long) rs_right);
    free_karaoke_frame(&frame);
    free_mask(&base_mask);
    return ok ? 0 : 1;
}

static int expect_three_segment_bidi_order(void)
{
    const char *text =
        "{\\an1\\pos(40,180)}<WWW|{\\k30}\xD7\x90{\\k30}\xD7\x91"
        "{\\k30}\xD7\x92>";
    const char *base = "{\\an1\\pos(40,180)}WWW";
    KaraokeFrame frame[3] = {0};
    Mask base_mask = {0};
    const long long times[] = {0, 300, 600};
    int err = 0;
    for (int i = 0; i < 3 && !err; i++)
        err = render_karaoke_frame(text, times[i], false, &frame[i]);
    if (!err)
        err = render_mask(base, &base_mask);

    uint64_t rc[3] = {0}, bc[3] = {0};
    double rx[3] = {0}, bx[3] = {0};
    if (!err && !base_mask.empty) {
        int base_y = (base_mask.y0 + base_mask.y1) / 2;
        rx[0] = plane_centroid_x(frame[0].primary, 0, 0,
                                 FRAME_W, base_mask.y0, &rc[0]);
        bx[0] = plane_centroid_x(frame[0].primary, base_mask.x0, base_y,
                                 base_mask.x1, base_mask.y1, &bc[0]);
        for (int i = 1; i < 3; i++) {
            rx[i] = positive_delta_centroid_x(
                frame[i - 1].primary, frame[i].primary,
                0, 0, FRAME_W, base_mask.y0, &rc[i]);
            bx[i] = positive_delta_centroid_x(
                frame[i - 1].primary, frame[i].primary,
                base_mask.x0, base_y, base_mask.x1, base_mask.y1, &bc[i]);
        }
    }
    bool ok = !err && rc[0] && rc[1] && rc[2] &&
        bc[0] && bc[1] && bc[2] &&
        rx[0] > rx[1] && rx[1] > rx[2] &&
        bx[0] > bx[1] && bx[1] > bx[2];
    if (!ok)
        fprintf(stderr,
                "::error title=three-segment RTL karaoke::three-segment RTL karaoke did not activate right to left: err=%d empty=%d reading coverage=[%llu,%llu,%llu] centroid=[%.3f,%.3f,%.3f] base coverage=[%llu,%llu,%llu] centroid=[%.3f,%.3f,%.3f]\n",
                err, base_mask.empty,
                (unsigned long long) rc[0], (unsigned long long) rc[1],
                (unsigned long long) rc[2], rx[0], rx[1], rx[2],
                (unsigned long long) bc[0], (unsigned long long) bc[1],
                (unsigned long long) bc[2], bx[0], bx[1], bx[2]);
    for (int i = 0; i < 3; i++)
        free_karaoke_frame(&frame[i]);
    free_mask(&base_mask);
    return ok ? 0 : 1;
}

static int expect_bidi_kf_direction(void)
{
    // Use the bundled monospaced test font so each quarter-frontier crosses
    // visible ink. RLO/PDF exercise the same RTL shaping and karaoke mapping
    // without depending on a platform's particular Hebrew glyph sidebearings.
    const char *text =
        "{\\an1\\pos(40,180)\\fnPixel Operator Mono}"
        "<MMMMMM|\xE2\x80\xAE{\\kf100}WWW{\\kf100}WWW\xE2\x80\xAC>";
    const char *base =
        "{\\an1\\pos(40,180)\\fnPixel Operator Mono}MMMMMM";
    const long long times[] = {250, 500, 750, 1000, 1250, 1500, 1750};
    KaraokeFrame frame[7] = {0};
    Mask base_mask = {0};
    int err = 0;
    for (int i = 0; i < 7 && !err; i++)
        err = render_karaoke_frame(text, times[i], false, &frame[i]);
    if (!err)
        err = render_mask(base, &base_mask);

    uint64_t rc[6] = {0}, bc[6] = {0};
    double rx[6] = {0}, bx[6] = {0};
    if (!err && !base_mask.empty) {
        int base_y = (base_mask.y0 + base_mask.y1) / 2;
        for (int i = 0; i < 6; i++) {
            int frame_index = i < 3 ? i : i + 1;
            rx[i] = plane_centroid_x(frame[frame_index].primary,
                                     0, 0, FRAME_W, base_mask.y0, &rc[i]);
            bx[i] = plane_centroid_x(frame[frame_index].primary,
                                     base_mask.x0, base_y,
                                     base_mask.x1, base_mask.y1, &bc[i]);
        }
    }
    bool covered = !err;
    for (int i = 0; i < 6; i++)
        covered &= rc[i] && bc[i];
    // A frontier can cross blank columns between glyph strokes at a sampled
    // time. Cumulative active-paint ownership may therefore plateau, but it
    // must never move right and must move strictly left over each segment.
    bool ok = covered &&
        rx[0] >= rx[1] && rx[1] >= rx[2] && rx[0] > rx[2] &&
        bx[0] >= bx[1] && bx[1] >= bx[2] && bx[0] > bx[2] &&
        rx[3] >= rx[4] && rx[4] >= rx[5] && rx[3] > rx[5] &&
        bx[3] >= bx[4] && bx[4] >= bx[5] && bx[3] > bx[5];
    if (!ok)
        fprintf(stderr,
                "::error title=RTL KF sweep::RTL kf cumulative active paint did not progress right to left at 25/50/75 percent: err=%d empty=%d reading coverage=[%llu,%llu,%llu;%llu,%llu,%llu] centroid=[%.3f,%.3f,%.3f;%.3f,%.3f,%.3f] base coverage=[%llu,%llu,%llu;%llu,%llu,%llu] centroid=[%.3f,%.3f,%.3f;%.3f,%.3f,%.3f]\n",
                err, base_mask.empty,
                (unsigned long long) rc[0], (unsigned long long) rc[1],
                (unsigned long long) rc[2], (unsigned long long) rc[3],
                (unsigned long long) rc[4], (unsigned long long) rc[5],
                rx[0], rx[1], rx[2], rx[3], rx[4], rx[5],
                (unsigned long long) bc[0], (unsigned long long) bc[1],
                (unsigned long long) bc[2], (unsigned long long) bc[3],
                (unsigned long long) bc[4], (unsigned long long) bc[5],
                bx[0], bx[1], bx[2], bx[3], bx[4], bx[5]);
    for (int i = 0; i < 7; i++)
        free_karaoke_frame(&frame[i]);
    free_mask(&base_mask);
    return ok ? 0 : 1;
}

static int expect_unequal_width_bidi_regions(void)
{
    const char *text =
        "{\\an1\\pos(40,180)}<WWWW|{\\k30}\xD7\x90\xD7\x90\xD7\x90"
        "{\\k30}\xD7\x91>";
    const char *base = "{\\an1\\pos(40,180)}WWWW";
    KaraokeFrame frame = {0};
    Mask base_mask = {0};
    int err = render_karaoke_frame(text, 0, false, &frame);
    if (!err)
        err = render_mask(base, &base_mask);

    int first_primary = FRAME_W, last_secondary = -1;
    uint64_t rpc = 0, rsc = 0, bpc = 0, bsc = 0;
    double rpx = 0, rsx = 0, bpx = 0, bsx = 0;
    if (!err && !base_mask.empty) {
        int base_y = (base_mask.y0 + base_mask.y1) / 2;
        for (int x = base_mask.x0; x < base_mask.x1; x++) {
            if (first_primary == FRAME_W &&
                    plane_coverage(frame.primary, x, base_y,
                                   x + 1, base_mask.y1))
                first_primary = x;
            if (plane_coverage(frame.secondary, x, base_y,
                               x + 1, base_mask.y1))
                last_secondary = x;
        }
        rpx = plane_centroid_x(frame.primary, 0, 0, FRAME_W,
                               base_mask.y0, &rpc);
        rsx = plane_centroid_x(frame.secondary, 0, 0, FRAME_W,
                               base_mask.y0, &rsc);
        bpx = plane_centroid_x(frame.primary, base_mask.x0, base_y,
                               base_mask.x1, base_mask.y1, &bpc);
        bsx = plane_centroid_x(frame.secondary, base_mask.x0, base_y,
                               base_mask.x1, base_mask.y1, &bsc);
    }
    double split = first_primary < FRAME_W && last_secondary >= 0 ?
        (first_primary + last_secondary + 1) / 2.0 : -1.0;
    double ratio = split >= 0 ?
        (split - base_mask.x0) / (base_mask.x1 - base_mask.x0) : 0.0;
    bool ok = !err && rpc && rsc && bpc && bsc &&
        rpx > rsx && bpx > bsx && ratio > 0.10 && ratio < 0.45;
    if (!ok)
        fprintf(stderr,
                "::error title=unequal-width RTL regions::unequal RTL segments did not map by visual width: err=%d empty=%d bounds=[%d,%d)x[%d,%d) first_primary=%d last_secondary=%d split=%.3f ratio=%.6f reading primary(coverage=%llu centroid=%.3f) secondary(coverage=%llu centroid=%.3f) base primary(coverage=%llu centroid=%.3f) secondary(coverage=%llu centroid=%.3f)\n",
                err, base_mask.empty, base_mask.x0, base_mask.x1,
                base_mask.y0, base_mask.y1, first_primary,
                last_secondary, split, ratio,
                (unsigned long long) rpc, rpx,
                (unsigned long long) rsc, rsx,
                (unsigned long long) bpc, bpx,
                (unsigned long long) bsc, bsx);
    free_karaoke_frame(&frame);
    free_mask(&base_mask);
    return ok ? 0 : 1;
}

static int expect_mixed_text_bidi_reading(void)
{
    const char *text =
        "{\\an1\\pos(40,180)}L<WW|{\\k30}\xD7\x90{\\k30}\xD7\x91>R";
    const char *base = "{\\an1\\pos(40,180)}LWWR";
    KaraokeFrame frame = {0};
    Mask base_mask = {0};
    int err = render_karaoke_frame(text, 0, false, &frame);
    if (!err)
        err = render_mask(base, &base_mask);

    uint64_t pc = 0, sc = 0;
    double px = 0, sx = 0;
    if (!err && !base_mask.empty) {
        px = plane_centroid_x(frame.primary, 0, 0, FRAME_W,
                              base_mask.y0, &pc);
        sx = plane_centroid_x(frame.secondary, 0, 0, FRAME_W,
                              base_mask.y0, &sc);
    }
    bool ok = !err && pc && sc && px > sx;
    if (!ok)
        fprintf(stderr,
                "::error title=mixed LTR and RTL furigana::surrounding LTR text changed RTL furigana ordering: err=%d empty=%d bounds=[%d,%d)x[%d,%d) primary(coverage=%llu centroid=%.3f) secondary(coverage=%llu centroid=%.3f)\n",
                err, base_mask.empty, base_mask.x0, base_mask.x1,
                base_mask.y0, base_mask.y1,
                (unsigned long long) pc, px,
                (unsigned long long) sc, sx);
    free_karaoke_frame(&frame);
    free_mask(&base_mask);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    int fail = 0;

    if (!load_test_font()) {
        fprintf(stderr,
                "could not load bundled compare/test/font1.ttf test font\n");
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "--geometry-only") == 0) {
        fail = test_ruby_geometry();
        free(test_font_data);
        return fail ? 1 : 0;
    }

    const char *basic_karaoke =
        "<\xE7\x97\x85|{\\k30}\xE3\x82\x84{\\k26}"
        "\xE3\x81\xBE{\\k10}\xE3\x81\x84>";
    const char *wait_karaoke =
        "<\xE5\x90\x8C|{\\k30}\xE3\x81\x8A{\\k20}"
        "{\\k50}\xE3\x81\xAA>";
    const char *cross_karaoke =
        "<\xE6\x8E\xB4|{\\k40}\xE3\x81\xA4{\\k60}"
        "\xE3\x81\x8B>\xE3\x82\x93{\\k70}\xE3\x81\xA0";
    const char *cross_extended =
        "<\xE6\x8E\xB4|{\\k40}\xE3\x81\xA4{\\k60}"
        "\xE3\x81\x8B>\xE3\x82\x93\xE3\x81\xA7\xE3\x81\x84"
        "\xE3\x82\x8B{\\k70}\xE3\x81\x9E";
    const char *seek_karaoke =
        "{\\fnPixel Operator Mono}<ABC|{\\k30}a{\\k26}b{\\k10}c>";
    const long long ordinary_steps[] = {0, 300, 700};
    const long long basic_steps[] = {0, 300, 560};
    const long long cross_steps[] = {0, 400, 1000};
    const long long kf_steps[] = {0, 100, 200, 300, 430, 560, 610};

    // The legacy-only path remains selected when no furigana group exists.
    fail |= expect_karaoke_steps("{\\k30}a{\\k40}b{\\k50}c",
                                 ordinary_steps, 3, true);
    fail |= expect_karaoke_same_at("{\\kf60}abc", "{\\K60}abc", 175);
    fail |= expect_karaoke_same_at("{\\kf60}abc", "{\\K60}abc", 600);
    fail |= expect_karaoke_same_at("{\\ko30}a{\\ko40}b",
                                    "{\\k30}a{\\k40}b", 300);
    fail |= expect_karaoke_steps("{\\kt50\\k30}a",
                                 (long long[]) {499, 500}, 2, true);
    fail |= expect_karaoke_same_at("{\\k0}a{\\k0}b{\\k30}c",
                                    "{\\k0}ab{\\k30}c", 0);

    fail |= expect_karaoke_steps(basic_karaoke, basic_steps, 3, true);
    fail |= expect_karaoke_same_at("<A|{\\b1\\k30}b>C",
                                    "<A|{\\k30}b>C", 100);
    for (int i = 0; i < 3; i++)
        fail |= expect_karaoke_same_at(basic_karaoke, basic_karaoke,
                                       basic_steps[i]);
    const long long seek_order[] = {560, 0, 300, 300, 560};
    KaraokeCounts seek_counts[5] = {0};
    KaraokeCounts direct_zero = {0}, direct_mid = {0};
    int seek_err = render_karaoke_sequence(
        seek_karaoke, seek_order, 5, seek_counts);
    int zero_err = render_karaoke_counts(seek_karaoke, 0, &direct_zero);
    int mid_err = render_karaoke_counts(seek_karaoke, 300, &direct_mid);
    if (seek_err || zero_err || mid_err ||
            !same_counts(seek_counts[0], seek_counts[4]) ||
            !same_counts(seek_counts[1], direct_zero) ||
            !same_counts(seek_counts[2], direct_mid) ||
            !same_counts(seek_counts[2], seek_counts[3])) {
        fprintf(stderr,
                "::error title=furigana seek history::furigana karaoke depends on render history: errors(sequence=%d zero=%d mid=%d) sequence=[(%llu,%llu),(%llu,%llu),(%llu,%llu),(%llu,%llu),(%llu,%llu)] direct_zero=(%llu,%llu) direct_mid=(%llu,%llu) equal(final=%d zero=%d mid=%d repeat=%d)\n",
                seek_err, zero_err, mid_err,
                (unsigned long long) seek_counts[0].primary,
                (unsigned long long) seek_counts[0].secondary,
                (unsigned long long) seek_counts[1].primary,
                (unsigned long long) seek_counts[1].secondary,
                (unsigned long long) seek_counts[2].primary,
                (unsigned long long) seek_counts[2].secondary,
                (unsigned long long) seek_counts[3].primary,
                (unsigned long long) seek_counts[3].secondary,
                (unsigned long long) seek_counts[4].primary,
                (unsigned long long) seek_counts[4].secondary,
                (unsigned long long) direct_zero.primary,
                (unsigned long long) direct_zero.secondary,
                (unsigned long long) direct_mid.primary,
                (unsigned long long) direct_mid.secondary,
                same_counts(seek_counts[0], seek_counts[4]),
                same_counts(seek_counts[1], direct_zero),
                same_counts(seek_counts[2], direct_mid),
                same_counts(seek_counts[2], seek_counts[3]));
        fail = 1;
    }

    KaraokeCounts wait_before, wait_during;
    if (render_karaoke_counts(wait_karaoke, 299, &wait_before) ||
            render_karaoke_counts(wait_karaoke, 400, &wait_during) ||
            !same_counts(wait_before, wait_during)) {
        fprintf(stderr, "empty furigana karaoke segment consumed visual width\n");
        fail = 1;
    }
    fail |= expect_karaoke_steps(wait_karaoke,
                                 (long long[]) {299, 400, 500}, 3, false);

    fail |= expect_karaoke_steps("{\\k50}<love|ai> {\\k30}<will|nara>",
                                 (long long[]) {0, 500}, 2, true);
    fail |= expect_karaoke_same_at(
        "<{\\k50}\xE7\x97\x85|\xE3\x82\x84\xE3\x81\xBE\xE3\x81\x84>",
        "<\xE7\x97\x85|\xE3\x82\x84\xE3\x81\xBE\xE3\x81\x84>", 0);
    fail |= expect_karaoke_same_at(
        "{\\k20}A<{\\k999}\xE7\x97\x85|\xE3\x82\x84\xE3\x81\xBE\xE3\x81\x84>B{\\k30}C",
        "{\\k20}A<\xE7\x97\x85|\xE3\x82\x84\xE3\x81\xBE\xE3\x81\x84>B{\\k30}C", 250);
    fail |= expect_karaoke_same_at("<{\\kf50}A|b>", "<A|b>", 250);
    fail |= expect_karaoke_same_at("<{\\K50}A|b>", "<A|b>", 250);
    fail |= expect_karaoke_same_at("<{\\ko50}A|b>", "<A|b>", 250);
    fail |= expect_karaoke_same_at("<{\\kO50}A|b>", "<A|b>", 250);
    fail |= expect_karaoke_same_at("<{\\kt50}A|b>", "<A|b>", 250);

    fail |= expect_karaoke_steps(cross_karaoke, cross_steps, 3, true);
    fail |= expect_karaoke_steps(cross_extended, cross_steps, 3, true);
    fail |= expect_karaoke_same_at(cross_karaoke, cross_karaoke, 650);

    const char *kf_furi =
        "<\xE7\x97\x85|{\\kf30}\xE3\x82\x84{\\kf26}"
        "\xE3\x81\xBE{\\kf10}\xE3\x81\x84>";
    const char *big_k_furi =
        "<\xE7\x97\x85|{\\K30}\xE3\x82\x84{\\K26}"
        "\xE3\x81\xBE{\\K10}\xE3\x81\x84>";
    fail |= expect_karaoke_steps(kf_furi, kf_steps, 7, true);
    for (int i = 0; i < 7; i++)
        fail |= expect_karaoke_same_at(kf_furi, big_k_furi, kf_steps[i]);
    fail |= expect_karaoke_steps("<A|{\\kt50\\ko30}b>",
                                 (long long[]) {499, 500}, 2, true);
    fail |= expect_karaoke_steps(
        "<\xE7\x97\x85|{\\kO30}\xE3\x82\x84{\\kO26}"
        "\xE3\x81\xBE{\\kO10}\xE3\x81\x84>",
        (long long[]) {0, 300, 560}, 3, true);
    fail |= expect_karaoke_steps(
        "<\xE6\x8E\xB4|{\\kO40}\xE3\x81\xA4{\\kO60}"
        "\xE3\x81\x8B>\xE3\x82\x93{\\kO70}\xE3\x81\xA0",
        (long long[]) {0, 400, 1000}, 3, true);
    // The wait exposes the inactive sentinel before each reversed ko starts.
    fail |= expect_ko_outline_activation(
        "{\\frz180\\kt10\\ko30}A", 99, 100);
    fail |= expect_furi_ko_base_outline_activation(
        "{\\an1\\pos(40,180)\\frz180\\kt10}<A|{\\ko30}a>",
        "{\\an1\\pos(40,180)\\frz180}A", 99, 100);
    fail |= expect_furi_ko_base_outline_activation(
        "{\\an1\\pos(40,180)\\frz180\\kt10}<A|{\\ko30}a{\\ko30}b>",
        "{\\an1\\pos(40,180)\\frz180}A", 99, 100);
    fail |= expect_furi_ko_base_outline_activation(
        "{\\an1\\pos(40,180)\\kt10}<\xE7\x97\x85|{\\ko30}\xE3\x82\x84"
        "{\\ko30}\xE3\x81\xBE{\\ko30}\xE3\x81\x84>",
        "{\\an1\\pos(40,180)}\xE7\x97\x85", 99, 100);
    fail |= expect_karaoke_steps("<ABCD|{\\k30}a{\\k30}bb{\\k30}c>",
                                 (long long[]) {0, 300, 600}, 3, true);
    fail |= expect_karaoke_steps("<AB|{\\k30}W{\\k30}i{\\k30}WWW>",
                                 (long long[]) {0, 300, 600}, 3, true);
    fail |= expect_karaoke_steps("<A|{\\k20}a><B|{\\k30}b>C{\\k40}D",
                                 (long long[]) {0, 200, 500}, 3, true);
    fail |= expect_karaoke_steps("{\\k20}A<B|{\\k30}b>C{\\k40}D",
                                 (long long[]) {0, 200, 500}, 3, true);
    fail |= expect_karaoke_steps("<A|{\\k20}a>{\\r}B{\\k30}C",
                                 (long long[]) {0, 200}, 2, true);
    fail |= expect_karaoke_same_at(basic_karaoke, basic_karaoke, 560);
    fail |= expect_cross_segment_spatial_ownership();
    fail |= expect_unequal_width_base_regions();
    fail |= expect_bidi_visual_region_order();
    fail |= expect_three_segment_bidi_order();
    fail |= expect_bidi_kf_direction();
    fail |= expect_unequal_width_bidi_regions();
    fail |= expect_mixed_text_bidi_reading();

    fail |= expect_different("<A|B>", "{\\furi0}<A|B>");
    fail |= expect_same("<test>", "{\\furi0}<test>");
    fail |= expect_same("<{\\c&H3535C5&}test>",
                        "{\\furi0}<{\\c&H3535C5&}test>");
    fail |= expect_literal_color();
    fail |= expect_same("<{ignored|pipe}test>",
                        "{\\furi0}<{ignored|pipe}test>");
    fail |= expect_same("<A|B|C|D>", "{\\furi0}<A|B|C|D>");
    fail |= expect_same("<A|{\\c&H3535C5&}B|C|D>",
                        "{\\furi0}<A|{\\c&H3535C5&}B|C|D>");
    fail |= expect_same("<ABC\\|\\DEF>", "{\\furi0}<ABC|DEF>");
    fail |= expect_same("<ABC\\|\\DEF|B>", "<ABC\\|DEF|B>");
    fail |= expect_same("<A\\|\\B|C\\|\\D|E\\|\\F>",
                        "<A\\|B|C\\|D|E\\|F>");
    fail |= expect_same("<A\\|\\B|C|D|E>", "{\\furi0}<A|B|C|D|E>");
    fail |= expect_same("<A|{ignored|pipe}B|C>", "<A|B|C>");
    fail |= expect_same("<A|B>", "<A|B|>");
    fail |= expect_different("<A|B|C>", "<A|B>");
    fail |= expect_different("<A||C>", "{\\furi0}<A||C>");
    fail |= expect_same("<A||>", "{\\furi0}<A||>");
    fail |= expect_same("<|B|C>", "{\\furi0}<|B|C>");
    fail |= expect_same("<A|B|C|<D|E>>", "{\\furi0}<A|B|C|<D|E>>");
    fail |= expect_same("<cool>", "{\\furi0}<cool>");
    fail |= expect_same("<dramatic>", "{\\furi0}<dramatic>");
    fail |= expect_same("<A|>", "{\\furi0}<A|>");
    fail |= expect_same("<|B>", "{\\furi0}<|B>");
    fail |= expect_same("<A|B", "{\\furi0}<A|B");
    fail |= expect_same("<A|{\\k30B>", "{\\furi0}<A|{\\k30B>");
    fail |= expect_same("<A|{\\b1}B>", "<A|{\\b1}B>");
    fail |= expect_same("<A|{\\kO30}B>", "<A|{\\kO30}B>");
    fail |= expect_same("<A|{\\k30}>", "<A|{\\k30}>");
    fail |= expect_same("<A|{\\k30}b|c>", "<A|{\\k30}b|c>");
    fail |= expect_same("A|B>", "{\\furi0}A|B>");
    fail |= expect_same("<>", "{\\furi0}<>");
    fail |= expect_same("\\<", "{\\furi0}<");
    fail |= expect_same("\\>", "{\\furi0}>");
    fail |= expect_same("\\|", "{\\furi0}|");
    fail |= expect_same("\\\\", "{\\furi0}\\");
    fail |= expect_different("{\\furi0}<A|B>{\\furi1}<C|D>",
                             "{\\furi0}<A|B><C|D>");
    fail |= expect_different("<A|B><C|D>", "{\\furi0}<A|B><C|D>");
    fail |= expect_same("<A|B>", "{\\furis50}<A|B>");
    fail |= expect_different("<A|B>", "{\\furis80}<A|B>");
    fail |= expect_different("{\\furisx80}<A|B>",
                             "{\\furisy80}<A|B>");
    fail |= expect_same("<A|B>", "{\\furifsp10}<A|B>");
    fail |= expect_different("<A|BBBB>", "{\\furifsp10}<A|BBBB>");
    // The test canvas and script resolution are 1:1, so 4% of \fs48 is 1.92.
    fail |= expect_same("<A|B>", "{\\furiap1}<A|B>");
    fail |= expect_same("<A|B>", "{\\furipos(0,1.92)}<A|B>");
    fail |= expect_same("{\\fs96}<A|B>",
                        "{\\fs96\\furipos(0,3.84)}<A|B>");
    fail |= expect_same("{\\furis80}<A|B>",
                        "{\\furis80\\furipos(0,1.92)}<A|B>");
    fail |= expect_different("<A|B>", "{\\furiap0}<A|B>");
    fail |= expect_same("{\\furiap0}<A|B>",
                        "{\\furipos(0,0)}<A|B>");
    fail |= expect_different("<A|B>", "{\\furipos(8,0)}<A|B>");
    fail |= expect_y_order("{\\furipos(0,8)}<A|B>",
                           "{\\furipos(0,-8)}<A|B>");
    // furipos stores an upward-positive offset, which placement subtracts.
    // Relative +Y must move ruby up; do not convert it like screen positions.
    fail |= expect_same("{\\furipos(0,0)\\furipos(~+0,~+8)}<A|B>",
                        "{\\furipos(0,8)}<A|B>");
    fail |= expect_same("{\\furipos(0,0)\\furipos(~+0,~-8)}<A|B>",
                        "{\\furipos(0,-8)}<A|B>");
    fail |= expect_relative_furi_directions();
    fail |= expect_same("{\\furipos(0,3)}<A|B>",
                        "{\\furiap1\\furipos(0,3)}<A|B>");
    fail |= expect_same("{\\furipos(0,3)}<A|B>",
                        "{\\furipos(0,3)\\furiap1}<A|B>");
    fail |= expect_same("{\\furipos(0,3)}<A|B>",
                        "{\\furiap0\\furipos(0,3)}<A|B>");
    fail |= expect_same("{\\furipos(0,3)}<A|B>",
                        "{\\furipos(0,3)\\furiap0}<A|B>");
    fail |= expect_same("{\\furipos(2,3)}<A|B>",
                        "{\\furiap0\\furipos(2,3)}<A|B>");
    fail |= expect_same("{\\furiap0}<A|B><C|D>",
                        "{\\furiap0}<A|B>{\\furiap0}<C|D>");
    fail |= expect_same("{\\furiap0}<A|B>{\\r}<C|D>",
                        "{\\furiap0}<A|B>{\\r\\furiap1}<C|D>");
    fail |= expect_same("{\\furipos(0,3)}<A|B>{\\furiap1}<C|D>",
                        "{\\furipos(0,3)}<A|B><C|D>");
    fail |= expect_same("{\\furipos(0,3)}<A|B>{\\furipos}<C|D>",
                        "{\\furipos(0,3)}<A|B>{\\furipos\\furiap1}<C|D>");
    fail |= expect_same("<A|BBBB>", "{\\furistyle0}<A|BBBB>");
    fail |= expect_same("{\\furistyle0}<A|BBBB>",
                        "{\\furistyle1}<A|BBBB>");
    fail |= expect_different("{\\furistyle0}<A|BBBB>",
                             "{\\furistyle2}<A|BBBB>");
    fail |= expect_same("{\\furistyle0}<BBBB|A>",
                        "{\\furistyle2}<BBBB|A>");
    fail |= expect_same("{\\furistyle2\\furistyle99}<A|BBBB>",
                        "{\\furistyle2}<A|BBBB>");
    fail |= expect_different("{\\furistyle0}<A|BBBB> {\\furistyle2}<A|BBBB>",
                             "{\\furistyle0}<A|BBBB> <A|BBBB>");

    /* Keep ruby and base in separate vertical bands for width assertions.
     * Latin stand-ins make the geometry independent of system CJK fonts. */
    const char *layout_prefix =
        "{\\an7\\pos(20,70)\\bord0\\furichangepos1\\furipos(0,20)}";
    char one_short[128], one_two[128], one_three[128];
    char surrounded_short[128], surrounded_long[128];
    char adjacent_short[128], adjacent_clear[128];
    char overlap_short[128], overlap_long[128], overlap_single[128];
    char chain_short[128], chain_long0[128], chain_long1[128];
    char chain_colored[256];
    char fit_short[128], fit_long[128];
    snprintf(one_short, sizeof(one_short), "%sLH<A|I>RH", layout_prefix);
    snprintf(one_two, sizeof(one_two), "%sLH<A|WW>RH", layout_prefix);
    snprintf(one_three, sizeof(one_three), "%sLH<A|WWW>RH", layout_prefix);
    snprintf(surrounded_short, sizeof(surrounded_short),
             "%sABC<A|I>DEF", layout_prefix);
    snprintf(surrounded_long, sizeof(surrounded_long),
             "%sABC<A|WWW>DEF", layout_prefix);
    snprintf(adjacent_short, sizeof(adjacent_short),
             "%sLH<WW|I><WW|I>RH", layout_prefix);
    snprintf(adjacent_clear, sizeof(adjacent_clear),
             "%sLH<WW|l><WW|l>RH", layout_prefix);
    snprintf(overlap_short, sizeof(overlap_short),
             "%sLH<A|I><B|I>RH", layout_prefix);
    snprintf(overlap_long, sizeof(overlap_long),
             "%sLH<A|WWWW><B|MMMM>RH", layout_prefix);
    snprintf(overlap_single, sizeof(overlap_single),
             "%s<A|WWWW>", layout_prefix);
    snprintf(chain_short, sizeof(chain_short),
             "%sLH<A|I><B|I><C|I>RH", layout_prefix);
    snprintf(chain_long0, sizeof(chain_long0),
             "%s{\\furistyle0}LH<A|WWW><B|MMM><C|WWW>RH",
             layout_prefix);
    snprintf(chain_long1, sizeof(chain_long1),
             "%s{\\furistyle1}LH<A|WWW><B|MMM><C|WWW>RH",
             layout_prefix);
    snprintf(chain_colored, sizeof(chain_colored),
             "%s{\\furistyle0}LH{\\c&H0000FF&}<A|WWW>"
             "{\\c&H00FF00&}<B|MMM>{\\c&HFF0000&}<C|WWW>RH",
             layout_prefix);
    snprintf(fit_short, sizeof(fit_short),
             "%s{\\furistyle2}LH<A|I>RH", layout_prefix);
    snprintf(fit_long, sizeof(fit_long),
             "%s{\\furistyle2}LH<A|WWWW>RH", layout_prefix);

    // Cases A/B: two- and three-glyph ruby keep a one-glyph base advance.
    fail |= expect_same_base_span(one_short, one_two);
    fail |= expect_same_base_span(one_short, one_three);
    // Case C: adjacent ruby whose ink is already separate adds no spacing.
    fail |= expect_same_base_span(adjacent_short, adjacent_clear);
    // Case D: actual ruby/ruby overlap adds less than a full ruby width.
    fail |= expect_overlap_spacing_bounded(
        overlap_short, overlap_long, overlap_single);
    // Case E: ordinary neighboring text does not reserve ruby overhang.
    fail |= expect_same_base_span(surrounded_short, surrounded_long);
    // Case F: a collision chain converges identically for styles 0 and 1.
    fail |= expect_same(chain_long0, chain_long1);
    fail |= expect_overlap_spacing_bounded(
        chain_short, chain_long0, overlap_single);
    fail |= expect_colored_furi_separate(chain_colored);
    // Case G: style 2 still fits long ruby without changing base advance.
    fail |= expect_same_base_span(fit_short, fit_long);

    fail |= expect_same(
        "\xE3\x82\x82\xE3\x81\x86<\xE4\xB8\x80|test>"
        "\xE4\xBA\xBA\xE3\x81\x98\xE3\x82\x83\xE3\x81\xAA"
        "\xE3\x81\x84\xE3\x82\x93\xE3\x81\xA0",
        "{\\furistyle0}\xE3\x82\x82\xE3\x81\x86<\xE4\xB8\x80|test>"
        "\xE4\xBA\xBA\xE3\x81\x98\xE3\x82\x83\xE3\x81\xAA"
        "\xE3\x81\x84\xE3\x82\x93\xE3\x81\xA0");
    fail |= expect_same(
        "<\xE5\xA4\xA2|\xE3\x82\x86\xE3\x82\x81>\xE3\x81\xAE"
        "<\xE8\xA9\xB1|\xE3\x81\xAF\xE3\x81\xAA\xE3\x81\x97>",
        "{\\furistyle0}<\xE5\xA4\xA2|\xE3\x82\x86\xE3\x82\x81>"
        "\xE3\x81\xAE<\xE8\xA9\xB1|\xE3\x81\xAF\xE3\x81\xAA\xE3\x81\x97>");
    fail |= expect_same(
        "<\xE6\x84\x9B|\xE3\x81\x82\xE3\x81\x84>"
        "<\xE5\x9F\x8E|\xE3\x81\x98\xE3\x82\x87\xE3\x81\x86>"
        "<\xE6\x81\x8B|\xE3\x82\x8C\xE3\x82\x93>"
        "<\xE5\xA4\xAA|\xE3\x81\x9F>"
        "<\xE9\x83\x8E|\xE3\x82\x8D\xE3\x81\x86>",
        "{\\furistyle0}<\xE6\x84\x9B|\xE3\x81\x82\xE3\x81\x84>"
        "<\xE5\x9F\x8E|\xE3\x81\x98\xE3\x82\x87\xE3\x81\x86>"
        "<\xE6\x81\x8B|\xE3\x82\x8C\xE3\x82\x93>"
        "<\xE5\xA4\xAA|\xE3\x81\x9F>"
        "<\xE9\x83\x8E|\xE3\x82\x8D\xE3\x81\x86>");
    fail |= expect_same("A\\NB", "{\\furi0}A\\NB");
    fail |= expect_bottom_anchor_with_taller_block(
        "{\\an2\\furichangepos1}TOP\\N<A|BBBB>", "{\\an2}TOP\\NA");
    fail |= expect_top_anchor_with_taller_block(
        "{\\an8\\furichangepos1\\furisy100}<A|B>\\NBOTTOM",
        "{\\an8\\furichangepos1\\furisy50}<A|B>\\NBOTTOM");
    fail |= expect_center_anchor_with_taller_block(
        "{\\an5\\furichangepos1}TOP\\N<A|BBBB>", "{\\an5}TOP\\NA");
    fail |= expect_same_height(
        "<A|BBBB>", "<A|BBBB><A|BBBB>");
    fail |= expect_partition_tops_aligned(
        "<A|BBBB>    <_|BBBB>", 2);
    fail |= expect_partition_tops_aligned(
        "<A|BBBB>    <_|BBBB>    <g|BBBB>", 3);
    fail |= expect_bottom_anchor_with_taller_block(
        "{\\an2\\furichangepos1}<A|BBBB>",
        "{\\an2\\furichangepos1\\furiap0}<A|BBBB>");

    fail |= test_ruby_geometry();

    free(test_font_data);
    return fail ? 1 : 0;
}
