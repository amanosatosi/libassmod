#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ass_vertical.h"

typedef struct {
    RenderContext state;
    GlyphInfo glyphs[32];
    FriBidiChar text[32];
    LineInfo lines[2];
    OutlineHashValue outline;
} Fixture;

static void init(Fixture *f, const uint32_t *symbols, int length,
                 int profile, int direction)
{
    memset(f, 0, sizeof(*f));
    f->outline.outline[0].n_points = 1;
    f->state.native_vertical = true;
    f->state.vertical_profile = profile;
    f->state.vertical_direction = direction;
    f->state.object_scale = 1;
    f->state.screen_scale_x = 1;
    f->state.screen_scale_y = 1;
    f->state.text_info.glyphs = f->glyphs;
    f->state.text_info.event_text = f->text;
    f->state.text_info.lines = f->lines;
    f->state.text_info.length = length;
    for (int i = 0; i < length; i++) {
        GlyphInfo *g = &f->glyphs[i];
        g->symbol = symbols[i];
        g->outline = &f->outline;
        g->font_size = 40;
        g->scale_x = g->scale_y = 1;
        g->advance.x = g->cluster_advance.x = 24 * 64;
        g->fill_bbox = (ASS_Rect) {0, -30 * 64, 24 * 64, 0};
    }
    ass_vertical_prepare(&f->state);
}

static bool expect(bool ok, const char *message)
{
    if (!ok)
        fprintf(stderr, "%s\n", message);
    return ok;
}

static double center_y(const GlyphInfo *g)
{
    return d6_to_double(g->pos.y) +
        d6_to_double(g->fill_bbox.y_min + g->fill_bbox.y_max) * 0.5;
}

