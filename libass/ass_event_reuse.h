/* SPDX-License-Identifier: ISC
 * Frame-local reuse of ordinary text geometry. Included by ass_render.c.
 */

/* Copy normalized geometry ranges without the large inline paint palettes.
 * GlyphInfo is zero-initialized by parsing, including these ranges' padding.
 */
typedef struct {
    unsigned char shaping[offsetof(GlyphInfo, c)];
    unsigned char geometry[offsetof(GlyphInfo, distort_enabled) -
                           offsetof(GlyphInfo, line)];
    unsigned italic, bold;
    int flags;
    BorderLayerState borders[ASS_BORDER_LAYERS_MAX];
    uint8_t alpha[2];
} RepeatedGlyphKey;

typedef struct repeated_event_geometry {
    double context[40];
    RepeatedGlyphKey *input;
    int length;
    CombinedBitmapInfo *runs;
    unsigned count;
    EventImages bounds;
    size_t bytes;
    bool ready;
} RepeatedEventGeometry;

void ass_clear_repeated_geometry(ASS_Renderer *priv)
{
    RepeatedEventGeometry *memo = priv->repeated_geometry;
    if (!memo)
        return;
    for (unsigned i = 0; i < memo->count; i++)
        ass_cache_dec_ref(memo->runs[i].image);
    free(memo->runs);
    free(memo->input);
    free(memo);
    priv->repeated_geometry = NULL;
}

/* Syntax only limits admission; identity comes from parsed glyph/state data.
 * A single leading paint state avoids changing shaping/run boundaries when
 * paint is replaced. Unknown and extended tags deliberately miss this path.
 */
static bool repeated_override_block(const char *p, const char *end,
                                    bool clip_only)
{
    static const char *const tags[] = {
        "iclip", "clip", "alpha", "1c", "2c", "3c", "4c",
        "1a", "2a", "3a", "4a", "c", "pos", "org", "an", "q",
        "fn", "fs", "b", "i", "fsp", "fscx", "fscy",
        "frx", "fry", "frz", "fr", "fax", "fay",
        "xbord", "ybord", "bord", "xshad", "yshad", "shad",
        "blur", "be",
    };
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p == end) break;
        if (*p++ != '\\') return false;
        if (!clip_only && p + 1 < end && p[0] == 't' && p[1] == '(') {
            const char *begin = p + 2, *close = begin;
            int depth = 1;
            while (close < end && depth) {
                if (*close == '(') depth++;
                if (*close == ')') depth--;
                if (depth) close++;
            }
            if (depth) return false;
            const char *nested = begin;
            while (nested < close && *nested != '\\') {
                if (!strchr("0123456789,+-. \t", *nested)) return false;
                nested++;
            }
            if (nested == close ||
                !repeated_override_block(nested, close, true)) return false;
            p = close + 1;
            continue;
        }
        const char *tag = NULL;
        size_t n = 0;
        for (unsigned i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
            size_t len = strlen(tags[i]);
            if ((size_t) (end - p) < len || strncmp(p, tags[i], len)) continue;
            bool family = !strcmp(tags[i], "fn");
            char next = p + len < end ? p[len] : 0;
            if (!family && ((next >= 'a' && next <= 'z') ||
                            (next >= 'A' && next <= 'Z'))) continue;
            tag = tags[i]; n = len; break;
        }
        if (!tag) return false;
        bool clip = !strcmp(tag, "clip") || !strcmp(tag, "iclip");
        if (clip_only && !clip) return false;
        p += n;
        const char *arg = p;
        while (p < end && *p != '\\') p++;
        if (clip) {
            int commas = 0;
            if (arg == p || *arg != '(') return false;
            for (const char *s = arg; s < p; s++) {
                if (!strchr("0123456789,+-.() \t", *s)) return false;
                commas += *s == ',';
            }
            if (commas != 3) return false;
        } else if (strcmp(tag, "fn")) {
            /* Reject nested operands and malformed tag suffixes. */
            for (const char *s = arg; s < p; s++)
                if (!strchr("0123456789,+-.() &HhABCDEFabcdef\t", *s))
                    return false;
        }
    }
    return true;
}

