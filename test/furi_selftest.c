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

static int render_mask(const char *text, Mask *mask)
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

static void free_mask(Mask *mask)
{
    free(mask->alpha);
    mask->alpha = NULL;
}

static bool same_mask(const Mask *a, const Mask *b)
{
    return !memcmp(a->alpha, b->alpha, FRAME_W * FRAME_H);
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
        fprintf(stderr, "unequal shaped reading widths did not own unequal base regions\n");
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
        fprintf(stderr, "bidi-reordered reading did not map to visual base regions\n");
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
        fprintf(stderr, "three-segment RTL karaoke did not activate right to left\n");
    for (int i = 0; i < 3; i++)
        free_karaoke_frame(&frame[i]);
    free_mask(&base_mask);
    return ok ? 0 : 1;
}

static int expect_bidi_kf_direction(void)
{
    const char *text =
        "{\\an1\\pos(40,180)}<WW|{\\kf100}\xD7\x90"
        "{\\kf100}\xD7\x91>";
    const char *base = "{\\an1\\pos(40,180)}WW";
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
        rx[3] = positive_delta_centroid_x(
            frame[3].primary, frame[4].primary,
            0, 0, FRAME_W, base_mask.y0, &rc[3]);
        bx[3] = positive_delta_centroid_x(
            frame[3].primary, frame[4].primary,
            base_mask.x0, base_y, base_mask.x1, base_mask.y1, &bc[3]);
        for (int i = 4; i < 6; i++) {
            rx[i] = positive_delta_centroid_x(
                frame[i].primary, frame[i + 1].primary,
                0, 0, FRAME_W, base_mask.y0, &rc[i]);
            bx[i] = positive_delta_centroid_x(
                frame[i].primary, frame[i + 1].primary,
                base_mask.x0, base_y, base_mask.x1, base_mask.y1, &bc[i]);
        }
    }
    bool covered = !err;
    for (int i = 0; i < 6; i++)
        covered &= rc[i] && bc[i];
    bool ok = covered &&
        rx[0] > rx[1] && rx[1] > rx[2] &&
        bx[0] > bx[1] && bx[1] > bx[2] &&
        rx[3] > rx[4] && rx[4] > rx[5] &&
        bx[3] > bx[4] && bx[4] > bx[5];
    if (!ok)
        fprintf(stderr, "RTL kf sweep did not progress right to left\n");
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
        fprintf(stderr, "unequal RTL segments did not map by visual width\n");
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
        fprintf(stderr, "surrounding LTR text changed RTL furigana ordering\n");
    free_karaoke_frame(&frame);
    free_mask(&base_mask);
    return ok ? 0 : 1;
}

int main(void)
{
    int fail = 0;

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
    KaraokeCounts seek_counts[5];
    KaraokeCounts direct_zero, direct_mid;
    if (render_karaoke_sequence(basic_karaoke, seek_order, 5, seek_counts) ||
            render_karaoke_counts(basic_karaoke, 0, &direct_zero) ||
            render_karaoke_counts(basic_karaoke, 300, &direct_mid) ||
            !same_counts(seek_counts[0], seek_counts[4]) ||
            !same_counts(seek_counts[1], direct_zero) ||
            !same_counts(seek_counts[2], direct_mid) ||
            !same_counts(seek_counts[2], seek_counts[3])) {
        fprintf(stderr, "furigana karaoke depends on render history\n");
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

    return fail ? 1 : 0;
}
