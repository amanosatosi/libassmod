#include "config.h"
#include "ass_vertical.h"
#include "ass_compat.h"
#include "ass_priv.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>

#ifdef CONFIG_UNIBREAK
#include <linebreak.h>
#endif

typedef enum {
    VERTICAL_UNIT_CJK = 1,
    VERTICAL_UNIT_LATIN = 2,
    VERTICAL_UNIT_MYANMAR = 3,
} VerticalUnitType;

typedef struct {
    int first, end, column, breaks_before;
    bool sideways, ink, break_after;
    VerticalUnitType type;
    double top, advance, spacing, width, em;
    double x0, x1, y0, y1;
    double cell_x0, cell_y0;
} VerticalUnit;

typedef struct {
    double left, width, height;
    int alignment;
} VerticalColumn;

static int32_t layout_d6(double value)
{
    if (!isfinite(value))
        return 0;
    return double_to_d6(fmax(-16000000.0, fmin(16000000.0, value)));
}

static bool cjk_strong(uint32_t c)
{
    if ((c >= 0x3400 && c <= 0x9fff) ||
           (c >= 0xf900 && c <= 0xfaff) ||
           (c >= 0x3040 && c <= 0x30ff) ||
           (c >= 0x31f0 && c <= 0x31ff) ||
           (c >= 0x1100 && c <= 0x11ff) ||
           (c >= 0x3130 && c <= 0x318f) ||
           (c >= 0xac00 && c <= 0xd7af))
        return true;
    if (c < 0x3000)
        return false;
    hb_script_t script = hb_unicode_script(hb_unicode_funcs_get_default(), c);
    return script == HB_SCRIPT_HAN || script == HB_SCRIPT_HIRAGANA ||
           script == HB_SCRIPT_KATAKANA || script == HB_SCRIPT_HANGUL;
}

static bool myanmar_char(uint32_t c)
{
    return (c >= 0x1000 && c <= 0x109f) ||
           (c >= 0xa9e0 && c <= 0xa9ff) ||
           (c >= 0xaa60 && c <= 0xaa7f);
}

static bool cjk_vertical_form(uint32_t c)
{
    return cjk_strong(c) || (c >= 0x3000 && c <= 0x303f) ||
           (c >= 0xfe10 && c <= 0xfe4f) ||
           (c >= 0xff01 && c <= 0xff65);
}

static VerticalUnitType classify(uint32_t c)
{
    if (myanmar_char(c))
        return VERTICAL_UNIT_MYANMAR;
    if (cjk_vertical_form(c))
        return VERTICAL_UNIT_CJK;
    return VERTICAL_UNIT_LATIN;
}

void ass_vertical_prepare(RenderContext *state)
{
    TextInfo *text = &state->text_info;
    if (!state->native_vertical)
        return;
    if (!state->vertical_direction) {
        int cjk = 0, other = 0;
        bool first_cjk = false, have_strong = false;
        for (int i = 0; i < text->length; i++) {
            uint32_t c = text->glyphs[i].symbol;
            bool is_cjk = cjk_strong(c);
            bool is_other = myanmar_char(c) ||
                hb_unicode_script(hb_unicode_funcs_get_default(), c) == HB_SCRIPT_LATIN;
            cjk += is_cjk;
            other += is_other;
            if (!have_strong && (is_cjk || is_other)) {
                first_cjk = is_cjk;
                have_strong = true;
            }
        }
        state->vertical_direction = cjk > other ||
            (cjk && cjk == other && first_cjk) ? 1 : 2;
    }
    text->native_vertical = true;
    for (int i = 0; i < text->length; i++) {
        GlyphInfo *g = &text->glyphs[i];
        text->event_text[i] = g->symbol;
        g->native_vertical = true;
        VerticalUnitType type = state->vertical_profile ?
            (VerticalUnitType) state->vertical_profile : classify(g->symbol);
        g->vertical_substitute = type == VERTICAL_UNIT_CJK &&
            cjk_vertical_form(g->symbol);
    }
}