static bool repeated_event_eligible(RenderContext *state)
{
    ASS_Event *event = state->event;
    if (state->renderer->debug_disable_event_reuse ||
        (event->Name && *event->Name) || (event->Effect && *event->Effect) ||
        state->renderer->track->colorcode.n_actors ||
        state->renderer->track->colorcode.has_applied_styles ||
        state->border_style != 1 || state->flags || state->native_vertical ||
        state->chat_enabled || state->n_scroll_contexts || state->column_event ||
        state->text_info.n_furi_groups || state->clip_drawing_text.str ||
        state->distort_enabled || state->curved_path_outline ||
        state->motion.relative_x || state->motion.relative_y ||
        state->n_pos_transforms)
        return false;
    const char *p = event->Text;
    while (*p == '{') {
        const char *end = strchr(++p, '}');
        if (!end || !repeated_override_block(p, end, false)) return false;
        p = end + 1;
    }
    if (strchr(p, '{') || strchr(p, '}')) return false;
    const GlyphInfo *first = state->text_info.glyphs;
    for (int i = 0; i < state->text_info.length; i++) {
        const GlyphInfo *g = first + i;
        if (g->drawing_text.str || g->font->desc.vertical ||
            g->has_rnd || g->has_jitter ||
            g->effect_type != EF_NONE || g->pattern.has_cycle ||
            memcmp(g->c, first->c, sizeof(g->c))) return false;
    }
    return true;
}

static void repeated_context_key(RenderContext *s, double key[40])
{
    /* Renderer/track/font-selection configuration is constant within a frame.
     * Include every event-wide layout input affected by admitted overrides.
     * All remaining admitted geometry (including font choice) is in GlyphInfo.
     */
    double values[40] = {
        s->event->Style, s->event->MarginL, s->event->MarginR, s->event->MarginV,
        s->alignment, s->text_alignment, s->justify, s->wrap_style,
        s->font_encoding, s->explicit, s->evt_type, s->detect_collisions,
        s->pos_x, s->pos_y, s->org_x, s->org_y, s->have_origin,
        s->screen_scale_x, s->screen_scale_y,
        s->border_scale_x, s->border_scale_y, s->blur_scale_x, s->blur_scale_y,
        s->font_size, s->scale_x, s->scale_y, s->hspacing,
        s->frx, s->fry, s->frz, s->fax, s->fay,
        s->border_x, s->border_y, s->shadow_x, s->shadow_y,
        s->blur_x, s->blur_y, s->be, s->overrides,
    };
    memcpy(key, values, sizeof(values));
}

static void repeated_glyph_key(RepeatedGlyphKey *dst, const GlyphInfo *src)
{
    memset(dst, 0, sizeof(*dst));
    memcpy(dst->shaping, src, sizeof(dst->shaping));
    memcpy(dst->geometry, &src->line, sizeof(dst->geometry));
    dst->italic = src->italic;
    dst->bold = src->bold;
    dst->flags = src->flags;
    memcpy(dst->borders, src->border_layers, sizeof(dst->borders));
    /* Only these alpha classes affect FILTER_FILL_IN_BORDER/SHADOW for the
     * admitted ordinary text path. Other layer alphas only affect painting.
     */
    for (int j = 0; j < 2; j++) {
        unsigned a = _a(src->c[j]);
        dst->alpha[j] = a == 0 ? 0 : a == 255 ? 255 : 1;
    }
    for (int j = 0; j < ASS_BORDER_LAYERS_MAX; j++) {
        dst->borders[j].color = 0;
        memset(&dst->borders[j].gradient, 0, sizeof(dst->borders[j].gradient));
    }
}