int main(void)
{
    bool ok = true;
    Fixture f;
    static const uint32_t cjk[] = {0x4e00, 0x3002, 0x53e3};
    init(&f, cjk, 3, 0, 0);
    f.glyphs[1].fill_bbox = (ASS_Rect) {0, -8 * 64, 8 * 64, 0};
    ok &= expect(f.state.vertical_profile == 1 &&
                 f.state.vertical_direction == 1, "CJK auto profile/direction");
    ok &= expect(ass_vertical_layout(&f.state, 300), "CJK layout allocation");
    ok &= expect(fabs(center_y(&f.glyphs[1]) - center_y(&f.glyphs[0]) - 40) < 1 &&
                 fabs(center_y(&f.glyphs[2]) - center_y(&f.glyphs[1]) - 40) < 1,
                 "small CJK punctuation changed full-em rhythm");
    init(&f, cjk, 3, 1, 0);
    f.state.vertical_spacing = 5;
    ok &= expect(ass_vertical_layout(&f.state, 300) &&
                 fabs(center_y(&f.glyphs[1]) - center_y(&f.glyphs[0]) - 45) < 1,
                 "vsp did not add spacing between units");

    static const uint32_t latin[] = {'i', 'l', 'm', 'g'};
    init(&f, latin, 4, 0, 0);
    f.glyphs[0].fill_bbox = (ASS_Rect) {0, -18 * 64, 5 * 64, 0};
    f.glyphs[1].fill_bbox = (ASS_Rect) {0, -28 * 64, 5 * 64, 0};
    f.glyphs[3].fill_bbox = (ASS_Rect) {0, -18 * 64, 22 * 64, 8 * 64};
    ok &= expect(f.state.vertical_profile == 2 &&
                 f.state.vertical_direction == 2, "Latin auto profile/direction");
    ok &= expect(ass_vertical_layout(&f.state, 300), "Latin layout allocation");
    ok &= expect(d6_to_double(f.glyphs[0].cluster_advance.y) > 14 &&
                 d6_to_double(f.glyphs[0].cluster_advance.y) < 40 &&
                 d6_to_double(f.glyphs[3].cluster_advance.y) >
                 d6_to_double(f.glyphs[0].cluster_advance.y),
                 "Latin optical advance did not follow fill height");

    static const uint32_t myanmar[] =
        {0x1019, 0x103c, 0x1014, 0x103a, 0x1019, 0x102c};
    init(&f, myanmar, 6, 0, 0);
    ok &= expect(f.state.vertical_profile == 3 &&
                 f.state.vertical_direction == 2, "Myanmar auto profile/direction");
    ok &= expect(ass_vertical_layout(&f.state, 300), "Myanmar layout allocation");
    ok &= expect(f.glyphs[0].pos.y == f.glyphs[1].pos.y &&
                 f.glyphs[1].pos.y == f.glyphs[3].pos.y &&
                 f.glyphs[4].pos.y > f.glyphs[3].pos.y &&
                 f.glyphs[4].pos.y == f.glyphs[5].pos.y,
                 "Myanmar codepoints were split inside layout syllables");

    static const uint32_t columns[] = {0x65e5, '\n', 0x672c};
    init(&f, columns, 3, 1, 0);
    ok &= expect(ass_vertical_layout(&f.state, 300), "left column layout");
    ok &= expect(f.glyphs[0].pos.x > f.glyphs[2].pos.x,
                 "CJK default column did not progress left");
    init(&f, columns, 3, 1, 2);
    ok &= expect(ass_vertical_layout(&f.state, 300), "right column layout");
    ok &= expect(f.glyphs[0].pos.x < f.glyphs[2].pos.x,
                 "vdir2 did not progress right");
    double base_column_gap = d6_to_double(f.glyphs[2].pos.x - f.glyphs[0].pos.x);
    init(&f, columns, 3, 1, 2);
    f.state.vertical_column_spacing = 9;
    ok &= expect(ass_vertical_layout(&f.state, 300) &&
                 d6_to_double(f.glyphs[2].pos.x - f.glyphs[0].pos.x) >
                     base_column_gap + 8,
                 "vcolsp did not add column spacing");
    static const uint32_t latin_columns[] = {'A', '\n', 'B'};
    init(&f, latin_columns, 3, 2, 0);
    ok &= expect(ass_vertical_layout(&f.state, 300) &&
                 f.glyphs[2].pos.x > f.glyphs[0].pos.x,
                 "Latin N did not progress right");
    static const uint32_t myanmar_columns[] = {0x1019, '\n', 0x1014};
    init(&f, myanmar_columns, 3, 3, 0);
    ok &= expect(ass_vertical_layout(&f.state, 300) &&
                 f.glyphs[2].pos.x > f.glyphs[0].pos.x,
                 "Myanmar N did not progress right");

    static const uint32_t wrapped[] = {0x65e5, 0x672c, 0x8a9e, 0x6b21};
    init(&f, wrapped, 4, 1, 0);
    ok &= expect(ass_vertical_layout(&f.state, 85), "automatic wrapping layout");
    ok &= expect(f.glyphs[0].pos.x == f.glyphs[1].pos.x &&
                 f.glyphs[2].pos.x < f.glyphs[1].pos.x &&
                 f.glyphs[2].pos.y == f.glyphs[0].pos.y,
                 "height overflow did not create a new CJK column");

    static const uint32_t mixed[] = {0x65e5, 'T', 'V', 0x30a2, 0x30cb, 0x30e1};
    init(&f, mixed, 6, 0, 0);
    ok &= expect(f.state.vertical_profile == 1 &&
                 f.state.vertical_direction == 1,
                 "embedded Latin reversed CJK event profile");
    ok &= expect(ass_vertical_layout(&f.state, 300) &&
                 f.glyphs[0].curved_angle == 0 &&
                 f.glyphs[1].curved_angle == -90 &&
                 f.glyphs[3].curved_angle == 0,
                 "mixed CJK unit orientation is wrong");
    init(&f, mixed, 6, 2, 0);
    ok &= expect(f.state.vertical_profile == 2 &&
                 f.state.vertical_direction == 2,
                 "explicit vtype did not override strong CJK text");

    static const uint32_t jamo[] = {0x1100, 0x1161, 0x11ab, 0x4e00};
    init(&f, jamo, 4, 1, 0);
    GlyphInfo extra = f.glyphs[0];
    extra.next = NULL;
    extra.offset.x = 4 * 64;
    f.glyphs[0].next = &extra;
    f.glyphs[1].skip = f.glyphs[2].skip = true;
    ok &= expect(ass_vertical_layout(&f.state, 300), "Jamo cluster layout");
    ok &= expect(extra.pos.y == f.glyphs[0].pos.y &&
                 f.glyphs[3].pos.y > f.glyphs[0].pos.y,
                 "shaped cluster chain was split");

    static const uint32_t empty_ink[] = {'A', ' ', 0x200b, 'B'};
    init(&f, empty_ink, 4, 2, 0);
    f.glyphs[1].outline = NULL;
    f.glyphs[2].outline = NULL;
    ok &= expect(ass_vertical_layout(&f.state, 300), "zero-ink layout");
    ok &= expect(f.glyphs[1].cluster_advance.y > 0 &&
                 f.glyphs[2].cluster_advance.y > 0 &&
                 f.glyphs[3].pos.y > f.glyphs[0].pos.y,
                 "space or zero-width unit collapsed the cursor");

    init(&f, latin, 4, 2, 0);
    ok &= expect(ass_vertical_layout(&f.state, 300), "base effect fixture");
    int32_t base_y = f.glyphs[2].pos.y;
    init(&f, latin, 4, 2, 0);
    for (int i = 0; i < 4; i++) {
        f.glyphs[i].border_x = f.glyphs[i].border_y = 20;
        f.glyphs[i].shadow_x = f.glyphs[i].shadow_y = 20;
        f.glyphs[i].blur_x = f.glyphs[i].blur_y = 10;
    }
    ok &= expect(ass_vertical_layout(&f.state, 300) &&
                 f.glyphs[2].pos.y == base_y,
                 "paint effects changed base vertical positions");

    return ok ? 0 : 1;
}
