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

static int expect_top_anchor_with_taller_block(const char *with_furi,
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
    bool ok = !mf.empty && !mn.empty && abs(mf.y0 - mn.y0) <= 1 &&
        mf.y1 > mn.y1;
    if (!ok)
        fprintf(stderr, "expected top anchor and taller block: `%s` vs `%s`\n",
                with_furi, without_furi);
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

int main(void)
{
    int fail = 0;

    fail |= expect_different("<A|B>", "{\\furi0}<A|B>");
    fail |= expect_same("<cool>", "{\\furi0}<cool>");
    fail |= expect_same("<dramatic>", "{\\furi0}<dramatic>");
    fail |= expect_same("<A|>", "{\\furi0}<A|>");
    fail |= expect_same("<|B>", "{\\furi0}<|B>");
    fail |= expect_same("<A|B", "{\\furi0}<A|B");
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
    fail |= expect_different("<A|B>", "{\\furipos(8,0)}<A|B>");
    fail |= expect_y_order("{\\furipos(0,8)}<A|B>",
                           "{\\furipos(0,-8)}<A|B>");
    fail |= expect_same("<A|BBBB>", "{\\furistyle0}<A|BBBB>");
    fail |= expect_same("{\\furistyle0}<A|BBBB>",
                        "{\\furistyle1}<A|BBBB>");
    fail |= expect_different("{\\furistyle0}<A|BBBB>",
                             "{\\furistyle2}<A|BBBB>");
    fail |= expect_same("{\\furistyle2\\furistyle99}<A|BBBB>",
                        "{\\furistyle2}<A|BBBB>");
    fail |= expect_different("{\\furistyle0}<A|BBBB> {\\furistyle2}<A|BBBB>",
                             "{\\furistyle0}<A|BBBB> <A|BBBB>");
    fail |= expect_same("A\\NB", "{\\furi0}A\\NB");
    fail |= expect_bottom_anchor_with_taller_block(
        "{\\an2}TOP\\N<A|BBBB>", "{\\an2}TOP\\NA");
    fail |= expect_top_anchor_with_taller_block(
        "{\\an8}<A|BBBB>\\NBOTTOM", "{\\an8}A\\NBOTTOM");
    fail |= expect_center_anchor_with_taller_block(
        "{\\an5}TOP\\N<A|BBBB>", "{\\an5}TOP\\NA");
    fail |= expect_same_height(
        "<A|BBBB>", "<A|BBBB><A|BBBB>");

    return fail ? 1 : 0;
}