void ass_vertical_mark_syllables(RenderContext *state)
{
    if (!state->native_vertical)
        return;
    TextInfo *text = &state->text_info;
    /* A boundary between layout syllables is also a safe HarfBuzz run
     * boundary. Keep all codepoints of each syllable in the same run. */
    for (int i = 1; i < text->length; i++) {
        if (myanmar_char(text->glyphs[i].symbol) &&
                ass_myanmar_layout_break(
                    (const uint32_t *) text->event_text,
                    text->length, i))
            text->glyphs[i].starts_new_run = true;
    }
}

static void extend_bbox(VerticalUnit *unit, double x0, double x1,
                        double y0, double y1)
{
    if (!unit->ink) {
        unit->x0 = x0; unit->x1 = x1;
        unit->y0 = y0; unit->y1 = y1;
        unit->ink = true;
    } else {
        unit->x0 = fmin(unit->x0, x0);
        unit->x1 = fmax(unit->x1, x1);
        unit->y0 = fmin(unit->y0, y0);
        unit->y1 = fmax(unit->y1, y1);
    }
}

/* Measure exactly the same positioned fill outlines used for placement.
 * Decorative outlines and post-layout transforms are deliberately absent. */
static void measure_unit(VerticalUnit *unit, GlyphInfo *glyphs,
                         double spacing)
{
    double pen = 0.0;
    double em = 0.0, em_x = 0.0;
    for (int i = unit->first; i < unit->end; i++) {
        GlyphInfo *root = &glyphs[i];
        if (root->skip)
            continue;
        em = fmax(em, root->font_size * root->scale_y);
        em_x = fmax(em_x, root->font_size * root->scale_x);
        double inner_x = pen, inner_y = 0.0;
        for (GlyphInfo *g = root; g; g = g->next) {
            if (g->outline && g->outline->outline[0].n_points) {
                double x0 = inner_x + d6_to_double(g->offset.x + g->fill_bbox.x_min);
                double x1 = inner_x + d6_to_double(g->offset.x + g->fill_bbox.x_max);
                double y0 = inner_y + d6_to_double(g->offset.y + g->vshift + g->fill_bbox.y_min);
                double y1 = inner_y + d6_to_double(g->offset.y + g->vshift + g->fill_bbox.y_max);
                if (unit->sideways)
                    extend_bbox(unit, -y1, -y0, x0, x1);
                else
                    extend_bbox(unit, x0, x1, y0, y1);
            }
            inner_x += d6_to_double(g->advance.x);
            inner_y += d6_to_double(g->advance.y);
        }
        pen += d6_to_double(root->cluster_advance.x);
    }
    em = isfinite(em) ? fmax(1.0, fmin(em, 1000000.0)) : 1.0;
    em_x = isfinite(em_x) ? fmax(1.0, fmin(em_x, 1000000.0)) : 1.0;
    unit->em = em;
    double ink_h = unit->ink ? fmax(0.0, unit->y1 - unit->y0) : 0.0;
    double ink_w = unit->ink ? fmax(0.0, unit->x1 - unit->x0) : 0.0;
    if (unit->type == VERTICAL_UNIT_CJK) {
        unit->advance = fmax(em, ink_h);
        unit->width = fmax(em_x, ink_w);
    } else {
        double min_advance = unit->type == VERTICAL_UNIT_MYANMAR ?
            0.55 * em : 0.36 * em;
        unit->advance = fmax(ink_h, min_advance);
        unit->width = fmax(ink_w, unit->type == VERTICAL_UNIT_MYANMAR ?
            0.65 * em_x : 0.4 * em_x);
        if (!unit->ink && pen > 0)
            unit->advance = fmax(unit->advance, fmin(pen, 0.6 * em));
    }
    unit->advance = isfinite(unit->advance) ?
        fmax(1.0, fmin(unit->advance, 1000000.0)) : em;
    double default_gap = unit->type == VERTICAL_UNIT_MYANMAR ?
        0.18 * em : unit->type == VERTICAL_UNIT_LATIN ? 0.14 * em : 0.0;
    unit->spacing = isfinite(spacing) ?
        fmax(-0.9 * unit->advance,
             fmin(default_gap + spacing, 1000000.0)) : default_gap;
    if (!unit->ink)
        unit->x0 = unit->x1 = unit->y0 = unit->y1 = 0.0;
    /* The cell encloses fill ink and has a minimum em-derived extent.
     * Advancing between cell edges accounts for unlike glyph bearings. */
    unit->cell_x0 = (unit->x0 + unit->x1 - unit->width) * 0.5;
    unit->cell_y0 = (unit->y0 + unit->y1 - unit->advance) * 0.5;
}

