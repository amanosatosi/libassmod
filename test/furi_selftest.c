#include <limits.h>
#include <stdbool.h>
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

static void add_test_font(ASS_Library *lib)
{
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
    size_t len = strlen(prefix) + strlen(text) + 2;
    char *script = malloc(len);
    if (!script)
        return NULL;
    snprintf(script, len, "%s%s\n", prefix, text);
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
    /* Layout uses exact 26.6 glyph bounds and intentionally adds no safety
     * gap.  Rasterizing two touching bounds can cover the same device-pixel
     * column through rounding/antialiasing without a geometric overlap. */
    ok = ok && right[0] <= left[1] + 1 && right[1] <= left[2] + 1;
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

int main(void)
{
    int fail = 0;

    if (!load_test_font()) {
        fprintf(stderr,
                "could not load bundled compare/test/font1.ttf test font\n");
        return 1;
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
        "{\\an7\\pos(20,70)\\bord0\\furipos(0,20)}";
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
        "{\\an2}TOP\\N<A|BBBB>", "{\\an2}TOP\\NA");
    fail |= expect_top_anchor_with_taller_block(
        "{\\an8\\furisy100}<A|B>\\NBOTTOM",
        "{\\an8\\furisy50}<A|B>\\NBOTTOM");
    fail |= expect_center_anchor_with_taller_block(
        "{\\an5}TOP\\N<A|BBBB>", "{\\an5}TOP\\NA");
    fail |= expect_same_height(
        "<A|BBBB>", "<A|BBBB><A|BBBB>");
    fail |= expect_partition_tops_aligned(
        "<A|BBBB>    <_|BBBB>", 2);
    fail |= expect_partition_tops_aligned(
        "<A|BBBB>    <_|BBBB>    <g|BBBB>", 3);
    fail |= expect_bottom_anchor_with_taller_block(
        "{\\an2}<A|BBBB>", "{\\an2\\furiap0}<A|BBBB>");

    free(test_font_data);
    return fail ? 1 : 0;
}