static bool repeated_geometry_lookup(RenderContext *state)
{
    ASS_Renderer *priv = state->renderer;
    if (!repeated_event_eligible(state)) {
        ass_clear_repeated_geometry(priv);
        return false;
    }
    double key[40];
    repeated_context_key(state, key);
    RepeatedEventGeometry *memo = priv->repeated_geometry;
    int length = state->text_info.length;
    if (memo && memo->ready && memo->length == length &&
        !memcmp(memo->context, key, sizeof(key))) {
        bool equal = true;
        for (int i = 0; i < length && equal; i++) {
            RepeatedGlyphKey glyph;
            repeated_glyph_key(&glyph, state->text_info.glyphs + i);
            equal = !memcmp(&glyph, memo->input + i, sizeof(glyph));
        }
        if (equal) {
            priv->repeated_event_stats.reuse_hits++;
            return true;
        }
    }
    ass_clear_repeated_geometry(priv);
    size_t limit = FFMIN((size_t) (2 * MEGABYTE),
                         priv->cache.composite_max_size / 16);
    if (limit <= sizeof(*memo) ||
        (size_t) length > (limit - sizeof(*memo)) / sizeof(RepeatedGlyphKey))
        return false;
    memo = calloc(1, sizeof(*memo));
    if (!memo) return false;
    memo->input = malloc((size_t) length * sizeof(RepeatedGlyphKey));
    if (!memo->input) { free(memo); return false; }
    memo->length = length;
    memo->bytes = sizeof(*memo) + (size_t) length * sizeof(RepeatedGlyphKey);
    memcpy(memo->context, key, sizeof(key));
    for (int i = 0; i < length; i++)
        repeated_glyph_key(memo->input + i, state->text_info.glyphs + i);
    priv->repeated_geometry = memo;
    return false;
}

static void repeated_geometry_store(RenderContext *state, EventImages *bounds)
{
    ASS_Renderer *priv = state->renderer;
    RepeatedEventGeometry *memo = priv->repeated_geometry;
    if (!memo || memo->ready) return;
    TextInfo *text = &state->text_info;
    size_t limit = FFMIN((size_t) (2 * MEGABYTE),
                         priv->cache.composite_max_size / 16);
    if (!text->n_bitmaps || memo->bytes > limit ||
        text->n_bitmaps > (limit - memo->bytes) / sizeof(CombinedBitmapInfo)) {
        ass_clear_repeated_geometry(priv); return;
    }
    for (unsigned i = 0; i < text->n_bitmaps; i++)
        if (!text->combined_bitmaps[i].image) {
            ass_clear_repeated_geometry(priv); return;
        }
    memo->runs = malloc(text->n_bitmaps * sizeof(*memo->runs));
    if (!memo->runs) { ass_clear_repeated_geometry(priv); return; }
    memo->count = text->n_bitmaps;
    memcpy(memo->runs, text->combined_bitmaps, memo->count * sizeof(*memo->runs));
    for (unsigned i = 0; i < memo->count; i++) {
        CombinedBitmapInfo *run = memo->runs + i;
        /* Composite cache owns its component array; retain only the value. */
        run->bitmaps = NULL;
        run->bitmap_count = run->max_bitmap_count = 0;
        ass_cache_inc_ref(run->image);
    }
    memo->bounds = *bounds;
    memo->bounds.imgs = NULL;
    memo->bounds.imgs_rgba = NULL;
    memo->bounds.event = NULL;
    memo->bytes += memo->count * sizeof(*memo->runs);
    memo->ready = true;
    priv->repeated_event_stats.memo_bytes = memo->bytes;
}

static void repeated_geometry_render(RenderContext *state, EventImages *images,
                                     ASS_ImageRGBA **rgba_out)
{
    RepeatedEventGeometry *memo = state->renderer->repeated_geometry;
    GlyphInfo *paint = state->text_info.glyphs;
    for (unsigned i = 0; i < memo->count; i++) {
        CombinedBitmapInfo *run = memo->runs + i;
        memcpy(run->c, paint->c, sizeof(run->c));
        memcpy(run->base_c, paint->c, sizeof(run->base_c));
        memcpy(run->border_layers, paint->border_layers, sizeof(run->border_layers));
    }
    *images = memo->bounds;
    images->event = state->event;
    CombinedBitmapInfo *saved = state->text_info.combined_bitmaps;
    unsigned count = state->text_info.n_bitmaps;
    state->text_info.combined_bitmaps = memo->runs;
    state->text_info.n_bitmaps = memo->count;
    images->imgs = render_text(state, rgba_out ? &images->imgs_rgba : NULL);
    if (rgba_out) *rgba_out = images->imgs_rgba;
    state->text_info.combined_bitmaps = saved;
    state->text_info.n_bitmaps = count;
}