static bool sideways_cluster(uint32_t c, VerticalUnitType type)
{
    if (type != VERTICAL_UNIT_CJK || c == ' ' || c == 0xa0)
        return false;
    return !cjk_vertical_form(c) && !myanmar_char(c) && c >= 0x21;
}

static void place_unit(VerticalUnit *unit, GlyphInfo *glyphs,
                       double cell_left)
{
    double x = cell_left - unit->cell_x0;
    double y = unit->top - unit->cell_y0;
    double pen = 0.0;
    for (int i = unit->first; i < unit->end; i++) {
        GlyphInfo *root = &glyphs[i];
        if (root->skip)
            continue;
        double cluster_advance = d6_to_double(root->cluster_advance.x);
        double inner_x = pen, inner_y = 0.0;
        for (GlyphInfo *g = root; g; g = g->next) {
            double gx = inner_x + d6_to_double(g->offset.x);
            double gy = inner_y + d6_to_double(g->offset.y + g->vshift);
            g->pos.x = layout_d6(x + (unit->sideways ? -gy : gx));
            g->pos.y = layout_d6(y + (unit->sideways ? gx : gy));
            g->curved_angle = unit->sideways ? -90.0 : 0.0;
            g->line = 0; /* columns share the single ASS block metric line */
            inner_x += d6_to_double(g->advance.x);
            inner_y += d6_to_double(g->advance.y);
        }
        root->cluster_advance.x = 0;
        root->cluster_advance.y = layout_d6(unit->advance);
        pen += cluster_advance;
    }
}

bool ass_vertical_layout(RenderContext *state, double max_height)
{
    TextInfo *text = &state->text_info;
    int length = text->length;
    size_t n = (size_t) length;
    if (n > (SIZE_MAX - sizeof(VerticalColumn)) /
            (sizeof(VerticalUnit) + sizeof(VerticalColumn)))
        return false;
    void *storage = calloc(1, n * sizeof(VerticalUnit) +
                            (n + 1) * sizeof(VerticalColumn));
    if (!storage)
        return false;
    VerticalUnit *units = storage;
    VerticalColumn *cols = (VerticalColumn *) (units + n);
    int count = 0, pending_breaks = 0;
#ifdef CONFIG_UNIBREAK
    char *unibrks = NULL;
    if (state->renderer && text->breaks &&
            (state->renderer->track->parser_priv->feature_flags &
             FEATURE_MASK(ASS_FEATURE_WRAP_UNICODE))) {
        unibrks = text->breaks;
        set_linebreaks_utf32(text->event_text, length,
            state->renderer->track->Language, unibrks);
    }
#endif
    double spacing = state->vertical_spacing * state->object_scale *
                     state->screen_scale_y;
    for (int i = 0; i < length;) {
        GlyphInfo *root = &text->glyphs[i];
        if (root->symbol == '\n') {
            pending_breaks++;
            root->skip = true;
            i++;
            continue;
        }
        if (root->skip) {
            i++;
            continue;
        }
        VerticalUnit *unit = &units[count++];
        unit->first = i;
        unit->end = i + 1;
        unit->breaks_before = pending_breaks;
        pending_breaks = 0;
        unit->type = state->vertical_profile ?
            (VerticalUnitType) state->vertical_profile : classify(root->symbol);
        unit->sideways = sideways_cluster(root->symbol, unit->type);
        if (myanmar_char(root->symbol)) {
            int end = i + 1;
            while (end < length && text->glyphs[end].symbol != '\n' &&
                   (text->glyphs[end].skip ||
                    (myanmar_char(text->glyphs[end].symbol) &&
                     !ass_myanmar_layout_break(
                         (const uint32_t *) text->event_text, length, end))))
                end++;
            unit->end = end;
        }
        unit->break_after = unit->type == VERTICAL_UNIT_CJK ||
            unit->type == VERTICAL_UNIT_MYANMAR ||
            root->symbol == ' ' || root->symbol == 0xa0;
#ifdef CONFIG_UNIBREAK
        if (unibrks && unibrks[unit->end - 1] == LINEBREAK_ALLOWBREAK)
            unit->break_after = true;
#endif
        measure_unit(unit, text->glyphs, spacing);
        i = unit->end;
    }
    for (int i = 0; i + 1 < count; i++)
        if (!units[i + 1].breaks_before)
            units[i].spacing = fmax(units[i].spacing,
                                    units[i + 1].spacing);
    max_height = fmax(max_height, 1.0);
    int column = 0;
    double block_height = 0.0;
    for (int start = 0; start < count;) {
        if (units[start].breaks_before)
            column += units[start].breaks_before;
        int end = start;
        int legal = -1;
        double height = 0.0;
        while (end < count && (end == start || !units[end].breaks_before)) {
            double next = height +
                (end > start ? units[end - 1].spacing : 0.0) +
                units[end].advance;
            if (end > start && state->wrap_style != 2 &&
                    next > max_height)
                break;
            height = next;
            end++;
            if (units[end - 1].break_after)
                legal = end;
        }
        if (end < count && !units[end].breaks_before &&
                state->wrap_style != 2 &&
                height + units[end - 1].spacing +
                    units[end].advance > max_height &&
                legal > start)
            end = legal;
        double top = 0.0;
        cols[column].alignment = ass_line_alignment(state, units[start].first);
        for (int j = start; j < end; j++) {
            if (j > start)
                top += units[j - 1].spacing;
            units[j].column = column;
            units[j].top = top;
            top += units[j].advance;
            cols[column].width = fmax(cols[column].width,
                                      units[j].width);
        }
        cols[column].height = top;
        block_height = fmax(block_height, top);
        if (end == start) /* a malformed zero-length unit must not loop */
            break;
        start = end;
        if (start < count && !units[start].breaks_before)
            column++;
    }
    column += pending_breaks;
    int columns = column + 1;
    double fallback = count ? units[0].em : 1.0;
    double gap = state->vertical_column_spacing * state->object_scale *
                 state->screen_scale_x;
    if (!isfinite(gap))
        gap = 0.0;
    gap = fmax(-1000000.0, fmin(gap, 1000000.0));
    for (int c = 0; c < columns; c++)
        cols[c].width = fmax(cols[c].width, fallback * 0.5);
    int direction = state->vertical_direction == 1 ? -1 : 1;
    gap = fmax(0.0, gap);
    cols[0].left = -cols[0].width * 0.5;
    for (int c = 1; c < columns; c++)
        cols[c].left = direction > 0 ?
            cols[c - 1].left + cols[c - 1].width + gap :
            cols[c - 1].left - cols[c].width - gap;
    ASS_DRect bbox = {DBL_MAX, DBL_MAX, -DBL_MAX, -DBL_MAX};
    for (int c = 0; c < columns; c++) {
        bbox.x_min = fmin(bbox.x_min, cols[c].left);
        bbox.x_max = fmax(bbox.x_max,
                          cols[c].left + cols[c].width);
    }
    bbox.y_min = 0.0;
    bbox.y_max = block_height;
    for (int i = 0; i < count; i++) {
        VerticalUnit *unit = &units[i];
        VerticalColumn *col = &cols[unit->column];
        int halign = col->alignment & 3;
        int valign = col->alignment & 12;
        double spare_y = block_height - col->height;
        unit->top += valign == VALIGN_CENTER ? spare_y * 0.5 :
                     valign == VALIGN_SUB ? spare_y : 0.0;
        double spare_x = col->width - unit->width;
        double cell_left = col->left +
            (halign == HALIGN_CENTER ? spare_x * 0.5 :
             halign == HALIGN_RIGHT ? spare_x : 0.0);
        place_unit(unit, text->glyphs, cell_left);
    }
    text->vertical_bbox = bbox;
    text->n_lines = 1;
    text->height = block_height;
    text->lines[0].asc = 0.0;
    text->lines[0].desc = block_height;
    text->lines[0].len = length;
    text->lines[0].offset = 0;
    free(storage);
    return true;
}
