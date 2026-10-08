/*
 * Copyright (C) 2006 Evgeniy Stepanov <eugeni.stepanov@gmail.com>
 *
 * This file is part of libass.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"
#include "ass_compat.h"

#include <assert.h>
#include <math.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>
#include <float.h>
#include <stddef.h>

#ifdef CONFIG_UNIBREAK
#include <linebreak.h>
#endif

#include "ass.h"
#include "ass_outline.h"
#include "ass_render.h"
#include "ass_parse.h"
#include "ass_priv.h"
#include "ass_distort.h"
#include "ass_rnd.h"
#include "ass_shaper.h"
#include "ass_chat.h"
#include "ass_vertical.h"

size_t ass_bitmap_construct(void *key, void *value, void *priv);
size_t ass_composite_construct(void *key, void *value, void *priv);
static bool build_rnd_bitmaps(RenderContext *state, GlyphInfo *info,
                              OutlineHashValue *outline_src,
                              const double m[3][3], const double z_basis[3],
                              ASS_Vector *pos, ASS_Vector *pos_o,
                              bool need_border);
static inline bool border_layer_has_size(const BorderLayerState *layer);
static bool border_layers_state_equal(const BorderLayerState *a,
                                      const BorderLayerState *b);
static bool has_multi_border_layers(const BorderLayerState *layers);
static bool layer1_filter_differs(const GlyphInfo *info);
static void sync_glyph_layer1_border(GlyphInfo *info);
static void capture_column_style(RenderContext *state, ColumnStyleState *style,
                                 unsigned fields);
static double glyph_border_max_x(const GlyphInfo *info);
static double glyph_border_max_y(const GlyphInfo *info);
static Bitmap *combined_border_bitmap(CombinedBitmapInfo *info, int layer);
static Bitmap *composite_border_bitmap(CompositeHashValue *value, int layer);

static void free_scroll_contexts(RenderContext *state)
{
    for (int i = 0; i < state->n_scroll_contexts; i++) {
        state->scroll_contexts[i].definition->users--;
        free(state->scroll_contexts[i].advances);
    }
    free(state->scroll_contexts);
    state->scroll_contexts = NULL;
    state->n_scroll_contexts = state->max_scroll_contexts = 0;
    state->scroll_id = 0;
    state->scroll_clip_active = false;
}

static void select_scroll_clip(RenderContext *state, int id)
{
    state->scroll_clip_active = false;
    if (id <= 0 || id > state->n_scroll_contexts)
        return;
    ASS_ScrollContext *ctx = &state->scroll_contexts[id - 1];
    if (!ctx->show_lines || !ctx->rows)
        return;
    state->scroll_clip_active = true;
    /* The viewport stays at normal layout coordinates for either direction:
     * positive displacement exits its top, negative exits its bottom. Clamp
     * before conversion; motion retains fractional rasterizer precision. */
    state->scroll_clip_y0 = (int) floor(FFMINMAX(ctx->top, 0, state->renderer->height));
    state->scroll_clip_y1 = (int) ceil(FFMINMAX(ctx->bottom, 0, state->renderer->height));
}

bool ass_scroll_start(RenderContext *state, const char *start, const char *end)
{
    ASS_Renderer *priv = state->renderer;
    ASS_ScrollDefinition *def;
    size_t length = end - start;
    for (def = priv->scroll_cache; def; def = def->next)
        if (def->source_len == length &&
                def->default_duration == state->scroll_duration &&
                !memcmp(def->source, start, length))
            break;
    if (!def) {
        def = ass_scroll_parse(start, end, state->scroll_duration);
        if (!def)
            return false;
        /* Retain at least the current definition even if it is very large.
         * Evict only unused entries, so arbitrary many contexts stay valid. */
        size_t bytes = length + def->count * sizeof(*def->cues);
        ASS_ScrollDefinition **link = &priv->scroll_cache;
        while (*link) {
            ASS_ScrollDefinition *old = *link;
            bytes += old->source_len + old->count * sizeof(*old->cues) +
                     old->rows * sizeof(*old->advances);
            if (bytes > MEGABYTE && !old->users) {
                *link = old->next;
                ass_scroll_free(old);
            } else {
                link = &old->next;
            }
        }
        def->next = priv->scroll_cache;
        priv->scroll_cache = def;
    }
    if (state->n_scroll_contexts == state->max_scroll_contexts) {
        int capacity = state->max_scroll_contexts;
        if (capacity > INT_MAX / 2)
            return false;
        capacity = capacity ? capacity * 2 : 4;
        if (!ASS_REALLOC_ARRAY(state->scroll_contexts, capacity))
            return false;
        state->max_scroll_contexts = capacity;
    }
    state->scroll_contexts[state->n_scroll_contexts++] = (ASS_ScrollContext) {
        .definition = def,
        .last_line = -1,
        .show_lines = state->scroll_show_lines,
    };
    def->users++;
    state->scroll_id = state->n_scroll_contexts;
    return true;
}

#define BS4_ROUNDED_BOX_SCALE 4096
static Bitmap *bitmap_ref_border_bitmap(BitmapRef *ref, int layer);
static ASS_Vector bitmap_ref_border_pos(BitmapRef *ref, int layer);

#define MAX_GLYPHS_INITIAL 1024
#define MAX_LINES_INITIAL 64
#define MAX_BITMAPS_INITIAL 16
#define MAX_SUB_BITMAPS_INITIAL 64
#define SUBPIXEL_MASK 63
#define STROKER_PRECISION 16     // stroker error in integer units, unrelated to final accuracy
#define RASTERIZER_PRECISION 16  // rasterizer spline approximation error in 1/64 pixel units
#define POSITION_PRECISION 8.0   // rough estimate of transform error in 1/64 pixel units
#define MAX_PERSP_SCALE 16.0
#define SUBPIXEL_ORDER 3  // ~ log2(64 / POSITION_PRECISION)
#define BLUR_PRECISION (1.0 / 256)  // blur error as fraction of full input range
#define NBSP 0xa0   // unicode non-breaking space character
#define FURI_AUTO_GAP_FACTOR 0.04

typedef struct {
    int start, end, name_end;
    ASS_ChatReceipt receipt;
    bool has_preset;
    ChatBubbleStyle empty_bubble;
    uint32_t receipt_color;
    ASS_DRect bounds;
    double width, height;
    double stack_top;
} ChatRange;

typedef struct {
    double panel_width, panel_pad, bubble_pad, message_gap;
    double bubble_gap, header_pad, radius, max_text_width;
    double viewport_max, viewport_min;
} ChatMetrics;

static ChatMetrics chat_choose_metrics(RenderContext *state,
                                       const ASS_ChatScene *chat,
                                       const ChatRange *ranges,
                                       const ChatRange *title);
static bool render_chat_scene(RenderContext *state, ASS_Event *event,
                              EventImages *event_images,
                              ASS_ImageRGBA **rgba_out,
                              const ASS_ChatScene *chat, ChatRange *ranges,
                              ChatRange *title, const ChatMetrics *metrics);

void ass_free_glyph_render_resources(GlyphInfo *info)
{
    if (!info)
        return;
    if (info->has_distort_bitmap) {
        ass_free_bitmap(&info->distort_bitmap);
        ass_free_bitmap(&info->distort_bitmap_o);
        for (int i = 0; i < ASS_BORDER_LAYERS_MAX - 1; i++)
            ass_free_bitmap(&info->distort_bitmap_border[i]);
        info->bm = NULL;
        info->bm_o = NULL;
        for (int i = 0; i < ASS_BORDER_LAYERS_MAX - 1; i++)
            info->bm_border[i] = NULL;
        info->has_distort_bitmap = false;
    }
    if (info->has_distort_outline && info->distorted_outline) {
        ass_outline_free(&info->distorted_outline->outline[0]);
        ass_outline_free(&info->distorted_outline->outline[1]);
        free(info->distorted_outline);
    }
    info->distorted_outline = NULL;
    info->has_distort_outline = false;
}

static void free_glyph_list_chains(GlyphInfo *glyphs, int length)
{
    for (int i = 0; i < length; i++) {
        GlyphInfo *info = glyphs[i].next;
        glyphs[i].next = NULL;
        while (info) {
            GlyphInfo *next = info->next;
            ass_free_glyph_render_resources(info);
            free(info);
            info = next;
        }
    }
}

static void free_furi_groups(TextInfo *text_info)
{
    for (int i = 0; i < text_info->n_furi_groups; i++) {
        FuriGroup *group = &text_info->furi_groups[i];
        free_glyph_list_chains(group->glyphs, group->length);
        for (int j = 0; j < group->length; j++) {
            GlyphInfo *info = &group->glyphs[j];
            ass_free_glyph_render_resources(info);
        }
        free(group->glyphs);
        free(group->event_text);
        free(group->karaoke_regions);
    }
    free(text_info->furi_groups);
    text_info->furi_groups = NULL;
    text_info->n_furi_groups = 0;
    text_info->max_furi_groups = 0;
}

static void free_column_layout(TextInfo *text_info)
{
    free(text_info->column_glyphs);
    free(text_info->column_defaults);
    free(text_info->column_widths);
    free(text_info->column_spacing);
    free(text_info->column_align);
    text_info->column_glyphs = NULL;
    text_info->column_defaults = NULL;
    text_info->column_widths = NULL;
    text_info->column_spacing = NULL;
    text_info->column_align = NULL;
    text_info->max_column_glyphs = 0;
    text_info->max_columns = 0;
    text_info->column_rows = 0;
    text_info->column_count = 0;
}

static bool text_info_init(TextInfo* text_info)
{
    text_info->max_bitmaps = MAX_BITMAPS_INITIAL;
    text_info->max_glyphs = MAX_GLYPHS_INITIAL;
    text_info->max_lines = MAX_LINES_INITIAL;
    text_info->n_bitmaps = 0;
    text_info->combined_bitmaps = calloc(MAX_BITMAPS_INITIAL, sizeof(CombinedBitmapInfo));
    text_info->glyphs = calloc(MAX_GLYPHS_INITIAL, sizeof(GlyphInfo));
    text_info->event_text = calloc(MAX_GLYPHS_INITIAL, sizeof(FriBidiChar));
    text_info->breaks = malloc(MAX_GLYPHS_INITIAL);
    text_info->lines = calloc(MAX_LINES_INITIAL, sizeof(LineInfo));

    if (!text_info->combined_bitmaps || !text_info->glyphs || !text_info->lines ||
        !text_info->breaks || !text_info->event_text)
        return false;

    return true;
}

static void text_info_done(TextInfo* text_info)
{
    free_furi_groups(text_info);
    free_column_layout(text_info);
    free(text_info->karaoke_segments);
    free(text_info->glyphs);
    free(text_info->event_text);
    free(text_info->breaks);
    free(text_info->lines);
    free(text_info->combined_bitmaps);
}

static bool render_context_init(RenderContext *state, ASS_Renderer *priv)
{
    state->renderer = priv;

    if (!text_info_init(&state->text_info))
        return false;

    if (!(state->shaper = ass_shaper_new(priv->cache.metrics_cache, priv->cache.face_size_metrics_cache)))
        return false;

    if (!(state->furi_shaper = ass_shaper_new(priv->cache.metrics_cache, priv->cache.face_size_metrics_cache)))
        return false;

    return ass_rasterizer_init(&priv->engine, &state->rasterizer, RASTERIZER_PRECISION);
}

static void render_context_done(RenderContext *state)
{
    free_scroll_contexts(state);
    ass_rasterizer_done(&state->rasterizer);

    if (state->shaper)
        ass_shaper_free(state->shaper);
    if (state->furi_shaper)
        ass_shaper_free(state->furi_shaper);

    text_info_done(&state->text_info);
    ass_free_override_buffers(state);
}

ASS_Renderer *ass_renderer_init(ASS_Library *library)
{
    int error;
    FT_Library ft;
    ASS_Renderer *priv = 0;
    int vmajor, vminor, vpatch;

    ass_msg(library, MSGL_INFO, "libass API version: 0x%X", LIBASS_VERSION);
    ass_msg(library, MSGL_INFO, "libass source: %s", CONFIG_SOURCEVERSION);

    error = FT_Init_FreeType(&ft);
    if (error) {
        ass_msg(library, MSGL_FATAL, "%s failed", "FT_Init_FreeType");
        goto fail;
    }

    FT_Library_Version(ft, &vmajor, &vminor, &vpatch);
    ass_msg(library, MSGL_V, "Raster: FreeType %d.%d.%d",
           vmajor, vminor, vpatch);

    priv = calloc(1, sizeof(ASS_Renderer));
    if (!priv) {
        FT_Done_FreeType(ft);
        goto fail;
    }

    priv->library = library;
    priv->ftlibrary = ft;
    // images_root and related stuff is zero-filled in calloc

    unsigned flags = ASS_CPU_FLAG_ALL;
#if CONFIG_LARGE_TILES
    flags |= ASS_FLAG_LARGE_TILES;
#endif
    priv->engine = ass_bitmap_engine_init(flags);

    priv->cache.font_cache = ass_font_cache_create();
    priv->cache.bitmap_cache = ass_bitmap_cache_create();
    priv->cache.composite_cache = ass_composite_cache_create();
    priv->cache.outline_cache = ass_outline_cache_create();
    priv->cache.face_size_metrics_cache = ass_face_size_metrics_cache_create();
    priv->cache.metrics_cache = ass_glyph_metrics_cache_create();
    if (!priv->cache.font_cache || !priv->cache.bitmap_cache ||
        !priv->cache.composite_cache || !priv->cache.outline_cache ||
        !priv->cache.face_size_metrics_cache || !priv->cache.metrics_cache)
        goto fail;

    priv->cache.glyph_max = GLYPH_CACHE_MAX;
    priv->cache.bitmap_max_size = BITMAP_CACHE_MAX_SIZE;
    priv->cache.composite_max_size = COMPOSITE_CACHE_MAX_SIZE;
    priv->rgba_output_max_size = RGBA_OUTPUT_MAX_SIZE;

    if (!render_context_init(&priv->state, priv))
        goto fail;

    priv->user_override_style.Name = "OverrideStyle"; // name insignificant

    priv->settings.font_size_coeff = 1.;
    priv->settings.selective_style_overrides = ASS_OVERRIDE_BIT_SELECTIVE_FONT_SCALE;

    ass_shaper_info(library);
    priv->settings.shaper = ASS_SHAPING_COMPLEX;

    ass_msg(library, MSGL_V, "Initialized");

    return priv;

fail:
    ass_msg(library, MSGL_ERR, "Initialization failed");
    ass_renderer_done(priv);

    return NULL;
}

void ass_renderer_done(ASS_Renderer *render_priv)
{
    if (!render_priv)
        return;

    ass_frame_unref(render_priv->images_root);
    ass_frame_unref(render_priv->prev_images_root);

    ass_clear_repeated_geometry(render_priv);
    ass_cache_done(render_priv->cache.composite_cache);
    ass_cache_done(render_priv->cache.bitmap_cache);
    ass_cache_done(render_priv->cache.outline_cache);
    ass_cache_done(render_priv->cache.face_size_metrics_cache);
    ass_cache_done(render_priv->cache.metrics_cache);
    ass_cache_done(render_priv->cache.font_cache);

    if (render_priv->fontselect)
        ass_fontselect_free(render_priv->fontselect);
    if (render_priv->ftlibrary)
        FT_Done_FreeType(render_priv->ftlibrary);
    free(render_priv->eimg);
    ass_clear_tag_images_internal(render_priv);

    render_context_done(&render_priv->state);

    for (int i = 0; i < 8; i++) {
        free(render_priv->chat_cache[i].source);
        ass_chat_free(render_priv->chat_cache[i].scene);
    }
    while (render_priv->scroll_cache) {
        ASS_ScrollDefinition *next = render_priv->scroll_cache->next;
        ass_scroll_free(render_priv->scroll_cache);
        render_priv->scroll_cache = next;
    }

    free(render_priv->settings.default_font);
    free(render_priv->settings.default_family);

    free(render_priv->user_override_style.FontName);

    free(render_priv);
}

/**
 * \brief Create a new ASS_Image
 * Parameters are the same as ASS_Image fields.
 */
static ASS_Image *my_draw_bitmap(ASS_Renderer *render_priv,
                                 unsigned char *bitmap, int bitmap_w,
                                 int bitmap_h, int stride, int dst_x,
                                 int dst_y, uint32_t color,
                                 CompositeHashValue *source)
{
    ASS_ImagePriv *img = NULL;
    if (!source && render_priv->debug_fail_next_owned_image_allocation)
        render_priv->debug_fail_next_owned_image_allocation = false;
    else
        img = malloc(sizeof(ASS_ImagePriv));
    /* The caller retains the bitmap on failure. This makes allocation
     * failure a single-owner path instead of freeing here and again in
     * render_glyph()/render_glyph_i(). */
    if (!img)
        return NULL;

    render_priv->repeated_event_stats.images++;
    img->result.w = bitmap_w;
    img->result.h = bitmap_h;
    img->result.stride = stride;
    img->result.bitmap = bitmap;
    img->result.color = color;
    img->result.dst_x = dst_x;
    img->result.dst_y = dst_y;

    img->source = source;
    ass_cache_inc_ref(source);
    img->buffer = source ? NULL : bitmap;
    img->ref_count = 0;
    if (!source)
        ass_aligned_retag(bitmap, ASS_ALIGNED_ALLOC_LEGACY_IMAGE, img,
                          "legacy image ownership transfer");

    return &img->result;
}

static uint32_t finalize_legacy_color(const CombinedBitmapInfo *combined,
                                      uint32_t color)
{
    if (combined)
        ass_apply_fade_color(&color, combined->fade_color);
    return color;
}

/**
 * \brief Mapping between script and screen coordinates
 */
static double x2scr_pos(ASS_Renderer *render_priv, double x)
{
    return x * render_priv->frame_content_width / render_priv->par_scale_x / render_priv->track->PlayResX +
        render_priv->settings.left_margin;
}
static double x2scr_left(RenderContext *state, double x)
{
    ASS_Renderer *render_priv = state->renderer;
    if (state->explicit || !render_priv->settings.use_margins)
        return x2scr_pos(render_priv, x);
    return x * render_priv->fit_width / render_priv->par_scale_x /
        render_priv->track->PlayResX;
}
static double x2scr_right(RenderContext *state, double x)
{
    ASS_Renderer *render_priv = state->renderer;
    if (state->explicit || !render_priv->settings.use_margins)
        return x2scr_pos(render_priv, x);
    return x * render_priv->fit_width / render_priv->par_scale_x /
        render_priv->track->PlayResX +
        (render_priv->width - render_priv->fit_width);
}
static double x2scr_pos_scaled(ASS_Renderer *render_priv, double x)
{
    return x * render_priv->frame_content_width / render_priv->track->PlayResX +
        render_priv->settings.left_margin;
}
/**
 * \brief Mapping between script and screen coordinates
 */
static double y2scr_pos(ASS_Renderer *render_priv, double y)
{
    return y * render_priv->frame_content_height / render_priv->track->PlayResY +
        render_priv->settings.top_margin;
}
static double y2scr(RenderContext *state, double y)
{
    ASS_Renderer *render_priv = state->renderer;
    if (state->explicit || !render_priv->settings.use_margins)
        return y2scr_pos(render_priv, y);
    return y * render_priv->fit_height /
        render_priv->track->PlayResY +
        (render_priv->height - render_priv->fit_height) * 0.5;
}

// the same for toptitles
static double y2scr_top(RenderContext *state, double y)
{
    ASS_Renderer *render_priv = state->renderer;
    if (state->explicit || !render_priv->settings.use_margins)
        return y2scr_pos(render_priv, y);
    return y * render_priv->fit_height /
        render_priv->track->PlayResY;
}
// the same for subtitles
static double y2scr_sub(RenderContext *state, double y)
{
    ASS_Renderer *render_priv = state->renderer;
    if (state->explicit || !render_priv->settings.use_margins)
        return y2scr_pos(render_priv, y);
    return y * render_priv->fit_height /
        render_priv->track->PlayResY +
        (render_priv->height - render_priv->fit_height);
}

static double x2scr_offset(RenderContext *state, double x)
{
    ASS_Renderer *render_priv = state->renderer;
    if (state->explicit || !render_priv->settings.use_margins)
        return x * render_priv->frame_content_width /
            render_priv->par_scale_x / render_priv->track->PlayResX;
    return x * render_priv->fit_width /
        render_priv->par_scale_x / render_priv->track->PlayResX;
}

static double y2scr_offset(RenderContext *state, double y)
{
    ASS_Renderer *render_priv = state->renderer;
    if (state->explicit || !render_priv->settings.use_margins)
        return y * render_priv->frame_content_height /
            render_priv->track->PlayResY;
    return y * render_priv->fit_height /
        render_priv->track->PlayResY;
}

static void append_rgba_tail(ASS_ImageRGBA ***tail, ASS_ImageRGBA *img)
{
    if (!tail || !*tail || !img)
        return;
    **tail = img;
    *tail = &img->next;
}

static inline void clear_image_fill_layer(ImageFillLayer *layer)
{
    layer->enabled = false;
    layer->path = (ASS_StringView) {NULL, 0};
    layer->xoffset = 0;
    layer->yoffset = 0;
}

static bool image_fill_state_equal(const ImageFillState *a,
                                   const ImageFillState *b)
{
    for (int i = 0; i < 4; i++) {
        const ImageFillLayer *la = &a->layer[i];
        const ImageFillLayer *lb = &b->layer[i];
        if (la->enabled != lb->enabled ||
            la->xoffset != lb->xoffset ||
            la->yoffset != lb->yoffset)
            return false;
        if (la->enabled && !ass_string_equal(la->path, lb->path))
            return false;
    }
    return true;
}

static bool secondary_outline_equal(const KaraokeOutlinePaint *a,
                                    const KaraokeOutlinePaint *b);

static inline int wrap_image_coord(int c, int size)
{
    int out = c % size;
    if (out < 0)
        out += size;
    return out;
}

static inline uint8_t vsf_cov64_from_mask(uint8_t cov)
{
    // VSFilter coverage is effectively 6-bit (0..64) in its mixer path.
    return (uint8_t) ((cov + 2) >> 2);
}

static inline const uint8_t *tag_image_pixel(const ASS_TagImageEntry *img, int tx, int ty)
{
    // VSFilter stores rows upside-down in this lookup path.
    int row = img->height - 1 - ty;
    return img->rgba + (size_t) row * img->stride + (size_t) tx * 4;
}

static inline void sample_tag_image(const ASS_TagImageEntry *img, int x, int y,
                                    int subpix_x, int subpix_y,
                                    uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a)
{
    if (img->width <= 0 || img->height <= 0) {
        *r = *g = *b = *a = 0;
        return;
    }
    int tx = wrap_image_coord(x, img->width);
    int ty = wrap_image_coord(y, img->height);

    const uint8_t *dst11 = tag_image_pixel(img, tx, ty);
    uint8_t rr = dst11[0], gg = dst11[1], bb = dst11[2], aa = dst11[3];
    if (!subpix_x && !subpix_y) {
        *r = rr;
        *g = gg;
        *b = bb;
        *a = aa;
        return;
    }

    // VSFilterMod compatibility: mode-2 texture sampling uses 1/8 subpixel
    // interpolation against left/up neighbors without wraparound.
    bool has_left = tx > 0;
    bool has_up = ty < img->height - 1;
    if (has_left && !has_up) {
        const uint8_t *dst12 = tag_image_pixel(img, tx - 1, ty);
        rr = (uint8_t) ((rr * (8 - subpix_x) + dst12[0] * subpix_x) >> 3);
        gg = (uint8_t) ((gg * (8 - subpix_x) + dst12[1] * subpix_x) >> 3);
        bb = (uint8_t) ((bb * (8 - subpix_x) + dst12[2] * subpix_x) >> 3);
        aa = (uint8_t) ((aa * (8 - subpix_x) + dst12[3] * subpix_x) >> 3);
    } else if (has_up && !has_left) {
        const uint8_t *dst21 = tag_image_pixel(img, tx, ty + 1);
        rr = (uint8_t) ((rr * subpix_y + dst21[0] * (8 - subpix_y)) >> 3);
        gg = (uint8_t) ((gg * subpix_y + dst21[1] * (8 - subpix_y)) >> 3);
        bb = (uint8_t) ((bb * subpix_y + dst21[2] * (8 - subpix_y)) >> 3);
        aa = (uint8_t) ((aa * subpix_y + dst21[3] * (8 - subpix_y)) >> 3);
    } else if (has_left && has_up) {
        const uint8_t *dst12 = tag_image_pixel(img, tx - 1, ty);
        const uint8_t *dst21 = tag_image_pixel(img, tx, ty + 1);
        const uint8_t *dst22 = tag_image_pixel(img, tx - 1, ty + 1);
        rr = (uint8_t) (((((dst21[0] * (8 - subpix_x) + dst22[0] * subpix_x) >> 3) * subpix_y) +
                         (((rr       * (8 - subpix_x) + dst12[0] * subpix_x) >> 3) * (8 - subpix_y))) >> 3);
        gg = (uint8_t) (((((dst21[1] * (8 - subpix_x) + dst22[1] * subpix_x) >> 3) * subpix_y) +
                         (((gg       * (8 - subpix_x) + dst12[1] * subpix_x) >> 3) * (8 - subpix_y))) >> 3);
        bb = (uint8_t) (((((dst21[2] * (8 - subpix_x) + dst22[2] * subpix_x) >> 3) * subpix_y) +
                         (((bb       * (8 - subpix_x) + dst12[2] * subpix_x) >> 3) * (8 - subpix_y))) >> 3);
        aa = (uint8_t) (((((dst21[3] * (8 - subpix_x) + dst22[3] * subpix_x) >> 3) * subpix_y) +
                         (((aa       * (8 - subpix_x) + dst12[3] * subpix_x) >> 3) * (8 - subpix_y))) >> 3);
    }

    *r = rr;
    *g = gg;
    *b = bb;
    *a = aa;
}

#define KARAOKE_SECONDARY_OUTLINE_LAYER 4

static bool render_layer_uses_image(const CombinedBitmapInfo *info, int layer)
{
    return layer >= 0 && layer < 4 && info->image_fill.layer[layer].enabled;
}

static bool skip_transparent_paint(RenderContext *state,
                                   const CombinedBitmapInfo *info,
                                   uint32_t color, int layer)
{
    if (state->renderer->debug_keep_transparent_images || _a(color) != 255)
        return false;
    /* Alpha gradients replace solid alpha. Leave extended paint paths alone;
     * legacy placeholders may also carry visible RGBA image/pattern output. */
    if (info && (layer < 0 || layer >= 4 ||
        info->gradient.layer[layer].color_enabled ||
        info->gradient.layer[layer].alpha_enabled ||
        info->mangetsu_gradient.layer[layer].active ||
        info->mangetsu_gradient.alpha[layer].active ||
        info->pattern.has_cycle || info->pattern.propagate_polka ||
        info->pattern.polka_face[0].has_color ||
        info->pattern.polka_face[1].has_color ||
        info->pattern.polka_face[2].has_color ||
        render_layer_uses_image(info, layer)))
        return false;
    state->renderer->repeated_event_stats.transparent_skips++;
    return true;
}

static uint32_t secondary_outline_color(const CombinedBitmapInfo *info)
{
    uint32_t color = (info->secondary_outline.color & 0xFFFFFF00u) |
                     _a(info->base_c[2]);
    ass_apply_fade(&color, info->fade);
    return color;
}

static inline bool polka_hit(int x, int y, int period, int row_period,
                             int radius)
{
    int py = y * 16 + 8;
    int row = py / row_period;
    int dy = py % row_period - row_period / 2;
    int px = x * 16 + 8 + (row & 1 ? period / 2 : 0);
    int dx = px % period - period / 2;
    return (int64_t) dx * dx + (int64_t) dy * dy <=
           (int64_t) radius * radius;
}

static ASS_ImageRGBA *render_bitmap_rgba(RenderContext *state,
                                         CombinedBitmapInfo *info,
                                         const uint8_t *mask, int w, int h,
                                         int stride, int dst_x, int dst_y,
                                         int src_x, int src_y,
                                         int full_w, int full_h,
                                         int subpix_x, int subpix_y,
                                         int layer, unsigned type)
{
    ASS_Renderer *render_priv = state->renderer;
    ASS_ImageRGBA *img =
        ass_rgba_image_alloc(render_priv, w, h, dst_x, dst_y, type,
                             ASS_RGBA_OWNER_EVENT, "rendered glyph tile");
    if (!img)
        return NULL;
    int rgba_stride = img->stride;
    uint8_t *rgba = img->rgba;
    uint8_t *blend_rgb = NULL;
    int blend_stride = 0;
    if (info->blend_mode > ASS_BLEND_NORMAL &&
            info->blend_mode <= ASS_BLEND_SUBSTRACT_INVERSE &&
            ass_rgba_image_alloc_blend_rgb(img, info->blend_mode)) {
        ASS_ImageRGBAPriv *rgba_priv = ass_rgba_image_private(
            img, "render blend source");
        blend_rgb = rgba_priv->blend_rgb;
        blend_stride = rgba_priv->blend_stride;
        memset(blend_rgb, 0, rgba_priv->blend_alloc_size);
    }

    if (full_w <= 0)
        full_w = w;
    if (full_h <= 0)
        full_h = h;
    subpix_x &= 7;
    subpix_y &= 7;
    int64_t denom_w = (full_w > 1) ? (int64_t) (full_w - 1) : 0;
    int64_t denom_h = (full_h > 1) ? (int64_t) (full_h - 1) : 0;
    int vis_h = full_h - src_y;
    if (vis_h < 0)
        vis_h = 0;
    if (vis_h > h)
        vis_h = h;
    int clip_diff = full_h - (src_y + vis_h);
    if (clip_diff < 0)
        clip_diff = 0;

    bool secondary_outline = layer == KARAOKE_SECONDARY_OUTLINE_LAYER;
    PolkaPaint polka = {0};
    if (!info->from_drawing && layer >= 0 && layer < 3) {
        polka = info->pattern.propagate_polka ?
            info->pattern.polka_face[0] : (PolkaPaint) {0};
        PolkaPaint explicit_paint = info->pattern.polka_face[layer];
        if (explicit_paint.has_color) {
            polka.color = explicit_paint.color;
            polka.has_color = true;
        }
        if (explicit_paint.has_size) {
            polka.size = explicit_paint.size;
            polka.has_size = true;
        }
        if (explicit_paint.has_spacing) {
            polka.spacing = explicit_paint.spacing;
            polka.has_spacing = true;
        }
    }
    double dot_size = polka.size * state->screen_scale_y;
    double dot_spacing = (polka.has_spacing ? polka.spacing :
                          2.5 * polka.size) * state->screen_scale_y;
    bool use_polka = polka.has_color && polka.has_size &&
        dot_size > 0 && dot_spacing > 0;
    int dot_period = use_polka ? FFMAX(1, (int) lround(dot_spacing * 16)) : 0;
    int dot_row_period = use_polka ?
        FFMAX(1, (int) lround(dot_spacing * 0.8660254038 * 16)) : 0;
    int dot_radius = use_polka ? FFMAX(1, (int) lround(dot_size * 8)) : 0;
    uint32_t dot_color = polka.color;
    if (use_polka)
        ass_apply_fade_color(&dot_color, info->fade_color);
    GradientValues secondary_values;
    const GradientValues *vals;
    const MangetsuGradientLayer *mangetsu;
    if (secondary_outline) {
        secondary_values = info->gradient.layer[2];
        if (info->secondary_outline.type == KARAOKE_OUTLINE_VECTOR) {
            secondary_values.color_enabled = true;
            memcpy(secondary_values.color, info->secondary_outline.vector.color,
                   sizeof(secondary_values.color));
        } else {
            secondary_values.color_enabled = false;
        }
        vals = &secondary_values;
        mangetsu = info->secondary_outline.type ==
                KARAOKE_OUTLINE_GRADIENT ?
            &info->secondary_outline.gradient : NULL;
    } else {
        vals = &info->gradient.layer[layer];
        mangetsu = layer < MANGETSU_GRADIENT_LAYERS ?
            &info->mangetsu_gradient.layer[layer] : NULL;
    }
    bool use_mangetsu = mangetsu && mangetsu->active &&
        ((mangetsu->coordinate_mode == MANGETSU_GRADIENT_ATTACHED &&
          mangetsu->rect.valid) ||
         mangetsu->coordinate_mode == MANGETSU_GRADIENT_POSITIONED_RECT);
    int alpha_layer = secondary_outline ? 2 : layer;
    const MangetsuGradientLayer *mangetsu_alpha =
        alpha_layer < MANGETSU_GRADIENT_LAYERS ?
            &info->mangetsu_gradient.alpha[alpha_layer] : NULL;
    bool use_mangetsu_alpha = mangetsu_alpha &&
        mangetsu_alpha->active && mangetsu_alpha->rect.valid;
    const ImageFillLayer empty_image_fill = {0};
    const ImageFillLayer *image_fill = secondary_outline ?
        &empty_image_fill : &info->image_fill.layer[layer];
    const ASS_TagImageEntry *tag_image = NULL;
    if (image_fill->enabled)
        tag_image = ass_lookup_tag_image(render_priv, render_priv->track,
                                         image_fill->path);
    bool use_tag_image = tag_image != NULL;
    bool draw_img_compat = use_tag_image && info->from_drawing;
    int tex_phase_bias_x = 0;
    int tex_phase_bias_y = 0;
    int cov_x0 = 0, cov_x1 = w > 0 ? w - 1 : 0;
    int cov_y0 = 0, cov_y1 = h > 0 ? h - 1 : 0;
    if (use_tag_image && src_x == 0 && w > 0 && h > 0) {
        // Find coverage bounds for this bitmap slice.
        // In drawing mode we only use this to clamp out guard padding.
        int min_x = w, max_x = -1;
        int min_y = h, max_y = -1;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                if (!mask[y * stride + x])
                    continue;
                if (x < min_x)
                    min_x = x;
                if (x > max_x)
                    max_x = x;
                if (y < min_y)
                    min_y = y;
                if (y > max_y)
                    max_y = y;
            }
        }
        if (max_x >= 0 && max_y >= 0) {
            cov_x0 = min_x;
            cov_x1 = max_x;
            cov_y0 = min_y;
            cov_y1 = max_y;
            bool apply_draw_phase_bias = !draw_img_compat;
            if (draw_img_compat && tag_image && tag_image->width > 0 &&
                tag_image->width <= 16)
                apply_draw_phase_bias = true;
            if (apply_draw_phase_bias)
                tex_phase_bias_x = min_x;

            bool apply_draw_phase_bias_y = false;
            if (draw_img_compat && tag_image && tag_image->width > 0 &&
                tag_image->width <= 2)
                apply_draw_phase_bias_y = true;
            if (apply_draw_phase_bias_y)
                tex_phase_bias_y = min_y;

            // In draw mode, column 0 can contain tiny AA edge coverage from
            // libass' guard expansion. If that column is much weaker than
            // column 1, treat it as padding for texture phase anchoring.
            if (apply_draw_phase_bias && draw_img_compat && cov_x0 == 0 && w > 1) {
                int sum0 = 0;
                int sum1 = 0;
                for (int y = 0; y < h; y++) {
                    sum0 += mask[y * stride + 0];
                    sum1 += mask[y * stride + 1];
                }
                if (sum0 * 8 < sum1) {
                    cov_x0 = 1;
                    tex_phase_bias_x = 1;
                }
            }

            // Likewise for row 0 on narrow/tall strip textures.
            if (apply_draw_phase_bias_y && cov_y0 == 0 && h > 1) {
                int sum0 = 0;
                int sum1 = 0;
                for (int x = 0; x < w; x++) {
                    sum0 += mask[0 * stride + x];
                    sum1 += mask[1 * stride + x];
                }
                if (sum0 * 8 < sum1) {
                    cov_y0 = 1;
                    tex_phase_bias_y = 1;
                }
            }
        }
    }

    uint32_t base_color = secondary_outline ?
        ((info->secondary_outline.color & 0xFFFFFF00u) |
         _a(info->base_c[2])) : info->base_c[layer];
    uint8_t base_alpha = _a(base_color);
    uint8_t fade = info->fade;
    uint8_t style_alpha = base_alpha;
    if (fade > 0)
        style_alpha = mult_alpha(style_alpha, fade);
    uint8_t style_opacity = 255 - style_alpha;

    // Flat paint needs neither gradient coordinates nor per-pixel color fade.
    // Keep the generic path's exact truncation for every mask coverage value,
    // including the straight RGB sidecar used by non-normal blend modes.
    if (!use_tag_image && !use_mangetsu && !use_mangetsu_alpha &&
            !use_polka && !vals->color_enabled && !vals->alpha_enabled &&
            (int64_t) w * h >= 256) {
        uint32_t color = base_color;
        ass_apply_fade_color(&color, info->fade_color);
        uint8_t r = _r(color), g = _g(color), b = _b(color);
        uint8_t pixels[256][4];
        for (int cov = 0; cov < 256; cov++) {
            uint8_t a = (uint8_t) ((cov * style_opacity) / 255);
            pixels[cov][0] = (uint8_t) ((r * a) / 255);
            pixels[cov][1] = (uint8_t) ((g * a) / 255);
            pixels[cov][2] = (uint8_t) ((b * a) / 255);
            pixels[cov][3] = a;
        }
        const uint8_t straight[4] = {r, g, b, 0};
        int valid_w = (int) FFMIN((int64_t) w,
                                  FFMAX((int64_t) full_w - src_x, 0));
        for (int y = 0; y < h; y++) {
            uint8_t *row = rgba + (size_t) y * rgba_stride;
            if ((int64_t) src_y + y >= full_h) {
                memset(row, 0, (size_t) w * 4);
                continue;
            }
            const uint8_t *src = mask + (size_t) y * stride;
            if (blend_rgb) {
                uint8_t *blend = blend_rgb + (size_t) y * blend_stride;
                for (int x = 0; x < valid_w; x++) {
                    memcpy(row + (size_t) x * 4, pixels[src[x]], 4);
                    // Zero-coverage sidecar pixels were cleared at allocation.
                    if (src[x])
                        memcpy(blend + (size_t) x * 4, straight, 4);
                }
            } else {
                for (int x = 0; x < valid_w; x++)
                    memcpy(row + (size_t) x * 4, pixels[src[x]], 4);
            }
            if (valid_w < w)
                memset(row + (size_t) valid_w * 4, 0, (size_t) (w - valid_w) * 4);
        }
        return img;
    }

    for (int y = 0; y < h; y++) {
        int32_t vf = 0;
        if (denom_h > 0) {
            int64_t num_v = ((int64_t) (src_y + y)) << 16;
            vf = (int32_t) (num_v / denom_h);
        }
        uint8_t *row = rgba + y * rgba_stride;
        const uint8_t *src = mask + y * stride;
        for (int x = 0; x < w; x++) {
            int gx = src_x + x;
            int gy = src_y + y;
            if (gx >= full_w || gy >= full_h) {
                row[4 * x + 0] = 0;
                row[4 * x + 1] = 0;
                row[4 * x + 2] = 0;
                row[4 * x + 3] = 0;
                continue;
            }
            // In drawing mode, clamp to actual covered span so guard/padding
            // columns do not create wrapped texture seams.
            if (draw_img_compat && src_x == 0 &&
                (x < cov_x0 || x > cov_x1 || y < cov_y0 || y > cov_y1)) {
                row[4 * x + 0] = 0;
                row[4 * x + 1] = 0;
                row[4 * x + 2] = 0;
                row[4 * x + 3] = 0;
                continue;
            }
            uint8_t cov = src[x];
            uint8_t cov64 = draw_img_compat ? vsf_cov64_from_mask(cov) : 0;
            if ((!draw_img_compat && !cov) || (draw_img_compat && !cov64)) {
                row[4 * x + 0] = 0;
                row[4 * x + 1] = 0;
                row[4 * x + 2] = 0;
                row[4 * x + 3] = 0;
                continue;
            }
            if (use_tag_image) {
                uint8_t sr, sg, sb, sa;
                // VSFilterMod compatibility: use visible-height Y coordinates
                // (top row starts from h-1) plus bottom clip compensation.
                sample_tag_image(tag_image,
                                 src_x + x + image_fill->xoffset - tex_phase_bias_x,
                                 vis_h - 1 - y + image_fill->yoffset + clip_diff + tex_phase_bias_y,
                                 subpix_x, subpix_y,
                                 &sr, &sg, &sb, &sa);
                uint32_t image_color = ((uint32_t) sr << 24) |
                    ((uint32_t) sg << 16) | ((uint32_t) sb << 8) | sa;
                ass_apply_fade_color(&image_color, info->fade_color);
                sr = _r(image_color);
                sg = _g(image_color);
                sb = _b(image_color);
                if (use_polka && polka_hit(gx, gy, dot_period,
                                           dot_row_period, dot_radius)) {
                    sr = _r(dot_color);
                    sg = _g(dot_color);
                    sb = _b(dot_color);
                }
                uint8_t layer_opacity = (uint8_t) ((sa * style_opacity + 127) / 255);
                uint8_t A = draw_img_compat ?
                    (uint8_t) ((cov64 * layer_opacity) >> 6) :
                    (uint8_t) ((cov * layer_opacity + 127) / 255);
                row[4 * x + 0] = (uint8_t) ((sr * A + 127) / 255);
                row[4 * x + 1] = (uint8_t) ((sg * A + 127) / 255);
                row[4 * x + 2] = (uint8_t) ((sb * A + 127) / 255);
                row[4 * x + 3] = A;
                if (blend_rgb) {
                    uint8_t *blend = blend_rgb + (size_t) y * blend_stride +
                        (size_t) x * 4;
                    blend[0] = sr;
                    blend[1] = sg;
                    blend[2] = sb;
                }
                continue;
            }
            int32_t uf = 0;
            if (denom_w > 0) {
                int64_t num_u = ((int64_t) (src_x + x)) << 16;
                uf = (int32_t) (num_u / denom_w);
            }
            uint32_t color;
            if (use_mangetsu && mangetsu->coordinate_mode ==
                    MANGETSU_GRADIENT_POSITIONED_RECT) {
                /* Pixel centres are tested in final frame coordinates. */
                if (!ass_mangetsu_positioned_gradient_sample_color(
                        mangetsu, dst_x + x + 0.5, dst_y + y + 0.5, &color))
                    color = base_color;
            } else if (use_mangetsu) {
                color = ass_mangetsu_gradient_sample_color(
                    mangetsu, dst_x + x + 0.5, dst_y + y + 0.5);
            } else {
                color = vals->color_enabled ?
                    ass_gradient_sample_color_fixed(vals, uf, vf) : base_color;
            }
            ass_apply_fade_color(&color, info->fade_color);
            if (use_polka && polka_hit(gx, gy, dot_period,
                                       dot_row_period, dot_radius))
                color = (dot_color & 0xFFFFFF00u) | _a(color);
            uint8_t alpha = use_mangetsu_alpha ?
                ass_mangetsu_gradient_sample_alpha(mangetsu_alpha,
                                                   dst_x + x + 0.5,
                                                   dst_y + y + 0.5) :
                (vals->alpha_enabled ?
                    ass_gradient_sample_alpha_fixed(vals, uf, vf) :
                    base_alpha);
            if (fade > 0)
                alpha = mult_alpha(alpha, fade);
            uint8_t A = (uint8_t) ((cov * (255 - alpha)) / 255);
            row[4 * x + 0] = (uint8_t) ((_r(color) * A) / 255);
            row[4 * x + 1] = (uint8_t) ((_g(color) * A) / 255);
            row[4 * x + 2] = (uint8_t) ((_b(color) * A) / 255);
            row[4 * x + 3] = A;
            if (blend_rgb) {
                uint8_t *blend = blend_rgb + (size_t) y * blend_stride +
                    (size_t) x * 4;
                blend[0] = _r(color);
                blend[1] = _g(color);
                blend[2] = _b(color);
            }
        }
    }

    return img;
}

static unsigned char *copy_bitmap_region(const Bitmap *bm, int x0, int y0,
                                          int w, int h, int align,
                                          int *stride_out)
{
    if (!bm || !bm->buffer || !stride_out || w <= 0 || h <= 0 || align <= 0)
        return NULL;

    size_t aligned_stride = ass_align((unsigned) align, (size_t) w);
    if (aligned_stride < (size_t) w || aligned_stride > INT_MAX ||
        aligned_stride > (SIZE_MAX - (unsigned) align) / (size_t) h ||
        aligned_stride > (size_t) (INT_MAX - align) / (size_t) h)
        return NULL;

    int stride = (int) aligned_stride;
    size_t size = aligned_stride * (size_t) h + (unsigned) align;
    unsigned char *buf = ass_aligned_alloc_tagged(
        (unsigned) align, size, false,
        ASS_ALIGNED_ALLOC_LEGACY_IMAGE, bm);
    if (!buf)
        return NULL;

    for (int y = 0; y < h; y++) {
        unsigned char *dst = buf + (size_t) y * stride;
        unsigned char *src = bm->buffer + (ptrdiff_t) (y0 + y) * bm->stride + x0;
        memcpy(dst, src, w);
    }

    *stride_out = stride;
    return buf;
}

/* Split a progressive native karaoke bitmap on the inline (Y) axis.  Both
 * ordinary and inverse clipping pass their already-clipped rectangles here. */
static ASS_Image **render_vertical_karaoke_rect(RenderContext *state,
        CombinedBitmapInfo *combined, Bitmap *bm, int dst_x, int dst_y,
        Rect rect, int brk, uint32_t color, uint32_t color2,
        int layer1, int layer2, unsigned type, CompositeHashValue *source,
        ASS_Image **tail, ASS_ImageRGBA ***rgba_tail,
        uint8_t rgba_sub_x, uint8_t rgba_sub_y)
{
    ASS_Renderer *render_priv = state->renderer;
    if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0)
        return tail;
    for (int half = 0; half < 2; half++) {
        Rect part = rect;
        if (!half)
            part.y1 = FFMIN(part.y1, brk);
        else
            part.y0 = FFMAX(part.y0, brk);
        if (part.y1 <= part.y0)
            continue;
        if (skip_transparent_paint(state, combined, half ? color2 : color,
                                  half ? layer2 : layer1))
            continue;
        int w = part.x1 - part.x0, h = part.y1 - part.y0;
        int stride = bm->stride;
        unsigned char *buffer = bm->buffer +
            (ptrdiff_t) part.y0 * bm->stride + part.x0;
        if (!source) {
            buffer = copy_bitmap_region(bm, part.x0, part.y0, w, h,
                        1 << render_priv->engine.align_order, &stride);
            if (!buffer)
                continue;
        }
        int layer = half ? layer2 : layer1;
        uint32_t paint = finalize_legacy_color(combined,
                                               half ? color2 : color);
        if (rgba_tail && combined && render_layer_uses_image(combined, layer))
            paint = (paint & 0xffffff00u) | 0xffu;
        ASS_Image *img = my_draw_bitmap(render_priv, buffer, w, h, stride,
                          dst_x + part.x0, dst_y + part.y0, paint, source);
        if (!img) {
            if (!source)
                ass_aligned_free_tagged(buffer,
                    ASS_ALIGNED_ALLOC_LEGACY_IMAGE, bm);
            continue;
        }
        img->type = type;
        *tail = img;
        tail = &img->next;
        if (rgba_tail)
            append_rgba_tail(rgba_tail,
                render_bitmap_rgba(state, combined, buffer, w, h, stride,
                    dst_x + part.x0, dst_y + part.y0, part.x0, part.y0,
                    bm->logical_w, bm->logical_h, rgba_sub_x, rgba_sub_y,
                    layer, type));
    }
    return tail;
}

/* Mask only the glyphs intersected by a curved frontier. Completed/waiting
 * glyphs continue to reference cached bitmaps. Row spans avoid per-pixel path
 * sampling and keep the two paint masks exactly complementary. */
static ASS_Image **render_curved_karaoke_rect(RenderContext *state,
        CombinedBitmapInfo *combined, Bitmap *bm, int dst_x, int dst_y,
        Rect rect, uint32_t color, uint32_t color2, int layer1, int layer2,
        unsigned type, CompositeHashValue *source, ASS_Image **tail,
        ASS_ImageRGBA ***rgba_tail, uint8_t rgba_sub_x, uint8_t rgba_sub_y)
{
    if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0)
        return tail;
    ASS_Renderer *render_priv = state->renderer;
    const double *wipe = combined->karaoke_wipe;
    double x0 = dst_x + rect.x0 + 0.5, x1 = dst_x + rect.x1 - 0.5;
    double y0 = dst_y + rect.y0 + 0.5, y1 = dst_y + rect.y1 - 0.5;
    double low = fmin(wipe[0] * x0, wipe[0] * x1) +
                 fmin(wipe[1] * y0, wipe[1] * y1) + wipe[2];
    double high = fmax(wipe[0] * x0, wipe[0] * x1) +
                  fmax(wipe[1] * y0, wipe[1] * y1) + wipe[2];
    int w = rect.x1 - rect.x0, h = rect.y1 - rect.y0;
    for (int half = 0; half < 2; half++) {
        if (half ? high < 0 : low >= 0)
            continue;
        if (skip_transparent_paint(state, combined, half ? color2 : color,
                                  half ? layer2 : layer1))
            continue;
        bool masked = half ? low < 0 : high >= 0;
        int stride = bm->stride;
        unsigned char *buffer = bm->buffer +
            (ptrdiff_t) rect.y0 * bm->stride + rect.x0;
        CompositeHashValue *owner = source;
        if (masked || !source) {
            buffer = copy_bitmap_region(bm, rect.x0, rect.y0, w, h,
                        1 << render_priv->engine.align_order, &stride);
            if (!buffer)
                continue;
            owner = NULL;
        }
        if (masked) {
            for (int y = 0; y < h; y++) {
                double row = wipe[1] * (y0 + y) + wipe[2];
                int cut;
                bool primary_left = wipe[0] >= 0;
                if (wipe[0] == 0) {
                    cut = row < 0 ? w : 0;
                } else {
                    double edge = -row / wipe[0] - x0;
                    /* Strict primary half-plane; equality belongs to waiting. */
                    double split = primary_left ? ceil(edge) : floor(edge) + 1;
                    cut = (int) FFMINMAX(split, 0, w);
                }
                bool keep_left = primary_left != (bool) half;
                unsigned char *line = buffer + (ptrdiff_t) y * stride;
                if (keep_left)
                    memset(line + cut, 0, w - cut);
                else
                    memset(line, 0, cut);
            }
        }
        int layer = half ? layer2 : layer1;
        uint32_t paint = finalize_legacy_color(combined, half ? color2 : color);
        if (rgba_tail && render_layer_uses_image(combined, layer))
            paint = (paint & 0xffffff00u) | 0xffu;
        ASS_Image *img = my_draw_bitmap(render_priv, buffer, w, h, stride,
                          dst_x + rect.x0, dst_y + rect.y0, paint, owner);
        if (!img) {
            if (!owner)
                ass_aligned_free_tagged(buffer, ASS_ALIGNED_ALLOC_LEGACY_IMAGE, bm);
            continue;
        }
        img->type = type;
        *tail = img;
        tail = &img->next;
        if (rgba_tail)
            append_rgba_tail(rgba_tail,
                render_bitmap_rgba(state, combined, buffer, w, h, stride,
                    dst_x + rect.x0, dst_y + rect.y0, rect.x0, rect.y0,
                    bm->logical_w, bm->logical_h, rgba_sub_x, rgba_sub_y,
                    layer, type));
    }
    return tail;
}

/*
 * \brief Convert bitmap glyphs into ASS_Image list with inverse clipping
 *
 * Inverse clipping with the following strategy:
 * - find rectangle from (x0, y0) to (cx0, y1)
 * - find rectangle from (cx0, y0) to (cx1, cy0)
 * - find rectangle from (cx0, cy1) to (cx1, y1)
 * - find rectangle from (cx1, y0) to (x1, y1)
 * These rectangles can be invalid and in this case are discarded.
 * Afterwards, they are clipped against the screen coordinates.
 * In an additional pass, the rectangles need to be split up left/right for
 * karaoke effects.  This can result in a lot of bitmaps (6 to be exact).
 */
static ASS_Image **render_glyph_i(RenderContext *state,
                                  CombinedBitmapInfo *combined,
                                  Bitmap *bm, int dst_x, int dst_y,
                                  uint32_t color, uint32_t color2, int brk,
                                  ASS_Image **tail, unsigned type,
                                  CompositeHashValue *source,
                                  int layer1, int layer2,
                                  ASS_ImageRGBA ***rgba_tail)
{
    ASS_Renderer *render_priv = state->renderer;
    int i, j, x0, y0, x1, y1, cx0, cy0, cx1, cy1, sx, sy, zx, zy;
    Rect r[4];
    ASS_Image *img;

    dst_x += bm->left;
    dst_y += bm->top;
    brk -= combined && combined->native_vertical &&
           combined->effect_type == EF_KARAOKE_KF ? dst_y : dst_x;

    // we still need to clip against screen boundaries
    zx = x2scr_pos_scaled(render_priv, 0);
    zy = y2scr_pos(render_priv, 0);
    sx = x2scr_pos_scaled(render_priv, render_priv->track->PlayResX);
    sy = y2scr_pos(render_priv, render_priv->track->PlayResY);

    x0 = 0;
    y0 = 0;
    int logical_w = bm->logical_w > 0 ? bm->logical_w : bm->w;
    int logical_h = bm->logical_h > 0 ? bm->logical_h : bm->h;
    x1 = FFMIN(logical_w, bm->w);
    y1 = FFMIN(logical_h, bm->h);
    uint8_t rgba_sub_x = bm->sub_x;
    uint8_t rgba_sub_y = bm->sub_y;
    if (combined && combined->from_drawing) {
        rgba_sub_x = combined->draw_sub_x;
        rgba_sub_y = combined->draw_sub_y;
    }
    cx0 = state->clip_x0 - dst_x;
    cy0 = state->clip_y0 - dst_y;
    cx1 = state->clip_x1 - dst_x;
    cy1 = state->clip_y1 - dst_y;

    // calculate rectangles and discard invalid ones while we're at it.
    i = 0;
    r[i].x0 = x0;
    r[i].y0 = y0;
    r[i].x1 = (cx0 > x1) ? x1 : cx0;
    r[i].y1 = y1;
    if (r[i].x1 > r[i].x0 && r[i].y1 > r[i].y0) i++;
    r[i].x0 = (cx0 < 0) ? x0 : cx0;
    r[i].y0 = y0;
    r[i].x1 = (cx1 > x1) ? x1 : cx1;
    r[i].y1 = (cy0 > y1) ? y1 : cy0;
    if (r[i].x1 > r[i].x0 && r[i].y1 > r[i].y0) i++;
    r[i].x0 = (cx0 < 0) ? x0 : cx0;
    r[i].y0 = (cy1 < 0) ? y0 : cy1;
    r[i].x1 = (cx1 > x1) ? x1 : cx1;
    r[i].y1 = y1;
    if (r[i].x1 > r[i].x0 && r[i].y1 > r[i].y0) i++;
    r[i].x0 = (cx1 < 0) ? x0 : cx1;
    r[i].y0 = y0;
    r[i].x1 = x1;
    r[i].y1 = y1;
    if (r[i].x1 > r[i].x0 && r[i].y1 > r[i].y0) i++;

    // clip each rectangle to screen coordinates
    for (j = 0; j < i; j++) {
        r[j].x0 = (r[j].x0 + dst_x < zx) ? zx - dst_x : r[j].x0;
        r[j].y0 = (r[j].y0 + dst_y < zy) ? zy - dst_y : r[j].y0;
        r[j].x1 = (r[j].x1 + dst_x > sx) ? sx - dst_x : r[j].x1;
        r[j].y1 = (r[j].y1 + dst_y > sy) ? sy - dst_y : r[j].y1;
        if (state->chat_clip_active) {
            r[j].x0 = FFMAX(r[j].x0, state->chat_clip_x0 - dst_x);
            r[j].x1 = FFMIN(r[j].x1, state->chat_clip_x1 - dst_x);
            r[j].y0 = FFMAX(r[j].y0, state->chat_clip_y0 - dst_y);
            r[j].y1 = FFMIN(r[j].y1, state->chat_clip_y1 - dst_y);
        }
        if (state->scroll_clip_active) {
            r[j].y0 = FFMAX(r[j].y0, state->scroll_clip_y0 - dst_y);
            r[j].y1 = FFMIN(r[j].y1, state->scroll_clip_y1 - dst_y);
        }
        if (state->karaoke_clip_enabled) {
            r[j].x0 = FFMAX(r[j].x0, state->karaoke_clip_x0 - dst_x);
            r[j].x1 = FFMIN(r[j].x1, state->karaoke_clip_x1 - dst_x);
        }
    }

    if (combined && combined->curved_karaoke && layer1 != layer2) {
        for (j = 0; j < i; j++)
            tail = render_curved_karaoke_rect(state, combined, bm,
                dst_x, dst_y, r[j], color, color2, layer1, layer2,
                type, source, tail, rgba_tail, rgba_sub_x, rgba_sub_y);
        return tail;
    }

    if (combined && combined->native_vertical &&
            combined->effect_type == EF_KARAOKE_KF) {
        for (j = 0; j < i; j++)
            tail = render_vertical_karaoke_rect(state, combined, bm,
                dst_x, dst_y, r[j], brk, color, color2, layer1, layer2,
                type, source, tail, rgba_tail, rgba_sub_x, rgba_sub_y);
        return tail;
    }

        // draw the rectangles
        for (j = 0; j < i; j++) {
            int lbrk = brk;
            // kick out rectangles that are invalid now
            if (r[j].x1 <= r[j].x0 || r[j].y1 <= r[j].y0)
                continue;
            // split up into left and right for karaoke, if needed
            if (lbrk > r[j].x0 &&
                !skip_transparent_paint(state, combined, color, layer1)) {
                if (lbrk > r[j].x1) lbrk = r[j].x1;
                int sub_w = lbrk - r[j].x0;
                int sub_h = r[j].y1 - r[j].y0;
                int sub_stride = bm->stride;
                unsigned char *sub_buf = bm->buffer + r[j].y0 * bm->stride + r[j].x0;
                if (!source) {
                    sub_buf = copy_bitmap_region(bm, r[j].x0, r[j].y0, sub_w, sub_h,
                                                 1 << render_priv->engine.align_order, &sub_stride);
                    if (!sub_buf)
                        break;
                }
                uint32_t legacy_color = finalize_legacy_color(combined, color);
                if (rgba_tail && combined &&
                    render_layer_uses_image(combined, layer1))
                    legacy_color = (legacy_color & 0xFFFFFF00u) | 0xFFu;
                img = my_draw_bitmap(render_priv, sub_buf, sub_w, sub_h, sub_stride,
                                     dst_x + r[j].x0, dst_y + r[j].y0, legacy_color, source);
                if (!img) {
                    if (!source)
                        ass_aligned_free_tagged(
                            sub_buf, ASS_ALIGNED_ALLOC_LEGACY_IMAGE, bm);
                    break;
                }
                img->type = type;
                *tail = img;
                tail = &img->next;
                if (rgba_tail) {
                    append_rgba_tail(rgba_tail,
                                     render_bitmap_rgba(state, combined,
                                     sub_buf, sub_w, sub_h, sub_stride,
                                     dst_x + r[j].x0, dst_y + r[j].y0,
                                     r[j].x0, r[j].y0,
                                     bm->logical_w, bm->logical_h,
                                     rgba_sub_x, rgba_sub_y,
                                     layer1, type));
                }
            }
            if (lbrk < r[j].x1 &&
                !skip_transparent_paint(state, combined, color2, layer2)) {
                if (lbrk < r[j].x0) lbrk = r[j].x0;
                int sub_w = r[j].x1 - lbrk;
                int sub_h = r[j].y1 - r[j].y0;
                int sub_stride = bm->stride;
                unsigned char *sub_buf = bm->buffer + r[j].y0 * bm->stride + lbrk;
                if (!source) {
                    sub_buf = copy_bitmap_region(bm, lbrk, r[j].y0, sub_w, sub_h,
                                                 1 << render_priv->engine.align_order, &sub_stride);
                    if (!sub_buf)
                        break;
                }
                uint32_t legacy_color = finalize_legacy_color(combined, color2);
                if (rgba_tail && combined &&
                    render_layer_uses_image(combined, layer2))
                    legacy_color = (legacy_color & 0xFFFFFF00u) | 0xFFu;
                img = my_draw_bitmap(render_priv, sub_buf, sub_w, sub_h, sub_stride,
                                     dst_x + lbrk, dst_y + r[j].y0, legacy_color, source);
                if (!img) {
                    if (!source)
                        ass_aligned_free_tagged(
                            sub_buf, ASS_ALIGNED_ALLOC_LEGACY_IMAGE, bm);
                    break;
                }
                img->type = type;
                *tail = img;
                tail = &img->next;
                if (rgba_tail) {
                    append_rgba_tail(rgba_tail,
                                     render_bitmap_rgba(state, combined,
                                     sub_buf, sub_w, sub_h, sub_stride,
                                     dst_x + lbrk, dst_y + r[j].y0,
                                     lbrk, r[j].y0,
                                     bm->logical_w, bm->logical_h,
                                     rgba_sub_x, rgba_sub_y,
                                     layer2, type));
                }
            }
    }

    return tail;
}

/**
 * \brief convert bitmap glyph into ASS_Image struct(s)
 * \param bit freetype bitmap glyph, FT_PIXEL_MODE_GRAY
 * \param dst_x bitmap x coordinate in video frame
 * \param dst_y bitmap y coordinate in video frame
 * \param color first color, RGBA
 * \param color2 second color, RGBA
 * \param brk x coordinate relative to glyph origin, color is used to the left of brk, color2 - to the right
 * \param tail pointer to the last image's next field, head of the generated list should be stored here
 * \return pointer to the new list tail
 * Performs clipping. Uses my_draw_bitmap for actual bitmap conversion.
 */
static ASS_Image **
render_glyph(RenderContext *state, CombinedBitmapInfo *combined,
             Bitmap *bm, int dst_x, int dst_y,
             uint32_t color, uint32_t color2, int brk, ASS_Image **tail,
             unsigned type, CompositeHashValue *source,
             int layer1, int layer2, ASS_ImageRGBA ***rgba_tail)
{
    // Inverse clipping in use?
    if (state->clip_mode)
        return render_glyph_i(state, combined, bm, dst_x, dst_y, color, color2,
                              brk, tail, type, source, layer1, layer2,
                              rgba_tail);

    // brk is absolute
    // color = color left of brk
    // color2 = color right of brk
    int b_x0, b_y0, b_x1, b_y1; // visible part of the bitmap
    int clip_x0, clip_y0, clip_x1, clip_y1;
    int tmp;
    ASS_Image *img;
    ASS_Renderer *render_priv = state->renderer;
    uint8_t rgba_sub_x = bm->sub_x;
    uint8_t rgba_sub_y = bm->sub_y;
    if (combined && combined->from_drawing) {
        rgba_sub_x = combined->draw_sub_x;
        rgba_sub_y = combined->draw_sub_y;
    }

    dst_x += bm->left;
    dst_y += bm->top;
    brk -= combined && combined->native_vertical &&
           combined->effect_type == EF_KARAOKE_KF ? dst_y : dst_x;

    // clipping
    clip_x0 = FFMINMAX(state->clip_x0, 0, render_priv->width);
    clip_y0 = FFMINMAX(state->clip_y0, 0, render_priv->height);
    clip_x1 = FFMINMAX(state->clip_x1, 0, render_priv->width);
    clip_y1 = FFMINMAX(state->clip_y1, 0, render_priv->height);
    if (state->chat_clip_active) {
        clip_x0 = FFMAX(clip_x0, state->chat_clip_x0);
        clip_x1 = FFMIN(clip_x1, state->chat_clip_x1);
        clip_y0 = FFMAX(clip_y0, state->chat_clip_y0);
        clip_y1 = FFMIN(clip_y1, state->chat_clip_y1);
    }
    if (state->scroll_clip_active) {
        clip_y0 = FFMAX(clip_y0, state->scroll_clip_y0);
        clip_y1 = FFMIN(clip_y1, state->scroll_clip_y1);
    }
    if (state->karaoke_clip_enabled) {
        clip_x0 = FFMAX(clip_x0, state->karaoke_clip_x0);
        clip_x1 = FFMIN(clip_x1, state->karaoke_clip_x1);
    }
    b_x0 = 0;
    b_y0 = 0;
    int logical_w = bm->logical_w > 0 ? bm->logical_w : bm->w;
    int logical_h = bm->logical_h > 0 ? bm->logical_h : bm->h;
    b_x1 = FFMIN(logical_w, bm->w);
    b_y1 = FFMIN(logical_h, bm->h);

    tmp = dst_x - clip_x0;
    if (tmp < 0)
        b_x0 = -tmp;
    tmp = dst_y - clip_y0;
    if (tmp < 0)
        b_y0 = -tmp;
    tmp = clip_x1 - dst_x - bm->w;
    if (tmp < 0)
        b_x1 = bm->w + tmp;
    tmp = clip_y1 - dst_y - bm->h;
    if (tmp < 0)
        b_y1 = bm->h + tmp;

    if ((b_y0 >= b_y1) || (b_x0 >= b_x1))
        return tail;

    if (combined && combined->curved_karaoke && layer1 != layer2) {
        Rect rect = {b_x0, b_y0, b_x1, b_y1};
        return render_curved_karaoke_rect(state, combined, bm,
            dst_x, dst_y, rect, color, color2, layer1, layer2,
            type, source, tail, rgba_tail, rgba_sub_x, rgba_sub_y);
    }

    if (combined && combined->native_vertical &&
            combined->effect_type == EF_KARAOKE_KF) {
        Rect rect = {b_x0, b_y0, b_x1, b_y1};
        return render_vertical_karaoke_rect(state, combined, bm,
            dst_x, dst_y, rect, brk, color, color2, layer1, layer2,
            type, source, tail, rgba_tail, rgba_sub_x, rgba_sub_y);
    }

    if (brk > b_x0 &&
        !skip_transparent_paint(state, combined, color, layer1)) {
        if (brk > b_x1)
            brk = b_x1;
        int sub_w = brk - b_x0;
        int sub_h = b_y1 - b_y0;
        int sub_stride = bm->stride;
        unsigned char *sub_buf = bm->buffer + bm->stride * b_y0 + b_x0;
        if (!source) {
            sub_buf = copy_bitmap_region(bm, b_x0, b_y0, sub_w, sub_h,
                                         1 << render_priv->engine.align_order, &sub_stride);
            if (!sub_buf)
                return tail;
        }
        uint32_t legacy_color = finalize_legacy_color(combined, color);
        if (rgba_tail && combined &&
            render_layer_uses_image(combined, layer1))
            legacy_color = (legacy_color & 0xFFFFFF00u) | 0xFFu;
        img = my_draw_bitmap(render_priv, sub_buf, sub_w, sub_h, sub_stride,
                             dst_x + b_x0, dst_y + b_y0, legacy_color, source);
        if (!img) {
            if (!source)
                ass_aligned_free_tagged(
                    sub_buf, ASS_ALIGNED_ALLOC_LEGACY_IMAGE, bm);
            return tail;
        }
        img->type = type;
        *tail = img;
        tail = &img->next;
        if (rgba_tail) {
            append_rgba_tail(rgba_tail,
                             render_bitmap_rgba(state, combined,
                                 sub_buf, sub_w, sub_h, sub_stride,
                                 dst_x + b_x0, dst_y + b_y0,
                                 b_x0, b_y0,
                                 bm->logical_w, bm->logical_h,
                                 rgba_sub_x, rgba_sub_y,
                                 layer1, type));
        }
    }
    if (brk < b_x1 &&
        !skip_transparent_paint(state, combined, color2, layer2)) {
        if (brk < b_x0)
            brk = b_x0;
        int sub_w = b_x1 - brk;
        int sub_h = b_y1 - b_y0;
        int sub_stride = bm->stride;
        unsigned char *sub_buf = bm->buffer + bm->stride * b_y0 + brk;
        if (!source) {
            sub_buf = copy_bitmap_region(bm, brk, b_y0, sub_w, sub_h,
                                         1 << render_priv->engine.align_order, &sub_stride);
            if (!sub_buf)
                return tail;
        }
        uint32_t legacy_color = finalize_legacy_color(combined, color2);
        if (rgba_tail && combined &&
            render_layer_uses_image(combined, layer2))
            legacy_color = (legacy_color & 0xFFFFFF00u) | 0xFFu;
        img = my_draw_bitmap(render_priv, sub_buf, sub_w, sub_h, sub_stride,
                             dst_x + brk, dst_y + b_y0, legacy_color, source);
        if (!img) {
            if (!source)
                ass_aligned_free_tagged(
                    sub_buf, ASS_ALIGNED_ALLOC_LEGACY_IMAGE, bm);
            return tail;
        }
        img->type = type;
        *tail = img;
        tail = &img->next;
        if (rgba_tail) {
            append_rgba_tail(rgba_tail,
                             render_bitmap_rgba(state, combined,
                                 sub_buf, sub_w, sub_h, sub_stride,
                                 dst_x + brk, dst_y + b_y0,
                                 brk, b_y0,
                                 bm->logical_w, bm->logical_h,
                                 rgba_sub_x, rgba_sub_y,
                                 layer2, type));
        }
    }
    return tail;
}

static ASS_Image **render_glyph_karaoke_region(
        RenderContext *state, CombinedBitmapInfo *combined, Bitmap *bm,
        int dst_x, int dst_y, uint32_t color, uint32_t color2, int brk,
        int clip_x0, int clip_x1, ASS_Image **tail, unsigned type,
        CompositeHashValue *source, int layer1, int layer2,
        ASS_ImageRGBA ***rgba_tail)
{
    bool saved_enabled = state->karaoke_clip_enabled;
    int saved_x0 = state->karaoke_clip_x0;
    int saved_x1 = state->karaoke_clip_x1;
    state->karaoke_clip_enabled = true;
    state->karaoke_clip_x0 = clip_x0;
    state->karaoke_clip_x1 = clip_x1;
    tail = render_glyph(state, combined, bm, dst_x, dst_y, color, color2,
                        brk, tail, type, source, layer1, layer2, rgba_tail);
    state->karaoke_clip_enabled = saved_enabled;
    state->karaoke_clip_x0 = saved_x0;
    state->karaoke_clip_x1 = saved_x1;
    return tail;
}

static bool furi_base_region_bounds(RenderContext *state,
                                    CombinedBitmapInfo *info,
                                    const FuriKaraokeRegion *region,
                                    int *x0, int *x1)
{
    if (!info->furi_base_karaoke || info->furi_group < 0 ||
            info->furi_group >= state->text_info.n_furi_groups)
        return false;

    int32_t width = info->furi_base_end - info->furi_base_start;
    int32_t rel0 = info->furi_base_start +
        ass_lrint(width * region->start);
    int32_t rel1 = info->furi_base_start +
        ass_lrint(width * region->end);
    if (info->furi_base_reverse) {
        rel0 = info->furi_base_end - ass_lrint(width * region->start);
        rel1 = info->furi_base_end - ass_lrint(width * region->end);
    }
    int pos0 = lround(d6_to_double(info->leftmost_x) +
        d6_to_double(rel0) * state->renderer->par_scale_x);
    int pos1 = lround(d6_to_double(info->leftmost_x) +
        d6_to_double(rel1) * state->renderer->par_scale_x);
    *x0 = FFMIN(pos0, pos1);
    *x1 = FFMAX(pos0, pos1);
    return *x0 < *x1;
}

static int furi_base_frontier(RenderContext *state,
                              CombinedBitmapInfo *info, double amount)
{
    int32_t width = info->furi_base_end - info->furi_base_start;
    int32_t rel = info->furi_base_reverse ?
        info->furi_base_end - ass_lrint(width * amount) :
        info->furi_base_start + ass_lrint(width * amount);
    return lround(d6_to_double(info->leftmost_x) +
        d6_to_double(rel) * state->renderer->par_scale_x);
}

static ASS_Image **render_furi_base_border_regions(
        RenderContext *state, CombinedBitmapInfo *info, Bitmap *bm,
        uint32_t color, bool secondary_allowed, ASS_Image **tail,
        CompositeHashValue *source,
        ASS_ImageRGBA ***rgba_tail)
{
    FuriGroup *group = &state->text_info.furi_groups[info->furi_group];
    int64_t now = state->renderer->time - state->event->Start;
    bool needs_regions = secondary_allowed &&
        info->secondary_outline.type != KARAOKE_OUTLINE_UNSET;
    for (int i = 0; i < group->n_karaoke_regions; i++) {
        int segment = group->karaoke_regions[i].segment;
        if (segment >= 0 &&
                segment < state->text_info.n_karaoke_segments &&
                (state->text_info.karaoke_segments[segment].effect_type ==
                     EF_KARAOKE_KO ||
                 state->text_info.karaoke_segments[segment].effect_type ==
                     EF_KARAOKE_REVEAL)) {
            needs_regions = true;
            break;
        }
    }
    if (!needs_regions)
        return render_glyph(state, info, bm, info->x, info->y, color, 0,
                            100000000, tail, IMAGE_TYPE_OUTLINE, source,
                            2, 2, rgba_tail);

    int base0 = furi_base_frontier(state, info, 0.0);
    int base1 = furi_base_frontier(state, info, 1.0);
    for (int i = 0; i < group->n_karaoke_regions; i++) {
        FuriKaraokeRegion *region = &group->karaoke_regions[i];
        if (region->segment < 0 ||
                region->segment >= state->text_info.n_karaoke_segments)
            continue;
        KaraokeSegment *segment =
            &state->text_info.karaoke_segments[region->segment];
        if ((segment->effect_type == EF_KARAOKE_KO ||
             segment->effect_type == EF_KARAOKE_REVEAL) &&
                now < segment->start)
            continue;

        int x0, x1;
        if (!furi_base_region_bounds(state, info, region, &x0, &x1))
            continue;
        int clip_x0 = x0 == FFMIN(base0, base1) ? -100000000 : x0;
        int clip_x1 = x1 == FFMAX(base0, base1) ? 100000000 : x1;
        uint32_t first_color = color, second_color = 0;
        int first_layer = 2, second_layer = 2;
        int brk = 100000000;
        bool secondary = secondary_allowed &&
                         info->secondary_outline.type !=
                             KARAOKE_OUTLINE_UNSET;
        if (secondary && segment->effect_type != EF_KARAOKE_KO &&
                segment->effect_type != EF_KARAOKE_REVEAL) {
            uint32_t waiting = secondary_outline_color(info);
            if (segment->effect_type == EF_KARAOKE_KF &&
                    now >= segment->start && now < segment->end &&
                    segment->end > segment->start) {
                double amount = (double) (now - segment->start) /
                                (segment->end - segment->start);
                double frontier = region->rtl ?
                    region->end - (region->end - region->start) * amount :
                    region->start + (region->end - region->start) * amount;
                second_color = waiting;
                second_layer = KARAOKE_SECONDARY_OUTLINE_LAYER;
                if (region->rtl ^ info->furi_base_reverse) {
                    first_color = waiting;
                    second_color = color;
                    first_layer = KARAOKE_SECONDARY_OUTLINE_LAYER;
                    second_layer = 2;
                }
                brk = furi_base_frontier(state, info, frontier);
            } else if (now < segment->start) {
                first_color = waiting;
                first_layer = KARAOKE_SECONDARY_OUTLINE_LAYER;
            }
        }
        tail = render_glyph_karaoke_region(
            state, info, bm, info->x, info->y, first_color, second_color, brk,
            clip_x0, clip_x1, tail, IMAGE_TYPE_OUTLINE, source,
            first_layer, second_layer, rgba_tail);
    }
    return tail;
}

static ASS_Image **render_furi_base_character_regions(
        RenderContext *state, CombinedBitmapInfo *info, ASS_Image **tail,
        ASS_ImageRGBA ***rgba_tail)
{
    FuriGroup *group = &state->text_info.furi_groups[info->furi_group];
    int64_t now = state->renderer->time - state->event->Start;
    int base0 = furi_base_frontier(state, info, 0.0);
    int base1 = furi_base_frontier(state, info, 1.0);
    for (int i = 0; i < group->n_karaoke_regions; i++) {
        FuriKaraokeRegion *region = &group->karaoke_regions[i];
        if (region->segment < 0 ||
                region->segment >= state->text_info.n_karaoke_segments)
            continue;
        KaraokeSegment *segment =
            &state->text_info.karaoke_segments[region->segment];
        if (segment->effect_type == EF_KARAOKE_REVEAL &&
                now < segment->start)
            continue;
        int x0, x1;
        if (!furi_base_region_bounds(state, info, region, &x0, &x1))
            continue;
        int clip_x0 = x0 == FFMIN(base0, base1) ? -100000000 : x0;
        int clip_x1 = x1 == FFMAX(base0, base1) ? 100000000 : x1;

        uint32_t color = info->c[0], color2 = 0;
        int brk = 100000000;
        int layer1 = 0, layer2 = 0;
        if (segment->effect_type == EF_KARAOKE_KF &&
                now >= segment->start && now < segment->end &&
                segment->end > segment->start) {
            double amount = (double) (now - segment->start) /
                            (segment->end - segment->start);
            double frontier = region->rtl ?
                region->end - (region->end - region->start) * amount :
                region->start + (region->end - region->start) * amount;
            color2 = info->c[1];
            layer2 = 1;
            if (region->rtl ^ info->furi_base_reverse) {
                uint32_t tmp = color;
                color = color2;
                color2 = tmp;
                layer1 = 1;
                layer2 = 0;
            }
            brk = furi_base_frontier(state, info, frontier);
        } else if (now < segment->start) {
            color = info->c[1];
            layer1 = 1;
        }
        tail = render_glyph_karaoke_region(
            state, info, info->bm, info->x, info->y, color, color2, brk,
            clip_x0, clip_x1, tail, IMAGE_TYPE_CHARACTER, info->image,
            layer1, layer2, rgba_tail);
    }
    return tail;
}

static ASS_Image **render_furi_base_reveal_regions(
        RenderContext *state, CombinedBitmapInfo *info, Bitmap *bm,
        uint32_t color, unsigned type, int paint_layer, ASS_Image **tail,
        CompositeHashValue *source, ASS_ImageRGBA ***rgba_tail)
{
    FuriGroup *group = &state->text_info.furi_groups[info->furi_group];
    bool has_reveal = false;
    for (int i = 0; i < group->n_karaoke_regions; i++) {
        int index = group->karaoke_regions[i].segment;
        if (index >= 0 && index < state->text_info.n_karaoke_segments &&
                state->text_info.karaoke_segments[index].effect_type ==
                    EF_KARAOKE_REVEAL) {
            has_reveal = true;
            break;
        }
    }
    if (!has_reveal)
        return render_glyph(state, info, bm, info->x, info->y, color, 0,
                            100000000, tail, type, source,
                            paint_layer, paint_layer, rgba_tail);

    int64_t now = state->renderer->time - state->event->Start;
    int base0 = furi_base_frontier(state, info, 0.0);
    int base1 = furi_base_frontier(state, info, 1.0);
    for (int i = 0; i < group->n_karaoke_regions; i++) {
        FuriKaraokeRegion *region = &group->karaoke_regions[i];
        if (region->segment < 0 ||
                region->segment >= state->text_info.n_karaoke_segments)
            continue;
        KaraokeSegment *segment =
            &state->text_info.karaoke_segments[region->segment];
        if (segment->effect_type == EF_KARAOKE_REVEAL &&
                now < segment->start)
            continue;
        int x0, x1;
        if (!furi_base_region_bounds(state, info, region, &x0, &x1))
            continue;
        int clip_x0 = x0 == FFMIN(base0, base1) ? -100000000 : x0;
        int clip_x1 = x1 == FFMAX(base0, base1) ? 100000000 : x1;
        tail = render_glyph_karaoke_region(
            state, info, bm, info->x, info->y, color, 0, 100000000,
            clip_x0, clip_x1, tail, type, source,
            paint_layer, paint_layer, rgba_tail);
    }
    return tail;
}

static ASS_Image **render_border_layer(RenderContext *state,
                                       CombinedBitmapInfo *info,
                                       int layer, ASS_Image **tail,
                                       ASS_ImageRGBA ***rgba_tail)
{
    Bitmap *bm = combined_border_bitmap(info, layer);
    if (!bm)
        return tail;

    if (!info->furi_base_karaoke &&
            (info->effect_type == EF_KARAOKE_KO ||
             info->effect_type == EF_KARAOKE_REVEAL) &&
            (info->effect_timing <= 0))
        return tail;

    if (layer == 0) {
        MangetsuGradientLayer saved_mangetsu = info->mangetsu_gradient.layer[2];
        MangetsuGradientLayer saved_mangetsu_alpha =
            info->mangetsu_gradient.alpha[2];
        info->mangetsu_gradient.layer[2] = info->mangetsu_gradient.border[0];
        info->mangetsu_gradient.alpha[2] =
            info->mangetsu_gradient.border_alpha[0];
        if (info->furi_base_karaoke)
            tail = render_furi_base_border_regions(
                state, info, bm, info->c[2], true, tail, info->image,
                rgba_tail);
        else if (info->secondary_outline.type != KARAOKE_OUTLINE_UNSET &&
                 (info->effect_type == EF_KARAOKE ||
                  info->effect_type == EF_KARAOKE_KF)) {
            uint32_t active = info->c[2];
            uint32_t waiting = secondary_outline_color(info);
            uint32_t left = active, right = waiting;
            int left_layer = 2;
            int right_layer = KARAOKE_SECONDARY_OUTLINE_LAYER;
            if (info->effect_type == EF_KARAOKE) {
                if (info->effect_timing <= 0) {
                    left = waiting;
                    left_layer = KARAOKE_SECONDARY_OUTLINE_LAYER;
                }
                right = 0;
                right_layer = left_layer;
            } else if (info->karaoke_reverse) {
                left = waiting;
                right = active;
                left_layer = KARAOKE_SECONDARY_OUTLINE_LAYER;
                right_layer = 2;
            }
            tail = render_glyph(state, info, bm, info->x, info->y,
                                left, right,
                                info->effect_type == EF_KARAOKE_KF ?
                                    info->effect_timing : 1000000,
                                tail, IMAGE_TYPE_OUTLINE, info->image,
                                left_layer, right_layer, rgba_tail);
        } else {
            tail = render_glyph(state, info, bm, info->x, info->y,
                                info->c[2], 0, 1000000, tail,
                                IMAGE_TYPE_OUTLINE, info->image, 2, 2,
                                rgba_tail);
        }
        info->mangetsu_gradient.layer[2] = saved_mangetsu;
        info->mangetsu_gradient.alpha[2] = saved_mangetsu_alpha;
        return tail;
    }

    uint32_t saved_base = info->base_c[2];
    PolkaPaint saved_polka = info->pattern.polka_face[2];
    GradientValues saved_gradient = info->gradient.layer[2];
    MangetsuGradientLayer saved_mangetsu = info->mangetsu_gradient.layer[2];
    MangetsuGradientLayer saved_mangetsu_alpha =
        info->mangetsu_gradient.alpha[2];
    ImageFillLayer saved_image = info->image_fill.layer[2];
    info->base_c[2] = info->border_layers[layer].color;
    info->pattern.polka_face[2] = info->pattern.polka_border[layer];
    info->gradient.layer[2] = info->border_layers[layer].gradient;
    info->mangetsu_gradient.layer[2] = info->mangetsu_gradient.border[layer];
    info->mangetsu_gradient.alpha[2] =
        info->mangetsu_gradient.border_alpha[layer];
    clear_image_fill_layer(&info->image_fill.layer[2]);

    uint32_t color = info->border_layers[layer].color;
    ass_apply_fade(&color, info->fade);

    if (info->furi_base_karaoke)
        tail = render_furi_base_border_regions(
            state, info, bm, color, false, tail, info->image, rgba_tail);
    else
        tail = render_glyph(state, info, bm, info->x, info->y, color,
                            0, 1000000, tail, IMAGE_TYPE_OUTLINE, info->image,
                            2, 2, rgba_tail);

    info->base_c[2] = saved_base;
    info->pattern.polka_face[2] = saved_polka;
    info->gradient.layer[2] = saved_gradient;
    info->mangetsu_gradient.layer[2] = saved_mangetsu;
    info->mangetsu_gradient.alpha[2] = saved_mangetsu_alpha;
    info->image_fill.layer[2] = saved_image;
    return tail;
}

static bool quantize_transform(double m[3][3], ASS_Vector *pos,
                               ASS_DVector *offset, bool first,
                               BitmapHashKey *key)
{
    // Full transform:
    // x_out = (m_xx * x + m_xy * y + m_xz) / z,
    // y_out = (m_yx * x + m_yy * y + m_yz) / z,
    // z     =  m_zx * x + m_zy * y + m_zz.

    const double max_val = 1000000;

    const ASS_Rect *bbox = &key->outline->cbox;
    double x0 = (bbox->x_min + bbox->x_max) / 2.0;
    double y0 = (bbox->y_min + bbox->y_max) / 2.0;
    double dx = (bbox->x_max - bbox->x_min) / 2.0 + 64;
    double dy = (bbox->y_max - bbox->y_min) / 2.0 + 64;

    // Change input coordinates' origin to (x0, y0),
    // after that transformation x:[-dx, dx], y:[-dy, dy],
    // max|x| = dx and max|y| = dy.
    for (int i = 0; i < 3; i++)
        m[i][2] += m[i][0] * x0 + m[i][1] * y0;

    if (m[2][2] <= 0)
        return false;

    double w = 1 / m[2][2];
    // Transformed center of bounding box
    double center[2] = { m[0][2] * w, m[1][2] * w };
    // Change output coordinates' origin to center,
    // m_xz and m_yz is skipped as it becomes 0 and no longer needed.
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++)
            m[i][j] -= m[2][j] * center[i];

    double delta[2] = {0};
    if (!first) {
        delta[0] = offset->x;
        delta[1] = offset->y;
    }

    int32_t qr[2];  // quantized center position
    for (int i = 0; i < 2; i++) {
        center[i] /= 64 >> SUBPIXEL_ORDER;
        center[i] -= delta[i];
        if (!(fabs(center[i]) < max_val))
            return false;
        qr[i] = ass_lrint(center[i]);
    }

    // Minimal bounding box z coordinate
    double z0 = m[2][2] - fabs(m[2][0]) * dx - fabs(m[2][1]) * dy;
    // z0 clamped to z_center / MAX_PERSP_SCALE to mitigate problems with small z
    w = 1.0 / POSITION_PRECISION / FFMAX(z0, m[2][2] / MAX_PERSP_SCALE);
    double mul[2] = { dx * w, dy * w };  // 1 / q_x, 1 / q_y

    // z0 = m_zz - |m_zx| * dx - |m_zy| * dy,
    // m_zz = z0 + |m_zx| * dx + |m_zy| * dy,
    // z = m_zx * x + m_zy * y + m_zz
    //  = m_zx * (x + sign(m_zx) * dx) + m_zy * (y + sign(m_zy) * dy) + z0.

    // Let D(f) denote the absolute error of a quantity f.
    // Our goal is to determine tolerable error for matrix coefficients,
    // so that the total error of the output x_out, y_out is still acceptable.
    // As glyph dimensions are usually larger than a couple of pixels, errors
    // will be relatively small and we can use first order approximation.

    // z0 is effectively a scale factor and can thus be treated as a constant.
    // Error of constants is obviously zero, so:  D(dx) = D(dy) = D(z0) = 0.
    // For arbitrary quantities A, B, C with C not zero, the following holds true:
    //   D(A * B) <= D(A) * max|B| + max|A| * D(B),
    //   D(1 / C) <= D(C) * max|1 / C^2|.
    // Write ~ for 'same magnitude' and ~= for 'approximately'.

    // D(x_out) = D((m_xx * x + m_xy * y) / z)
    //  <= D(m_xx * x + m_xy * y) * max|1 / z| + max|m_xx * x + m_xy * y| * D(1 / z)
    //  <= (D(m_xx) * dx + D(m_xy) * dy) / z0 + (|m_xx| * dx + |m_xy| * dy) * D(z) / z0^2,
    // D(y_out) = D((m_yx * x + m_yy * y) / z)
    //  <= D(m_yx * x + m_yy * y) * max|1 / z| + max|m_yx * x + m_yy * y| * D(1 / z)
    //  <= (D(m_yx) * dx + D(m_yy) * dy) / z0 + (|m_yx| * dx + |m_yy| * dy) * D(z) / z0^2,
    // |m_xx| * dx + |m_xy| * dy = x_lim,
    // |m_yx| * dx + |m_yy| * dy = y_lim,
    // D(z) <= 2 * (D(m_zx) * dx + D(m_zy) * dy),
    // D(x_out) <= (D(m_xx) * dx + D(m_xy) * dy) / z0
    //       + 2 * (D(m_zx) * dx + D(m_zy) * dy) * x_lim / z0^2,
    // D(y_out) <= (D(m_yx) * dx + D(m_yy) * dy) / z0
    //       + 2 * (D(m_zx) * dx + D(m_zy) * dy) * y_lim / z0^2.

    // To estimate acceptable error in a matrix coefficient, pick ACCURACY for this substep,
    // set error in all other coefficients to zero and solve the system
    // D(x_out) <= ACCURACY, D(y_out) <= ACCURACY for desired D(m_ij).
    // Note that ACCURACY isn't equal to total error.
    // Total error is larger than each ACCURACY, but still of the same magnitude.
    // Via our choice of ACCURACY, we get a total error of up to several POSITION_PRECISION.

    // Quantization steps (pick: ACCURACY = POSITION_PRECISION):
    // D(m_xx), D(m_yx) ~ q_x = POSITION_PRECISION * z0 / dx,
    // D(m_xy), D(m_yy) ~ q_y = POSITION_PRECISION * z0 / dy,
    // qm_xx = round(m_xx / q_x), qm_xy = round(m_xy / q_y),
    // qm_yx = round(m_yx / q_x), qm_yy = round(m_yy / q_y).

    int32_t qm[3][2];
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            double val = m[i][j] * mul[j];
            if (!(fabs(val) < max_val))
                return false;
            qm[i][j] = ass_lrint(val);
        }

    // x_lim = |m_xx| * dx + |m_xy| * dy
    //  ~= |qm_xx| * q_x * dx + |qm_xy| * q_y * dy
    //  = (|qm_xx| + |qm_xy|) * POSITION_PRECISION * z0,
    // y_lim = |m_yx| * dx + |m_yy| * dy
    //  ~= |qm_yx| * q_x * dx + |qm_yy| * q_y * dy
    //  = (|qm_yx| + |qm_yy|) * POSITION_PRECISION * z0,
    // max(x_lim, y_lim) / z0 ~= w
    //  = max(|qm_xx| + |qm_xy|, |qm_yx| + |qm_yy|) * POSITION_PRECISION.

    // Quantization steps (pick: ACCURACY = 2 * POSITION_PRECISION):
    // D(m_zx) ~ POSITION_PRECISION * z0^2 / max(x_lim, y_lim) / dx ~= q_zx = q_x / w,
    // D(m_zy) ~ POSITION_PRECISION * z0^2 / max(x_lim, y_lim) / dy ~= q_zy = q_y / w,
    // qm_zx = round(m_zx / q_zx), qm_zy = round(m_zy / q_zy).

    int32_t qmx = abs(qm[0][0]) + abs(qm[0][1]);
    int32_t qmy = abs(qm[1][0]) + abs(qm[1][1]);
    w = POSITION_PRECISION * FFMAX(qmx, qmy);
    mul[0] *= w;
    mul[1] *= w;

    for (int j = 0; j < 2; j++) {
        double val = m[2][j] * mul[j];
        if (!(fabs(val) < max_val))
            return false;
        qm[2][j] = ass_lrint(val);
    }

    if (first && offset) {
        offset->x = center[0] - qr[0];
        offset->y = center[1] - qr[1];
    }
    *pos = (ASS_Vector) {
        .x = qr[0] >> SUBPIXEL_ORDER,
        .y = qr[1] >> SUBPIXEL_ORDER,
    };
    key->offset.x = qr[0] & ((1 << SUBPIXEL_ORDER) - 1);
    key->offset.y = qr[1] & ((1 << SUBPIXEL_ORDER) - 1);
    key->matrix_x.x = qm[0][0];  key->matrix_x.y = qm[0][1];
    key->matrix_y.x = qm[1][0];  key->matrix_y.y = qm[1][1];
    key->matrix_z.x = qm[2][0];  key->matrix_z.y = qm[2][1];
    return true;
}

static void restore_transform(double m[3][3], const BitmapHashKey *key)
{
    const ASS_Rect *bbox = &key->outline->cbox;
    double x0 = (bbox->x_min + bbox->x_max) / 2.0;
    double y0 = (bbox->y_min + bbox->y_max) / 2.0;
    double dx = (bbox->x_max - bbox->x_min) / 2.0 + 64;
    double dy = (bbox->y_max - bbox->y_min) / 2.0 + 64;

    // Arbitrary scale has chosen so that z0 = 1
    double q_x = POSITION_PRECISION / dx;
    double q_y = POSITION_PRECISION / dy;
    m[0][0] = key->matrix_x.x * q_x;
    m[0][1] = key->matrix_x.y * q_y;
    m[1][0] = key->matrix_y.x * q_x;
    m[1][1] = key->matrix_y.y * q_y;

    int32_t qmx = abs(key->matrix_x.x) + abs(key->matrix_x.y);
    int32_t qmy = abs(key->matrix_y.x) + abs(key->matrix_y.y);
    double scale_z = 1.0 / POSITION_PRECISION / FFMAX(qmx, qmy);
    m[2][0] = key->matrix_z.x * q_x * scale_z;  // qm_zx * q_zx
    m[2][1] = key->matrix_z.y * q_y * scale_z;  // qm_zy * q_zy

    m[0][2] = m[1][2] = 0;
    m[2][2] = 1 + fabs(m[2][0]) * dx + fabs(m[2][1]) * dy;
    m[2][2] = FFMIN(m[2][2], MAX_PERSP_SCALE);

    double center[2] = {
        key->offset.x * (64 >> SUBPIXEL_ORDER),
        key->offset.y * (64 >> SUBPIXEL_ORDER),
    };
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 3; j++)
            m[i][j] += m[2][j] * center[i];

    for (int i = 0; i < 3; i++)
        m[i][2] -= m[i][0] * x0 + m[i][1] * y0;
}

// Calculate bitmap memory footprint
static inline size_t bitmap_size(const Bitmap *bm)
{
    return bm->stride * bm->h;
}

static void motion_timing(const MotionState *motion, RenderContext *state,
                          int32_t *t1, int32_t *t2)
{
    int32_t duration = state->event->Duration;
    if (motion->has_timing) {
        *t1 = motion->t1;
        *t2 = motion->t2;
    } else {
        *t1 = 0;
        *t2 = duration;
    }

    if (*t1 <= 0 && *t2 <= 0) {
        *t1 = 0;
        *t2 = duration;
    }

    if (*t1 > *t2) {
        int32_t tmp = *t2;
        *t2 = *t1;
        *t1 = tmp;
    }
}

static double motion_progress_at(RenderContext *state,
                                 const MotionState *motion, int64_t t)
{
    int32_t t1, t2;
    motion_timing(motion, state, &t1, &t2);

    if (t <= t1)
        return 0.;
    if (t >= t2)
        return 1.;

    int32_t delta_t = (uint32_t) t2 - t1;
    if (!delta_t)
        return 1.;

    return ((double) (int32_t) ((uint32_t) t - t1)) / delta_t;
}

static ASS_DVector evaluate_base_motion_at(RenderContext *state, int64_t time)
{
    MotionState *m = &state->motion;
    switch (m->type) {
    case MOTION_POS:
        return (ASS_DVector) {
            m->relative_x ? state->pos_x + m->x1 : m->x1,
            m->relative_y ? state->pos_y + m->y1 : m->y1,
        };
    case MOTION_MOVE: {
        double k = motion_progress_at(state, m, time);
        double x = m->x1 + (m->x2 - m->x1) * k;
        double y = m->y1 + (m->y2 - m->y1) * k;
        return (ASS_DVector) {x, y};
    }
    case MOTION_MOVER: {
        double k = motion_progress_at(state, m, time);
        double x = m->x1 + (m->x2 - m->x1) * k;
        double y = m->y1 + (m->y2 - m->y1) * k;
        double angle = m->angle1 + (m->angle2 - m->angle1) * k;
        double radius = m->radius1 + (m->radius2 - m->radius1) * k;
        // VSFilterMod angles grow clockwise with 0° pointing right.
        double theta = -angle * (M_PI / 180.0);
        x += cos(theta) * radius;
        y += sin(theta) * radius;
        return (ASS_DVector) {x, y};
    }
    case MOTION_MOVES3: {
        double k = motion_progress_at(state, m, time);
        double inv = 1 - k;
        double x = inv * inv * m->x1 + 2 * inv * k * m->x2 + k * k * m->x3;
        double y = inv * inv * m->y1 + 2 * inv * k * m->y2 + k * k * m->y3;
        return (ASS_DVector) {x, y};
    }
    case MOTION_MOVES4: {
        double k = motion_progress_at(state, m, time);
        double inv = 1 - k;
        double inv2 = inv * inv;
        double k2 = k * k;
        double x = inv2 * inv * m->x1 + 3 * inv2 * k * m->x2 +
                   3 * inv * k2 * m->x3 + k2 * k * m->x4;
        double y = inv2 * inv * m->y1 + 3 * inv2 * k * m->y2 +
                   3 * inv * k2 * m->y3 + k2 * k * m->y4;
        return (ASS_DVector) {x, y};
    }
    default:
        return (ASS_DVector) {state->pos_x, state->pos_y};
    }
}

static ASS_DVector evaluate_motion_at(RenderContext *state, int64_t time)
{
    ASS_DVector pos = evaluate_base_motion_at(state, time);
    if (state->pos_override_x)
        pos.x = state->pos_offset.x;
    else if (state->pos_offset.x != 0.0)
        pos.x += state->pos_offset.x;
    if (state->pos_override_y)
        pos.y = state->pos_offset.y;
    else if (state->pos_offset.y != 0.0)
        pos.y += state->pos_offset.y;
    return pos;
}

static inline uint32_t jitter_rand15(uint32_t *state)
{
    *state = *state * 214013u + 2531011u;
    return (*state >> 16) & 0x7FFFu;
}

static int32_t jitter_extent_to_int(double value)
{
    if (value <= 0.0)
        return 0;
    int64_t rounded = llround(value);
    if (rounded < 0)
        rounded = 0;
    if (rounded > INT32_MAX)
        rounded = INT32_MAX;
    return (int32_t) rounded;
}

static ASS_DVector jitter_compute_offset(const JitterState *j, long long time_100ns)
{
    if (!j->enabled)
        return (ASS_DVector) {0, 0};

    int32_t left = jitter_extent_to_int(j->left);
    int32_t right = jitter_extent_to_int(j->right);
    int32_t up = jitter_extent_to_int(j->up);
    int32_t down = jitter_extent_to_int(j->down);

    if (!left && !right && !up && !down)
        return (ASS_DVector) {0, 0};

    double period = j->period;
    if (period < 1.0)
        period = 1.0;

    double bucket_d = floor((double) FFMAX(time_100ns, 0) / period);
    if (bucket_d < 0)
        bucket_d = 0;
    long long bucket = bucket_d >= (double) LLONG_MAX ? LLONG_MAX : (long long) bucket_d;
    uint32_t bucket32 = (uint32_t) bucket;

    uint32_t base_seed = (j->has_seed && j->has_period) ? j->seed : 0;
    uint32_t rseed = (base_seed + bucket32) * 100u;

    uint32_t state = rseed;
    int64_t xamp = (int64_t) left + right;
    if (xamp > INT32_MAX)
        xamp = INT32_MAX;
    int32_t x = 0;
    if (xamp > 0) {
        uint32_t rand_val = jitter_rand15(&state);
        x = (int32_t) (rand_val % (uint32_t) xamp) - left;
    }

    int64_t yamp = (int64_t) up + down;
    if (yamp > INT32_MAX)
        yamp = INT32_MAX;
    int32_t y = 0;
    if (yamp > 0) {
        uint32_t rand_val = jitter_rand15(&state);
        y = (int32_t) (rand_val % (uint32_t) yamp) - up;
    }

    return (ASS_DVector) {(double) x / 8.0, (double) y / 8.0};
}

#if DEBUG_LEVEL >= 2
static void jitter_run_debug_tests(void)
{
    static bool ran = false;
    if (ran)
        return;
    ran = true;

    JitterState def = ass_jitter_default_state();
    ASS_DVector off = jitter_compute_offset(&def, 0);
    assert(off.x == 0.0 && off.y == 0.0);

    JitterState no_period_a = ass_jitter_default_state();
    no_period_a.enabled = true;
    no_period_a.left = no_period_a.right = no_period_a.up = no_period_a.down = 8;
    JitterState no_period_b = no_period_a;
    no_period_a.seed = 1234;
    no_period_a.has_seed = true;
    no_period_b.seed = 5678;
    no_period_b.has_seed = true;
    ASS_DVector np_a = jitter_compute_offset(&no_period_a, 50000);
    ASS_DVector np_b = jitter_compute_offset(&no_period_b, 50000);
    assert(np_a.x == np_b.x && np_a.y == np_b.y);

    JitterState sample = ass_jitter_default_state();
    sample.enabled = true;
    sample.left = sample.right = sample.up = sample.down = 16;
    sample.has_period = true;
    sample.period = 200000.0;
    sample.has_seed = true;
    sample.seed = 7;

    long long bucket_time = (long long) (sample.period / 2);
    ASS_DVector bucket0 = jitter_compute_offset(&sample, bucket_time);
    ASS_DVector bucket0_repeat = jitter_compute_offset(&sample, bucket_time);
    assert(bucket0.x == bucket0_repeat.x && bucket0.y == bucket0_repeat.y);
    assert(bucket0.x == 0.5 && bucket0.y == 0.5);

    ASS_DVector bucket1 = jitter_compute_offset(&sample,
            (long long) (sample.period + bucket_time));
    assert(bucket1.x == 1.375 && bucket1.y == -0.5);
    assert(bucket0.x != bucket1.x || bucket0.y != bucket1.y);
}
#endif

static long long jitter_current_time(RenderContext *state)
{
    long long now = state->renderer->time - state->event->Start;
    if (now <= 0)
        return 0;
    long long limit = LLONG_MAX / 10000;
    return now > limit ? LLONG_MAX : now * 10000;
}

static void update_glyph_jitter_offsets_list(RenderContext *state,
                                             GlyphInfo *glyphs, int length,
                                             long long time_100ns)
{
    for (int i = 0; i < length; i++) {
        for (GlyphInfo *info = glyphs + i; info; info = info->next) {
            double dx = 0.0;
            double dy = 0.0;
            if (info->has_jitter) {
                ASS_DVector offset = jitter_compute_offset(&info->jitter, time_100ns);
                dx = x2scr_offset(state, offset.x);
                dy = y2scr_offset(state, offset.y);
            }
            info->jitter_dx = dx;
            info->jitter_dy = dy;
        }
    }
}

static ASS_DVector movevc_offset(RenderContext *state)
{
    MoveVCState *mv = &state->movevc;
    if (!mv->active)
        return (ASS_DVector) {0, 0};

    double x = mv->x1, y = mv->y1;
    if (mv->animated) {
        int32_t t1 = mv->has_timing ? mv->t1 : 0;
        int32_t t2 = mv->has_timing ? mv->t2 : state->event->Duration;
        int32_t delta_t = (uint32_t) t2 - t1;
        int t = state->renderer->time - state->event->Start;
        double k;
        if (t <= t1)
            k = 0.;
        else if (t >= t2)
            k = 1.;
        else if (delta_t)
            k = ((double) (int32_t) ((uint32_t) t - t1)) / delta_t;
        else
            k = 1.;
        x = k * (mv->x2 - mv->x1) + mv->x1;
        y = k * (mv->y2 - mv->y1) + mv->y1;
    }

    return (ASS_DVector) {x, y};
}

typedef struct {
    double scale;
    ASS_DVector offset;
} ClipTransform;

/* Both shapes use the same script-space affine transform. Never modify the
 * parsed coordinates or cached outline: each frame starts from that source. */
static ClipTransform clip_transform(const RenderContext *state,
                                     ASS_DVector center)
{
    double scale = state->clip_scale / 100.0;
    return (ClipTransform) {
        .scale = scale,
        .offset = {center.x * (1 - scale) + state->clip_pos.x,
                   center.y * (1 - scale) + state->clip_pos.y},
    };
}

/* Bounds of a Bezier axis, including interior extrema rather than its control
 * polygon. B-splines have already been converted to cubics by drawing parsing. */
static void clip_curve_bounds(const double p[4], int order,
                               double *min, double *max)
{
    *min = FFMIN(*min, FFMIN(p[0], p[order]));
    *max = FFMAX(*max, FFMAX(p[0], p[order]));
    if (order == 1)
        return;
    double a = order == 3 ? -p[0] + 3 * p[1] - 3 * p[2] + p[3] : 0;
    double b = (p[0] - 2 * p[1] + p[2]) * (order == 3 ? 2 : 1);
    double c = p[1] - p[0];
    double roots[2];
    int count = 0;
    if (a == 0) {
        if (b != 0)
            roots[count++] = -c / b;
    } else {
        double disc = b * b - 4 * a * c;
        if (disc >= 0) {
            double q = -0.5 * (b + copysign(sqrt(disc), b));
            roots[count++] = q / a;
            if (q != 0)
                roots[count++] = c / q;
        }
    }
    for (int i = 0; i < count; i++) {
        double t = roots[i];
        if (t <= 0 || t >= 1)
            continue;
        double v[4];
        memcpy(v, p, sizeof(v));
        for (int n = order; n > 0; n--)
            for (int j = 0; j < n; j++)
                v[j] += (v[j + 1] - v[j]) * t;
        *min = FFMIN(*min, v[0]);
        *max = FFMAX(*max, v[0]);
    }
}

static ASS_DVector clip_outline_center(OutlineHashValue *value)
{
    if (!value->clip_center_valid) {
        const ASS_Outline *ol = &value->outline[0];
        ASS_DRect box = {DBL_MAX, DBL_MAX, -DBL_MAX, -DBL_MAX};
        size_t point = 0, contour = 0;
        for (size_t i = 0; i < ol->n_segments; i++) {
            int order = ol->segments[i] & OUTLINE_COUNT_MASK;
            bool end = ol->segments[i] & OUTLINE_CONTOUR_END;
            double x[4] = {0}, y[4] = {0};
            for (int j = 0; j <= order; j++) {
                size_t k = end && j == order ? contour : point + j;
                x[j] = ol->points[k].x;
                y[j] = ol->points[k].y;
            }
            clip_curve_bounds(x, order, &box.x_min, &box.x_max);
            clip_curve_bounds(y, order, &box.y_min, &box.y_max);
            point += order;
            if (end)
                contour = point;
        }
        value->clip_center = (ASS_DVector) {
            (box.x_min + box.x_max) / 2,
            (box.y_min + box.y_max) / 2,
        };
        value->clip_center_valid = true;
    }
    return value->clip_center;
}

static ASS_DRect clip_rectangle(const RenderContext *state)
{
    ASS_DRect box = {state->clip_x0, state->clip_y0,
                     state->clip_x1, state->clip_y1};
    if (!state->clip_rectangle_set ||
            (state->clip_scale == 100 && !state->clip_pos.x && !state->clip_pos.y))
        return box;
    ASS_DVector center = {(box.x_min + box.x_max) / 2,
                           (box.y_min + box.y_max) / 2};
    ClipTransform tr = clip_transform(state, center);
    box.x_min = box.x_min * tr.scale + tr.offset.x;
    box.x_max = box.x_max * tr.scale + tr.offset.x;
    box.y_min = box.y_min * tr.scale + tr.offset.y;
    box.y_max = box.y_max * tr.scale + tr.offset.y;
    return box;
}

static int clip_screen_coord(const RenderContext *state, double value)
{
    if (!state->clip_rectangle_set ||
            (state->clip_scale == 100 && !state->clip_pos.x && !state->clip_pos.y))
        return lround(value);
    return lround(FFMINMAX(value, INT_MIN, INT_MAX));
}

/* Shared by legacy masks, RGBA masks and box painting; outline and transformed
 * bitmap caching remain in the normal pipeline. Pure movement reuses bitmaps. */
static Bitmap *vector_clip_bitmap(RenderContext *state, ASS_Vector *pos)
{
    ASS_Renderer *render_priv = state->renderer;

    OutlineHashKey ol_key;
    ol_key.type = OUTLINE_DRAWING;
    ol_key.u.drawing.text = state->clip_drawing_text;

    BitmapHashKey key = {0};
    key.outline = ass_cache_get(render_priv->cache.outline_cache, &ol_key, render_priv);
    if (!key.outline || !key.outline->valid)
        return NULL;

    /*
     * Aegisub emits partial vector clips while the first two vertices are
     * being placed. A path with fewer than three outline points cannot
     * bound a filled area. Do not rasterize it as a usable clip mask.
     */
    if (key.outline->outline[0].n_points < 3)
        return NULL;

    double m[3][3] = {{0}};
    int32_t scale_base = lshiftwrapi(1, state->clip_drawing_scale - 1);
    double w = scale_base > 0 ? (1.0 / scale_base) : 0;
    m[0][0] = state->screen_scale_x * w;
    m[1][1] = state->screen_scale_y * w;
    m[2][2] = 1;

    m[0][2] = int_to_d6(render_priv->settings.left_margin);
    m[1][2] = int_to_d6(render_priv->settings.top_margin);
    if (state->clip_scale != 100 || state->clip_pos.x || state->clip_pos.y) {
        ASS_DVector center = {0};
        if (state->clip_scale != 100) {
            center = clip_outline_center(key.outline);
            center.x *= w / 64;
            center.y *= w / 64;
        }
        ClipTransform tr = clip_transform(state, center);
        m[0][0] *= tr.scale;
        m[1][1] *= tr.scale;
        m[0][2] += tr.offset.x * state->screen_scale_x * 64;
        m[1][2] += tr.offset.y * state->screen_scale_y * 64;
    }
    ASS_DVector mvc = movevc_offset(state);
    if (mvc.x || mvc.y) {
        m[0][2] += mvc.x * state->screen_scale_x * 64;
        m[1][2] += mvc.y * state->screen_scale_y * 64;
    }

    if (!quantize_transform(m, pos, NULL, true, &key))
        return NULL;

    return ass_cache_get(render_priv->cache.bitmap_cache, &key, state);
}

/**
 * Iterate through a list of bitmaps and blend with clip vector, if
 * applicable. The blended bitmaps are added to a free list which is freed
 * at the start of a new frame.
 */
static void blend_vector_clip(RenderContext *state, ASS_Image *head)
{
    if (!state->clip_drawing_text.str)
        return;
    if (state->clip_scale == 0) {
        if (!state->clip_drawing_mode)
            for (ASS_Image *cur = head; cur; cur = cur->next)
                cur->w = cur->h = 0;
        return;
    }

    ASS_Renderer *render_priv = state->renderer;
    ASS_Vector pos;
    Bitmap *clip_bm = vector_clip_bitmap(state, &pos);
    if (!clip_bm || !clip_bm->buffer || !clip_bm->w || !clip_bm->h) {
        /* Empty normal vector clips hide the subtitle; inverse clips
         * exclude nothing. Keep both rendering backends consistent. */
        if (!state->clip_drawing_mode)
            for (ASS_Image *cur = head; cur; cur = cur->next)
                cur->w = cur->h = 0;
        return;
    }

    // Iterate through bitmaps and blend/clip them
    for (ASS_Image *cur = head; cur; cur = cur->next) {
        int ax, ay, aw, ah, as;
        int bw, bh, bs;
        int aleft, atop, bleft, btop;
        unsigned char *abuffer, *bbuffer, *nbuffer;

        abuffer = cur->bitmap;
        bbuffer = clip_bm->buffer;
        ax = cur->dst_x;
        ay = cur->dst_y;
        aw = cur->w;
        ah = cur->h;
        as = cur->stride;
        bw = clip_bm->w;
        bh = clip_bm->h;
        bs = clip_bm->stride;
        if (aw <= 0 || ah <= 0 || as <= 0 || bw <= 0 || bh <= 0 || bs <= 0)
            continue;

        int64_t ax0 = ax, ay0 = ay;
        int64_t bx0 = (int64_t) pos.x + clip_bm->left;
        int64_t by0 = (int64_t) pos.y + clip_bm->top;
        int64_t left = FFMAX(ax0, bx0);
        int64_t top = FFMAX(ay0, by0);
        int64_t right = FFMIN(ax0 + aw, bx0 + bw);
        int64_t bottom = FFMIN(ay0 + ah, by0 + bh);
        if (left >= right || top >= bottom) {
            if (!state->clip_drawing_mode)
                cur->w = cur->h = cur->stride = 0;
            continue;
        }
        aleft = (int) (left - ax0);
        atop = (int) (top - ay0);
        int w = (int) (right - left);
        int h = (int) (bottom - top);
        bleft = (int) (left - bx0);
        btop = (int) (top - by0);

        unsigned align = 1 << render_priv->engine.align_order;
        if (state->clip_drawing_mode) {
            if ((size_t) as > (SIZE_MAX - align) / (size_t) ah)
                break;
            size_t alloc_size = (size_t) as * ah + align;
            nbuffer = ass_aligned_alloc_tagged(
                align, alloc_size, false, ASS_ALIGNED_ALLOC_CLIP_BUFFER, cur);
            if (!nbuffer)
                break;

            memcpy(nbuffer, abuffer, (size_t) as * ah);
            render_priv->engine.imul_bitmaps(
                                             nbuffer + (size_t) atop * as + aleft, as,
                                             bbuffer + (size_t) btop * bs + bleft, bs,
                                             w, h);
        } else {
            if (left > INT_MAX || top > INT_MAX) {
                cur->w = cur->h = cur->stride = 0;
                continue;
            }

            size_t ns_size = ass_align(align, (size_t) w);
            if (ns_size < (size_t) w || ns_size > INT_MAX ||
                ns_size > (SIZE_MAX - align) / (size_t) h)
                break;
            unsigned ns = (unsigned) ns_size;
            nbuffer = ass_aligned_alloc_tagged(
                align, ns_size * h + align, false,
                ASS_ALIGNED_ALLOC_CLIP_BUFFER, cur);
            if (!nbuffer)
                break;

            render_priv->engine.mul_bitmaps(nbuffer, ns,
                                            abuffer + (size_t) atop * as + aleft, as,
                                            bbuffer + (size_t) btop * bs + bleft, bs,
                                            w, h);
            cur->dst_x = (int) left;
            cur->dst_y = (int) top;
            cur->w = w;
            cur->h = h;
            cur->stride = ns;
        }

        ASS_ImagePriv *priv = (ASS_ImagePriv *) cur;
        unsigned char *old_buffer = priv->buffer;
        priv->buffer = cur->bitmap = nbuffer;
        ass_aligned_retag(nbuffer, ASS_ALIGNED_ALLOC_CLIP_BUFFER, priv,
                          "legacy vector clip ownership transfer");
        ass_cache_dec_ref(priv->source);
        priv->source = NULL;
        if (old_buffer)
            ass_aligned_free_tagged(
                old_buffer, ASS_ALIGNED_ALLOC_LEGACY_IMAGE, priv);
    }
}

static void blend_vector_clip_rgba(RenderContext *state, ASS_ImageRGBA *head)
{
    if (!head || !state->clip_drawing_text.str)
        return;

    if (state->clip_scale == 0) {
        if (!state->clip_drawing_mode)
            for (ASS_ImageRGBA *cur = head; cur; cur = cur->next)
                cur->w = cur->h = 0;
        return;
    }

    ASS_Renderer *render_priv = state->renderer;
    ASS_Vector pos;
    Bitmap *clip_bm = vector_clip_bitmap(state, &pos);
    if (!clip_bm || !clip_bm->buffer || !clip_bm->w || !clip_bm->h) {
        if (!state->clip_drawing_mode)
            for (ASS_ImageRGBA *cur = head; cur; cur = cur->next)
                cur->w = cur->h = 0;
        return;
    }

    for (ASS_ImageRGBA *cur = head; cur; cur = cur->next) {
        int aw, ah, as;
        int ax, ay, bw, bh, bs;
        int aleft, atop, bleft, btop;
        ASS_ImageRGBAPriv *rgba_priv = ass_rgba_image_private(
            cur, "vector clip");
        if (!rgba_priv || !ass_rgba_image_view_valid(cur, "vector clip source"))
            break;
        uint8_t *abuffer = cur->rgba;
        uint8_t *bbuffer = clip_bm->buffer;
        ax = cur->dst_x;
        ay = cur->dst_y;
        aw = cur->w;
        ah = cur->h;
        as = cur->stride;
        bw = clip_bm->w;
        bh = clip_bm->h;
        bs = clip_bm->stride;
        if (aw <= 0 || ah <= 0 || as <= 0 || bw <= 0 || bh <= 0 || bs <= 0)
            continue;

        int64_t ax0 = ax, ay0 = ay;
        int64_t bx0 = (int64_t) pos.x + clip_bm->left;
        int64_t by0 = (int64_t) pos.y + clip_bm->top;
        int64_t left = FFMAX(ax0, bx0);
        int64_t top = FFMAX(ay0, by0);
        int64_t right = FFMIN(ax0 + aw, bx0 + bw);
        int64_t bottom = FFMIN(ay0 + ah, by0 + bh);
        if (left >= right || top >= bottom) {
            if (!state->clip_drawing_mode)
                cur->w = cur->h = 0;
            continue;
        }
        aleft = (int) (left - ax0);
        atop = (int) (top - ay0);
        int wclip = (int) (right - left);
        int hclip = (int) (bottom - top);
        bleft = (int) (left - bx0);
        btop = (int) (top - by0);

        if (state->clip_drawing_mode) {
            size_t alloc_size;
            uint8_t *nbuffer =
                ass_rgba_alloc_buffer_stride(render_priv, as, ah,
                                             rgba_priv->alloc_size,
                                             &alloc_size,
                                             "vector clip replacement");
            if (!nbuffer)
                break;
            for (int y = 0; y < ah; y++)
                memcpy(nbuffer + (size_t) y * as,
                       abuffer + (size_t) y * as, (size_t) aw * 4);
            for (int y = 0; y < hclip; y++) {
                uint8_t *dst = nbuffer + (size_t) (atop + y) * as +
                    (size_t) aleft * 4;
                uint8_t *src_mask = bbuffer + (size_t) (btop + y) * bs + bleft;
                for (int x = 0; x < wclip; x++) {
                    uint8_t mval = 255 - src_mask[x];
                    for (int c = 0; c < 4; c++) {
                        dst[4 * x + c] =
                            (uint8_t) ((dst[4 * x + c] * mval + 127) / 255);
                    }
                }
            }
            ass_rgba_image_replace_buffer(cur, nbuffer, alloc_size, aw, ah, as);
        } else {
            if (left > INT_MAX || top > INT_MAX) {
                cur->w = cur->h = 0;
                continue;
            }

            int ns;
            size_t alloc_size;
            uint8_t *nbuffer =
                ass_rgba_alloc_buffer(render_priv, wclip, hclip,
                                      rgba_priv->alloc_size, &ns,
                                      &alloc_size,
                                      "vector clip replacement");
            if (!nbuffer)
                break;
            for (int y = 0; y < hclip; y++) {
                uint8_t *dst = nbuffer + (size_t) y * ns;
                uint8_t *src = abuffer + (size_t) (atop + y) * as +
                    (size_t) aleft * 4;
                uint8_t *src_mask = bbuffer + (size_t) (btop + y) * bs + bleft;
                for (int x = 0; x < wclip; x++) {
                    uint8_t mval = src_mask[x];
                    for (int c = 0; c < 4; c++) {
                        dst[4 * x + c] =
                            (uint8_t) ((src[4 * x + c] * mval + 127) / 255);
                    }
                }
            }
            cur->dst_x = (int) left;
            cur->dst_y = (int) top;
            ass_rgba_image_crop_blend_rgb(cur, aleft, atop,
                                          wclip, hclip, ns);
            ass_rgba_image_replace_buffer(cur, nbuffer, alloc_size,
                                          wclip, hclip, ns);
        }
    }
}

/**
 * \brief Convert TextInfo struct to ASS_Image list
 * Splits glyphs in halves when needed (for \kf karaoke).
 */
static ASS_Image *render_text(RenderContext *state, ASS_ImageRGBA **out_rgba)
{
    ASS_Image *head;
    ASS_Image **tail = &head;
    ASS_ImageRGBA *rgba_head = NULL;
    ASS_ImageRGBA **rgba_tail = out_rgba ? &rgba_head : NULL;
    unsigned n_bitmaps = state->text_info.n_bitmaps;
    CombinedBitmapInfo *bitmaps = state->text_info.combined_bitmaps;

#define CHAT_BITMAP_VISIBLE(info) \
    (!(info)->chat_part || (info)->chat_part <= state->chat_visible)
#define CHAT_BITMAP_CLIP(info) do { \
    int part = (info)->chat_part; \
    state->chat_clip_active = part > 0 && state->chat_clips; \
    if (state->chat_clip_active) { \
        state->chat_clip_x0 = state->chat_clips[part].x0; \
        state->chat_clip_x1 = state->chat_clips[part].x1; \
    } \
    select_scroll_clip(state, (info)->scroll_id); \
} while (0)

    for (unsigned i = 0; i < n_bitmaps; i++) {
        CombinedBitmapInfo *info = &bitmaps[i];
        if (!CHAT_BITMAP_VISIBLE(info))
            continue;
        CHAT_BITMAP_CLIP(info);
        if (!info->bm_s || state->bs4_box_mode)
            continue;
        if (!info->furi_base_karaoke &&
                info->effect_type == EF_KARAOKE_REVEAL &&
                info->effect_timing <= 0)
            continue;

        if (info->furi_base_karaoke)
            tail = render_furi_base_reveal_regions(
                state, info, info->bm_s, info->c[3], IMAGE_TYPE_SHADOW, 3,
                tail, info->image, out_rgba ? &rgba_tail : NULL);
        else
            tail = render_glyph(
                state, info, info->bm_s, info->x, info->y, info->c[3], 0,
                1000000, tail, IMAGE_TYPE_SHADOW, info->image, 3, 3,
                out_rgba ? &rgba_tail : NULL);
    }

    for (unsigned i = 0; i < n_bitmaps; i++) {
        CombinedBitmapInfo *info = &bitmaps[i];
        if (!CHAT_BITMAP_VISIBLE(info))
            continue;
        CHAT_BITMAP_CLIP(info);
        for (int layer = ASS_BORDER_LAYERS_MAX - 1; layer >= 0; layer--)
            tail = render_border_layer(state, info, layer, tail,
                                       out_rgba ? &rgba_tail : NULL);
    }

    for (unsigned i = 0; i < n_bitmaps; i++) {
        CombinedBitmapInfo *info = &bitmaps[i];
        if (!CHAT_BITMAP_VISIBLE(info))
            continue;
        CHAT_BITMAP_CLIP(info);
        if (!info->bm)
            continue;

        if (info->furi_base_karaoke) {
            tail = render_furi_base_character_regions(
                state, info, tail, out_rgba ? &rgba_tail : NULL);
        } else if ((info->effect_type == EF_KARAOKE)
                || (info->effect_type == EF_KARAOKE_KO)
                || (info->effect_type == EF_KARAOKE_REVEAL)) {
            if (info->effect_timing > 0)
                tail =
                    render_glyph(state, info, info->bm, info->x, info->y,
                                 info->c[0], 0, 1000000, tail,
                                 IMAGE_TYPE_CHARACTER, info->image, 0, 0,
                                 out_rgba ? &rgba_tail : NULL);
            else if (info->effect_type != EF_KARAOKE_REVEAL)
                tail =
                    render_glyph(state, info, info->bm, info->x, info->y,
                                 info->c[1], 0, 1000000, tail,
                                 IMAGE_TYPE_CHARACTER, info->image, 1, 1,
                                 out_rgba ? &rgba_tail : NULL);
        } else if (info->effect_type == EF_KARAOKE_KF) {
            tail =
                render_glyph(state, info, info->bm, info->x, info->y, info->c[0],
                             info->c[1], info->effect_timing, tail,
                             IMAGE_TYPE_CHARACTER, info->image, 0, 1,
                             out_rgba ? &rgba_tail : NULL);
        } else
            tail =
                render_glyph(state, info, info->bm, info->x, info->y, info->c[0],
                             0, 1000000, tail, IMAGE_TYPE_CHARACTER, info->image,
                             0, 0, out_rgba ? &rgba_tail : NULL);
    }

    *tail = 0;
    state->chat_clip_active = false;
    state->scroll_clip_active = false;
    blend_vector_clip(state, head);
    if (out_rgba) {
        blend_vector_clip_rgba(state, rgba_head);
        *out_rgba = rgba_head;
    }
    return head;
#undef CHAT_BITMAP_VISIBLE
#undef CHAT_BITMAP_CLIP
}

static void compute_string_bbox(TextInfo *text, ASS_DRect *bbox)
{
    if (text->native_vertical) {
        *bbox = text->vertical_bbox;
        return;
    }
    if (text->length > 0) {
        bbox->x_min = +32000;
        bbox->x_max = -32000;
        bbox->y_min = -text->lines[0].asc;
        bbox->y_max = bbox->y_min + text->height;

        for (int i = 0; i < text->length; i++) {
            GlyphInfo *info = text->glyphs + i;
            if (info->skip) continue;
            double s = d6_to_double(info->pos.x);
            double e = s + d6_to_double(info->cluster_advance.x);
            bbox->x_min = FFMIN(bbox->x_min, s);
            bbox->x_max = FFMAX(bbox->x_max, e);
        }
    } else
        bbox->x_min = bbox->x_max = bbox->y_min = bbox->y_max = 0;
}

static void add_glyph_list_visual_bbox(GlyphInfo *glyphs, int length,
                                       ASS_DRect *bbox)
{
    for (int i = 0; i < length; i++) {
        GlyphInfo *root = glyphs + i;
        if (root->skip)
            continue;

        for (GlyphInfo *info = root; info; info = info->next) {
            double x = d6_to_double(info->pos.x);
            double y = d6_to_double(info->pos.y);
            bbox->x_min = FFMIN(bbox->x_min, x + d6_to_double(info->bbox.x_min));
            bbox->x_max = FFMAX(bbox->x_max, x + d6_to_double(info->bbox.x_max));
            bbox->y_min = FFMIN(bbox->y_min, y + d6_to_double(info->bbox.y_min));
            bbox->y_max = FFMAX(bbox->y_max, y + d6_to_double(info->bbox.y_max));
        }
    }
}

static void add_furi_to_bbox(TextInfo *text_info, ASS_DRect *bbox)
{
    for (int i = 0; i < text_info->n_furi_groups; i++) {
        FuriGroup *group = &text_info->furi_groups[i];
        add_glyph_list_visual_bbox(group->glyphs, group->length, bbox);
    }
}

/* Warped text uses the ink geometry after distortion and line/ruby placement.
 * Keep this separate from compute_string_bbox: ordinary \an deliberately
 * uses layout metrics, including the first line's ascent. */
static bool compute_warp_text_bbox(TextInfo *text_info, ASS_DRect *bbox)
{
    bool seen = false, warped = false;
    bbox->x_min = bbox->x_max = bbox->y_min = bbox->y_max = 0.0;

    for (int list = -1; list < text_info->n_furi_groups; list++) {
        GlyphInfo *glyphs = list < 0 ? text_info->glyphs :
            text_info->furi_groups[list].glyphs;
        int length = list < 0 ? text_info->length :
            text_info->furi_groups[list].length;
        for (int i = 0; i < length; i++) {
            GlyphInfo *root = glyphs + i;
            if (root->skip || root->drawing_text.str)
                continue;
            for (GlyphInfo *info = root; info; info = info->next) {
                OutlineHashValue *outline = info->has_distort_outline ?
                    info->distorted_outline : info->outline;
                if (!outline || (!outline->outline[0].n_points &&
                                 !outline->outline[1].n_points))
                    continue;
                warped |= info->has_distort_outline;
                double x = d6_to_double(info->pos.x);
                double y = d6_to_double(info->pos.y);
                double x0 = x + d6_to_double(info->bbox.x_min);
                double x1 = x + d6_to_double(info->bbox.x_max);
                double y0 = y + d6_to_double(info->bbox.y_min);
                double y1 = y + d6_to_double(info->bbox.y_max);
                if (!seen) {
                    bbox->x_min = x0;
                    bbox->x_max = x1;
                    bbox->y_min = y0;
                    bbox->y_max = y1;
                    seen = true;
                } else {
                    bbox->x_min = FFMIN(bbox->x_min, x0);
                    bbox->x_max = FFMAX(bbox->x_max, x1);
                    bbox->y_min = FFMIN(bbox->y_min, y0);
                    bbox->y_max = FFMAX(bbox->y_max, y1);
                }
            }
        }
    }
    return seen && warped;
}

static ASS_Style *handle_selective_style_overrides(RenderContext *state,
                                                   ASS_Style *rstyle)
{
    // The script style is the one the event was declared with.
    ASS_Renderer *render_priv = state->renderer;
    ASS_Style *script = render_priv->track->styles +
                        state->event->Style;
    // The user style was set with ass_set_selective_style_override().
    ASS_Style *user = &render_priv->user_override_style;
    ASS_Style *new = &state->override_style_temp_storage;
    int explicit = state->explicit;
    int requested = render_priv->settings.selective_style_overrides;
    double scale;

    // Either the event's style, or the style forced with a \r tag.
    if (!rstyle)
        rstyle = script;

    // Create a new style that contains a mix of the original style and
    // user_style (the user's override style). Copy only fields from the
    // script's style that are deemed necessary.
    *new = *rstyle;

    state->apply_font_scale =
        !explicit || !(requested & ASS_OVERRIDE_BIT_SELECTIVE_FONT_SCALE);

    // On positioned events, do not apply most overrides.
    if (explicit)
        requested = 0;

    if (requested & ASS_OVERRIDE_BIT_STYLE)
        requested |= ASS_OVERRIDE_BIT_FONT_NAME |
                     ASS_OVERRIDE_BIT_FONT_SIZE_FIELDS |
                     ASS_OVERRIDE_BIT_COLORS |
                     ASS_OVERRIDE_BIT_BORDER |
                     ASS_OVERRIDE_BIT_ATTRIBUTES;

    // Copies fields even not covered by any of the other bits.
    if (requested & ASS_OVERRIDE_FULL_STYLE)
        *new = *user;

    // The user style is supposed to be independent of the script resolution.
    // Treat the user style's values as if they were specified for a script with
    // PlayResY=288, and rescale the values to the current script.
    scale = render_priv->track->PlayResY / 288.0;

    if (requested & ASS_OVERRIDE_BIT_FONT_SIZE_FIELDS) {
        new->FontSize = user->FontSize * scale;
        new->Spacing = user->Spacing * scale;
        new->ScaleX = user->ScaleX;
        new->ScaleY = user->ScaleY;
    }

    if (requested & ASS_OVERRIDE_BIT_FONT_NAME) {
        new->FontName = user->FontName;
        new->treat_fontname_as_pattern = user->treat_fontname_as_pattern;
    }

    if (requested & ASS_OVERRIDE_BIT_COLORS) {
        new->PrimaryColour = user->PrimaryColour;
        new->SecondaryColour = user->SecondaryColour;
        new->OutlineColour = user->OutlineColour;
        new->BackColour = user->BackColour;
    }

    if (requested & ASS_OVERRIDE_BIT_ATTRIBUTES) {
        new->Bold = user->Bold;
        new->Italic = user->Italic;
        new->Underline = user->Underline;
        new->StrikeOut = user->StrikeOut;
    }

    if (requested & ASS_OVERRIDE_BIT_BORDER) {
        new->BorderStyle = user->BorderStyle;
        new->Outline = user->Outline * scale;
        new->Shadow = user->Shadow * scale;
    }

    if (requested & ASS_OVERRIDE_BIT_BLUR)
        new->Blur = user->Blur * scale;

    if (requested & ASS_OVERRIDE_BIT_ALIGNMENT)
        new->Alignment = user->Alignment;

    if (requested & ASS_OVERRIDE_BIT_JUSTIFY)
        new->Justify = user->Justify;

    if (requested & ASS_OVERRIDE_BIT_MARGINS) {
        new->MarginL = user->MarginL;
        new->MarginR = user->MarginR;
        new->MarginV = user->MarginV;
    }

    if (!new->FontName)
        new->FontName = rstyle->FontName;

    state->style = new;
    state->overrides = requested;

    return new;
}

ASS_Vector ass_layout_res(ASS_Renderer *render_priv)
{
    ASS_Track *track = render_priv->track;
    if (track->LayoutResX > 0 && track->LayoutResY > 0)
        return (ASS_Vector) { track->LayoutResX, track->LayoutResY };

    ASS_Settings *settings = &render_priv->settings;
    if (settings->storage_width > 0 && settings->storage_height > 0)
        return (ASS_Vector) { settings->storage_width, settings->storage_height };

    if (settings->par <= 0 || settings->par == 1 ||
            !render_priv->frame_content_width || !render_priv->frame_content_height)
        return (ASS_Vector) { track->PlayResX, track->PlayResY };
    if (settings->par > 1)
        return (ASS_Vector) {
            FFMAX(1, lround(track->PlayResY * render_priv->frame_content_width
                    / render_priv->frame_content_height / settings->par)),
            track->PlayResY
        };
    else
        return (ASS_Vector) {
            track->PlayResX,
            FFMAX(1, lround(track->PlayResX * render_priv->frame_content_height
                    / render_priv->frame_content_width * settings->par))
        };
}

static void init_font_scale(RenderContext *state)
{
    ASS_Renderer *render_priv = state->renderer;
    ASS_Settings *settings_priv = &render_priv->settings;

    double font_scr_w = render_priv->frame_content_width;
    double font_scr_h = render_priv->frame_content_height;
    if (!state->explicit && render_priv->settings.use_margins) {
        font_scr_w = render_priv->fit_width;
        font_scr_h = render_priv->fit_height;
    }

    state->screen_scale_x = font_scr_w / render_priv->track->PlayResX;
    state->screen_scale_y = font_scr_h / render_priv->track->PlayResY;

    ASS_Vector layout_res = ass_layout_res(render_priv);
    state->blur_scale_x = font_scr_w / layout_res.x;
    state->blur_scale_y = font_scr_h / layout_res.y;
    if (render_priv->track->ScaledBorderAndShadow) {
        state->border_scale_x = state->screen_scale_x;
        state->border_scale_y = state->screen_scale_y;
    } else {
        state->border_scale_x = state->blur_scale_x;
        state->border_scale_y = state->blur_scale_y;
    }

    if (state->apply_font_scale) {
        state->screen_scale_x *= settings_priv->font_size_coeff;
        state->screen_scale_y *= settings_priv->font_size_coeff;
        state->border_scale_x *= settings_priv->font_size_coeff;
        state->border_scale_y *= settings_priv->font_size_coeff;
        state->blur_scale_x *= settings_priv->font_size_coeff;
        state->blur_scale_y *= settings_priv->font_size_coeff;
    }
}

static ASS_ActorColorcode *find_actor_colorcode(ASS_Track *track,
                                                const char *name)
{
    if (!name || !*name)
        return NULL;

    ASS_ColorcodeConfig *cfg = &track->colorcode;
    for (int i = 0; i < cfg->n_actors; i++)
        if (!strcmp(cfg->actors[i].Name, name))
            return &cfg->actors[i];
    return NULL;
}

static bool colorcode_style_allowed(ASS_Track *track, const char *style_name)
{
    if (!style_name)
        return false;

    ASS_ColorcodeConfig *cfg = &track->colorcode;
    if (!cfg->has_applied_styles)
        return true;

    for (int i = 0; i < cfg->n_applied_styles; i++)
        if (!strcmp(cfg->applied_styles[i], style_name))
            return true;
    return false;
}

static void capture_effective_default_state(RenderContext *state)
{
    ColumnStyleState *style = &state->default_style;
    *style = (ColumnStyleState) {0};
    capture_column_style(state, style, COLUMN_STYLE_ALL_FIELDS);
    style->gradient = state->gradient;
    style->mangetsu_gradient = state->mangetsu_gradient;
    style->pattern = state->pattern;
    style->image_fill = state->image_fill;
    style->blend_mode = state->blend_mode;
    memcpy(style->border_layers, state->border_layers,
           sizeof(style->border_layers));
}

static void apply_colorcode_text(RenderContext *state, char *text)
{
    if (!text || !*text)
        return;

    bool old_mode = state->colorcode_parse;
    state->colorcode_parse = true;

    char *p = text;
    while (*p) {
        if (*p == '{') {
            char *end = strchr(p, '}');
            if (!end)
                break;
            ass_parse_override_block(state, p + 1, end);
            p = end + 1;
        } else if (*p == '\\') {
            char *end = strchr(p, '{');
            if (!end)
                end = p + strlen(p);
            ass_parse_tags(state, p, end, 1.0, false);
            p = end;
        } else {
            p++;
        }
    }

    state->colorcode_parse = old_mode;
}

static void apply_actor_colorcoding(RenderContext *state,
                                    bool explicit_style_reset,
                                    const char *active_style_name)
{
    ASS_Track *track = state->renderer->track;
    ASS_ColorcodeConfig *cfg = &track->colorcode;
    const char *event_name = state->event && state->event->Name ?
                             state->event->Name : "";

    ass_msg(state->renderer->library, MSGL_DBG2,
            "Mangetsu colorcoding check: actors=%d has_whitelist=%d "
            "whitelist_entries=%d event_name='%s' active_style='%s'",
            cfg->n_actors, cfg->has_applied_styles, cfg->n_applied_styles,
            event_name, active_style_name ? active_style_name : "");

    if (!cfg->n_actors) {
        ass_msg(state->renderer->library, MSGL_DBG2,
                "Mangetsu colorcoding skipped: no actor metadata");
        return;
    }

    if (!cfg->has_applied_styles && explicit_style_reset) {
        ass_msg(state->renderer->library, MSGL_DBG2,
                "Mangetsu colorcoding skipped: explicit style reset without whitelist");
        return;
    }

    bool style_allowed = colorcode_style_allowed(track, active_style_name);
    ass_msg(state->renderer->library, MSGL_DBG2,
            "Mangetsu colorcoding style whitelist result: %d", style_allowed);
    if (!style_allowed)
        return;

    ASS_ActorColorcode *actor = find_actor_colorcode(track, event_name);
    ass_msg(state->renderer->library, MSGL_DBG2,
            "Mangetsu colorcoding actor lookup for '%s': %s",
            event_name, actor ? "found" : "missing");
    if (!actor)
        return;

    apply_colorcode_text(state, actor->Text);
    ass_msg(state->renderer->library, MSGL_DBG2,
            "Mangetsu colorcoding applied actor '%s': text='%s'",
            actor->Name, actor->Text ? actor->Text : "");
}

static bool border_style_tag_value_valid(int32_t value)
{
    return value == 1 || value == 3 || value == 4 || value == 5;
}

static unsigned override_codepoint(char **p, char *end)
{
    if (ass_override_peek(p, end) < 0x80)
        return ass_override_next(p, end);

    // Include one lookahead byte for the existing UTF-8 decoder's malformed
    // sequence behavior. An annotation can even split UTF-8 bytes.
    char bytes[7] = {0};
    char *positions[6];
    char *next = *p;
    for (int i = 0; i < 6; i++) {
        bytes[i] = ass_override_next(&next, end);
        positions[i] = next;
        if (!bytes[i])
            break;
    }
    char *decoded = bytes;
    unsigned code = ass_utf8_get_char(&decoded);
    *p = positions[decoded - bytes - 1];
    return code;
}

static bool parse_border_style_tag_value(char *start, char *end,
                                         int *border_style)
{
    // strtoll's leading ASCII whitespace/sign rules, without constructing a
    // numeric string. Only 1/3/4/5 can succeed, so saturation at 6 is sufficient
    // even for arbitrarily long zero prefixes or overflowing invalid values.
    char *next = start;
    unsigned c = override_codepoint(&next, end);
    while (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') {
        start = next;
        c = override_codepoint(&next, end);
    }
    bool negative = c == '-';
    if (c == '+' || c == '-')
        start = next;
    int value = 0;
    bool have_digit = false;
    for (;;) {
        next = start;
        int digit = ass_unicode_decimal_value(override_codepoint(&next, end));
        if (digit < 0)
            break;
        value = FFMIN(value * 10 + digit, 6);
        have_digit = true;
        start = next;
    }
    // Preserve the old rskip_spaces/skip_spaces end-pointer check: trailing
    // whitespace made a BorderStyle value invalid, even ASCII space/tab.
    ass_override_peek(&start, end);
    if (!have_digit || negative || start != end || !border_style_tag_value_valid(value))
        return false;
    *border_style = value;
    return true;
}

static char *skip_parenthesized_tag_args(char *p, char *end, char **value_end)
{
    if (value_end)
        *value_end = end;
    if (ass_override_peek(&p, end) != '(')
        return p;

    int depth = 1;
    p++;
    while (p < end && depth > 0) {
        unsigned char c = ass_override_peek(&p, end);
        if (p == end)
            break;
        // The old scan trimmed a final ')' even with unbalanced parentheses.
        if (value_end)
            *value_end = c == ')' ? p : end;
        if (c == '(')
            depth++;
        else if (c == ')')
            depth--;
        p++;
    }
    return p;
}

static bool scan_border_style_override_block(RenderContext *state,
                                             char *p, char *end)
{
    while (p < end) {
        while (ass_override_peek(&p, end) && *p != '\\')
            p++;
        if (p >= end)
            return false;

        p++;
        ass_override_spaces(&p, end);
        char *name = p;
        while (ass_override_peek(&p, end) && *p != '\\' && *p != '(')
            p++;
        char *name_end = p;

        char *value = name;
        bool bs_tag = ass_override_prefix(&value, name_end, "bs");
        if (bs_tag) {
            int border_style;
            if (parse_border_style_tag_value(value, name_end,
                                             &border_style)) {
                state->line_border_style_set = true;
                state->line_border_style = border_style;
                state->parsed_tags |= PARSED_BORDER_STYLE;
                return true;
            }

            if (p < end && *p == '(') {
                char *arg_start = p + 1;
                char *value_end;
                skip_parenthesized_tag_args(p, end, &value_end);
                if (parse_border_style_tag_value(arg_start, value_end,
                                                 &border_style)) {
                    state->line_border_style_set = true;
                    state->line_border_style = border_style;
                    state->parsed_tags |= PARSED_BORDER_STYLE;
                    return true;
                }
            }
        }

        if (ass_override_prefix(&name, name_end, "t") &&
                !ass_override_peek(&name, name_end) &&
                p < end && *p == '(') {
            char *arg_start = p + 1;
            char *nested_end;
            char *arg_end = skip_parenthesized_tag_args(p, end, &nested_end);
            if (scan_border_style_override_block(state, arg_start,
                                                 nested_end))
                return true;
            p = arg_end;
        } else {
            p = skip_parenthesized_tag_args(p, end, NULL);
        }
    }

    return false;
}

static void scan_line_border_style_override(RenderContext *state, char *text)
{
    char *p = text;

    while (*p) {
        if (*p == '{') {
            char *end = strchr(p, '}');
            if (!end)
                break;
            if (scan_border_style_override_block(state, p + 1, end))
                return;
            p = end + 1;
        } else {
            p++;
        }
    }
}

/**
 * \brief partially reset render_context to style values
 * Works like {\r}: resets some style overrides
 */
void ass_reset_render_context_explicit(RenderContext *state, ASS_Style *style,
                                       bool explicit_style_reset)
{
    ASS_Renderer *render_priv = state->renderer;
    ASS_Style *script_style = render_priv->track->styles + state->event->Style;
    const char *active_style_name = style ? style->Name : script_style->Name;

    style = handle_selective_style_overrides(state, style);

    /* These alignment controls are override-level, unlike the event-wide
     * positioning tags. A style reset restores their legacy fallbacks. */
    state->line_alignment = 0;
    state->vertical_text_alignment = 0;
    state->warp_text_alignment = 0;
    state->curved_text_align = 0;
    state->clip_pos = (ASS_DVector) {0};
    state->clip_scale = 100;
    state->scroll_duration = 300;
    state->scroll_show_lines = 0;
    state->scroll_id = 0;

    init_font_scale(state);

    state->c[0] = style->PrimaryColour;
    state->c[1] = style->SecondaryColour;
    state->c[2] = style->OutlineColour;
    state->c[3] = style->BackColour;
    ass_gradient_state_reset(&state->gradient, state->c);
    ass_mangetsu_gradient_state_reset(&state->mangetsu_gradient);
    state->pattern = (TextPatternPaint) {0};
    state->secondary_outline = (KaraokeOutlinePaint) {0};
    for (int i = 0; i < 4; i++)
        clear_image_fill_layer(&state->image_fill.layer[i]);
    state->flags =
        (style->Underline ? DECO_UNDERLINE : 0) |
        (style->StrikeOut ? DECO_STRIKETHROUGH : 0);
    state->decoration_color_set = false;
    state->decoration_alpha_set = false;
    state->decoration_color = 0;
    state->decoration_alpha = 0;
    state->font_size = style->FontSize;

    state->family.str = style->FontName;
    state->family.len = strlen(style->FontName);
    state->treat_family_as_pattern = style->treat_fontname_as_pattern;
    state->bold = style->Bold;
    state->italic = style->Italic;
    ass_update_font(state);

    state->border_style = state->line_border_style_set ?
        state->line_border_style : style->BorderStyle;
    state->bs4_box_mode = state->border_style == 4;
    state->box_extra_x = 0;
    state->box_extra_y = 0;
    state->box_corner_radius = 0;
    for (int i = 0; i < ASS_BORDER_LAYERS_MAX; i++) {
        state->box_border_layers[i] = (BorderLayerState) {
            .enabled = false,
            .has_color = false,
            .has_alpha = false,
            .size_x = 0,
            .size_y = 0,
            .color = style->BackColour,
        };
        ass_gradient_values_reset(&state->box_border_layers[i].gradient,
                                  style->BackColour);
    }
    state->border_x = style->Outline;
    state->border_y = style->Outline;
    state->border_layers[0] = (BorderLayerState) {
        .enabled = style->Outline > 0,
        .has_color = true,
        .has_alpha = true,
        .size_x = style->Outline,
        .size_y = style->Outline,
        .color = style->OutlineColour,
    };
    state->border_layers[0].gradient = state->gradient.layer[2];
    for (int i = 1; i < ASS_BORDER_LAYERS_MAX; i++) {
        state->border_layers[i] = (BorderLayerState) {
            .enabled = false,
            .has_color = false,
            .has_alpha = false,
            .size_x = 0,
            .size_y = 0,
            .color = style->OutlineColour,
        };
        ass_gradient_values_reset(&state->border_layers[i].gradient,
                                  style->OutlineColour);
    }
    state->scale_x = style->ScaleX;
    state->scale_y = style->ScaleY;
    state->object_scale = 1.0;
    state->soft_scale = 1.0;
    state->hspacing = style->Spacing;
    state->fsvp = 0;
    state->fshp = 0;
    state->furi_enabled = true;
    state->furi_scale_x = 50.0;
    state->furi_scale_y = 50.0;
    state->furi_hspacing = 0.0;
    state->furi_style = 0;
    state->furi_offset_x = 0.0;
    state->furi_offset_y = 0.0;
    state->furi_auto_placement = true;
    state->furi_position_explicit = false;
    state->furi_place_auto = false;
    state->furi_change_pos = false;
    state->be = 0;
    state->blur_x = style->Blur;
    state->blur_y = style->Blur;
    state->shadow_x = style->Shadow;
    state->shadow_y = style->Shadow;
    state->frx = state->fry = 0.;
    state->frz = style->Angle;
    state->frs = 0.;
    state->fax = state->fay = 0.;
    state->font_encoding = style->Encoding;
    state->jitter = ass_jitter_default_state();
    state->z = 0.0;
    state->ortho = false;
    state->blend_mode = ASS_BLEND_NORMAL;
    state->rnd_x = state->rnd_y = state->rnd_z = 0;
    state->rnd_seed = 0;
    state->needs_rgba = false;
    state->distort_enabled = false;
    state->distort_extended = false;
    state->distort = (ASS_DistortParams) {
        .u1 = 1.0, .u2 = 1.0, .v2 = 1.0, .v3 = 1.0,
    };

    memset(state->chat_side, 0, sizeof(state->chat_side));
    state->chat_receipt = (ASS_ChatReceipt) {.mark = 2};
    state->chat_reset_serial++;
    capture_effective_default_state(state);
    apply_actor_colorcoding(state, explicit_style_reset, active_style_name);
    state->chat_bubble = (ChatBubbleStyle) {
        .fill = state->c[2],
        .border = state->c[2],
        .border_size = 0,
    };
    state->chat_title = (ChatTitleStyle) {0};
    capture_effective_default_state(state);
}

void ass_reset_render_context(RenderContext *state, ASS_Style *style)
{
    ass_reset_render_context_explicit(state, style, style != NULL);
}

/**
 * \brief Start new event. Reset state.
 */
static void
init_render_context(RenderContext *state, ASS_Event *event, bool chat_enabled)
{
    ASS_Renderer *render_priv = state->renderer;

    state->event = event;
    state->parsed_tags = 0;
    state->evt_type = EVENT_NORMAL;

    state->wrap_style = render_priv->track->WrapStyle;

    state->pos_x = 0;
    state->pos_y = 0;
    state->pos_offset = (ASS_DVector) {0};
    state->pos_override_x = state->pos_override_y = false;
    state->org_x = 0;
    state->org_y = 0;
    state->have_origin = 0;
    state->clip_x0 = 0;
    state->clip_y0 = 0;
    state->clip_x1 = render_priv->track->PlayResX;
    state->clip_y1 = render_priv->track->PlayResY;
    state->clip_mode = 0;
    state->clip_rectangle_set = false;
    state->clip_transform_tags = ass_event_has_clip_transforms(event->Text);
    state->chat_clip_active = false;
    state->chat_enabled = chat_enabled;
    state->chat_side_only_parse = false;
    state->chat_reset_serial = 0;
    state->scroll_clip_active = false;
    state->chat_visible = INT_MAX;
    state->chat_clips = NULL;
    state->detect_collisions = 1;
    state->fade = 0;
    state->drawing_scale = 0;
    state->pbo = 0;
    state->movevc = (MoveVCState) {0};
    state->motion = (MotionState) {0};
    state->motion.type = MOTION_NONE;
    state->pos_transforms = NULL;
    state->n_pos_transforms = 0;
    state->max_pos_transforms = 0;
    state->pos_transform_context = false;
    state->pos_transform_t1 = 0;
    state->pos_transform_t2 = 0;
    state->pos_transform_accel = 1.0;
    state->jitter = ass_jitter_default_state();
    state->rnd_x = state->rnd_y = state->rnd_z = 0;
    state->rnd_seed = 0;
    state->mangetsu_gradient_next_id = 0;
    state->pattern_cycle_serial = 0;
    state->event_has_cycle = false;
    state->column_event = false;
    state->column_active = false;
    state->column_row = 0;
    state->column_index = 0;
    state->column_base_style = (ColumnStyleState) {0};
    state->colorcode_parse = false;
    state->effect_type = EF_NONE;
    state->effect_timing = 0;
    state->effect_skip_timing = 0;
    state->reset_effect = false;
    state->karaoke_segment = -1;
    state->karaoke_cursor = 0;
    state->karaoke_effect_type = EF_NONE;
    state->karaoke_timeline_enabled = false;
    state->karaoke_only_parse = false;
    state->karaoke_alloc_failed = false;
    state->karaoke_tag_serial = 0;
    state->karaoke_clip_enabled = false;
    state->karaoke_clip_x0 = 0;
    state->karaoke_clip_x1 = 0;
    state->native_vertical = false;
    state->vertical_profile = 0;
    state->vertical_direction = 0;
    state->vertical_text_alignment = 0;
    state->vertical_spacing = 0.0;
    state->vertical_column_spacing = 0.0;
    state->text_info.native_vertical = false;
    state->fade_color = (FadeColorState) {0};
    state->distort_enabled = false;
    state->distort_extended = false;
    state->distort = (ASS_DistortParams) {
        .u1 = 1.0, .u2 = 1.0, .v2 = 1.0, .v3 = 1.0,
    };
    state->curved_path_outline = NULL;
    state->curved_text_align = 0;
    state->warp_text_alignment = 0;
    state->curved_text_x = 0.0;
    state->curved_text_y = 0.0;
    state->line_border_style_set = false;
    state->line_border_style = 0;

    ass_apply_transition_effects(state);
    if (!state->chat_enabled)
        scan_line_border_style_override(state, event->Text);
    state->explicit = state->evt_type != EVENT_NORMAL ||
                      ass_event_has_hard_overrides(event->Text);

    ass_reset_render_context(state, NULL);
    state->alignment = state->style->Alignment;
    state->text_alignment = state->alignment;
    state->justify = state->style->Justify;
}

static int column_halign_from_numpad(int align)
{
    switch (align) {
    case 2:
    case 5:
    case 8:
        return HALIGN_CENTER;
    case 3:
    case 6:
    case 9:
        return HALIGN_RIGHT;
    default:
        return HALIGN_LEFT;
    }
}

static bool ensure_column_count(TextInfo *text_info, int count)
{
    if (count <= text_info->max_columns)
        return true;

    int old_max = text_info->max_columns;
    int new_max = old_max ? old_max : 8;
    while (new_max < count) {
        if (new_max > INT_MAX / 2)
            return false;
        new_max *= 2;
    }

    if (!ASS_REALLOC_ARRAY(text_info->column_defaults, new_max) ||
            !ASS_REALLOC_ARRAY(text_info->column_widths, new_max) ||
            !ASS_REALLOC_ARRAY(text_info->column_spacing, new_max) ||
            !ASS_REALLOC_ARRAY(text_info->column_align, new_max))
        return false;

    for (int i = old_max; i < new_max; i++) {
        text_info->column_defaults[i] = (ColumnStyleDefault) {0};
        text_info->column_widths[i] = 0.0;
        text_info->column_spacing[i] = 1.0;
        text_info->column_align[i] = HALIGN_LEFT;
    }
    text_info->max_columns = new_max;
    return true;
}

static bool begin_column_layout(RenderContext *state)
{
    TextInfo *text_info = &state->text_info;

    if (!text_info->column_glyphs) {
        text_info->column_glyphs =
            malloc(text_info->max_glyphs * sizeof(*text_info->column_glyphs));
        if (!text_info->column_glyphs)
            return false;
        text_info->max_column_glyphs = text_info->max_glyphs;
    }

    if (!ensure_column_count(text_info, 1))
        return false;

    state->column_event = true;
    state->column_active = false;
    state->column_row = 0;
    state->column_index = 0;
    text_info->column_rows = 1;
    text_info->column_count = 1;
    return true;
}

static bool ensure_column_glyph_capacity(TextInfo *text_info)
{
    if (text_info->length < text_info->max_column_glyphs)
        return true;

    int new_max = text_info->max_glyphs;
    if (new_max <= text_info->max_column_glyphs)
        return false;
    if (!ASS_REALLOC_ARRAY(text_info->column_glyphs, new_max))
        return false;
    text_info->max_column_glyphs = new_max;
    return true;
}

static void capture_column_style(RenderContext *state, ColumnStyleState *style,
                                 unsigned fields)
{
    style->mask |= fields;

    if (fields & COLUMN_STYLE_FONT_NAME)
        style->family = state->family;
    if (fields & COLUMN_STYLE_FONT_SIZE)
        style->font_size = state->font_size;
    if (fields & COLUMN_STYLE_BOLD)
        style->bold = state->bold;
    if (fields & COLUMN_STYLE_ITALIC)
        style->italic = state->italic;
    if (fields & (COLUMN_STYLE_UNDERLINE | COLUMN_STYLE_STRIKEOUT))
        style->flags = state->flags;

    for (int i = 0; i < 4; i++)
        if (fields & (COLUMN_STYLE_COLOR_MASK(i) | COLUMN_STYLE_ALPHA_MASK(i)))
            style->c[i] = state->c[i];

    if (fields & COLUMN_STYLE_DECORATION_COLOR) {
        style->decoration_color_set = state->decoration_color_set;
        style->decoration_color = state->decoration_color;
    }
    if (fields & COLUMN_STYLE_DECORATION_ALPHA) {
        style->decoration_alpha_set = state->decoration_alpha_set;
        style->decoration_alpha = state->decoration_alpha;
    }
    if (fields & COLUMN_STYLE_BORDER_X)
        style->border_x = state->border_x;
    if (fields & COLUMN_STYLE_BORDER_Y)
        style->border_y = state->border_y;
    if (fields & COLUMN_STYLE_SHADOW_X)
        style->shadow_x = state->shadow_x;
    if (fields & COLUMN_STYLE_SHADOW_Y)
        style->shadow_y = state->shadow_y;
    if (fields & COLUMN_STYLE_BLUR) {
        style->blur_x = state->blur_x;
        style->blur_y = state->blur_y;
    }
    if (fields & COLUMN_STYLE_BE)
        style->be = state->be;
}

static void sync_state_layer1_border(RenderContext *state)
{
    state->border_layers[0].enabled = state->border_x > 0 || state->border_y > 0;
    state->border_layers[0].has_color = true;
    state->border_layers[0].has_alpha = true;
    state->border_layers[0].size_x = state->border_x;
    state->border_layers[0].size_y = state->border_y;
    state->border_layers[0].color = state->c[2];
    state->border_layers[0].gradient = state->gradient.layer[2];
}

static void capture_column_base_style(RenderContext *state)
{
    ColumnStyleState *style = &state->column_base_style;
    *style = (ColumnStyleState) {0};
    capture_column_style(state, style, COLUMN_STYLE_ALL_FIELDS);
    style->gradient = state->gradient;
    style->mangetsu_gradient = state->mangetsu_gradient;
    style->pattern = state->pattern;
    style->image_fill = state->image_fill;
    style->blend_mode = state->blend_mode;
    memcpy(style->border_layers, state->border_layers,
           sizeof(style->border_layers));
}

static void apply_column_base_style(RenderContext *state)
{
    const ColumnStyleState *style = &state->column_base_style;
    state->family = style->family;
    state->font_size = style->font_size;
    state->bold = style->bold;
    state->italic = style->italic;
    state->flags = (state->flags & ~(DECO_UNDERLINE | DECO_STRIKETHROUGH)) |
                   (style->flags & (DECO_UNDERLINE | DECO_STRIKETHROUGH));
    memcpy(state->c, style->c, sizeof(state->c));
    state->gradient = style->gradient;
    state->mangetsu_gradient = style->mangetsu_gradient;
    state->pattern = style->pattern;
    state->image_fill = style->image_fill;
    state->blend_mode = style->blend_mode;
    state->decoration_color_set = style->decoration_color_set;
    state->decoration_alpha_set = style->decoration_alpha_set;
    state->decoration_color = style->decoration_color;
    state->decoration_alpha = style->decoration_alpha;
    state->border_x = style->border_x;
    state->border_y = style->border_y;
    memcpy(state->border_layers, style->border_layers,
           sizeof(state->border_layers));
    state->shadow_x = style->shadow_x;
    state->shadow_y = style->shadow_y;
    state->blur_x = style->blur_x;
    state->blur_y = style->blur_y;
    state->be = style->be;
    ass_update_font(state);
}

static void apply_column_default_style(RenderContext *state,
                                       const ColumnStyleState *style)
{
    unsigned fields = style->mask;
    bool update_font = false;
    bool sync_border = false;

    if (fields & COLUMN_STYLE_FONT_NAME) {
        state->family = style->family;
        update_font = true;
    }
    if (fields & COLUMN_STYLE_FONT_SIZE)
        state->font_size = style->font_size;
    if (fields & COLUMN_STYLE_BOLD) {
        state->bold = style->bold;
        update_font = true;
    }
    if (fields & COLUMN_STYLE_ITALIC) {
        state->italic = style->italic;
        update_font = true;
    }
    if (fields & COLUMN_STYLE_UNDERLINE) {
        if (style->flags & DECO_UNDERLINE)
            state->flags |= DECO_UNDERLINE;
        else
            state->flags &= ~DECO_UNDERLINE;
    }
    if (fields & COLUMN_STYLE_STRIKEOUT) {
        if (style->flags & DECO_STRIKETHROUGH)
            state->flags |= DECO_STRIKETHROUGH;
        else
            state->flags &= ~DECO_STRIKETHROUGH;
    }

    for (int i = 0; i < 4; i++) {
        if (fields & COLUMN_STYLE_COLOR_MASK(i)) {
            state->c[i] = (style->c[i] & 0xFFFFFF00) | _a(state->c[i]);
            ass_gradient_disable_color(&state->gradient, i, state->c[i], 1);
            if (i == 2)
                ass_mangetsu_gradient_layer_reset(
                    &state->mangetsu_gradient.border[0]);
            else
                ass_mangetsu_gradient_layer_reset(
                    &state->mangetsu_gradient.layer[i]);
            clear_image_fill_layer(&state->image_fill.layer[i]);
            if (i == 2)
                sync_border = true;
        }
        if (fields & COLUMN_STYLE_ALPHA_MASK(i)) {
            state->c[i] = (state->c[i] & 0xFFFFFF00) | _a(style->c[i]);
            ass_gradient_disable_alpha(&state->gradient, i,
                                       _a(state->c[i]), 1);
            if (i == 2) {
                ass_mangetsu_gradient_layer_reset(
                    &state->mangetsu_gradient.border_alpha[0]);
                sync_border = true;
            } else {
                ass_mangetsu_gradient_layer_reset(
                    &state->mangetsu_gradient.alpha[i]);
            }
        }
    }

    if (fields & COLUMN_STYLE_DECORATION_COLOR) {
        state->decoration_color_set = style->decoration_color_set;
        state->decoration_color = style->decoration_color;
        ass_mangetsu_gradient_layer_reset(&state->mangetsu_gradient.layer[4]);
    }
    if (fields & COLUMN_STYLE_DECORATION_ALPHA) {
        state->decoration_alpha_set = style->decoration_alpha_set;
        state->decoration_alpha = style->decoration_alpha;
        ass_mangetsu_gradient_layer_reset(&state->mangetsu_gradient.alpha[4]);
    }
    if (fields & COLUMN_STYLE_BORDER_X) {
        state->border_x = style->border_x;
        sync_border = true;
    }
    if (fields & COLUMN_STYLE_BORDER_Y) {
        state->border_y = style->border_y;
        sync_border = true;
    }
    if (sync_border)
        sync_state_layer1_border(state);
    if (fields & COLUMN_STYLE_SHADOW_X)
        state->shadow_x = style->shadow_x;
    if (fields & COLUMN_STYLE_SHADOW_Y)
        state->shadow_y = style->shadow_y;
    if (fields & COLUMN_STYLE_BLUR) {
        state->blur_x = style->blur_x;
        state->blur_y = style->blur_y;
    }
    if (fields & COLUMN_STYLE_BE)
        state->be = style->be;

    if (update_font)
        ass_update_font(state);
}

static ColumnStyleDefault *get_column_default(RenderContext *state)
{
    TextInfo *text_info = &state->text_info;
    int column = state->column_index;
    if (column < 0 || !ensure_column_count(text_info, column + 1))
        return NULL;
    return &text_info->column_defaults[column];
}

static void finish_column_cell(RenderContext *state)
{
    if (!state->column_event || !state->column_active)
        return;

    ColumnStyleDefault *def = get_column_default(state);
    if (def && !def->set && def->style.mask)
        def->set = true;
}

static void apply_column_cell_style(RenderContext *state)
{
    if (!state->column_event || !state->column_active)
        return;

    apply_column_base_style(state);

    ColumnStyleDefault *def = get_column_default(state);
    if (def && def->set)
        apply_column_default_style(state, &def->style);
}

static void clear_column_defaults(TextInfo *text_info)
{
    for (int i = 0; i < text_info->max_columns; i++)
        text_info->column_defaults[i] = (ColumnStyleDefault) {0};
}

void ass_column_update_default(RenderContext *state, unsigned fields)
{
    if (!state->column_event || !state->column_active || !fields)
        return;

    ColumnStyleDefault *def = get_column_default(state);
    if (!def || def->set)
        return;

    capture_column_style(state, &def->style, fields);
}

void ass_column_set_mode(RenderContext *state, bool active)
{
    if (!state->column_event)
        return;

    if (state->column_active == active)
        return;

    if (active) {
        capture_column_base_style(state);
        state->column_active = true;
        apply_column_cell_style(state);
    } else {
        finish_column_cell(state);
        state->column_active = false;
        clear_column_defaults(&state->text_info);
        apply_column_base_style(state);
    }
}

void ass_column_set_align(RenderContext *state, int align)
{
    if (!state->column_event || !state->column_active)
        return;

    TextInfo *text_info = &state->text_info;
    int column = state->column_index;
    if (column < 0 || !ensure_column_count(text_info, column + 1))
        return;

    ColumnStyleDefault *def = &text_info->column_defaults[column];
    if (def->set)
        return;

    def->style.mask |= COLUMN_STYLE_ALIGN;
    text_info->column_align[column] = column_halign_from_numpad(align);
}

void ass_column_set_spacing(RenderContext *state, double spacing)
{
    if (!state->column_event || !state->column_active)
        return;

    TextInfo *text_info = &state->text_info;
    int column = state->column_index;
    if (column < 0 || !ensure_column_count(text_info, column + 1))
        return;

    text_info->column_spacing[column] = spacing <= 0 ? 1.0 : spacing;
}

static void free_temporary_composite(CompositeHashValue *image)
{
    ass_free_bitmap(&image->bm);
    ass_free_bitmap(&image->bm_o);
    ass_free_bitmap(&image->bm_s);
    for (int j = 0; j < ASS_BORDER_LAYERS_MAX - 1; j++)
        ass_free_bitmap(&image->bm_border[j]);
}

static void free_owned_source_image(CombinedBitmapInfo *info)
{
    if (!info->owned_source_image)
        return;
    free_temporary_composite(info->owned_source_image);
    free(info->owned_source_image);
    info->owned_source_image = NULL;
}

static void free_distortion_resources(RenderContext *state)
{
    TextInfo *text_info = &state->text_info;
    for (int i = 0; i < text_info->length; i++) {
        for (GlyphInfo *info = text_info->glyphs + i; info; info = info->next)
            ass_free_glyph_render_resources(info);
    }

    for (unsigned i = 0; i < text_info->n_bitmaps; i++) {
        CombinedBitmapInfo *info = &text_info->combined_bitmaps[i];
        free_owned_source_image(info);
        if (info->temp_image) {
            free_temporary_composite(info->temp_image);
            free(info->temp_image);
            info->temp_image = NULL;
        }
        if (info->has_distortion && info->bitmaps) {
            free(info->bitmaps);
            info->bitmaps = NULL;
        }
        info->has_distortion = false;
    }
}

static void free_render_context(RenderContext *state)
{
    free_scroll_contexts(state);
    free_distortion_resources(state);
    free_furi_groups(&state->text_info);
    free_column_layout(&state->text_info);
    free(state->text_info.karaoke_segments);
    state->text_info.karaoke_segments = NULL;
    state->text_info.n_karaoke_segments = 0;
    state->text_info.max_karaoke_segments = 0;
    state->font = NULL;
    state->family.str = NULL;
    state->family.len = 0;
    state->clip_drawing_text.str = NULL;
    state->clip_drawing_text.len = 0;
    state->curved_path_outline = NULL;
    state->text_info.length = 0;
    state->column_event = false;
    state->column_active = false;
    state->column_row = 0;
    state->column_index = 0;
    state->column_base_style = (ColumnStyleState) {0};
    free(state->pos_transforms);
    state->pos_transforms = NULL;
    state->n_pos_transforms = 0;
    state->max_pos_transforms = 0;
    ass_free_override_buffers(state);
    while (state->cycle_palettes) {
        CyclePalette *next = state->cycle_palettes->next;
        free(state->cycle_palettes);
        state->cycle_palettes = next;
    }
}

/**
 * \brief Get normal and outline (border) glyphs
 * \param info out: struct filled with extracted data
 * Tries to get both glyphs from cache.
 * If they can't be found, gets a glyph from font face, generates outline,
 * and add them to cache.
 */
static void
get_outline_glyph(RenderContext *state, GlyphInfo *info)
{
    ASS_Renderer *priv = state->renderer;
    OutlineHashValue *val;
    ASS_DVector scale, offset = {0};

    int32_t asc, desc;
    OutlineHashKey key;
    if (info->drawing_text.str) {
        key.type = OUTLINE_DRAWING;
        key.u.drawing.text = info->drawing_text;
        val = ass_cache_get(priv->cache.outline_cache, &key, priv);
        if (!val || !val->valid)
            return;

        int32_t scale_base = lshiftwrapi(1, info->drawing_scale - 1);
        double w = scale_base > 0 ? (1.0 / scale_base) : 0;
        scale.x = info->scale_x * w * state->screen_scale_x / priv->par_scale_x;
        scale.y = info->scale_y * w * state->screen_scale_y;
        desc = 64 * info->drawing_pbo;
        asc = val->asc - desc;

        offset.y = -asc * scale.y;
    } else {
        key.type = OUTLINE_GLYPH;
        GlyphHashKey *k = &key.u.glyph;
        k->font = info->font;
        k->size = info->font_size;
        k->face_index = info->face_index;
        k->glyph_index = info->glyph_index;
        k->bold = info->bold;
        k->italic = info->italic;
        k->flags = info->flags;

        val = ass_cache_get(priv->cache.outline_cache, &key, priv);
        if (!val || !val->valid)
            return;

        scale.x = info->scale_x;
        scale.y = info->scale_y;
        asc  = val->asc;
        desc = val->desc;
    }

    info->outline = val;
    info->transform.scale = scale;
    info->transform.offset = offset;

    info->bbox.x_min = ass_lrint(val->cbox.x_min * scale.x + offset.x);
    info->bbox.y_min = ass_lrint(val->cbox.y_min * scale.y + offset.y);
    info->bbox.x_max = ass_lrint(val->cbox.x_max * scale.x + offset.x);
    info->bbox.y_max = ass_lrint(val->cbox.y_max * scale.y + offset.y);

    if (info->drawing_text.str || priv->settings.shaper == ASS_SHAPING_SIMPLE) {
        info->cluster_advance.x = info->advance.x = ass_lrint(val->advance * scale.x);
        info->cluster_advance.y = info->advance.y = 0;
    }
    info->asc  = ass_lrint(asc  * scale.y);
    info->desc = ass_lrint(desc * scale.y);
}

size_t ass_outline_construct(void *key, void *value, void *priv)
{
    ASS_Renderer *render_priv = priv;
    OutlineHashKey *outline_key = key;
    OutlineHashValue *v = value;
    memset(v, 0, sizeof(*v));

    switch (outline_key->type) {
    case OUTLINE_GLYPH:
        {
            GlyphHashKey *k = &outline_key->u.glyph;
            FT_Face face = k->font->faces[k->face_index];
            FT_Face decoration_face = k->font->faces[0];
            ass_face_set_size(face, k->size);
            if (face != decoration_face &&
                    (k->flags & (DECO_UNDERLINE | DECO_STRIKETHROUGH)))
                ass_face_set_size(decoration_face, k->size);
            if (!ass_font_get_glyph(k->font, k->face_index, k->glyph_index,
                                    render_priv->settings.hinting))
                return 1;
            if (!ass_get_glyph_outline(&v->outline[0], &v->advance,
                                       face, decoration_face,
                                       k->flags))
                return 1;
            ass_font_get_asc_desc(k->font, k->face_index,
                                  &v->asc, &v->desc);
            break;
        }
    case OUTLINE_DRAWING:
        {
            ASS_Rect bbox;
            const char *text = outline_key->u.drawing.text.str;  // always zero-terminated
            if (!ass_drawing_parse(&v->outline[0], &bbox, text, render_priv->library))
                return 1;
            /*
             * Move-only ASS drawings have no contours but still determine
             * layout width and ascent. Legacy KFX uses them to position text
             * after \p0\r. Curved text checks for a drawable path separately.
             * Keep malformed outlines and out-of-range bounds rejected.
             */
            if (!!v->outline[0].n_points != !!v->outline[0].n_segments ||
                    bbox.x_min > bbox.x_max || bbox.y_min > bbox.y_max ||
                    (int64_t) bbox.x_max - bbox.x_min > INT_MAX ||
                    (int64_t) bbox.y_max - bbox.y_min > INT_MAX) {
                ass_outline_free(&v->outline[0]);
                return 1;
            }

            v->advance = bbox.x_max - bbox.x_min;
            v->asc = bbox.y_max - bbox.y_min;
            v->desc = 0;
            break;
        }
    case OUTLINE_BORDER:
        {
            BorderHashKey *k = &outline_key->u.border;
            if (!k->border.x && !k->border.y)
                break;
            if (!k->outline->outline[0].n_points)
                break;

            ASS_Outline src;
            if (!ass_outline_scale_pow2(&src, &k->outline->outline[0],
                                        k->scale_ord_x, k->scale_ord_y))
                return 1;
            if (!ass_outline_stroke(&v->outline[0], &v->outline[1], &src,
                                    k->border.x * STROKER_PRECISION,
                                    k->border.y * STROKER_PRECISION,
                                    STROKER_PRECISION,
                                    k->miter_join)) {
                ass_msg(render_priv->library, MSGL_WARN, "Cannot stroke outline");
                ass_outline_free(&v->outline[0]);
                ass_outline_free(&v->outline[1]);
                ass_outline_free(&src);
                return 1;
            }
            ass_outline_free(&src);
            break;
        }
    case OUTLINE_BOX:
        {
            ASS_Outline *ol = &v->outline[0];
            if (!ass_outline_alloc(ol, 4, 4))
                return 1;
            ol->points[0].x = ol->points[3].x = 0;
            ol->points[1].x = ol->points[2].x = 64;
            ol->points[0].y = ol->points[1].y = 0;
            ol->points[2].y = ol->points[3].y = 64;
            ol->segments[0] = OUTLINE_LINE_SEGMENT;
            ol->segments[1] = OUTLINE_LINE_SEGMENT;
            ol->segments[2] = OUTLINE_LINE_SEGMENT;
            ol->segments[3] = OUTLINE_LINE_SEGMENT | OUTLINE_CONTOUR_END;
            ol->n_points = ol->n_segments = 4;
            break;
        }
    case OUTLINE_ROUNDED_BOX:
        {
            int32_t rx = outline_key->u.rounded_box.radius_x;
            int32_t ry = outline_key->u.rounded_box.radius_y;
            if (rx <= 0 || ry <= 0 ||
                rx > BS4_ROUNDED_BOX_SCALE / 2 ||
                ry > BS4_ROUNDED_BOX_SCALE / 2)
                return 1;

            ASS_Outline *ol = &v->outline[0];
            if (!ass_outline_alloc(ol, 16, 8))
                return 1;

            const int32_t size = BS4_ROUNDED_BOX_SCALE;
            const int32_t kx = ass_lrint(rx * 0.5522847498307936);
            const int32_t ky = ass_lrint(ry * 0.5522847498307936);
            ASS_Vector points[16] = {
                {rx, 0}, {size - rx, 0}, {size - rx + kx, 0},
                {size, ry - ky}, {size, ry}, {size, size - ry},
                {size, size - ry + ky}, {size - rx + kx, size},
                {size - rx, size}, {rx, size}, {rx - kx, size},
                {0, size - ry + ky}, {0, size - ry}, {0, ry},
                {0, ry - ky}, {rx - kx, 0},
            };
            char segments[8] = {
                OUTLINE_LINE_SEGMENT, OUTLINE_CUBIC_SPLINE,
                OUTLINE_LINE_SEGMENT, OUTLINE_CUBIC_SPLINE,
                OUTLINE_LINE_SEGMENT, OUTLINE_CUBIC_SPLINE,
                OUTLINE_LINE_SEGMENT,
                OUTLINE_CUBIC_SPLINE | OUTLINE_CONTOUR_END,
            };
            memcpy(ol->points, points, sizeof(points));
            memcpy(ol->segments, segments, sizeof(segments));
            ol->n_points = 16;
            ol->n_segments = 8;
            break;
        }
    default:
        return 1;
    }

    rectangle_reset(&v->cbox);
    ass_outline_update_cbox(&v->outline[0], &v->cbox);
    ass_outline_update_cbox(&v->outline[1], &v->cbox);
    if (v->cbox.x_min > v->cbox.x_max || v->cbox.y_min > v->cbox.y_max)
        v->cbox.x_min = v->cbox.y_min = v->cbox.x_max = v->cbox.y_max = 0;
    v->valid = true;
    return 1;
}

/**
 * \brief Calculate outline transformation matrix
 */
static void calc_transform_matrix(RenderContext *state,
                                  GlyphInfo *info, double m[3][3], double z_basis[3])
{
    ASS_Renderer *render_priv = state->renderer;

    double frx = ASS_PI / 180 * info->frx;
    double fry = ASS_PI / 180 * info->fry;
    double frz = ASS_PI / 180 * info->frz;

    double sx = -sin(frx), cx = cos(frx);
    double sy =  sin(fry), cy = cos(fry);
    double sz = -sin(frz), cz = cos(frz);

    double fax = info->fax * info->scale_x / info->scale_y;
    double fay = info->fay * info->scale_y / info->scale_x;
    double dist_base = 20000 * state->blur_scale_y;
    double z_shift = info->z * state->blur_scale_y * 64.0;
    if (!isfinite(z_shift))
        z_shift = 0.0;
    double dist = dist_base;
    if (!isfinite(dist))
        dist = dist_base;
    if (dist < 1.0)
        dist = 1.0;
    else if (dist > 1e9)
        dist = 1e9;
    double x1[3] = { 1, fax, info->shift.x + info->asc * fax };
    double y1[3] = { fay, 1, info->shift.y };
    double z1[3] = { 0, 0, z_shift };

    /* Curved text rotates each shaped cluster in its own local frame.  Keep
     * the cluster's already-positioned translation fixed here; the ordinary
     * frz/frx/fry pass below still rotates the complete event around \org or
     * the ASS anchor. */
    if (info->curved_angle != 0.0) {
        double angle = ASS_PI / 180 * info->curved_angle;
        double local_s = -sin(angle), local_c = cos(angle);
        for (int i = 0; i < 3; i++) {
            double tx = i == 2 ? info->shift.x : 0.0;
            double ty = i == 2 ? info->shift.y : 0.0;
            double lx = x1[i] - tx;
            double ly = y1[i] - ty;
            x1[i] = lx * local_c - ly * local_s + tx;
            y1[i] = lx * local_s + ly * local_c + ty;
        }
    }

    double x2[3], y2[3], z2[3];
    for (int i = 0; i < 3; i++) {
        x2[i] = x1[i] * cz - y1[i] * sz;
        y2[i] = x1[i] * sz + y1[i] * cz;
        z2[i] = z1[i];
    }

    double y3[3], z3[3];
    for (int i = 0; i < 3; i++) {
        y3[i] = y2[i] * cx - z2[i] * sx;
        z3[i] = y2[i] * sx + z2[i] * cx;
    }

    double x4[3], z4[3];
    for (int i = 0; i < 3; i++) {
        x4[i] = x2[i] * cy - z3[i] * sy;
        z4[i] = x2[i] * sy + z3[i] * cy;
    }

    // VSFilterMod \ortho1: orthographic projection (no perspective divide by z).
    // Keep the rotated/sheared x/y basis and z-coupling into x/y (x4/y3),
    // but use a pure affine transform with constant depth.
    if (info->ortho) {
        double offs_x = info->pos.x - info->shift.x * render_priv->par_scale_x;
        double offs_y = info->pos.y - info->shift.y;

        for (int i = 0; i < 3; i++) {
            m[0][i] = x4[i] * render_priv->par_scale_x;
            m[1][i] = y3[i];
        }
        m[0][2] += offs_x;
        m[1][2] += offs_y;
        m[2][0] = 0.0;
        m[2][1] = 0.0;
        m[2][2] = 1.0;
        if (z_basis) {
            z_basis[0] = -cx * sy * render_priv->par_scale_x;
            z_basis[1] = -sx;
            z_basis[2] = 0;
        }
        return;
    }

    z4[2] += dist;

    double scale_x = dist * render_priv->par_scale_x;
    double offs_x = info->pos.x - info->shift.x * render_priv->par_scale_x;
    double offs_y = info->pos.y - info->shift.y;
    for (int i = 0; i < 3; i++) {
        m[0][i] = z4[i] * offs_x + x4[i] * scale_x;
        m[1][i] = z4[i] * offs_y + y3[i] * dist;
        m[2][i] = z4[i];
    }
    if (z_basis) {
        /* Local Z undergoes frx/fry, and participates in the perspective
         * denominator even when both angles are zero. */
        z_basis[0] = cx * cy * offs_x - cx * sy * scale_x;
        z_basis[1] = cx * cy * offs_y - sx * dist;
        z_basis[2] = cx * cy;
    }
}

/**
 * \brief Get bitmaps for a glyph
 * \param info glyph info
 * Tries to get glyph bitmaps from bitmap cache.
 * If they can't be found, they are generated by rotating and rendering the glyph.
 * After that, bitmaps are added to the cache.
 * They are returned in info->bm (glyph), info->bm_o (outline).
 */
static bool setup_border_outline_key(RenderContext *state, GlyphInfo *info,
                                     OutlineHashValue *outline,
                                     const double m[3][3],
                                     const double m2[3][3],
                                     double border_x, double border_y,
                                     OutlineHashKey *ol_key,
                                     double out_m[3][3],
                                     bool *zero_border)
{
    ASS_Renderer *render_priv = state->renderer;
    const ASS_Transform *tr = &info->transform;

    ol_key->type = OUTLINE_BORDER;
    BorderHashKey *k = &ol_key->u.border;
    k->outline = outline;
    k->miter_join = info->border_style == 5;

    double bord_x =
        64 * state->border_scale_x * border_x / tr->scale.x /
            render_priv->par_scale_x;
    double bord_y =
        64 * state->border_scale_y * border_y / tr->scale.y;

    const ASS_Rect *bbox = &outline->cbox;
    // Estimate bounding box half size after stroking
    double dx = (bbox->x_max - bbox->x_min) / 2.0 + (bord_x + 64);
    double dy = (bbox->y_max - bbox->y_min) / 2.0 + (bord_y + 64);

    // Matrix after quantize_transform() has
    // input and output origin at bounding box center.
    double mxx = fabs(m[0][0]), mxy = fabs(m[0][1]);
    double myx = fabs(m[1][0]), myy = fabs(m[1][1]);
    double mzx = fabs(m[2][0]), mzy = fabs(m[2][1]);

    double z0 = m[2][2] - mzx * dx - mzy * dy;
    double w = 1 / FFMAX(z0, m[2][2] / MAX_PERSP_SCALE);

    // Notation from quantize_transform(). Estimate acceptable stroker error.
    double x_lim = mxx * dx + mxy * dy;
    double y_lim = myx * dx + myy * dy;
    double rz = FFMAX(x_lim, y_lim) * w;

    w *= STROKER_PRECISION / POSITION_PRECISION;
    frexp(w * (FFMAX(mxx, myx) + mzx * rz), &k->scale_ord_x);
    frexp(w * (FFMAX(mxy, myy) + mzy * rz), &k->scale_ord_y);
    bord_x = ldexp(bord_x, k->scale_ord_x);
    bord_y = ldexp(bord_y, k->scale_ord_y);
    if (!(bord_x < OUTLINE_MAX && bord_y < OUTLINE_MAX))
        return false;
    k->border.x = ass_lrint(bord_x / STROKER_PRECISION);
    k->border.y = ass_lrint(bord_y / STROKER_PRECISION);
    if (!k->border.x && !k->border.y) {
        *zero_border = true;
        return true;
    }

    *zero_border = false;
    for (int i = 0; i < 3; i++) {
        out_m[i][0] = ldexp(m2[i][0], -k->scale_ord_x);
        out_m[i][1] = ldexp(m2[i][1], -k->scale_ord_y);
        out_m[i][2] = m2[i][2];
    }
    return true;
}

static bool load_border_bitmap(RenderContext *state, GlyphInfo *info,
                               BitmapHashKey *base_key,
                               OutlineHashKey *ol_key,
                               const double m[3][3],
                               ASS_Vector *pos_o,
                               ASS_DVector *offset,
                               bool distorted,
                               Bitmap *distort_bitmap,
                               Bitmap **out_bm)
{
    ASS_Renderer *render_priv = state->renderer;
    OutlineHashValue temp_outline = {0};
    OutlineHashValue *outline_border = NULL;
    bool ok = false;

    if (distorted) {
        if (!ass_outline_construct(ol_key, &temp_outline, render_priv) ||
                !temp_outline.valid)
            goto cleanup;
        outline_border = &temp_outline;
    } else {
        outline_border =
            ass_cache_get(render_priv->cache.outline_cache, ol_key, render_priv);
    }

    BitmapHashKey key = *base_key;
    key.outline = outline_border;
    double qm[3][3];
    memcpy(qm, m, sizeof(qm));
    if (!key.outline || !key.outline->valid ||
            !quantize_transform(qm, pos_o, offset, false, &key))
        goto cleanup;

    if (distorted) {
        memset(distort_bitmap, 0, sizeof(*distort_bitmap));
        if (ass_bitmap_construct(&key, distort_bitmap, state) &&
                distort_bitmap->buffer) {
            *out_bm = distort_bitmap;
            info->has_distort_bitmap = true;
            ok = true;
        }
    } else {
        *out_bm = ass_cache_get(render_priv->cache.bitmap_cache, &key, state);
        if (*out_bm && (*out_bm)->buffer)
            ok = true;
        else
            *out_bm = NULL;
    }

cleanup:
    if (distorted) {
        ass_outline_free(&temp_outline.outline[0]);
        ass_outline_free(&temp_outline.outline[1]);
    }
    return ok;
}

static void
get_bitmap_glyph(RenderContext *state, GlyphInfo *info,
                 int32_t *leftmost_x,
                 ASS_Vector *pos, ASS_Vector *pos_o,
                 ASS_DVector *offset, bool first, int flags)
{
    ASS_Renderer *render_priv = state->renderer;
    render_priv->repeated_event_stats.glyph_bitmap_requests++;

    OutlineHashValue *outline = info->distorted_outline ? info->distorted_outline : info->outline;
    bool distorted = info->distorted_outline && info->distort_enabled;
    info->has_distort_bitmap = false;

    if (!outline || info->symbol == '\n' || info->symbol == 0 || info->skip)
        return;
    /* A move-only drawing reserves layout metrics, not paint pixels. */
    if (info->drawing_text.str && !outline->outline[0].n_segments)
        return;

    double m1[3][3], m2[3][3], m[3][3], z_basis[3];
    const ASS_Transform *tr = &info->transform;
    calc_transform_matrix(state, info, m1, info->has_rnd ? z_basis : NULL);
    for (int i = 0; i < 3; i++) {
        m2[i][0] = m1[i][0] * tr->scale.x;
        m2[i][1] = m1[i][1] * tr->scale.y;
        m2[i][2] = m1[i][0] * tr->offset.x + m1[i][1] * tr->offset.y + m1[i][2];
    }
    memcpy(m, m2, sizeof(m));

    if (leftmost_x && ((info->effect_type == EF_KARAOKE_KF &&
                       !info->native_vertical) || info->furi_base_karaoke))
        ass_outline_update_min_transformed_x(&outline->outline[0], m, leftmost_x);

    BitmapHashKey key = {0};
    key.outline = outline;
    if (!quantize_transform(m, pos, offset, first, &key))
        return;

    *pos_o = *pos;

    bool rnd_active = info->has_rnd;
    if (rnd_active) {
        if (!build_rnd_bitmaps(state, info, outline, m1, z_basis, pos, pos_o,
                              (flags & FILTER_NONZERO_BORDER) &&
                              !(flags & FILTER_BORDER_STYLE_3)))
            return;
        if (!(flags & FILTER_BORDER_STYLE_3))
            return;
        // Opaque boxes retain their ordinary geometry; only the text boundary
        // is randomized. Keep the box bitmap in the same owned lifetime.
        distorted = true;
    }

    if (!rnd_active) {
        info->bm = NULL;
        info->bm_o = NULL;
        if (distorted) {
            memset(&info->distort_bitmap, 0, sizeof(info->distort_bitmap));
            if (ass_bitmap_construct(&key, &info->distort_bitmap, state) &&
                    info->distort_bitmap.buffer)
                info->bm = &info->distort_bitmap;
            info->has_distort_bitmap = info->bm != NULL;
        } else {
            info->bm = ass_cache_get(render_priv->cache.bitmap_cache, &key, state);
            if (!info->bm || !info->bm->buffer)
                info->bm = NULL;
        }

        *pos_o = *pos;
    }

    for (int i = 0; i < ASS_BORDER_LAYERS_MAX - 1; i++)
        info->bm_border[i] = NULL;

    if (flags & FILTER_BORDER_STYLE_3) {
        if (!(flags & (FILTER_NONZERO_BORDER | FILTER_NONZERO_SHADOW)))
            return;

        OutlineHashKey ol_key;
        ol_key.type = OUTLINE_BOX;

        ASS_DVector bord = {
            64 * info->border_x * state->border_scale_x /
                render_priv->par_scale_x,
            64 * info->border_y * state->border_scale_y,
        };
        double width = info->hspacing_scaled + info->advance.x;
        double height = info->asc + info->desc;

        ASS_DVector orig_scale;
        orig_scale.x = info->scale_x * info->scale_fix;
        orig_scale.y = info->scale_y * info->scale_fix;

        // Emulate the WTFish behavior of VSFilter, i.e. double-scale
        // the sizes of the opaque box.
        bord.x *= orig_scale.x;
        bord.y *= orig_scale.y;
        width  *= orig_scale.x;
        height *= orig_scale.y;

        // to avoid gaps
        bord.x = FFMAX(64, bord.x);
        bord.y = FFMAX(64, bord.y);

        ASS_DVector scale = {
            (width  + 2 * bord.x) / 64,
            (height + 2 * bord.y) / 64,
        };
        ASS_DVector box_offset = { -bord.x, -bord.y - info->asc };
        for (int i = 0; i < 3; i++) {
            m[i][0] = m1[i][0] * scale.x;
            m[i][1] = m1[i][1] * scale.y;
            m[i][2] = m1[i][0] * box_offset.x +
                      m1[i][1] * box_offset.y + m1[i][2];
        }

        if (load_border_bitmap(state, info, &key, &ol_key, m, pos_o, offset,
                               distorted, &info->distort_bitmap_o, &info->bm_o)) {
            if (!info->bm)
                *pos = *pos_o;
        } else {
            *pos_o = *pos;
            if (info->bm)
                info->bm_o = info->bm;
        }
        return;
    }

    if (!(flags & FILTER_NONZERO_BORDER))
        return;

    double outer_x = 0;
    double outer_y = 0;
    double prev_x = 0;
    double prev_y = 0;
    for (int layer = 0; layer < ASS_BORDER_LAYERS_MAX; layer++) {
        const BorderLayerState *border = &info->border_layers[layer];
        double size_x = layer == 0 ? info->border_x :
            (border->size_x > 0 ? border->size_x : 0);
        double size_y = layer == 0 ? info->border_y :
            (border->size_y > 0 ? border->size_y : 0);
        bool has_size = layer == 0 ? size_x > 0 || size_y > 0 :
                                      border_layer_has_size(border);
        if (!has_size)
            continue;
        outer_x += size_x;
        outer_y += size_y;
        if (outer_x <= prev_x && outer_y <= prev_y)
            continue;

        OutlineHashKey ol_key;
        double border_m[3][3];
        bool zero_border = false;
        if (!setup_border_outline_key(state, info, outline, m, m2,
                                      outer_x, outer_y, &ol_key, border_m,
                                      &zero_border))
            continue;

        Bitmap **target_bm =
            layer == 0 ? &info->bm_o : &info->bm_border[layer - 1];
        ASS_Vector *target_pos =
            layer == 0 ? pos_o : &info->pos_border[layer - 1];
        if (zero_border) {
            if (layer == 0) {
                info->bm_o = info->bm;
                *pos_o = *pos;
            }
            prev_x = outer_x;
            prev_y = outer_y;
            continue;
        }

        Bitmap *distort_target =
            layer == 0 ? &info->distort_bitmap_o :
                         &info->distort_bitmap_border[layer - 1];
        if (load_border_bitmap(state, info, &key, &ol_key, border_m,
                               target_pos, offset, distorted, distort_target,
                               target_bm)) {
            if (!info->bm)
                *pos = *target_pos;
            prev_x = outer_x;
            prev_y = outer_y;
        } else if (layer == 0) {
            *pos_o = *pos;
        }
    }
}

static inline size_t outline_size(const ASS_Outline* outline)
{
    return sizeof(ASS_Vector) * outline->n_points + outline->n_segments;
}

/* Randomize cached source geometry into a short-lived projected outline. The
 * path seed is independent of event order, frame order and host SIMD support. */
static bool rnd_transform_outline(ASS_Outline *dst, const ASS_Outline *src,
                                  const GlyphInfo *info, const double m[3][3],
                                  const double z_basis[3], uint32_t *rng)
{
    if (!src->n_points || !src->n_segments)
        return true;
    if (!ass_outline_alloc(dst, src->n_points, src->n_segments))
        return false;
    memcpy(dst->segments, src->segments, src->n_segments);
    dst->n_points = src->n_points;
    dst->n_segments = src->n_segments;
    const int32_t amplitude[3] = {info->rnd_x, info->rnd_y, info->rnd_z};
    double matrix[3][4];
    for (int i = 0; i < 3; i++) {
        memcpy(matrix[i], m[i], sizeof(m[i]));
        matrix[i][3] = z_basis[i];
    }
    /* Font outlines may be normalized to a different FreeType size; drawings
     * may have an ASS drawing scale. Neither changes the rnd path units.
     * Only the effective font X/Y scale multiplies the random displacement. */
    double scale_x = info->scale_x * info->scale_fix;
    double scale_y = info->scale_y * info->scale_fix;
    for (size_t first = 0; first < src->n_points; first += 4) {
        float offset[4][3];
        ass_rnd_group(rng, amplitude, offset);
        size_t count = FFMIN((size_t) 4, src->n_points - first);
        for (size_t j = 0; j < count; j++) {
            const ASS_Vector *pt = &src->points[first + j];
            double x = pt->x * info->transform.scale.x + info->transform.offset.x;
            double y = pt->y * info->transform.scale.y + info->transform.offset.y;
            double result[2];
            if (!ass_rnd_project(matrix, x + offset[j][0] * scale_x,
                                 y + offset[j][1] * scale_y, offset[j][2],
                                 info->ortho ? 1 : 1000, result) ||
                fabs(result[0]) >= OUTLINE_MAX || fabs(result[1]) >= OUTLINE_MAX)
                return false;
            dst->points[first + j] = (ASS_Vector) {
                ass_lrint(result[0]), ass_lrint(result[1])
            };
        }
    }
    return true;
}

static bool build_rnd_bitmaps(RenderContext *state, GlyphInfo *info,
                              OutlineHashValue *outline_src,
                              const double m[3][3], const double z_basis[3],
                              ASS_Vector *pos, ASS_Vector *pos_o,
                              bool need_border)
{
    ASS_Outline fill[2] = {{0}};
    ASS_Outline border[2] = {{0}};
    uint32_t rng = (uint32_t) info->rnd_seed;
    bool ok = false;
    info->bm = info->bm_o = NULL;
    info->distort_bitmap = (Bitmap) {0};
    info->distort_bitmap_o = (Bitmap) {0};
    for (int i = 0; i < ASS_BORDER_LAYERS_MAX - 1; i++) {
        info->bm_border[i] = NULL;
        info->distort_bitmap_border[i] = (Bitmap) {0};
        info->pos_border[i] = (ASS_Vector) {0};
    }
    /* Mark ownership before the first allocation so every failure unwinds. */
    info->has_distort_bitmap = true;
    *pos = *pos_o = (ASS_Vector) {0};
    if (!rnd_transform_outline(&fill[0], &outline_src->outline[0], info, m, z_basis, &rng) ||
        !rnd_transform_outline(&fill[1], &outline_src->outline[1], info, m, z_basis, &rng) ||
        !ass_outline_to_bitmap(state, &info->distort_bitmap, &fill[0], &fill[1]))
        goto done;
    if (info->distort_bitmap.buffer)
        info->bm = &info->distort_bitmap;
    if (need_border) {
        double outer_x = 0, outer_y = 0;
        for (int layer = 0; layer < ASS_BORDER_LAYERS_MAX; layer++) {
            const BorderLayerState *b = &info->border_layers[layer];
            double x = layer ? FFMAX(0, b->size_x) : FFMAX(0, info->border_x);
            double y = layer ? FFMAX(0, b->size_y) : FFMAX(0, info->border_y);
            if (!(x > 0 || y > 0))
                continue;
            outer_x += x;
            outer_y += y;
            /* VSFilterMod widens after transforming the randomized boundary.
             * Each layer strokes the same fill, never another random path. */
            double bx = 64 * state->border_scale_x * outer_x;
            double by = 64 * state->border_scale_y * outer_y;
            if (!isfinite(bx) || !isfinite(by) || bx >= OUTLINE_MAX || by >= OUTLINE_MAX)
                goto done;
            int ix = ass_lrint(bx), iy = ass_lrint(by);
            Bitmap *target = layer ? &info->distort_bitmap_border[layer - 1] :
                                     &info->distort_bitmap_o;
            int radius = FFMAX(ix, iy);
            if (!radius) {
                if (!layer)
                    info->bm_o = info->bm;
                continue;
            }
            int eps = FFMIN(STROKER_PRECISION, FFMAX(1, radius / 4));
            if (!fill[0].n_points ||
                !ass_outline_stroke(&border[0], &border[1], &fill[0], ix, iy,
                                    eps, info->border_style == 5) ||
                !ass_outline_to_bitmap(state, target, &border[0], &border[1]))
                goto done;
            if (target->buffer) {
                if (layer)
                    info->bm_border[layer - 1] = target;
                else
                    info->bm_o = target;
            }
            ass_outline_free(&border[0]);
            ass_outline_free(&border[1]);
        }
    }
    ok = true;
done:
    ass_outline_free(&fill[0]);
    ass_outline_free(&fill[1]);
    ass_outline_free(&border[0]);
    ass_outline_free(&border[1]);
    if (!ok) {
        /* Do not destroy the separately owned pre-rnd distortion outline. */
        ass_free_bitmap(&info->distort_bitmap);
        ass_free_bitmap(&info->distort_bitmap_o);
        info->bm = info->bm_o = NULL;
        for (int i = 0; i < ASS_BORDER_LAYERS_MAX - 1; i++) {
            ass_free_bitmap(&info->distort_bitmap_border[i]);
            info->bm_border[i] = NULL;
        }
        info->has_distort_bitmap = false;
    }
    return ok;
}

size_t ass_bitmap_construct(void *key, void *value, void *priv)
{
    RenderContext *state = priv;
    state->renderer->repeated_event_stats.bitmap_constructions++;
    BitmapHashKey *k = key;
    Bitmap *bm = value;

    double m[3][3];
    restore_transform(m, k);

    ASS_Outline outline[2];
    if (k->matrix_z.x || k->matrix_z.y) {
        ass_outline_transform_3d(&outline[0], &k->outline->outline[0], m);
        ass_outline_transform_3d(&outline[1], &k->outline->outline[1], m);
    } else {
        ass_outline_transform_2d(&outline[0], &k->outline->outline[0], m);
        ass_outline_transform_2d(&outline[1], &k->outline->outline[1], m);
    }


    if (!ass_outline_to_bitmap(state, bm, &outline[0], &outline[1]))
        memset(bm, 0, sizeof(*bm));
    else {
        bm->sub_x = (uint8_t) (k->offset.x & ((1 << SUBPIXEL_ORDER) - 1));
        bm->sub_y = (uint8_t) (k->offset.y & ((1 << SUBPIXEL_ORDER) - 1));
    }
    ass_outline_free(&outline[0]);
    ass_outline_free(&outline[1]);

    return sizeof(BitmapHashKey) + sizeof(Bitmap) + bitmap_size(bm) +
           sizeof(OutlineHashValue) + outline_size(&k->outline->outline[0]) + outline_size(&k->outline->outline[1]);
}

static inline double line_spacing(RenderContext *state)
{
    ASS_Renderer *render_priv = state->renderer;
    return render_priv->settings.line_spacing +
           state->fshp * state->object_scale * state->screen_scale_y;
}

static void measure_text_on_eol(RenderContext *state, double scale, int cur_line,
                                int max_asc, int max_desc,
                                double max_border_x, double max_border_y)
{
    TextInfo *text_info = &state->text_info;
    text_info->lines[cur_line].asc  = scale * max_asc;
    text_info->lines[cur_line].desc = scale * max_desc;
    text_info->height += scale * max_asc + scale * max_desc;
    // For *VSFilter compatibility do biased rounding on max_border*
    // https://github.com/Cyberbeing/xy-VSFilter/blob/xy_sub_filter_rc4@%7B2020-05-17%7D/src/subtitles/RTS.cpp#L1465
    text_info->border_bottom = (int) (state->border_scale_y * max_border_y + 0.5);
    if (cur_line == 0)
        text_info->border_top = text_info->border_bottom;
    // VSFilter takes max \bordx into account for collision, even if far from edge
    text_info->border_x = FFMAX(text_info->border_x,
            (int) (state->border_scale_x * max_border_x + 0.5));
}


/**
 * This function goes through text_info and calculates text parameters.
 * The following text_info fields are filled:
 *   height
 *   border_top
 *   border_bottom
 *   border_x
 *   lines[].asc
 *   lines[].desc
 */
static void measure_text(RenderContext *state)
{
    TextInfo *text_info = &state->text_info;
    text_info->height = 0;
    text_info->border_x = 0;

    int cur_line = 0;
    double scale = 0.5 / 64;
    int max_asc = 0, max_desc = 0;
    double max_border_y = 0, max_border_x = 0;
    bool empty_trimmed_line = true;
    for (int i = 0; i < text_info->length; i++) {
        if (text_info->glyphs[i].linebreak) {
            measure_text_on_eol(state, scale, cur_line,
                    max_asc, max_desc, max_border_x, max_border_y);
            empty_trimmed_line = true;
            max_asc = max_desc = 0;
            max_border_y = max_border_x = 0;
            scale = 0.5 / 64;
            cur_line++;
        }
        GlyphInfo *cur = text_info->glyphs + i;
        // VSFilter ignores metrics of line-leading/trailing (trimmed)
        // whitespace, except when the line becomes empty after trimming
        if (empty_trimmed_line && !cur->is_trimmed_whitespace) {
            empty_trimmed_line = false;
            // Forget metrics of line-leading whitespace
            max_asc = max_desc = 0;
            max_border_y = max_border_x = 0;
        } else if (!empty_trimmed_line && cur->is_trimmed_whitespace) {
            // Ignore metrics of line-trailing whitespace
            continue;
        }
        max_asc  = FFMAX(max_asc,  cur->asc);
        max_desc = FFMAX(max_desc, cur->desc);
        max_border_y = FFMAX(max_border_y, glyph_border_max_y(cur));
        max_border_x = FFMAX(max_border_x, glyph_border_max_x(cur));
        if (cur->symbol != '\n')
            scale = 1.0 / 64;
    }
    assert(cur_line == text_info->n_lines - 1);
    measure_text_on_eol(state, scale, cur_line,
            max_asc, max_desc, max_border_x, max_border_y);
    text_info->height += cur_line * line_spacing(state);
}

/**
 * Mark extra whitespace for later removal.
 */
#define IS_WHITESPACE(x) ((x->symbol == ' ' || x->symbol == '\n') \
                          && !x->linebreak)
static void trim_whitespace(RenderContext *state)
{
    int i, j;
    GlyphInfo *cur;
    TextInfo *ti = &state->text_info;

    if (!ti->length)
      return;

    // Mark trailing spaces
    i = ti->length - 1;
    cur = ti->glyphs + i;
    while (i && IS_WHITESPACE(cur)) {
        cur->skip = true;
        cur->is_trimmed_whitespace = true;
        cur = ti->glyphs + --i;
    }

    // Mark leading whitespace
    i = 0;
    cur = ti->glyphs;
    while (i < ti->length && IS_WHITESPACE(cur)) {
        cur->skip = true;
        cur->is_trimmed_whitespace = true;
        cur = ti->glyphs + ++i;
    }
    if (i < ti->length)
        cur->starts_new_run = true;

    // Mark all extraneous whitespace inbetween
    // XXX: should this really start at 0 again?
    for (i = 0; i < ti->length; ++i) {
        cur = ti->glyphs + i;
        if (cur->linebreak) {
            // Mark whitespace before
            j = i - 1;
            cur = ti->glyphs + j;
            // Use > instead of >= to avoid UB from moving the pointer outside valid range.
            // White space at j == 0 was already trimmed in the "leading" loop before anyway.
            while (j > 0 && IS_WHITESPACE(cur)) {
                cur->skip = true;
                cur->is_trimmed_whitespace = true;
                cur = ti->glyphs + --j;
            }

            // A break itself can contain a whitespace, too
            cur = ti->glyphs + i;
            if (cur->symbol == ' ' || cur->symbol == '\n') {
                cur->skip = true;
                cur->is_trimmed_whitespace = true;
                // Mark whitespace after
                j = i + 1;
                cur = ti->glyphs + j;
                while (j < ti->length && IS_WHITESPACE(cur)) {
                    cur->skip = true;
                    cur->is_trimmed_whitespace = true;
                    cur = ti->glyphs + ++j;
                }
                i = j - 1;
            }
            if (cur < ti->glyphs + ti->length)
                cur->starts_new_run = true;
        }
    }
}
#undef IS_WHITESPACE

#ifdef CONFIG_UNIBREAK
    #define ALLOWBREAK(glyph, index) (unibrks ? unibrks[index] == LINEBREAK_ALLOWBREAK : glyph == ' ')
    #define FORCEBREAK(glyph, index) (unibrks ? unibrks[index] == LINEBREAK_MUSTBREAK  : glyph == '\n')
#else
    #define ALLOWBREAK(glyph, index) (glyph == ' ')
    #define FORCEBREAK(glyph, index) (glyph == '\n')
#endif

/*
 * Starts a new line on the first breakable character after overflow
 */
static void
wrap_lines_naive(RenderContext *state, double max_text_width, char *unibrks)
{
    ASS_Renderer *render_priv = state->renderer;
    TextInfo *text_info = &state->text_info;
    GlyphInfo *s1  = text_info->glyphs; // current line start
    int last_breakable = -1;
    int break_type = 0;

    text_info->n_lines = 1;
    for (int i = 0; i < text_info->length; ++i) {
        GlyphInfo *cur = text_info->glyphs + i;
        int break_at = -1;
        double s_offset = d6_to_double(s1->bbox.x_min + s1->pos.x);
        double len = d6_to_double(cur->bbox.x_max + cur->pos.x) - s_offset;

        if (FORCEBREAK(cur->symbol, i)) {
            break_type = 2;
            break_at = i;
            ass_msg(render_priv->library, MSGL_DBG2,
                    "forced line break at %d", break_at);
        } else if (len >= max_text_width &&
                   cur->symbol != ' ' /* get trimmed */ &&
                   (state->wrap_style != 2)) {
            break_type = 1;
            break_at = last_breakable;
            if (break_at < 0 && state->chat_enabled && cur > s1)
                break_at = i - 1; // long unbroken chat tokens retain the gutter
            if (break_at >= 0)
                ass_msg(render_priv->library, MSGL_DBG2, "line break at %d",
                        break_at);
        }
        if (ALLOWBREAK(cur->symbol, i)) {
            last_breakable = i;
        }

        if (break_at != -1 && break_at + 1 < text_info->length) {
            // need to use one more line
            if (text_info->n_lines >= text_info->max_lines) {
                // Try to raise the maximum number of lines
                bool success = false;
                if (text_info->max_lines <= INT_MAX / 2) {
                    text_info->max_lines *= 2;
                    success = ASS_REALLOC_ARRAY(text_info->lines, text_info->max_lines);
                }
                // If realloc fails it's screwed and due to error-info not propagating (FIXME),
                // the best we can do is to avoid UB by discarding the previous break
                if (!success) {
                    s1->linebreak = 0;
                    text_info->n_lines--;
                }
            }

            // marking break_at+1 as start of a new line
            int lead = break_at + 1; // the first symbol of the new line
            text_info->glyphs[lead].linebreak = break_type;
            last_breakable = -1;
            s1 = text_info->glyphs + lead;
            text_info->n_lines++;
        }
    }
}

/*
 * Rewind from a linestart position back to the first non-whitespace (0x20)
 * character. Trailing ASCII whitespace gets trimmed in rendering.
 * Assumes both arguments are part of the same array.
 * start2 is never dereferenced.
 */
static inline GlyphInfo *rewind_trailing_spaces(GlyphInfo *start1, GlyphInfo* start2)
{
    GlyphInfo *g = start2;
    do {
        --g;
    } while ((g > start1) && (g->symbol == ' '));
    return g;
}

/*
 * Shift soft linebreaks to balance out line lengths
 * Does not change the linebreak count
 * FIXME: implement style 0 and 3 correctly
 */
static void
wrap_lines_rebalance(RenderContext *state, double max_text_width, char *unibrks)
{
    TextInfo *text_info = &state->text_info;
    int exit = 0;

#define DIFF(x,y) (((x) < (y)) ? (y - x) : (x - y))
    while (!exit && state->wrap_style != 1) {
        exit = 1;
        GlyphInfo  *s1, *s2, *s3;
        s3 = text_info->glyphs;
        s1 = s2 = 0;
        for (int i = 0; i <= text_info->length; ++i) {
            GlyphInfo *cur = text_info->glyphs + i;
            if ((i == text_info->length) || cur->linebreak) {
                s1 = s2;
                s2 = s3;
                // WARNING: this may point one past the end and thus
                // must ONLY be used for pointer comparison; never dereferenced!
                s3 = cur;
                if (s1 && (s2->linebreak == 1)) {       // have at least 2 lines, and linebreak is 'soft'
                    double l1, l2, l1_new, l2_new;

                    // Find last word of line and trim surrounding whitespace before measuring
                    // (whitespace ' ' will also get trimmed in rendering)
                    GlyphInfo *w = rewind_trailing_spaces(s1, s2);
                    GlyphInfo *e1_old = w;
                    while ((w > s1) && (!ALLOWBREAK(w->symbol, w - text_info->glyphs))) {
                        --w;
                    }
                    GlyphInfo *e1 = w;
                    while ((e1 > s1) && (e1->symbol == ' ')) {
                        --e1;
                    }
                    if (w->symbol == ' ')
                        ++w;
                    if (w == s1)
                        continue; // Merging linebreaks is never beneficial

                    GlyphInfo *e2 = rewind_trailing_spaces(s2, s3);

                    l1 = d6_to_double(
                        (e1_old->bbox.x_max + e1_old->pos.x) -
                        (s1->bbox.x_min + s1->pos.x));
                    l2 = d6_to_double(
                        (e2->bbox.x_max + e2->pos.x) -
                        (s2->bbox.x_min + s2->pos.x));
                    l1_new = d6_to_double(
                        (e1->bbox.x_max + e1->pos.x) -
                        (s1->bbox.x_min + s1->pos.x));
                    l2_new = d6_to_double(
                        (e2->bbox.x_max + e2->pos.x) -
                        (w->bbox.x_min + w->pos.x));

                    if (DIFF(l1_new, l2_new) < DIFF(l1, l2)) {
                        w->linebreak = 1;
                        s2->linebreak = 0;
                        s2 = w;
                        exit = 0;
                    }
                }
            }
            if (i == text_info->length)
                break;
        }

    }
    assert(text_info->n_lines >= 1);
#undef DIFF
}

static void
wrap_lines_measure(RenderContext *state, char *unibrks)
{
    TextInfo *text_info = &state->text_info;
    int cur_line = 1;
    int i = 0;

    while (i < text_info->length && text_info->glyphs[i].skip)
        ++i;

    if (i == text_info->length) {
        text_info->lines[0].len = 0;
        text_info->lines[0].offset = 0;
        return;
    }

    double pen_shift_x = d6_to_double(-text_info->glyphs[i].pos.x);
    double pen_shift_y = 0.;

    for (i = 0; i < text_info->length; ++i) {
        GlyphInfo *cur = text_info->glyphs + i;

        if (cur->linebreak) {
            while (i < text_info->length - 1 && cur->skip && !FORCEBREAK(cur->symbol, i))
                cur = text_info->glyphs + ++i;
            double height =
                text_info->lines[cur_line - 1].desc +
                text_info->lines[cur_line].asc;
            text_info->lines[cur_line - 1].len = i -
                text_info->lines[cur_line - 1].offset;
            text_info->lines[cur_line].offset = i;
            cur_line++;
            pen_shift_x = d6_to_double(-cur->pos.x);
            pen_shift_y += height + line_spacing(state);
        }
        cur->pos.x += double_to_d6(pen_shift_x);
        cur->pos.y += double_to_d6(pen_shift_y);
    }
    text_info->lines[cur_line - 1].len =
        text_info->length - text_info->lines[cur_line - 1].offset;
}

#undef ALLOWBREAK
#undef FORCEBREAK

/**
 * \brief rearrange text between lines
 * \param max_text_width maximal text line width in pixels
 * The algo is similar to the one in libvo/sub.c:
 * 1. Place text, wrapping it when current line is full
 * 2. Try moving words from the end of a line to the beginning of the next one while it reduces
 * the difference in lengths between this two lines.
 * The result may not be optimal, but usually is good enough.
 *
 * FIXME: implement style 0 and 3 correctly
 */
static void
wrap_lines_smart(RenderContext *state, double max_text_width)
{
    char *unibrks = NULL;

#ifdef CONFIG_UNIBREAK
    ASS_Renderer *render_priv = state->renderer;
    TextInfo *text_info = &state->text_info;
    if (render_priv->track->parser_priv->feature_flags & FEATURE_MASK(ASS_FEATURE_WRAP_UNICODE)) {
        unibrks = text_info->breaks;
        set_linebreaks_utf32(
            text_info->event_text, text_info->length,
            render_priv->track->Language, unibrks);
#if UNIBREAK_VERSION < 0x0500UL
        // Prior to 5.0 libunibreaks always ended text with LINE_BREAKMUSTBREAK, matching
        // Unicode spec, but messing with our text-overflow detection.
        // Thus reevaluate the last char in a different context.
        // (Later versions set either MUSTBREAK or the newly added INDETERMINATE)
        unibrks[text_info->length - 1] = is_line_breakable(
            text_info->event_text[text_info->length - 1],
            ' ',
            render_priv->track->Language
        );
#endif
    }
#endif

    wrap_lines_naive(state, max_text_width, unibrks);
    wrap_lines_rebalance(state, max_text_width, unibrks);

    trim_whitespace(state);
    measure_text(state);
    wrap_lines_measure(state, unibrks);
}

/**
 * \brief Calculate base point for positioning and rotation
 * \param bbox text bbox
 * \param alignment alignment
 * \param bx, by out: base point coordinates
 */
static void get_base_point(ASS_DRect *bbox, int alignment, double *bx, double *by)
{
    const int halign = alignment & 3;
    const int valign = alignment & 12;
    if (bx)
        switch (halign) {
        case HALIGN_LEFT:
            *bx = bbox->x_min;
            break;
        case HALIGN_CENTER:
            *bx = (bbox->x_max + bbox->x_min) / 2.0;
            break;
        case HALIGN_RIGHT:
            *bx = bbox->x_max;
            break;
        }
    if (by)
        switch (valign) {
        case VALIGN_TOP:
            *by = bbox->y_min;
            break;
        case VALIGN_CENTER:
            *by = (bbox->y_max + bbox->y_min) / 2.0;
            break;
        case VALIGN_SUB:
            *by = bbox->y_max;
            break;
        }
}

/**
 * \brief Adjust the glyph's font size and scale factors to ensure smooth
 *  scaling and handle pathological font sizes. The main problem here is
 *  freetype's grid fitting, which destroys animations by font size, or will
 *  result in incorrect final text size if font sizes are very small and
 *  scale factors very large. See Google Code issue #46.
 * \param priv guess what
 * \param glyph the glyph to be modified
 */
static void
fix_glyph_scaling(ASS_Renderer *priv, GlyphInfo *glyph)
{
    double ft_size;
    if (priv->settings.hinting == ASS_HINTING_NONE) {
        // arbitrary, not too small to prevent grid fitting rounding effects
        // XXX: this is a rather crude hack
        ft_size = 256.0;
    } else {
        // If hinting is enabled, we want to pass the real font size
        // to freetype. Normalize scale_y to 1.0.
        ft_size = glyph->scale_y * glyph->font_size;
    }

    if (!ft_size || !glyph->font_size)
        return;

    double mul = glyph->font_size / ft_size;
    glyph->scale_fix = 1 / mul;
    glyph->scale_x *= mul;
    glyph->scale_y *= mul;
    glyph->font_size = ft_size;
}

// Initial run splitting based purely on the characters' styles
static void split_style_runs_list(GlyphInfo *glyphs, int length,
                                  bool unified_karaoke)
{
    if (length <= 0)
        return;

    Effect last_effect_type = glyphs[0].effect_type;
    glyphs[0].starts_new_run = true;
    glyphs[0].distort_style_run_id = 0;
    int distort_run = 0;
    for (int i = 1; i < length; i++) {
        GlyphInfo *info = glyphs + i;
        GlyphInfo *last = glyphs + (i - 1);
        Effect effect_type = info->effect_type;
        info->starts_new_run =
            info->scroll_id != last->scroll_id ||
            info->effect_timing ||  // but ignore effect_skip_timing
            (unified_karaoke &&
             info->karaoke_segment != last->karaoke_segment) ||
            (effect_type != EF_NONE && effect_type != last_effect_type) ||
            info->drawing_text.str ||
            last->drawing_text.str ||
            !ass_string_equal(last->font->desc.family, info->font->desc.family) ||
            last->font->desc.vertical != info->font->desc.vertical ||
            last->vertical_substitute != info->vertical_substitute ||
            last->font_size != info->font_size ||
            last->c[0] != info->c[0] ||
            last->c[1] != info->c[1] ||
            last->c[2] != info->c[2] ||
            last->c[3] != info->c[3] ||
            last->fade != info->fade ||
            last->fade_color.active != info->fade_color.active ||
            last->fade_color.color != info->fade_color.color ||
            last->fade_color.amount != info->fade_color.amount ||
            !ass_gradient_equal(&last->gradient, &info->gradient) ||
            !ass_mangetsu_gradient_state_equal(&last->mangetsu_gradient,
                                               &info->mangetsu_gradient) ||
            memcmp(&last->pattern, &info->pattern, sizeof(info->pattern)) ||
            !secondary_outline_equal(&last->secondary_outline,
                                     &info->secondary_outline) ||
            !image_fill_state_equal(&last->image_fill, &info->image_fill) ||
            last->blend_mode != info->blend_mode ||
            last->be != info->be ||
            last->blur_x != info->blur_x ||
            last->blur_y != info->blur_y ||
            last->shadow_x != info->shadow_x ||
            last->shadow_y != info->shadow_y ||
            last->frx != info->frx ||
            last->fry != info->fry ||
            last->frz != info->frz ||
            last->z != info->z ||
            last->ortho != info->ortho ||
            last->fax != info->fax ||
            last->fay != info->fay ||
            last->scale_x != info->scale_x ||
            last->scale_y != info->scale_y ||
            last->border_style != info->border_style ||
            last->border_x != info->border_x ||
            last->border_y != info->border_y ||
            !border_layers_state_equal(last->border_layers, info->border_layers) ||
            last->hspacing != info->hspacing ||
            last->italic != info->italic ||
            last->bold != info->bold ||
            ((last->flags ^ info->flags) & ~DECO_ROTATE);
        // Line trimming later adds shaping boundaries to starts_new_run.
        // Keep the original effective-style boundaries for multiline distort.
        distort_run += info->starts_new_run;
        info->distort_style_run_id = distort_run;
        if (effect_type != EF_NONE)
            last_effect_type = effect_type;
    }
}

static void split_style_runs(RenderContext *state)
{
    TextInfo *text_info = &state->text_info;
    bool unified_karaoke = text_info->n_furi_groups &&
                           text_info->n_karaoke_segments;
    split_style_runs_list(text_info->glyphs, text_info->length,
                          unified_karaoke);
}

static bool furi_escapes_char(char c)
{
    return c == '<' || c == '>' || c == '|' || c == '\\';
}

static unsigned get_next_char_bounded(RenderContext *state, char **str, char *end)
{
    char *p = *str;
    unsigned chr;
    if (p >= end)
        return 0;
    if (*p == '\t') {
        ++p;
        *str = p;
        return ' ';
    }
    if (*p == '\\' && p + 1 < end) {
        if (p + 2 < end && p[1] == '|' && p[2] == '\\') {
            *str = p + 3;
            return '|';
        } else if ((p[1] == 'N') || ((p[1] == 'n') &&
                              (state->wrap_style == 2))) {
            p += 2;
            *str = p;
            return '\n';
        } else if (p[1] == 'n') {
            p += 2;
            *str = p;
            return ' ';
        } else if (p[1] == 'h') {
            p += 2;
            *str = p;
            return NBSP;
        } else if (p[1] == '{') {
            p += 2;
            *str = p;
            return '{';
        } else if (p[1] == '}') {
            p += 2;
            *str = p;
            return '}';
        } else if (state->furi_enabled && furi_escapes_char(p[1])) {
            chr = (unsigned char) p[1];
            p += 2;
            *str = p;
            return chr;
        }
    }

    char *next = p;
    chr = ass_utf8_get_char(&next);
    if (next > end) {
        chr = (unsigned char) *p;
        next = p + 1;
    }
    *str = next;
    return chr;
}

static bool ensure_glyph_capacity(GlyphInfo **glyphs, FriBidiChar **event_text,
                                  char **breaks, int *length, int *max_glyphs)
{
    if (*length < *max_glyphs)
        return true;

    int base = *max_glyphs ? *max_glyphs : 8;
    int new_max = 2 * FFMIN(FFMAX(base, *length / 2 + 1), INT_MAX / 2);
    if (*length >= new_max)
        return false;
    if (!ASS_REALLOC_ARRAY(*glyphs, new_max) ||
            !ASS_REALLOC_ARRAY(*event_text, new_max) ||
            (breaks && !ASS_REALLOC_ARRAY(*breaks, new_max)))
        return false;
    *max_glyphs = new_max;
    return true;
}

static bool append_glyph_to_target(RenderContext *state,
                                   GlyphInfo **glyphs,
                                   FriBidiChar **event_text,
                                   char **breaks,
                                   int *length,
                                   int *max_glyphs,
                                   unsigned code,
                                   ASS_StringView drawing_text,
                                   bool is_furi,
                                   int furi_group)
{
    ASS_Renderer *render_priv = state->renderer;

    if (!state->font)
        return false;

    if (!ensure_glyph_capacity(glyphs, event_text, breaks, length, max_glyphs))
        return false;

    bool main_text = glyphs == &state->text_info.glyphs;
    if (state->column_event && main_text) {
        if (!ensure_column_glyph_capacity(&state->text_info))
            return false;
        if (!ensure_column_count(&state->text_info, state->column_index + 1))
            return false;
    }

    GlyphInfo *info = &(*glyphs)[*length];
    memset(info, 0, sizeof(GlyphInfo));

    double object_scale = state->object_scale;
    if (drawing_text.str) {
        info->drawing_text = drawing_text;
        info->drawing_scale = state->drawing_scale;
        info->drawing_pbo = lround(state->pbo * object_scale);
    }

    double scale_x = state->scale_x * object_scale;
    double scale_y = state->scale_y * object_scale;
    if (state->soft_scale != 1.0) {
        scale_x *= state->soft_scale;
        scale_y *= state->soft_scale;
    }
    double hspacing = state->hspacing;
    if (is_furi) {
        scale_x *= state->furi_scale_x / 100.0;
        scale_y *= state->furi_scale_y / 100.0;
        hspacing = state->furi_hspacing;
    }

    info->symbol = code;
    info->scroll_id = state->scroll_id;
    info->font = state->font;
    for (int i = 0; i < 4; i++)
        info->c[i] = state->c[i];
    if (state->chat_enabled)
        info->chat_bubble = state->chat_bubble;
    info->gradient = state->gradient;
    info->mangetsu_gradient = state->mangetsu_gradient;
    info->pattern = state->pattern;
    info->secondary_outline = state->secondary_outline;
    info->image_fill = state->image_fill;
    info->blend_mode = state->blend_mode;
    info->line = 0;

    if (main_text && state->column_event && state->column_active) {
        info->effect_type = EF_NONE;
        info->effect_timing = 0;
        info->effect_skip_timing = 0;
        info->reset_effect = false;
    } else {
        info->effect_type = state->effect_type;
        info->effect_timing = state->effect_timing;
        info->effect_skip_timing = state->effect_skip_timing;
        info->reset_effect = state->reset_effect;
    }
    info->font_size = fabs(state->font_size * state->screen_scale_y);
    info->be = state->be;
    info->blur_x = state->blur_x * object_scale;
    info->blur_y = state->blur_y * object_scale;
    info->shadow_x = state->shadow_x * object_scale;
    info->shadow_y = state->shadow_y * object_scale;
    info->scale_x = scale_x;
    info->scale_y = scale_y;
    info->soft_scale = state->soft_scale;
    info->border_style = state->border_style;
    info->border_x = state->border_x * object_scale;
    info->border_y = state->border_y * object_scale;
    memcpy(info->border_layers, state->border_layers, sizeof(info->border_layers));
    for (int i = 0; i < ASS_BORDER_LAYERS_MAX; i++) {
        info->border_layers[i].size_x *= object_scale;
        info->border_layers[i].size_y *= object_scale;
        if (info->border_layers[i].has_blur)
            info->border_layers[i].blur *= object_scale;
    }
    sync_glyph_layer1_border(info);
    info->hspacing = hspacing;
    info->bold = state->bold;
    info->italic = state->italic;
    info->flags = state->flags;
    if (info->font->desc.vertical && code >= VERTICAL_LOWER_BOUND)
        info->flags |= DECO_ROTATE;
    int decoration_flags = info->flags & (DECO_UNDERLINE | DECO_STRIKETHROUGH);
    info->has_custom_decoration =
        decoration_flags && (state->decoration_color_set ||
                             state->decoration_alpha_set ||
                             state->mangetsu_gradient.layer[4].active ||
                             state->mangetsu_gradient.alpha[4].active);
    if (info->has_custom_decoration) {
        info->decoration_flags = decoration_flags | (info->flags & DECO_ROTATE);
        memcpy(info->decoration_c, info->c, sizeof(info->decoration_c));
        uint32_t decoration = state->decoration_color_set ?
            (state->decoration_color & 0xFFFFFF00) | _a(info->c[0]) :
            info->c[0];
        if (state->decoration_alpha_set)
            decoration = (decoration & 0xFFFFFF00) |
                         (state->decoration_alpha & 0xFF);
        info->decoration_c[0] = decoration;
        info->decoration_c[1] = decoration;
        info->flags &= ~(DECO_UNDERLINE | DECO_STRIKETHROUGH);
    }
    info->frx = state->frx;
    info->fry = state->fry;
    info->frs = state->frs;
    info->frz = state->frz + info->frs;
    info->native_vertical = state->native_vertical;
    info->z = state->z;
    info->ortho = state->ortho;
    info->fax = state->fax;
    info->fay = state->fay;
    info->fade = state->fade;
    info->vshift = -double_to_d6(state->fsvp * object_scale *
                                 state->screen_scale_y);
    if (state->jitter.enabled) {
        info->has_jitter = true;
        info->jitter = state->jitter;
        info->jitter.left *= object_scale;
        info->jitter.right *= object_scale;
        info->jitter.up *= object_scale;
        info->jitter.down *= object_scale;
    }
    info->has_rnd = state->rnd_x > 0 || state->rnd_y > 0 ||
        (state->rnd_z > 0 && !(info->ortho && !info->frx && !info->fry));
    info->rnd_seed = state->rnd_seed;
    info->rnd_x = state->rnd_x;
    info->rnd_y = state->rnd_y;
    info->rnd_z = state->rnd_z;
    info->distort_enabled = state->distort_enabled;
    info->distort_extended = state->distort_extended;
    info->distort = state->distort;
    info->distort_bbox = (ASS_DRect) {0};
    info->distorted_outline = NULL;
    info->has_distort_bitmap = false;
    info->has_distort_outline = false;
    info->is_furi = is_furi;
    info->furi_group = furi_group;
    info->karaoke_segment = state->karaoke_segment;
    info->fade_color = state->fade_color;

    info->hspacing_scaled = 0;
    info->scale_fix = 1;

    if (!drawing_text.str) {
        info->hspacing_scaled = double_to_d6(info->hspacing *
                state->screen_scale_x / render_priv->par_scale_x *
                info->scale_x);
        fix_glyph_scaling(render_priv, info);
    }

    if (state->column_event && main_text) {
        state->text_info.column_glyphs[*length] = (ColumnGlyphInfo) {
            .row = state->column_row,
            .column = state->column_index,
            .active = state->column_active,
        };
        if (state->column_active) {
            state->text_info.column_rows =
                FFMAX(state->text_info.column_rows, state->column_row + 1);
            state->text_info.column_count =
                FFMAX(state->text_info.column_count, state->column_index + 1);
        }
    }

    (*length)++;
    return true;
}

static bool secondary_outline_equal(const KaraokeOutlinePaint *a,
                                    const KaraokeOutlinePaint *b)
{
    if (a->type != b->type)
        return false;
    if (a->type == KARAOKE_OUTLINE_UNSET)
        return true;
    if (a->color != b->color)
        return false;
    if (a->type == KARAOKE_OUTLINE_VECTOR)
        return a->vector.color_enabled == b->vector.color_enabled &&
               !memcmp(a->vector.color, b->vector.color,
                       sizeof(a->vector.color));
    if (a->type == KARAOKE_OUTLINE_GRADIENT)
        return ass_mangetsu_gradient_layer_equal(&a->gradient, &b->gradient);
    return true;
}

static bool append_furi_base(RenderContext *state, char *start, char *end,
                             int group_id)
{
    TextInfo *text_info = &state->text_info;
    char *p = start;
    while (p < end) {
        if (*p == '{') {
            char *close = memchr(p + 1, '}', end - (p + 1));
            if (!close)
                return false;
            // Base-side overrides are not part of the ruby feature.  Skip
            // the whole block, most importantly without letting karaoke
            // tags mutate the shared event timeline.
            p = close + 1;
            continue;
        }

        unsigned code = get_next_char_bounded(state, &p, end);
        if (!code)
            break;
        if (!append_glyph_to_target(state, &text_info->glyphs,
                                    &text_info->event_text,
                                    &text_info->breaks, &text_info->length,
                                    &text_info->max_glyphs, code,
                                    (ASS_StringView) {NULL, 0}, false,
                                    group_id))
            return false;
        state->effect_type = EF_NONE;
        state->effect_timing = 0;
        state->effect_skip_timing = 0;
        state->reset_effect = false;
    }
    return true;
}

static bool append_furi_reading(RenderContext *state, char *start, char *end,
                                FuriGroup *group, int group_id)
{
    char *p = start;
    while (p < end) {
        if (*p == '{') {
            char *close = memchr(p + 1, '}', end - (p + 1));
            if (!close)
                return false;
            unsigned serial = state->karaoke_tag_serial;
            ass_parse_karaoke_override_block(state, p + 1, close);
            if (state->karaoke_tag_serial != serial)
                group->has_internal_karaoke = true;
            p = close + 1;
            continue;
        }

        unsigned code = get_next_char_bounded(state, &p, end);
        if (!code)
            break;
        if (!append_glyph_to_target(state, &group->glyphs,
                                    &group->event_text, NULL, &group->length,
                                    &group->max_glyphs, code,
                                    (ASS_StringView) {NULL, 0}, true,
                                    group_id))
            return false;
        state->effect_type = EF_NONE;
        state->effect_timing = 0;
        state->effect_skip_timing = 0;
        state->reset_effect = false;
    }
    return true;
}

static inline bool border_layer_has_size(const BorderLayerState *layer)
{
    return layer->enabled && (layer->size_x > 0 || layer->size_y > 0);
}

static bool border_layer_state_equal(const BorderLayerState *a,
                                     const BorderLayerState *b)
{
    return a->enabled == b->enabled &&
           a->has_color == b->has_color &&
           a->has_alpha == b->has_alpha &&
           a->has_blur == b->has_blur &&
           a->has_be == b->has_be &&
           a->size_x == b->size_x &&
           a->size_y == b->size_y &&
           (!a->has_blur || a->blur == b->blur) &&
           (!a->has_be || a->be == b->be) &&
           a->color == b->color &&
           !memcmp(&a->gradient, &b->gradient, sizeof(a->gradient));
}

static bool border_layers_state_equal(const BorderLayerState *a,
                                      const BorderLayerState *b)
{
    for (int i = 0; i < ASS_BORDER_LAYERS_MAX; i++)
        if (!border_layer_state_equal(&a[i], &b[i]))
            return false;
    return true;
}

static bool has_multi_border_layers(const BorderLayerState *layers)
{
    for (int i = 1; i < ASS_BORDER_LAYERS_MAX; i++)
        if (border_layer_has_size(&layers[i]))
            return true;
    return false;
}

static bool layer1_filter_differs(const GlyphInfo *info)
{
    const BorderLayerState *border = &info->border_layers[0];
    return (border->has_blur &&
            (border->blur != info->blur_x || border->blur != info->blur_y)) ||
           (border->has_be && border->be != info->be);
}

static void sync_glyph_layer1_border(GlyphInfo *info)
{
    /*
     * Keep the normal ASS border authoritative from the legacy glyph fields.
     * Existing compatibility paths update border_x/y and c[2]; numbered
     * extension layers must not perturb layout or bitmap generation when
     * authors do not use extra border layers.
     */
    info->border_layers[0].enabled = info->border_x > 0 || info->border_y > 0;
    info->border_layers[0].has_color = true;
    info->border_layers[0].has_alpha = true;
    info->border_layers[0].size_x = info->border_x;
    info->border_layers[0].size_y = info->border_y;
    info->border_layers[0].color = info->c[2];
    info->border_layers[0].gradient = info->gradient.layer[2];
}

static double glyph_border_max_x(const GlyphInfo *info)
{
    double max = info->border_x > 0 ? info->border_x : 0;
    for (int i = 1; i < ASS_BORDER_LAYERS_MAX; i++)
        if (border_layer_has_size(&info->border_layers[i]))
            max += info->border_layers[i].size_x > 0 ?
                   info->border_layers[i].size_x : 0;
    return max;
}

static double glyph_border_max_y(const GlyphInfo *info)
{
    double max = info->border_y > 0 ? info->border_y : 0;
    for (int i = 1; i < ASS_BORDER_LAYERS_MAX; i++)
        if (border_layer_has_size(&info->border_layers[i]))
            max += info->border_layers[i].size_y > 0 ?
                   info->border_layers[i].size_y : 0;
    return max;
}

static Bitmap *combined_border_bitmap(CombinedBitmapInfo *info, int layer)
{
    return layer == 0 ? info->bm_o : info->bm_border[layer - 1];
}

static Bitmap *composite_border_bitmap(CompositeHashValue *value, int layer)
{
    return layer == 0 ? &value->bm_o : &value->bm_border[layer - 1];
}

static Bitmap *bitmap_ref_border_bitmap(BitmapRef *ref, int layer)
{
    return layer == 0 ? ref->bm_o : ref->bm_border[layer - 1];
}

static ASS_Vector bitmap_ref_border_pos(BitmapRef *ref, int layer)
{
    return layer == 0 ? ref->pos_o : ref->pos_border[layer - 1];
}

typedef struct {
    char *base_start;
    char *base_end;
    char *furi_start;
    char *furi_end;
    char *gyaku_start;
    char *gyaku_end;
    char *end;
} FuriCandidate;

typedef enum {
    FURI_CANDIDATE_NONE = 0,
    FURI_CANDIDATE_LITERAL,
    FURI_CANDIDATE_GROUP,
} FuriCandidateType;

static bool furi_part_has_text(char *start, char *end)
{
    for (char *p = start; p < end;) {
        if (*p == '{') {
            char *close = memchr(p + 1, '}', end - (p + 1));
            if (!close)
                return true;
            p = close + 1;
        } else {
            return true;
        }
    }
    return false;
}

static FuriCandidateType parse_furi_candidate(char *p, FuriCandidate *candidate)
{
    if (*p != '<')
        return FURI_CANDIDATE_NONE;

    char *pipes[2] = {NULL, NULL};
    int separators = 0;
    bool malformed = false;
    for (char *q = p + 1; *q; q++) {
        if (*q == '\\' && q[1] == 'N')
            malformed = true;
        // Consume the entire literal-pipe escape before looking for fields.
        if (*q == '\\' && q[1] == '|' && q[2] == '\\') {
            q += 2;
            continue;
        }
        if (*q == '\\' && (furi_escapes_char(q[1]) ||
                           q[1] == '{' || q[1] == '}')) {
            q++;
            continue;
        }
        if (*q == '{') {
            char *close = strchr(q + 1, '}');
            if (!close)
                return FURI_CANDIDATE_NONE;
            // Neither pipes nor angle brackets in an ASS override are syntax.
            q = close;
            continue;
        }
        if (*q == '<' || *q == '}' || *q == '\n' || *q == '\r')
            malformed = true;
        if (*q == '|') {
            if (separators < 2)
                pipes[separators] = q;
            if (separators < 3)
                separators++;
        } else if (*q == '>') {
            candidate->end = q + 1;
            if (malformed || separators < 1 || separators > 2)
                return FURI_CANDIDATE_LITERAL;
            candidate->base_start = p + 1;
            candidate->base_end = pipes[0];
            candidate->furi_start = pipes[0] + 1;
            candidate->furi_end = separators == 2 ? pipes[1] : q;
            candidate->gyaku_start = separators == 2 ? pipes[1] + 1 : q;
            candidate->gyaku_end = q;
            if (!furi_part_has_text(candidate->base_start, candidate->base_end) ||
                    (!furi_part_has_text(candidate->furi_start, candidate->furi_end) &&
                     !furi_part_has_text(candidate->gyaku_start, candidate->gyaku_end)))
                return FURI_CANDIDATE_LITERAL;
            return FURI_CANDIDATE_GROUP;
        }
    }
    return FURI_CANDIDATE_NONE;
}

static FuriGroup *append_new_furi_group(TextInfo *text_info)
{
    if (text_info->n_furi_groups >= text_info->max_furi_groups) {
        int new_max = text_info->max_furi_groups ?
            2 * text_info->max_furi_groups : 8;
        if (!ASS_REALLOC_ARRAY(text_info->furi_groups, new_max))
            return NULL;
        text_info->max_furi_groups = new_max;
    }

    FuriGroup *group = &text_info->furi_groups[text_info->n_furi_groups++];
    memset(group, 0, sizeof(*group));
    return group;
}

static bool append_furi_group(RenderContext *state, const FuriCandidate *candidate)
{
    TextInfo *text_info = &state->text_info;
    FuriGroup *group = append_new_furi_group(text_info);
    if (!group)
        return false;

    int group_id = text_info->n_furi_groups - 1;
    group->base_start = text_info->length;
    group->base_group = group_id;
    group->owns_base = true;
    group->place_auto = state->furi_place_auto;
    group->change_pos = state->furi_change_pos;
    group->style = state->furi_style;
    group->scale_x = state->furi_scale_x;
    group->scale_y = state->furi_scale_y;
    group->hspacing = state->furi_hspacing;
    group->offset_x = state->furi_offset_x * state->object_scale;
    group->offset_y = state->furi_offset_y * state->object_scale;
    // Match the base glyph's screen-scaled \fs before hinting normalization.
    group->auto_gap = fabs(state->font_size * state->screen_scale_y) *
        state->object_scale * FURI_AUTO_GAP_FACTOR;
    group->auto_placement = state->furi_auto_placement;
    group->position_explicit = state->furi_position_explicit;

    if (!append_furi_base(state, candidate->base_start, candidate->base_end,
                          group_id))
        return false;

    group->base_len = text_info->length - group->base_start;
    if (group->base_len <= 0)
        return false;
    for (int i = 0; i < group->base_len; i++) {
        GlyphInfo *info = &text_info->glyphs[group->base_start + i];
        info->is_furi_base = true;
        info->furi_group = group_id;
    }

    bool normal = furi_part_has_text(candidate->furi_start, candidate->furi_end);
    bool gyaku = furi_part_has_text(candidate->gyaku_start, candidate->gyaku_end);
    if (!normal) {
        group->opposite_side = true;
        return append_furi_reading(state, candidate->gyaku_start,
                                    candidate->gyaku_end, group, group_id);
    }

    // Copy only configuration before filling either sidecar. Reallocation of
    // the group array must not leave a stale pointer to the owning sidecar.
    FuriGroup other = *group;
    if (!append_furi_reading(state, candidate->furi_start,
                             candidate->furi_end, group, group_id))
        return false;
    if (gyaku) {
        group = append_new_furi_group(text_info);
        if (!group)
            return false;
        *group = other;
        group->owns_base = false;
        group->opposite_side = true;
        int other_id = text_info->n_furi_groups - 1;
        if (!append_furi_reading(state, candidate->gyaku_start,
                                 candidate->gyaku_end, group, other_id))
            return false;
    }
    return true;
}

static int parse_column_tag_value(char *start, char *end)
{
    ass_override_spaces(&start, end);
    int value = ass_unicode_decimal_value(override_codepoint(&start, end));
    ass_override_spaces(&start, end);
    if (start != end || (value != 0 && value != 1))
        return -1;
    return value;
}

static void scan_column_override_block(char *p, char *end, bool *active,
                                       bool *seen_active)
{
    while (p < end) {
        while (ass_override_peek(&p, end) && *p != '\\')
            p++;
        if (p >= end)
            break;

        p++;
        ass_override_spaces(&p, end);
        char *name = p;
        while (ass_override_peek(&p, end) && *p != '\\' && *p != '(')
            p++;
        char *name_end = p;

        if (ass_override_prefix(&name, name_end, "col")) {
            int value = parse_column_tag_value(name, name_end);
            if (value == 0) {
                *active = false;
            } else if (value == 1) {
                *active = true;
                *seen_active = true;
            }
        }

        p = skip_parenthesized_tag_args(p, end, NULL);
    }
}

static bool event_has_active_column(char *text)
{
    bool active = false;
    bool seen_active = false;
    char *p = text;

    while (*p) {
        if (*p == '\\' && p[1]) {
            p += 2;
        } else if (*p == '{') {
            char *end = strchr(p, '}');
            if (!end)
                break;
            scan_column_override_block(p + 1, end, &active, &seen_active);
            p = end + 1;
        } else {
            p++;
        }
    }

    return seen_active;
}

/* Append fixed header/name text through the same glyph path as message bodies. */
static bool append_chat_literal(RenderContext *state, const char *text)
{
    TextInfo *info = &state->text_info;
    char *p = (char *) text;
    while (*p) {
        unsigned code = ass_utf8_get_char(&p);
        if (!append_glyph_to_target(state, &info->glyphs, &info->event_text,
                                    &info->breaks, &info->length,
                                    &info->max_glyphs, code,
                                    (ASS_StringView) {NULL, 0}, false, -1))
            return false;
    }
    return true;
}

static bool append_chat_break(RenderContext *state)
{
    TextInfo *info = &state->text_info;
    state->effect_type = EF_NONE;
    state->effect_timing = 0;
    state->effect_skip_timing = 0;
    state->reset_effect = false;
    return append_glyph_to_target(state, &info->glyphs, &info->event_text,
                                  &info->breaks, &info->length,
                                  &info->max_glyphs, '\n',
                                  (ASS_StringView) {NULL, 0}, false, -1);
}

// Parse one part of an event, retaining render state between chat messages.
static bool parse_event_fragment(RenderContext *state, char *p)
{
    TextInfo *text_info = &state->text_info;
    char *q;
    char *ordinary_angle_end = NULL;

    // Event parsing.
    while (true) {
        ASS_StringView drawing_text = {NULL, 0};

        // get next char, executing style override
        // this affects render_context
        unsigned code = 0;
        while (*p) {
            if ((*p == '{') && (q = strchr(p, '}'))) {
                p = ass_parse_override_block(state, p + 1, q);
                assert(*p == '}');
                p++;
            } else if (state->drawing_scale) {
                q = p;
                if (*p == '{')
                    q++;
                while ((*q != '{') && (*q != 0))
                    q++;
                drawing_text.str = p;
                drawing_text.len = q - p;
                code = 0xfffc; // object replacement character
                p = q;
                break;
            } else if (state->column_event && state->column_active && *p == '|') {
                finish_column_cell(state);
                state->column_index++;
                if (!ensure_column_count(text_info, state->column_index + 1))
                    goto fail;
                text_info->column_count =
                    FFMAX(text_info->column_count, state->column_index + 1);
                apply_column_cell_style(state);
                p++;
            } else {
                if (ordinary_angle_end && p >= ordinary_angle_end)
                    ordinary_angle_end = NULL;
                if (!ordinary_angle_end && state->furi_enabled &&
                        !state->native_vertical && *p == '<') {
                    FuriCandidate candidate;
                    FuriCandidateType type = parse_furi_candidate(p, &candidate);
                    if (type == FURI_CANDIDATE_GROUP) {
                        if (!append_furi_group(state, &candidate))
                            goto fail;
                        p = candidate.end;
                        code = 0;
                        break;
                    } else if (type == FURI_CANDIDATE_LITERAL) {
                        // Leave the whole candidate on the ordinary text path,
                        // including normal override execution. Do not claim a
                        // nested '<' in this rejected candidate as a new group.
                        ordinary_angle_end = candidate.end;
                    }
                }
                if (ordinary_angle_end && p + 2 < ordinary_angle_end &&
                        p[0] == '\\' && p[1] == '|' && p[2] == '\\') {
                    code = '|';
                    p += 3;
                    break;
                }
                code = ass_get_next_char(state, &p);
                break;
            }
        }

        if (code == 0 && *p)
            continue;
        if (code == 0)
            break;

        if (!append_glyph_to_target(state, &text_info->glyphs,
                                    &text_info->event_text,
                                    &text_info->breaks,
                                    &text_info->length,
                                    &text_info->max_glyphs,
                                    code, drawing_text, false, -1))
            goto fail;

        if (state->column_event && code == '\n') {
            finish_column_cell(state);
            state->column_row++;
            state->column_index = 0;
            text_info->column_rows =
                FFMAX(text_info->column_rows, state->column_row + 1);
            apply_column_cell_style(state);
        }

        state->effect_type = EF_NONE;
        state->effect_timing = 0;
        state->effect_skip_timing = 0;
        state->reset_effect = false;
    }

    return !state->karaoke_alloc_failed;

fail:
    return false;
}

/* Borrow the existing paint state for one preset-styled message. There are no
 * allocations or glyph copies; overrides outside these visual channels (font,
 * panel, header, receipt state, etc.) keep their ordinary inheritance. */
static void chat_save_visual_state(RenderContext *state, ColumnStyleState *base)
{
    base->mask = 0;
    capture_column_style(state, base,
        COLUMN_STYLE_COLOR0 | COLUMN_STYLE_COLOR1 | COLUMN_STYLE_COLOR2 |
        COLUMN_STYLE_BORDER_X | COLUMN_STYLE_BORDER_Y);
    memcpy(base->gradient.layer, state->gradient.layer,
           3 * sizeof(*base->gradient.layer));
    memcpy(base->mangetsu_gradient.layer, state->mangetsu_gradient.layer,
           3 * sizeof(*base->mangetsu_gradient.layer));
    memcpy(base->mangetsu_gradient.alpha, state->mangetsu_gradient.alpha,
           3 * sizeof(*base->mangetsu_gradient.alpha));
    memcpy(base->mangetsu_gradient.border, state->mangetsu_gradient.border,
           sizeof(base->mangetsu_gradient.border));
    memcpy(base->mangetsu_gradient.border_alpha, state->mangetsu_gradient.border_alpha,
           sizeof(base->mangetsu_gradient.border_alpha));
    memcpy(base->border_layers, state->border_layers, sizeof(base->border_layers));
    base->pattern = state->pattern;
    memcpy(base->image_fill.layer, state->image_fill.layer,
           3 * sizeof(*base->image_fill.layer));
}

static void chat_restore_visual_state(RenderContext *state,
                                       const ColumnStyleState *base)
{
    memcpy(state->c, base->c, 3 * sizeof(*state->c));
    memcpy(state->gradient.layer, base->gradient.layer,
           3 * sizeof(*base->gradient.layer));
    memcpy(state->mangetsu_gradient.layer, base->mangetsu_gradient.layer,
           3 * sizeof(*base->mangetsu_gradient.layer));
    memcpy(state->mangetsu_gradient.alpha, base->mangetsu_gradient.alpha,
           3 * sizeof(*base->mangetsu_gradient.alpha));
    memcpy(state->mangetsu_gradient.border, base->mangetsu_gradient.border,
           sizeof(base->mangetsu_gradient.border));
    memcpy(state->mangetsu_gradient.border_alpha, base->mangetsu_gradient.border_alpha,
           sizeof(base->mangetsu_gradient.border_alpha));
    memcpy(state->border_layers, base->border_layers, sizeof(base->border_layers));
    state->border_x = base->border_x;
    state->border_y = base->border_y;
    state->pattern = base->pattern;
    memcpy(state->image_fill.layer, base->image_fill.layer,
           3 * sizeof(*base->image_fill.layer));
}

// Parse event text into one native glyph stream, with explicit chat ranges.
static bool parse_events(RenderContext *state, ASS_Event *event,
                         const ASS_ChatScene *chat, ChatRange *ranges,
                         ChatRange *title)
{
    TextInfo *info = &state->text_info;
    state->karaoke_timeline_enabled =
        strchr(event->Text, '<') && strchr(event->Text, '|');
    if (!chat) {
        if (event_has_active_column(event->Text) && !begin_column_layout(state))
            goto fail;
        if (parse_event_fragment(state, event->Text))
            return true;
        goto fail;
    }

    if (!parse_event_fragment(state, chat->prefix))
        goto fail;
    if (chat->title && *chat->title) {
        title->start = info->length;
        if (!append_chat_literal(state, chat->title))
            goto fail;
        title->end = info->length;
    }
    for (int i = 0; i < chat->count; i++) {
        const ASS_ChatMessage *message = &chat->messages[i];
        if (info->length && !append_chat_break(state))
            goto fail;
        ranges[i].start = info->length;
        char *body = message->text;
        if (chat->has_side_styles) {
            state->chat_side_only_parse = true;
            for (char *p = body; *p == '{';) {
                char *end = strchr(p + 1, '}');
                if (!end)
                    break;
                ass_parse_override_block(state, p + 1, end);
                p = end + 1;
            }
            state->chat_side_only_parse = false;
        }
        bool preset = state->chat_side[message->side].enabled;
        unsigned reset_serial = state->chat_reset_serial;
        ColumnStyleState base;
        ChatBubbleStyle base_bubble;
        if (preset) {
            chat_save_visual_state(state, &base);
            base_bubble = state->chat_bubble;
            ass_apply_chat_side_style(state, message->side);
        }
        while (*body == '{') {
            char *end = strchr(body + 1, '}');
            if (!end)
                break;
            ass_parse_override_block(state, body + 1, end);
            body = end + 1;
        }
        if (chat->mode != ASS_CHAT_MODE_ALIGNMENT_SHORTHAND &&
            chat->show_names &&
            message->speaker && *message->speaker) {
            if (!append_chat_literal(state, message->speaker) ||
                !append_chat_break(state))
                goto fail;
            ranges[i].name_end = info->length;
        } else
            ranges[i].name_end = ranges[i].start;
        if (!parse_event_fragment(state, body))
            goto fail;
        ranges[i].end = info->length;
        ranges[i].receipt = state->chat_receipt;
        ranges[i].has_preset = preset;
        if (preset)
            ranges[i].empty_bubble = state->chat_bubble;
        ranges[i].receipt_color = state->c[0];
        if (preset && reset_serial == state->chat_reset_serial) {
            chat_restore_visual_state(state, &base);
            state->chat_bubble = base_bubble;
        }
    }
    return true;

fail:
    free_render_context(state);
    return false;
}

// Process render_priv->text_info and load glyph outlines.
static void retrieve_glyphs_from_list(RenderContext *state,
                                      GlyphInfo *glyphs, int length)
{
    int i;

    for (i = 0; i < length; i++) {
        GlyphInfo *info = glyphs + i;
        GlyphInfo *root = info;
        do {
            info->distort_enabled = root->distort_enabled;
            info->distort_extended = root->distort_extended;
            info->distort = root->distort;
            get_outline_glyph(state, info);
            info->fill_bbox = info->bbox;
            info = info->next;
        } while (info);
        info = glyphs + i;

        // Add additional space after italic to non-italic style changes
        if (i && glyphs[i - 1].italic && !info->italic) {
            int back = i - 1;
            GlyphInfo *og = &glyphs[back];
            while (back && og->bbox.x_max - og->bbox.x_min == 0
                    && og->italic)
                og = &glyphs[--back];
            if (og->bbox.x_max > og->cluster_advance.x)
                og->cluster_advance.x = og->bbox.x_max;
        }

        // add horizontal letter spacing
        info->cluster_advance.x += info->hspacing_scaled;
    }
}

static void retrieve_glyphs(RenderContext *state)
{
    retrieve_glyphs_from_list(state, state->text_info.glyphs,
                              state->text_info.length);
}

// Preliminary layout (for line wrapping)
static void preliminary_layout_list(GlyphInfo *glyphs, int length)
{
    ASS_Vector pen = { 0, 0 };
    for (int i = 0; i < length; i++) {
        GlyphInfo *info = glyphs + i;
        ASS_Vector cluster_pen = pen;
        do {
            info->pos.x = cluster_pen.x;
            info->pos.y = cluster_pen.y;

            cluster_pen.x += info->advance.x;
            cluster_pen.y += info->advance.y;

            info = info->next;
        } while (info);
        info = glyphs + i;
        pen.x += info->cluster_advance.x;
        pen.y += info->cluster_advance.y;
    }
}

static void preliminary_layout(RenderContext *state)
{
    preliminary_layout_list(state->text_info.glyphs, state->text_info.length);
}

// Reorder text into visual order
static void reorder_text(RenderContext *state)
{
    ASS_Renderer *render_priv = state->renderer;
    TextInfo *text_info = &state->text_info;
    FriBidiStrIndex *cmap = ass_shaper_reorder(state->shaper, text_info);
    if (!cmap) {
        ass_msg(render_priv->library, MSGL_ERR, "Failed to reorder text");
        ass_shaper_cleanup(state->shaper, text_info);
        free_render_context(state);
        return;
    }

    // Reposition according to the map
    ASS_Vector pen = { 0, 0 };
    int lineno = 1;
    for (int i = 0; i < text_info->length; i++) {
        GlyphInfo *info = text_info->glyphs + cmap[i];
        // linebreak marks the first glyph of the new visual line.
        if (text_info->glyphs[i].linebreak) {
            pen.x = 0;
            pen.y += double_to_d6(text_info->lines[lineno-1].desc);
            pen.y += double_to_d6(text_info->lines[lineno].asc);
            pen.y += double_to_d6(line_spacing(state));
            lineno++;
        }
        int line_id = lineno - 1;
        for (GlyphInfo *g = info; g; g = g->next)
            g->line = line_id;
        if (info->skip)
            continue;
        ASS_Vector cluster_pen = pen;
        pen.x += info->cluster_advance.x;
        pen.y += info->cluster_advance.y;
        while (info) {
            info->pos.x = info->offset.x + cluster_pen.x;
            info->pos.y = info->offset.y + cluster_pen.y + info->vshift;
            cluster_pen.x += info->advance.x;
            cluster_pen.y += info->advance.y;
            info = info->next;
        }
    }
}

static TextInfo furi_group_text_info(FuriGroup *group)
{
    TextInfo text = {0};
    text.glyphs = group->glyphs;
    text.event_text = group->event_text;
    text.length = group->length;
    return text;
}

static bool reorder_furi_group(RenderContext *state, FuriGroup *group)
{
    TextInfo furi_text = furi_group_text_info(group);
    FriBidiStrIndex *cmap = ass_shaper_reorder(state->furi_shaper, &furi_text);
    if (!cmap)
        return false;

    ASS_Vector pen = {0, 0};
    for (int i = 0; i < group->length; i++) {
        GlyphInfo *info = group->glyphs + cmap[i];
        if (info->skip)
            continue;

        ASS_Vector cluster_pen = pen;
        pen.x += info->cluster_advance.x;
        pen.y += info->cluster_advance.y;
        while (info) {
            info->pos.x = info->offset.x + cluster_pen.x;
            info->pos.y = info->offset.y + cluster_pen.y + info->vshift;
            cluster_pen.x += info->advance.x;
            cluster_pen.y += info->advance.y;
            info = info->next;
        }
    }

    return true;
}

static int32_t clamp_i64_to_i32(int64_t value)
{
    if (value > INT_MAX)
        return INT_MAX;
    if (value < INT_MIN)
        return INT_MIN;
    return value;
}

static int32_t furi_base_advance_width(RenderContext *state, FuriGroup *group)
{
    TextInfo *text_info = &state->text_info;
    int64_t width = 0;

    for (int i = 0; i < group->base_len; i++) {
        GlyphInfo *root = &text_info->glyphs[group->base_start + i];
        if (!root->skip)
            width += root->cluster_advance.x;
    }

    return clamp_i64_to_i32(FFMAX(0, width));
}

static int32_t furi_text_advance_width(FuriGroup *group)
{
    int64_t width = 0;
    int32_t trailing_spacing = 0;
    bool have = false;

    for (int i = 0; i < group->length; i++) {
        GlyphInfo *root = &group->glyphs[i];
        if (root->skip)
            continue;
        width += root->cluster_advance.x;
        trailing_spacing = root->hspacing_scaled;
        have = true;
    }

    if (have)
        width -= trailing_spacing;
    return clamp_i64_to_i32(FFMAX(0, width));
}

static ASS_DVector evaluate_pos_segment(const RenderContext *state,
                                        ASS_DVector anchor,
                                        int64_t segment_start,
                                        int64_t time)
{
    ASS_DVector pos = anchor;
    for (int i = 0; i < state->n_pos_transforms; i++) {
        const PosTransformState *tr = &state->pos_transforms[i];
        if (tr->t1 > segment_start || tr->t2 <= segment_start)
            continue;

        double duration = (double) ((int64_t) tr->t2 - segment_start);
        double elapsed = (double) (time - segment_start);
        double progress = duration > 0.0 ? elapsed / duration : 1.0;
        progress = FFMINMAX(progress, 0.0, 1.0);
        double eased = pow(progress, tr->accel);
        pos.x += (tr->target_x - anchor.x) * eased;
        pos.y += (tr->target_y - anchor.y) * eased;
    }
    return pos;
}

/*
 * Evaluate all animated \pos transforms without accumulating per-frame state.
 * Every start/end boundary resolves the previous segment at that exact time,
 * then all transforms active on the new segment are rebased on the resolved
 * position.  Consequently rendering frames out of order gives the same result.
 */
static ASS_DVector evaluate_animated_position(RenderContext *state)
{
    int64_t now = state->renderer->time - state->event->Start;
    if (!state->n_pos_transforms)
        return evaluate_motion_at(state, now);

    int64_t first = INT64_MAX;
    for (int i = 0; i < state->n_pos_transforms; i++) {
        first = FFMIN(first, (int64_t) state->pos_transforms[i].t1);
        state->pos_transforms[i].target_x = state->pos_transforms[i].x;
        state->pos_transforms[i].target_y = state->pos_transforms[i].y;
    }

    if (now <= first)
        return evaluate_motion_at(state, now);

    ASS_DVector anchor = evaluate_motion_at(state, first);
    int64_t segment_start = first;
    for (;;) {
        int64_t next = INT64_MAX;
        for (int i = 0; i < state->n_pos_transforms; i++) {
            PosTransformState *tr = &state->pos_transforms[i];
            if (tr->t1 == segment_start) {
                if (tr->relative_x)
                    tr->target_x = anchor.x + tr->x;
                if (tr->relative_y)
                    tr->target_y = anchor.y + tr->y;
            }
            if (tr->t1 > segment_start)
                next = FFMIN(next, (int64_t) tr->t1);
            if (tr->t2 > segment_start)
                next = FFMIN(next, (int64_t) tr->t2);
        }

        int64_t sample = FFMIN(now, next);
        ASS_DVector pos = evaluate_pos_segment(state, anchor,
                                               segment_start, sample);
        if (now <= next || next == INT64_MAX)
            return pos;

        anchor = pos;
        segment_start = next;
    }
}

static bool furi_group_visual_x_bounds(FuriGroup *group, double *left,
                                       double *right);

static int32_t furi_text_reserved_width(FuriGroup *group)
{
    int32_t advance_width = furi_text_advance_width(group);
    double left, right;
    if (!furi_group_visual_x_bounds(group, &left, &right))
        return advance_width;

    int32_t visual_width = double_to_d6(right - left);
    return FFMAX(advance_width, visual_width);
}

static void scale_furi_group_x(FuriGroup *group, double scale)
{
    for (int i = 0; i < group->length; i++) {
        GlyphInfo *root = &group->glyphs[i];
        root->cluster_advance.x = double_to_d6(
                d6_to_double(root->cluster_advance.x) * scale);

        for (GlyphInfo *info = root; info; info = info->next) {
            info->offset.x = double_to_d6(d6_to_double(info->offset.x) * scale);
            info->advance.x = double_to_d6(d6_to_double(info->advance.x) * scale);
            info->bbox.x_min = double_to_d6(d6_to_double(info->bbox.x_min) * scale);
            info->bbox.x_max = double_to_d6(d6_to_double(info->bbox.x_max) * scale);
            info->hspacing_scaled = double_to_d6(
                    d6_to_double(info->hspacing_scaled) * scale);
            info->scale_x *= scale;
            info->transform.scale.x *= scale;
        }
    }
}

static void apply_furi_group_layout(RenderContext *state, FuriGroup *group)
{
    int32_t base_width = furi_base_advance_width(state, group);
    group->base_width = base_width;

    // Styles 0 and 1 keep the base's normal shaped advance.  Ruby may
    // overhang ordinary text; only ruby/ruby overlap is resolved later.
    if (group->style != 2)
        return;

    int32_t furi_width = furi_text_reserved_width(group);
    if (base_width > 0 && furi_width > base_width) {
        double scale = (double) base_width / furi_width;
        scale_furi_group_x(group, scale);
    }
}

static bool furi_group_visual_x_bounds(FuriGroup *group, double *left,
                                       double *right)
{
    bool have = false;
    *left = DBL_MAX;
    *right = -DBL_MAX;

    for (int i = 0; i < group->length; i++) {
        GlyphInfo *root = &group->glyphs[i];
        if (root->skip)
            continue;
        for (GlyphInfo *info = root; info; info = info->next) {
            double x = d6_to_double(info->pos.x);
            *left = FFMIN(*left, x + d6_to_double(info->bbox.x_min));
            *right = FFMAX(*right, x + d6_to_double(info->bbox.x_max));
            have = true;
        }
    }

    return have && *left < *right;
}

/* Occupied outline/stroke/shadow bounds in event-local screen units. Keep
 * these separate from base advances and typographic line metrics. Blur is a
 * soft filter, not an attachment edge. Include the rasterizer's one-pixel
 * antialias footprint on each edge; touching fringe pixels are visually
 * occupied even when the mathematical outlines do not touch. This padding
 * belongs only to ruby geometry, never ordinary glyph/line metrics. */
static bool furi_occupied_bounds(RenderContext *state, GlyphInfo *glyphs,
                                  int length, ASS_DRect *bounds,
                                  ASS_DRect *ink, double *top, double *bottom)
{
    *ink = (ASS_DRect) {DBL_MAX, DBL_MAX, -DBL_MAX, -DBL_MAX};
    if (top) *top = DBL_MAX;
    if (bottom) *bottom = -DBL_MAX;
    *bounds = (ASS_DRect) {DBL_MAX, DBL_MAX, -DBL_MAX, -DBL_MAX};
    for (int i = 0; i < length; i++) {
        if (glyphs[i].skip)
            continue;
        for (GlyphInfo *info = &glyphs[i]; info; info = info->next) {
            if (top) *top = FFMIN(*top, d6_to_double(info->pos.y - info->asc));
            if (bottom) *bottom = FFMAX(*bottom, d6_to_double(info->pos.y + info->desc));
            if (info->bbox.x_min >= info->bbox.x_max ||
                    info->bbox.y_min >= info->bbox.y_max)
                continue;
            double x = d6_to_double(info->pos.x);
            double y = d6_to_double(info->pos.y);
            ink->x_min = FFMIN(ink->x_min, x + d6_to_double(info->bbox.x_min));
            ink->x_max = FFMAX(ink->x_max, x + d6_to_double(info->bbox.x_max));
            ink->y_min = FFMIN(ink->y_min, y + d6_to_double(info->bbox.y_min));
            ink->y_max = FFMAX(ink->y_max, y + d6_to_double(info->bbox.y_max));
            double bx = glyph_border_max_x(info) * state->border_scale_x /
                        state->renderer->par_scale_x;
            double by = glyph_border_max_y(info) * state->border_scale_y;
            double sx = info->shadow_x * state->border_scale_x /
                        state->renderer->par_scale_x;
            double sy = info->shadow_y * state->border_scale_y;
            bounds->x_min = FFMIN(bounds->x_min,
                x + d6_to_double(info->bbox.x_min) - bx + FFMIN(0, sx) - 1);
            bounds->x_max = FFMAX(bounds->x_max,
                x + d6_to_double(info->bbox.x_max) + bx + FFMAX(0, sx) + 1);
            bounds->y_min = FFMIN(bounds->y_min,
                y + d6_to_double(info->bbox.y_min) - by + FFMIN(0, sy) - 1);
            bounds->y_max = FFMAX(bounds->y_max,
                y + d6_to_double(info->bbox.y_max) + by + FFMAX(0, sy) + 1);
        }
    }
    return bounds->x_min < bounds->x_max && bounds->y_min < bounds->y_max;
}

static void shift_furi_group(FuriGroup *group, int32_t dx, int32_t dy)
{
    for (int i = 0; i < group->length; i++)
        for (GlyphInfo *info = &group->glyphs[i]; info; info = info->next) {
            info->pos.x += dx;
            info->pos.y += dy;
        }
    group->annotation_bounds.x_min += d6_to_double(dx);
    group->annotation_bounds.x_max += d6_to_double(dx);
    group->annotation_bounds.y_min += d6_to_double(dy);
    group->annotation_bounds.y_max += d6_to_double(dy);
}

static void position_furi_group(RenderContext *state, FuriGroup *group)
{
    TextInfo *text = &state->text_info;
    ASS_DRect base_ink, reading_ink;
    double base_top, base_bottom;
    int line;
    group->geometry_valid = false;
    // Even an all-space reading has a logical side. Its visible companion
    // must remain opposite it when there is no annotation ink to measure.
    line = text->glyphs[group->base_start].line;
    group->below = group->place_auto && text->n_lines == 2 && line == 1;
    if (group->opposite_side)
        group->below = !group->below;
    if (!group->owns_base)
        group->below = !text->furi_groups[group->base_group].below;
    if (!furi_occupied_bounds(state, text->glyphs + group->base_start,
                                   group->base_len, &group->base_bounds,
                                   &base_ink, &base_top, &base_bottom) ||
            !furi_occupied_bounds(state, group->glyphs, group->length,
                                   &group->annotation_bounds, &reading_ink,
                                   NULL, NULL))
        return;

    // Retain the common typographic attachment height, extending it when ink
    // or strokes exceed an unusual font's ascender/descender metrics.
    base_top = FFMIN(base_top, group->base_bounds.y_min);
    base_bottom = FFMAX(base_bottom, group->base_bounds.y_max);

    ASS_DRect *ann = &group->annotation_bounds;
    double minimum = FFMAX(1.0 / 64, FFMAX(group->auto_gap * 0.5,
        (ann->y_max - ann->y_min) * FURI_AUTO_GAP_FACTOR));
    double gap = !group->position_explicit && group->auto_placement ?
                 group->auto_gap : 0.0;
    double offset = y2scr_offset(state, group->offset_y);
    // furipos remains an upward-positive offset from the upper attachment.
    // A sufficiently negative offset selects the lower side; smaller negative
    // offsets keep their legacy direction, with visual clearance enforced.
    double dy = base_top - ann->y_max - gap - offset;
    bool below = (ann->y_min + ann->y_max) / 2 + dy >
                 (group->base_bounds.y_min + group->base_bounds.y_max) / 2;
    if (group->place_auto && text->n_lines == 2) {
        below = line == 1;
        dy = below ? base_bottom - ann->y_min + gap - offset : dy;
    }
    double opposite_gap = FFMAX(0.0, below ? ann->y_min + dy - base_bottom :
                                            base_top - ann->y_max - dy);
    if (!group->owns_base) {
        FuriGroup *owner = &text->furi_groups[group->base_group];
        below = !owner->below;
        opposite_gap = owner->placement_gap;
        dy = below ? base_bottom + opposite_gap - ann->y_min :
                     base_top - opposite_gap - ann->y_max;
    } else if (group->opposite_side) {
        below = !below;
        dy = below ? base_bottom + opposite_gap - ann->y_min :
                     base_top - opposite_gap - ann->y_max;
    }
    // The same clearance constraint handles either side. Only ruby moves.
    dy = below ? FFMAX(dy, group->base_bounds.y_max + minimum - ann->y_min) :
                 FFMIN(dy, group->base_bounds.y_min - minimum - ann->y_max);
    double dx = (base_ink.x_min + base_ink.x_max -
                 reading_ink.x_min - reading_ink.x_max) / 2 +
                x2scr_offset(state, group->offset_x);
    shift_furi_group(group, double_to_d6(dx), double_to_d6(dy));
    group->below = below;
    group->placement_gap = FFMAX(0.0, below ? ann->y_min - base_bottom :
                                             base_top - ann->y_max);
    group->geometry_valid = true;
    for (int i = 0; i < group->length; i++)
        for (GlyphInfo *info = &group->glyphs[i]; info; info = info->next)
            info->line = line;
}

typedef struct {
    int group;
    int line;
    bool below;
    double left, right, gap, base_center;
} FuriPlacement;

static int compare_furi_placement(const void *a, const void *b)
{
    const FuriPlacement *pa = a, *pb = b;
    if (pa->line != pb->line)
        return pa->line < pb->line ? -1 : 1;
    if (pa->below != pb->below)
        return pa->below ? 1 : -1;
    // Overhang can put a later wide reading's left edge before an earlier
    // narrow one. Preserve visual base order instead of swapping readings.
    if (pa->base_center != pb->base_center)
        return pa->base_center < pb->base_center ? -1 : 1;
    if (pa->left != pb->left)
        return pa->left < pb->left ? -1 : 1;
    return pa->group - pb->group;
}

static int collect_furi_placements(TextInfo *text, FuriPlacement *placements)
{
    int n = 0;
    for (int i = 0; i < text->n_furi_groups; i++) {
        FuriGroup *group = &text->furi_groups[i];
        if (!group->geometry_valid)
            continue;
        placements[n++] = (FuriPlacement) {
            .group = i, .line = text->glyphs[group->base_start].line,
            .below = group->below,
            .left = group->annotation_bounds.x_min,
            .right = group->annotation_bounds.x_max,
            .base_center = (group->base_bounds.x_min + group->base_bounds.x_max) / 2,
            .gap = FFMAX(1.0 / 64, FFMAX(group->auto_gap,
                (group->annotation_bounds.y_max -
                 group->annotation_bounds.y_min) * 0.08)),
        };
    }
    qsort(placements, n, sizeof(*placements), compare_furi_placement);
    return n;
}

static bool same_furi_band(const FuriPlacement *a, const FuriPlacement *b)
{
    return a->line == b->line && a->below == b->below;
}

/* Recenter sidecars after base layout, line shifts and final transforms. */
static void position_furi_groups(RenderContext *state)
{
    TextInfo *text = &state->text_info;
    for (int i = 0; i < text->n_furi_groups; i++)
        position_furi_group(state, &text->furi_groups[i]);
}

static bool add_furi_spacing_before_group(RenderContext *state,
                                          FuriGroup *group, int line,
                                          int32_t spacing)
{
    TextInfo *text = &state->text_info;
    for (int i = group->base_start - 1; i >= 0; i--) {
        GlyphInfo *root = &text->glyphs[i];
        if (root->skip || root->line != line)
            continue;
        root->cluster_advance.x += spacing;
        return true;
    }
    return false;
}

static bool furi_reserves_vertical_space(TextInfo *text)
{
    for (int i = 0; i < text->n_furi_groups; i++)
        if (text->furi_groups[i].change_pos)
            return true;
    return false;
}

/* Horizontal accommodation is independent of vertical reservation. Both
 * annotation sides share the existing base-spacing path in either mode. */
static void resolve_furi_group_collisions(RenderContext *state)
{
    TextInfo *text = &state->text_info;
    int count = text->n_furi_groups;
    if (count < 2)
        return;
    FuriPlacement *p = calloc(count, sizeof(*p));
    int32_t *spacing = calloc(count, sizeof(*spacing));
    if (!p || !spacing) {
        free(p);
        free(spacing);
        return;
    }
    for (int attempt = 0; attempt < count; attempt++) {
        memset(spacing, 0, count * sizeof(*spacing));
        for (int i = 0; i < count; i++)
            position_furi_group(state, &text->furi_groups[i]);
        int n = collect_furi_placements(text, p);
        bool resolved = false;
        for (int i = 1; i < n; i++) {
            FuriGroup *group = &text->furi_groups[p[i].group];
            if (!same_furi_band(&p[i - 1], &p[i]) || group->style == 2)
                continue;
            double overlap = p[i - 1].right + FFMAX(p[i - 1].gap, p[i].gap) -
                             p[i].left;
            if (overlap <= 0)
                continue;
            int owner = group->base_group;
            spacing[owner] = FFMAX(spacing[owner], double_to_d6(overlap) + 1);
        }
        // The two sides share a base range. Reserve their maximum required
        // spacing, rather than counting the same collision twice.
        for (int i = 0; i < count; i++) {
            FuriGroup *group = &text->furi_groups[i];
            int line = text->glyphs[group->base_start].line;
            if (spacing[i] && add_furi_spacing_before_group(state, group,
                                                            line, spacing[i]))
                resolved = true;
        }
        if (!resolved)
            break;
        reorder_text(state);
    }
    free(p);
    free(spacing);
}

static void update_glyph_jitter_offsets(RenderContext *state)
{
#if DEBUG_LEVEL >= 2
    jitter_run_debug_tests();
#endif
    TextInfo *text_info = &state->text_info;
    long long time_100ns = jitter_current_time(state);

    update_glyph_jitter_offsets_list(state, text_info->glyphs,
                                     text_info->length, time_100ns);
    for (int i = 0; i < text_info->n_furi_groups; i++) {
        FuriGroup *group = &text_info->furi_groups[i];
        update_glyph_jitter_offsets_list(state, group->glyphs,
                                         group->length, time_100ns);
    }
}

typedef struct {
    int segment;
    double left;
    double right;
    bool rtl;
} FuriVisualKaraokeRegion;

static int compare_furi_karaoke_region(const void *a, const void *b)
{
    const FuriVisualKaraokeRegion *ra = a;
    const FuriVisualKaraokeRegion *rb = b;
    if (ra->left < rb->left)
        return -1;
    if (ra->left > rb->left)
        return 1;
    return (ra->segment > rb->segment) - (ra->segment < rb->segment);
}

static bool prepare_furi_karaoke_regions(TextInfo *text_info,
                                         FuriGroup *group)
{
    if (!group->owns_base || !group->has_internal_karaoke ||
            !text_info->n_karaoke_segments)
        return true;

    int count = text_info->n_karaoke_segments;
    double *left = malloc(count * sizeof(*left));
    double *right = malloc(count * sizeof(*right));
    uint8_t *direction = calloc(count, sizeof(*direction));
    if (!left || !right || !direction) {
        free(left);
        free(right);
        free(direction);
        return false;
    }
    for (int i = 0; i < count; i++) {
        left[i] = DBL_MAX;
        right[i] = -DBL_MAX;
    }

    for (int i = 0; i < group->length; i++) {
        GlyphInfo *root = &group->glyphs[i];
        int segment = root->karaoke_segment;
        if (root->skip || segment < 0 || segment >= count)
            continue;
        if (!(direction[segment] & 1))
            direction[segment] = 1 | (root->karaoke_rtl ? 2 : 0);

        double x0 = d6_to_double(root->pos.x);
        double x1 = x0 + d6_to_double(root->cluster_advance.x -
                                      root->hspacing_scaled);
        left[segment] = FFMIN(left[segment], FFMIN(x0, x1));
        right[segment] = FFMAX(right[segment], FFMAX(x0, x1));
        for (GlyphInfo *info = root; info; info = info->next) {
            double x = d6_to_double(info->pos.x);
            left[segment] = FFMIN(left[segment],
                                  x + d6_to_double(info->bbox.x_min));
            right[segment] = FFMAX(right[segment],
                                   x + d6_to_double(info->bbox.x_max));
        }
    }

    int regions = 0;
    double total = 0.0;
    for (int i = 0; i < count; i++) {
        if (left[i] >= right[i])
            continue;
        regions++;
        total += right[i] - left[i];
    }
    if (!regions || total <= 0.0) {
        free(left);
        free(right);
        free(direction);
        return true;
    }

    FuriVisualKaraokeRegion *visual =
        malloc(regions * sizeof(*visual));
    if (!visual) {
        free(left);
        free(right);
        free(direction);
        return false;
    }
    int visual_count = 0;
    for (int i = 0; i < count; i++) {
        if (left[i] >= right[i])
            continue;
        visual[visual_count++] = (FuriVisualKaraokeRegion) {
            .segment = i,
            .left = left[i],
            .right = right[i],
            .rtl = direction[i] & 2,
        };
    }
    // Segment ids are source-ordered; base ownership is visual after bidi.
    qsort(visual, visual_count, sizeof(*visual),
          compare_furi_karaoke_region);

    group->karaoke_regions = calloc(regions, sizeof(*group->karaoke_regions));
    if (!group->karaoke_regions) {
        free(visual);
        free(left);
        free(right);
        free(direction);
        return false;
    }

    double cursor = 0.0;
    for (int i = 0; i < visual_count; i++) {
        double width = (visual[i].right - visual[i].left) / total;
        FuriKaraokeRegion *region =
            &group->karaoke_regions[group->n_karaoke_regions++];
        region->segment = visual[i].segment;
        region->rtl = visual[i].rtl;
        region->start = cursor;
        cursor += width;
        region->end = cursor;
    }
    group->karaoke_regions[group->n_karaoke_regions - 1].end = 1.0;

    free(visual);
    free(left);
    free(right);
    free(direction);
    return true;
}

static bool prepare_furi_groups(RenderContext *state)
{
    TextInfo *text_info = &state->text_info;
    if (!text_info->n_furi_groups)
        return true;

    /* A furigana reading is one bidi paragraph. Karaoke/style run boundaries
     * still split shaping and rendering, but must not split bidi analysis. */
    ass_shaper_set_whole_text_layout(state->furi_shaper, true);
    ass_shaper_set_base_direction(state->furi_shaper,
            ass_resolve_base_direction(state->font_encoding));

    for (int i = 0; i < text_info->n_furi_groups; i++) {
        FuriGroup *group = &text_info->furi_groups[i];
        TextInfo furi_text = furi_group_text_info(group);

        split_style_runs_list(group->glyphs, group->length,
                              text_info->n_karaoke_segments > 0);
        ass_shaper_find_runs(state->furi_shaper, state->renderer,
                             group->glyphs, group->length);
        if (!ass_shaper_shape(state->furi_shaper, &furi_text))
            return false;

        retrieve_glyphs_from_list(state, group->glyphs, group->length);
        for (int j = 0; j < group->length; j++) {
            bool rtl = ass_shaper_is_rtl(state->furi_shaper, j);
            for (GlyphInfo *info = &group->glyphs[j]; info;
                 info = info->next)
                info->karaoke_rtl = rtl;
        }
        // Position once before measuring visual overhangs for reservation.
        if (!reorder_furi_group(state, group))
            return false;
        apply_furi_group_layout(state, group);
        if (!reorder_furi_group(state, group))
            return false;
        if (!prepare_furi_karaoke_regions(text_info, group))
            return false;
    }

    return true;
}

static void compute_line_baselines(RenderContext *state, double *baselines)
{
    TextInfo *text_info = &state->text_info;
    baselines[0] = 0.0;
    for (int i = 1; i < text_info->n_lines; i++) {
        baselines[i] = baselines[i - 1] +
            text_info->lines[i - 1].desc +
            text_info->lines[i].asc +
            line_spacing(state);
    }
}

static bool prepare_scroll_layout(RenderContext *state, double device_y)
{
    if (!state->n_scroll_contexts)
        return true;
    TextInfo *text = &state->text_info;
    double *baselines = ass_realloc_array(NULL, text->n_lines, sizeof(*baselines));
    if (!baselines)
        return false;
    compute_line_baselines(state, baselines);
    /* Glyphs remain in logical order; final line IDs incorporate wrapping.
     * Blank explicit rows count too. Ruby is deliberately not enumerated. */
    for (int i = 0; i < text->length; i++) {
        GlyphInfo *glyph = &text->glyphs[i];
        if (!glyph->scroll_id)
            continue;
        ASS_ScrollContext *ctx = &state->scroll_contexts[glyph->scroll_id - 1];
        int row = glyph->line;
        if (row < 0 || row >= text->n_lines || row == ctx->last_line)
            continue;
        if (ctx->rows == ctx->capacity) {
            size_t capacity = ctx->capacity ? ctx->capacity * 2 : 16;
            if (capacity < ctx->capacity ||
                    !ASS_REALLOC_ARRAY(ctx->advances, capacity)) {
                free(baselines);
                return false;
            }
            ctx->capacity = capacity;
        }
        double top = baselines[row] - text->lines[row].asc;
        double advance = text->lines[row].asc + text->lines[row].desc;
        if (row + 1 < text->n_lines)
            advance += line_spacing(state);
        advance = FFMAX(0.0, advance);
        if (!ctx->rows)
            ctx->top = device_y + top;
        if (ctx->show_lines > 0 && ctx->rows < (size_t) ctx->show_lines)
            ctx->bottom = device_y + top + text->lines[row].asc + text->lines[row].desc;
        ctx->advances[ctx->rows++] = advance;
        ctx->last_line = row;
    }
    free(baselines);
    int64_t now = state->renderer->time - state->event->Start;
    for (int i = 0; i < state->n_scroll_contexts; i++) {
        ASS_ScrollContext *ctx = &state->scroll_contexts[i];
        if (!ass_scroll_map(ctx->definition, ctx->advances, ctx->rows))
            return false;
        ctx->displacement = ass_scroll_evaluate(ctx->definition, now);
    }
    /* A scrolling event has stable author placement. Collision movement would
     * also move its viewport/footer and introduce frame-dependent snapping. */
    state->detect_collisions = 0;
    return true;
}

static void displace_scroll_list(RenderContext *state, GlyphInfo *glyphs, int length)
{
    for (int i = 0; i < length; i++)
        for (GlyphInfo *info = &glyphs[i]; info; info = info->next) {
            if (!info->scroll_id)
                continue;
            double dy = state->scroll_contexts[info->scroll_id - 1].displacement;
            double y = info->pos.y - dy * 64.0;
            info->pos.y = (int32_t) ass_lrint(FFMINMAX(y, INT_MIN, INT_MAX));
        }
}

static void displace_scroll_glyphs(RenderContext *state)
{
    if (!state->n_scroll_contexts)
        return;
    TextInfo *text = &state->text_info;
    displace_scroll_list(state, text->glyphs, text->length);
    for (int i = 0; i < text->n_furi_groups; i++) {
        FuriGroup *group = &text->furi_groups[i];
        int id = text->glyphs[group->base_start].scroll_id;
        for (int j = 0; j < group->length; j++)
            for (GlyphInfo *info = &group->glyphs[j]; info; info = info->next)
                info->scroll_id = id;
        displace_scroll_list(state, group->glyphs, group->length);
    }
}

static void update_text_height(RenderContext *state)
{
    TextInfo *text_info = &state->text_info;
    text_info->height = 0.0;
    for (int i = 0; i < text_info->n_lines; i++)
        text_info->height += text_info->lines[i].asc + text_info->lines[i].desc;
    text_info->height += (text_info->n_lines - 1) * line_spacing(state);
}

static void shift_glyph_list_line(GlyphInfo *glyphs, int length,
                                  const double *line_shift, int n_lines)
{
    for (int i = 0; i < length; i++) {
        int line = glyphs[i].line;
        if (line < 0 || line >= n_lines)
            continue;
        int32_t shift = double_to_d6(line_shift[line]);
        if (!shift)
            continue;
        for (GlyphInfo *info = &glyphs[i]; info; info = info->next)
            info->pos.y += shift;
    }
}

static void apply_line_shifts(RenderContext *state, const double *line_shift)
{
    TextInfo *text_info = &state->text_info;
    shift_glyph_list_line(text_info->glyphs, text_info->length,
                          line_shift, text_info->n_lines);
    for (int i = 0; i < text_info->n_furi_groups; i++) {
        FuriGroup *group = &text_info->furi_groups[i];
        shift_glyph_list_line(group->glyphs, group->length,
                              line_shift, text_info->n_lines);
    }
}

static bool furi_group_visual_bbox(FuriGroup *group,
                                   double *top, double *bottom)
{
    bool have = false;
    *top = DBL_MAX;
    *bottom = -DBL_MAX;

    for (int i = 0; i < group->length; i++) {
        GlyphInfo *root = &group->glyphs[i];
        if (root->skip)
            continue;

        for (GlyphInfo *info = root; info; info = info->next) {
            double y = d6_to_double(info->pos.y);
            *top = FFMIN(*top, y + d6_to_double(info->bbox.y_min));
            *bottom = FFMAX(*bottom, y + d6_to_double(info->bbox.y_max));
            have = true;
        }
    }

    return have;
}

static bool expand_furi_line_metrics(RenderContext *state)
{
    TextInfo *text_info = &state->text_info;
    if (!furi_reserves_vertical_space(text_info))
        return true;

    double *old_baselines = calloc(text_info->n_lines, sizeof(*old_baselines));
    double *new_baselines = calloc(text_info->n_lines, sizeof(*new_baselines));
    double *above = calloc(text_info->n_lines, sizeof(*above));
    double *below = calloc(text_info->n_lines, sizeof(*below));
    double *line_shift = calloc(text_info->n_lines, sizeof(*line_shift));
    if (!old_baselines || !new_baselines || !above || !below || !line_shift) {
        free(old_baselines);
        free(new_baselines);
        free(above);
        free(below);
        free(line_shift);
        return false;
    }

    compute_line_baselines(state, old_baselines);

    for (int i = 0; i < text_info->n_furi_groups; i++) {
        FuriGroup *group = &text_info->furi_groups[i];
        if (!group->change_pos || !group->geometry_valid)
            continue;
        int line = text_info->glyphs[group->base_start].line;
        if (line < 0 || line >= text_info->n_lines)
            continue;
        double top, bottom;
        if (!furi_group_visual_bbox(group, &top, &bottom))
            continue;
        top = FFMIN(top, group->annotation_bounds.y_min);
        bottom = FFMAX(bottom, group->annotation_bounds.y_max);
        double baseline = d6_to_double(text_info->glyphs[group->base_start].pos.y);
        above[line] = FFMAX(above[line], FFMAX(0.0,
            baseline - text_info->lines[line].asc - top));
        below[line] = FFMAX(below[line], FFMAX(0.0,
            bottom - baseline - text_info->lines[line].desc));
    }

    for (int i = 0; i < text_info->n_lines; i++) {
        text_info->lines[i].asc += above[i];
        text_info->lines[i].desc += below[i];
    }
    update_text_height(state);
    compute_line_baselines(state, new_baselines);

    for (int i = 0; i < text_info->n_lines; i++)
        line_shift[i] = new_baselines[i] - old_baselines[i];
    apply_line_shifts(state, line_shift);

    free(old_baselines);
    free(new_baselines);
    free(above);
    free(below);
    free(line_shift);
    return true;
}

static bool distort_params_match(const GlyphInfo *a, const GlyphInfo *b)
{
    if (!a->distort_enabled || !b->distort_enabled)
        return false;
    return a->distort_extended == b->distort_extended &&
           a->distort.u1 == b->distort.u1 && a->distort.v1 == b->distort.v1 &&
           a->distort.u2 == b->distort.u2 && a->distort.v2 == b->distort.v2 &&
           a->distort.u3 == b->distort.u3 && a->distort.v3 == b->distort.v3 &&
           a->distort.u0 == b->distort.u0 && a->distort.v0 == b->distort.v0;
}

static bool cycle_visible_unit(unsigned symbol)
{
    return symbol > 0x20 && symbol != 0x7F && symbol != 0xA0 &&
           !(symbol >= 0x2000 && symbol <= 0x200B) &&
           symbol != 0x2028 && symbol != 0x2029 && symbol != 0x3000;
}

static void apply_cycle_glyph_color(GlyphInfo *glyph, int layer,
                                    uint32_t color)
{
    if (layer < 3) {
        glyph->c[layer] = (color & 0xFFFFFF00u) | _a(glyph->c[layer]);
        glyph->gradient.layer[layer].color_enabled = false;
        glyph->mangetsu_gradient.layer[layer].active = false;
    } else {
        int border = layer - 3;
        BorderLayerState *paint = &glyph->border_layers[border];
        paint->color = (color & 0xFFFFFF00u) | _a(paint->color);
        paint->gradient.color_enabled = false;
        glyph->mangetsu_gradient.border[border].active = false;
    }
}

static PolkaPaint inherit_polka_paint(PolkaPaint base, PolkaPaint local)
{
    if (local.has_color) {
        base.color = local.color;
        base.has_color = true;
    }
    if (local.has_size) {
        base.size = local.size;
        base.has_size = true;
    }
    if (local.has_spacing) {
        base.spacing = local.spacing;
        base.has_spacing = true;
    }
    base.explicit_layer |= local.explicit_layer;
    return base;
}

/* Called after HarfBuzz has linked output glyphs to their source cluster.
 * The source-order roots are the orthographic/shaping units for mode 1; the
 * linked output glyphs are the units for mode 2. Whitespace consumes neither. */
static void apply_cycle_paint(RenderContext *state)
{
    if (!state->event_has_cycle)
        return;
    TextInfo *text = &state->text_info;
    uint32_t serial[3 + ASS_BORDER_LAYERS_MAX] = {0};
    unsigned index[3 + ASS_BORDER_LAYERS_MAX] = {0};
    for (int i = 0; i < text->length; i++) {
        GlyphInfo *root = &text->glyphs[i];
        if (root->skip || root->drawing_text.str ||
                !cycle_visible_unit(root->symbol))
            continue;
        for (int layer = 0; layer < 3 + ASS_BORDER_LAYERS_MAX; layer++) {
            const CyclePaint *paint = layer < 3 ?
                &root->pattern.cycle_face[layer] :
                &root->pattern.cycle_border[layer - 3];
            if (!paint->palette)
                continue;
            if (serial[layer] != paint->serial) {
                serial[layer] = paint->serial;
                index[layer] = 0;
            }
            const CyclePalette *palette = paint->palette;
            uint32_t color = palette->colors[index[layer] % palette->count];
            for (GlyphInfo *glyph = root; glyph; glyph = glyph->next) {
                if (paint->mode == 2)
                    color = palette->colors[index[layer]++ % palette->count];
                apply_cycle_glyph_color(glyph, layer, color);
            }
            if (paint->mode == 1)
                index[layer]++;
        }
    }

    /* A reading belongs to its base span. The first shaped base cluster is
     * the fallback when the ruby span has no finer one-to-one association. */
    for (int i = 0; i < text->n_furi_groups; i++) {
        FuriGroup *group = &text->furi_groups[i];
        GlyphInfo *base = NULL;
        for (int j = group->base_start;
             j < group->base_start + group->base_len && j < text->length;
             j++) {
            if (j >= 0 && !text->glyphs[j].skip &&
                    cycle_visible_unit(text->glyphs[j].symbol)) {
                base = &text->glyphs[j];
                break;
            }
        }
        if (!base)
            continue;
        for (int j = 0; j < group->length; j++)
            for (GlyphInfo *glyph = &group->glyphs[j]; glyph;
                 glyph = glyph->next) {
                for (int layer = 0; layer < 3; layer++)
                    if (base->pattern.cycle_face[layer].palette)
                        apply_cycle_glyph_color(glyph, layer,
                                                base->c[layer]);
                for (int layer = 0; layer < ASS_BORDER_LAYERS_MAX; layer++)
                    if (base->pattern.cycle_border[layer].palette)
                        apply_cycle_glyph_color(glyph, layer + 3,
                                     base->border_layers[layer].color);
                if (base->pattern.propagate_polka) {
                    for (int layer = 0; layer < 3; layer++)
                        glyph->pattern.polka_face[layer] =
                            inherit_polka_paint(base->pattern.polka_face[layer],
                                                glyph->pattern.polka_face[layer]);
                    for (int layer = 0; layer < ASS_BORDER_LAYERS_MAX; layer++)
                        glyph->pattern.polka_border[layer] =
                            inherit_polka_paint(base->pattern.polka_border[layer],
                                                glyph->pattern.polka_border[layer]);
                    glyph->pattern.propagate_polka = true;
                }
            }
    }
}

static bool distort_accumulate_bbox(const GlyphInfo *info,
                                    double *min_x, double *min_y,
                                    double *max_x, double *max_y)
{
    if (!info->outline)
        return false;

    const ASS_Outline *ol = &info->outline->outline[0];
    if (!ol->n_points)
        return false;

    double pos_x = info->pos.x;
    double pos_y = info->pos.y;
    double scale_x = info->transform.scale.x;
    double scale_y = info->transform.scale.y;
    double off_x = info->transform.offset.x;
    double off_y = info->transform.offset.y;

    for (size_t i = 0; i < ol->n_points; i++) {
        double x = ol->points[i].x * scale_x + off_x + pos_x;
        double y = ol->points[i].y * scale_y + off_y + pos_y;
        *min_x = FFMIN(*min_x, x);
        *max_x = FFMAX(*max_x, x);
        *min_y = FFMIN(*min_y, y);
        *max_y = FFMAX(*max_y, y);
    }
    return true;
}

static bool clone_outline(ASS_Outline *dst, const ASS_Outline *src)
{
    ass_outline_clear(dst);
    if (!src->n_points || !src->n_segments)
        return true;

    if (!ass_outline_alloc(dst, src->n_points, src->n_segments))
        return false;
    memcpy(dst->points, src->points, src->n_points * sizeof(ASS_Vector));
    memcpy(dst->segments, src->segments, src->n_segments);
    dst->n_points = src->n_points;
    dst->n_segments = src->n_segments;
    return true;
}

static bool distort_warp_glyph(GlyphInfo *info,
                               double min_x, double min_y,
                               double max_x, double max_y)
{
    OutlineHashValue *base = info->outline;
    if (!base || !info->distort_enabled ||
            (!base->outline[0].n_points && !base->outline[1].n_points))
        return false;
    if ((base->outline[0].n_points && !base->outline[0].n_segments) ||
        (base->outline[1].n_points && !base->outline[1].n_segments))
        return false;

    double w = max_x - min_x;
    double h = max_y - min_y;
    if (w <= 0 || h <= 0)
        return false;

    OutlineHashValue *distorted = calloc(1, sizeof(*distorted));
    if (!distorted)
        return false;

    if (!clone_outline(&distorted->outline[0], &base->outline[0]) ||
        !clone_outline(&distorted->outline[1], &base->outline[1])) {
        ass_outline_free(&distorted->outline[0]);
        ass_outline_free(&distorted->outline[1]);
        free(distorted);
        return false;
    }

    distorted->advance = base->advance;
    distorted->asc = base->asc;
    distorted->desc = base->desc;
    distorted->valid = true;

    double pos_x = info->pos.x;
    double pos_y = info->pos.y;
    double scale_x = info->transform.scale.x;
    double scale_y = info->transform.scale.y;
    double off_x = info->transform.offset.x;
    double off_y = info->transform.offset.y;

    for (int oi = 0; oi < 2; oi++) {
        ASS_Outline *ol = &distorted->outline[oi];
        for (size_t pi = 0; pi < ol->n_points; pi++) {
            double x = ol->points[pi].x * scale_x + off_x + pos_x;
            double y = ol->points[pi].y * scale_y + off_y + pos_y;

            ASS_DVector mapped = ass_distort_map_point(&info->distort, min_x, min_y,
                                                       max_x, max_y, x, y);

            double local_x = mapped.x - pos_x - off_x;
            double local_y = mapped.y - pos_y - off_y;
            if (scale_x != 0.0)
                local_x /= scale_x;
            if (scale_y != 0.0)
                local_y /= scale_y;

            ol->points[pi].x = ass_lrint(local_x);
            ol->points[pi].y = ass_lrint(local_y);
        }
    }

    rectangle_reset(&distorted->cbox);
    ass_outline_update_cbox(&distorted->outline[0], &distorted->cbox);
    ass_outline_update_cbox(&distorted->outline[1], &distorted->cbox);
    if (distorted->cbox.x_min > distorted->cbox.x_max ||
            distorted->cbox.y_min > distorted->cbox.y_max) {
        distorted->cbox.x_min = distorted->cbox.y_min = 0;
        distorted->cbox.x_max = distorted->cbox.y_max = 0;
    }

    info->distorted_outline = distorted;
    info->has_distort_outline = true;
    if (info->distort_extended)
        info->distort_bbox = (ASS_DRect) {min_x, min_y, max_x, max_y};
    info->bbox.x_min = ass_lrint(distorted->cbox.x_min * scale_x + off_x);
    info->bbox.y_min = ass_lrint(distorted->cbox.y_min * scale_y + off_y);
    info->bbox.x_max = ass_lrint(distorted->cbox.x_max * scale_x + off_x);
    info->bbox.y_max = ass_lrint(distorted->cbox.y_max * scale_y + off_y);

    return true;
}

static bool apply_distortion(RenderContext *state)
{
    TextInfo *text_info = &state->text_info;
    FriBidiStrIndex *cmap = ass_shaper_get_reorder_map(state->shaper);
    if (!cmap || text_info->length <= 0)
        return false;

    bool has_distortion = false;
    for (int i = 0; i < text_info->length; i++) {
        if (text_info->glyphs[i].distort_enabled) {
            has_distortion = true;
            break;
        }
    }
    if (!has_distortion)
        return false;

    /*
     * VSFilterMod compacts adjacent CText objects only while their effective
     * style matches. starts_new_run is recorded in logical order before bidi
     * reordering, so preserve those boundaries with stable logical run ids
     * while walking cmap in visual order.
     */
    int *style_run_ids = malloc(text_info->length * sizeof(*style_run_ids));
    if (!style_run_ids)
        return false;
    bool applied = false;
    int run_id = 0;
    for (int i = 0; i < text_info->length; i++) {
        if (i && text_info->glyphs[i].starts_new_run)
            run_id++;
        style_run_ids[i] = run_id;
    }

    for (int i = 0; i < text_info->length; i++) {
        int root_index = cmap[i];
        GlyphInfo *root = text_info->glyphs + root_index;
        if (root->symbol == '\n' || !root->distort_enabled)
            continue;
        int root_run_id = style_run_ids[root_index];
        bool multiline = root->distort_extended;

        double min_x = DBL_MAX, min_y = DBL_MAX;
        double max_x = -DBL_MAX, max_y = -DBL_MAX;
        bool has_bbox = false;

        int end = i;
        while (end < text_info->length) {
            int cur_index = cmap[end];
            GlyphInfo *cur = text_info->glyphs + cur_index;
            if (!multiline && ((text_info->glyphs[end].linebreak && end != i) ||
                               cur->symbol == '\n'))
                break;
            if (multiline ? cur->distort_style_run_id != root->distort_style_run_id :
                            style_run_ids[cur_index] != root_run_id)
                break;
            if (!distort_params_match(root, cur))
                break;
            // A hard break may belong to this extended unit but never supplies
            // outline geometry. Style/parameter boundaries still apply to it.
            if (cur->symbol == '\n') {
                end++;
                continue;
            }

            /*
             * Match VSFilterMod's path-point bounds. Whitespace has no path
             * points, but it stays in the unit and its advance is already
             * reflected in the positions of following glyphs.
             */
            for (GlyphInfo *g = cur; g; g = g->next)
                has_bbox |= distort_accumulate_bbox(g, &min_x, &min_y,
                                                    &max_x, &max_y);

            end++;
            if (cur->drawing_text.str)
                break;
        }

        if (!has_bbox || min_x >= max_x || min_y >= max_y) {
            i = end - 1;
            continue;
        }

        for (int j = i; j < end; j++) {
            GlyphInfo *cur = text_info->glyphs + cmap[j];
            if (cur->skip || cur->symbol == '\n')
                continue;
            /*
             * HarfBuzz can emit several positioned glyphs for one logical
             * cluster. They live on GlyphInfo::next and must all receive the
             * same unit warp.
             */
            for (GlyphInfo *g = cur; g; g = g->next) {
                if (g->outline) {
                    bool warped = distort_warp_glyph(g, min_x, min_y,
                                                     max_x, max_y);
                    if (!cur->drawing_text.str)
                        applied |= warped;
                }
            }
        }

        i = end - 1;
    }
    free(style_run_ids);
    return applied;
}

static void apply_baseline_shear(RenderContext *state)
{
    ASS_Renderer *render_priv = state->renderer;
    TextInfo *text_info = &state->text_info;
    FriBidiStrIndex *cmap = ass_shaper_get_reorder_map(state->shaper);
    int32_t shear = 0;
    bool whole_text_layout =
        render_priv->track->parser_priv->feature_flags &
        FEATURE_MASK(ASS_FEATURE_WHOLE_TEXT_LAYOUT);
    for (int i = 0; i < text_info->length; i++) {
        GlyphInfo *info = text_info->glyphs + cmap[i];
        if (text_info->glyphs[i].linebreak ||
            (!whole_text_layout && text_info->glyphs[i].starts_new_run))
            shear = 0;
        if (!info->scale_x || !info->scale_y)
            info->skip = true;
        if (info->skip)
            continue;
        double fay = info->fay / info->scale_x * info->scale_y;
        for (GlyphInfo *cur = info; cur; cur = cur->next) {
            cur->pos.y += shear + fay * cur->offset.x;
            shear += fay * cur->advance.x;
        }
    }
}

static void apply_baseline_rotation(RenderContext *state,
                                    double origin_x, double origin_y)
{
    TextInfo *text_info = &state->text_info;

    for (int i = 0; i < text_info->length; i++) {
        GlyphInfo *root = text_info->glyphs + i;
        if (root->frs == 0.0)
            continue;

        double angle = root->frs * ASS_PI / 180.0;
        double s = sin(angle);
        double c = cos(angle);

        for (GlyphInfo *info = root; info; info = info->next) {
            double x = d6_to_double(info->pos.x);
            double y = d6_to_double(info->pos.y);
            double rel_x = x - origin_x;
            double rel_y = y - origin_y;
            double new_x = origin_x + rel_x * c - rel_y * s;
            double new_y = origin_y + rel_x * s + rel_y * c;
            info->pos.x = double_to_d6(new_x);
            info->pos.y = double_to_d6(new_y);

            double adv_x = d6_to_double(info->advance.x);
            double adv_y = d6_to_double(info->advance.y);
            double new_adv_x = adv_x * c - adv_y * s;
            double new_adv_y = adv_x * s + adv_y * c;
            info->advance.x = double_to_d6(new_adv_x);
            info->advance.y = double_to_d6(new_adv_y);
        }

        double cadv_x = d6_to_double(root->cluster_advance.x);
        double cadv_y = d6_to_double(root->cluster_advance.y);
        double new_cadv_x = cadv_x * c - cadv_y * s;
        double new_cadv_y = cadv_x * s + cadv_y * c;
        root->cluster_advance.x = double_to_d6(new_cadv_x);
        root->cluster_advance.y = double_to_d6(new_cadv_y);
    }
}

static void align_lines(RenderContext *state, double max_text_width)
{
    TextInfo *text_info = &state->text_info;
    GlyphInfo *glyphs = text_info->glyphs;
    int i, j;
    double width = 0;
    int last_break = -1;
    int halign = state->line_alignment ?
        state->line_alignment : state->text_alignment & 3;
    int justify = state->justify;
    double max_width = 0;

    if (state->evt_type & EVENT_HSCROLL) {
        justify = halign;
        halign = HALIGN_LEFT;
    }

    for (i = 0; i <= text_info->length; ++i) {   // (text_info->length + 1) is the end of the last line
        if ((i == text_info->length) || glyphs[i].linebreak) {
            max_width = FFMAX(max_width,width);
            width = 0;
        }
        if (i < text_info->length && !glyphs[i].skip &&
                glyphs[i].symbol != '\n' && glyphs[i].symbol != 0) {
            width += d6_to_double(glyphs[i].cluster_advance.x);
        }
    }
    for (i = 0; i <= text_info->length; ++i) {   // (text_info->length + 1) is the end of the last line
        if ((i == text_info->length) || glyphs[i].linebreak) {
            double shift = 0;
            if (halign == HALIGN_LEFT) {    // left aligned, no action
                if (justify == ASS_JUSTIFY_RIGHT) {
                    shift = max_width - width;
                } else if (justify == ASS_JUSTIFY_CENTER) {
                    shift = (max_width - width) / 2.0;
                } else {
                    shift = 0;
                }
            } else if (halign == HALIGN_RIGHT) {    // right aligned
                if (justify == ASS_JUSTIFY_LEFT) {
                    shift = max_text_width - max_width;
                } else if (justify == ASS_JUSTIFY_CENTER) {
                    shift = max_text_width - max_width + (max_width - width) / 2.0;
                } else {
                    shift = max_text_width - width;
                }
            } else if (halign == HALIGN_CENTER) {   // centered
                if (justify == ASS_JUSTIFY_LEFT) {
                    shift = (max_text_width - max_width) / 2.0;
                } else if (justify == ASS_JUSTIFY_RIGHT) {
                    shift = (max_text_width - max_width) / 2.0 + max_width - width;
                } else {
                    shift = (max_text_width - width) / 2.0;
                }
            }
            for (j = last_break + 1; j < i; ++j) {
                GlyphInfo *info = glyphs + j;
                while (info) {
                    info->pos.x += double_to_d6(shift);
                    info = info->next;
                }
            }
            last_break = i - 1;
            width = 0;
        }
        if (i < text_info->length && !glyphs[i].skip &&
                glyphs[i].symbol != '\n' && glyphs[i].symbol != 0) {
            width += d6_to_double(glyphs[i].cluster_advance.x);
        }
    }
}

typedef struct {
    double min_x;
    double max_x;
    bool seen;
} ColumnCellMeasure;

static bool measure_column_glyph(const GlyphInfo *info, double *min_x, double *max_x)
{
    if (info->skip || info->symbol == '\n' || info->symbol == 0)
        return false;

    double x0 = d6_to_double(info->pos.x);
    double x1 = d6_to_double(info->pos.x + info->cluster_advance.x);

    if (info->outline) {
        x0 = FFMIN(x0, d6_to_double(info->pos.x + info->bbox.x_min));
        x1 = FFMAX(x1, d6_to_double(info->pos.x + info->bbox.x_max));
    }

    *min_x = FFMIN(x0, x1);
    *max_x = FFMAX(x0, x1);
    return true;
}

static void apply_column_layout(RenderContext *state)
{
    TextInfo *text_info = &state->text_info;
    if (!state->column_event || !text_info->column_glyphs ||
            text_info->column_rows <= 0 || text_info->column_count <= 1)
        return;

    int rows = text_info->column_rows;
    int columns = text_info->column_count;
    if (rows > INT_MAX / columns)
        return;

    int cell_count = rows * columns;
    ColumnCellMeasure *cells = calloc(cell_count, sizeof(*cells));
    double *row_origins = malloc(rows * sizeof(*row_origins));
    if (!cells || !row_origins) {
        free(cells);
        free(row_origins);
        return;
    }

    for (int i = 0; i < cell_count; i++) {
        cells[i].min_x = DBL_MAX;
        cells[i].max_x = -DBL_MAX;
    }
    for (int i = 0; i < rows; i++)
        row_origins[i] = DBL_MAX;
    for (int i = 0; i < columns; i++)
        text_info->column_widths[i] = 0.0;

    for (int i = 0; i < text_info->length; i++) {
        ColumnGlyphInfo meta = text_info->column_glyphs[i];
        if (!meta.active || meta.row < 0 || meta.row >= rows ||
                meta.column < 0 || meta.column >= columns)
            continue;

        double min_x, max_x;
        if (!measure_column_glyph(&text_info->glyphs[i], &min_x, &max_x))
            continue;

        ColumnCellMeasure *cell = &cells[meta.row * columns + meta.column];
        cell->min_x = FFMIN(cell->min_x, min_x);
        cell->max_x = FFMAX(cell->max_x, max_x);
        cell->seen = true;
        row_origins[meta.row] = FFMIN(row_origins[meta.row], min_x);
    }

    double table_origin = DBL_MAX;
    for (int row = 0; row < rows; row++) {
        if (row_origins[row] < DBL_MAX)
            table_origin = FFMIN(table_origin, row_origins[row]);
        for (int column = 0; column < columns; column++) {
            ColumnCellMeasure *cell = &cells[row * columns + column];
            if (cell->seen) {
                double width = cell->max_x - cell->min_x;
                text_info->column_widths[column] =
                    FFMAX(text_info->column_widths[column], width);
            }
        }
    }

    if (table_origin == DBL_MAX) {
        free(cells);
        free(row_origins);
        return;
    }

    double column_start = 0.0;
    for (int column = 0; column < columns; column++) {
        double width = text_info->column_widths[column];
        int halign = text_info->column_align[column];

        for (int row = 0; row < rows; row++) {
            ColumnCellMeasure *cell = &cells[row * columns + column];
            if (!cell->seen)
                continue;

            double cell_width = cell->max_x - cell->min_x;
            double align_shift = 0.0;
            if (halign == HALIGN_CENTER)
                align_shift = (width - cell_width) / 2.0;
            else if (halign == HALIGN_RIGHT)
                align_shift = width - cell_width;

            double target = table_origin + column_start + align_shift;
            double shift = target - cell->min_x;
            int32_t shift_d6 = double_to_d6(shift);
            for (int i = 0; i < text_info->length; i++) {
                ColumnGlyphInfo meta = text_info->column_glyphs[i];
                if (!meta.active || meta.row != row || meta.column != column)
                    continue;
                for (GlyphInfo *info = &text_info->glyphs[i]; info; info = info->next)
                    info->pos.x += shift_d6;
            }
        }

        column_start += width + text_info->column_spacing[column];
    }

    free(cells);
    free(row_origins);
}

static void calculate_rotation_params_list(RenderContext *state,
                                           GlyphInfo *glyphs, int length,
                                           ASS_DVector center,
                                           double device_x, double device_y)
{
    ASS_Renderer *render_priv = state->renderer;
    for (int i = 0; i < length; i++) {
        GlyphInfo *info = glyphs + i;
        while (info) {
            double jitter_dx = info->has_jitter ? info->jitter_dx : 0.0;
            double jitter_dy = info->has_jitter ? info->jitter_dy : 0.0;
            if (info->curved_angle == 0.0) {
                /* Preserve the ordinary-rendering expression exactly. */
                info->shift.x = info->pos.x + double_to_d6(
                    device_x + jitter_dx - center.x +
                    info->shadow_x * state->border_scale_x /
                    render_priv->par_scale_x);
                info->shift.y = info->pos.y + double_to_d6(
                    device_y + jitter_dy - center.y +
                    info->shadow_y * state->border_scale_y);
            } else {
                double shadow_x = info->shadow_x * state->border_scale_x /
                                  render_priv->par_scale_x;
                double shadow_y = info->shadow_y * state->border_scale_y;
                double angle = -info->curved_angle * ASS_PI / 180.0;
                double c = cos(angle), s = sin(angle);
                double x = shadow_x;
                shadow_x = x * c - shadow_y * s;
                shadow_y = x * s + shadow_y * c;
                info->shift.x = info->pos.x + double_to_d6(
                    device_x + jitter_dx - center.x + shadow_x);
                info->shift.y = info->pos.y + double_to_d6(
                    device_y + jitter_dy - center.y + shadow_y);
            }
            info = info->next;
        }
    }
}

static void calculate_rotation_params(RenderContext *state, ASS_DRect *bbox,
                                      double device_x, double device_y,
                                      const ASS_DVector *object_anchor)
{
    ASS_Renderer *render_priv = state->renderer;
    TextInfo *text_info = &state->text_info;
    ASS_DVector center;
    if (state->have_origin) {
        center.x = x2scr_pos(render_priv, state->org_x);
        center.y = y2scr_pos(render_priv, state->org_y);
    } else if (object_anchor) {
        center = *object_anchor;
    } else {
        double bx = 0., by = 0.;
        get_base_point(bbox, state->alignment, &bx, &by);
        center.x = device_x + bx;
        center.y = device_y + by;
    }

    calculate_rotation_params_list(state, text_info->glyphs,
                                   text_info->length, center,
                                   device_x, device_y);
    for (int i = 0; i < text_info->n_furi_groups; i++) {
        FuriGroup *group = &text_info->furi_groups[i];
        calculate_rotation_params_list(state, group->glyphs,
                                       group->length, center,
                                       device_x, device_y);
    }
}

static int quantize_blur(double radius, int32_t *shadow_mask)
{
    // Gaussian filter kernel (1D):
    // G(x, r2) = exp(-x^2 / (2 * r2)) / sqrt(2 * pi * r2),
    // position unit is 1/64th of pixel, r = 64 * radius, r2 = r^2.

    // Difference between kernels with different but near r2:
    // G(x, r2 + dr2) - G(x, r2) ~= dr2 * G(x, r2) * (x^2 - r2) / (2 * r2^2).
    // Maximal possible error relative to full pixel value is half of
    // integral (from -inf to +inf) of absolute value of that difference.
    // E_max ~= dr2 / 2 * integral(G(x, r2) * |x^2 - r2| / (2 * r2^2), x)
    //  = dr2 / (4 * r2) * integral(G(y, 1) * |y^2 - 1|, y)
    //  = dr2 / (4 * r2) * 4 / sqrt(2 * pi * e)
    //  ~ dr2 / (4 * r2) ~= dr / (2 * r).
    // E_max ~ BLUR_PRECISION / 2 as we have 2 dimensions.

    // To get discretized blur radius solve the following
    // differential equation (n--quantization index):
    // dr(n) / dn = BLUR_PRECISION * r + POSITION_PRECISION, r(0) = 0,
    // r(n) = (exp(BLUR_PRECISION * n) - 1) * POSITION_PRECISION / BLUR_PRECISION,
    // n = log(1 + r * BLUR_PRECISION / POSITION_PRECISION) / BLUR_PRECISION.

    // To get shadow offset quantization estimate difference of
    // G(x + dx, r2) - G(x, r2) ~= dx * G(x, r2) * (-x / r2).
    // E_max ~= dx / 2 * integral(G(x, r2) * |x| / r2, x)
    //  = dx / sqrt(2 * pi * r2) ~ dx / (2 * r).
    // 2^ord ~ dx ~ BLUR_PRECISION * r + POSITION_PRECISION.

    const double scale = 64 * BLUR_PRECISION / POSITION_PRECISION;
    radius *= scale;

    int ord;
    // ord = floor(log2(BLUR_PRECISION * r + POSITION_PRECISION))
    //     = floor(log2(64 * radius * BLUR_PRECISION + POSITION_PRECISION))
    //     = floor(log2((radius * scale + 1) * POSITION_PRECISION)),
    // floor(log2(x)) = frexp(x) - 1 = frexp(x / 2).
    frexp((1 + radius) * (POSITION_PRECISION / 2), &ord);
    *shadow_mask = ((uint32_t) 1 << ord) - 1;
    return ass_lrint(log1p(radius) / BLUR_PRECISION);
}

static double restore_blur(int qblur)
{
    const double scale = 64 * BLUR_PRECISION / POSITION_PRECISION;
    double sigma = expm1(BLUR_PRECISION * qblur) / scale;
    return sigma * sigma;
}

static void set_border_filters(FilterDesc *filter,
                               const BorderLayerState *layers,
                               double blur_scale_x, double blur_scale_y)
{
    for (int layer = 0; layer < ASS_BORDER_LAYERS_MAX; layer++) {
        const BorderLayerState *border = &layers[layer];
        filter->border_be[layer] = border->has_be ? border->be : filter->be;
        if (border->has_blur) {
            int32_t unused_mask;
            filter->border_blur_x[layer] =
                quantize_blur(border->blur * blur_scale_x, &unused_mask);
            filter->border_blur_y[layer] =
                quantize_blur(border->blur * blur_scale_y, &unused_mask);
        } else {
            filter->border_blur_x[layer] = filter->blur_x;
            filter->border_blur_y[layer] = filter->blur_y;
        }
    }
}

static void reset_gradient_rects(TextInfo *text_info)
{
    for (int i = 0; i < text_info->n_lines; i++) {
        LineInfo *ln = &text_info->lines[i];
        rectangle_reset(&ln->grad_char);
        rectangle_reset(&ln->grad_outline);
        rectangle_reset(&ln->grad_shadow);
        ln->grad_char_valid = false;
        ln->grad_outline_valid = false;
        ln->grad_shadow_valid = false;
    }
}

static void update_line_rect(LineInfo *ln, ASS_Rect *rect, bool *valid,
                             int x0, int y0, int x1, int y1)
{
    rectangle_update(rect, x0, y0, x1, y1);
    *valid = true;
}

static void compute_line_gradient_rects(RenderContext *state)
{
    TextInfo *text_info = &state->text_info;
    reset_gradient_rects(text_info);
    for (unsigned i = 0; i < text_info->n_bitmaps; i++) {
        CombinedBitmapInfo *info = &text_info->combined_bitmaps[i];
        if (info->line < 0 || info->line >= text_info->n_lines)
            continue;
        LineInfo *ln = &text_info->lines[info->line];
        if (info->bm) {
            int x0 = info->x + info->bm->left;
            int y0 = info->y + info->bm->top;
            update_line_rect(ln, &ln->grad_char, &ln->grad_char_valid,
                             x0, y0, x0 + info->bm->w, y0 + info->bm->h);
        }
        // Use character box as fallback for outline/shadow to keep gradient
        // anchoring tied to the text layout (closer to VSFilterMod).
        if (!ln->grad_outline_valid && ln->grad_char_valid) {
            ln->grad_outline = ln->grad_char;
            ln->grad_outline_valid = true;
        }
        if (!ln->grad_shadow_valid && ln->grad_char_valid) {
            ln->grad_shadow = ln->grad_char;
            ln->grad_shadow_valid = true;
        }
    }

    // For BorderStyle=4-style boxes, the "shadow" is the opaque background box. If there
    // is no separate shadow bitmap, anchor its gradient to the character box
    // so the box uses the same line-space rectangle.
    if (state->bs4_box_mode) {
        for (int i = 0; i < text_info->n_lines; i++) {
            LineInfo *ln = &text_info->lines[i];
            if (!ln->grad_shadow_valid && ln->grad_char_valid) {
                ln->grad_shadow = ln->grad_char;
                ln->grad_shadow_valid = true;
            }
        }
    }
}

static void update_mangetsu_rect(GradientRect *rect,
                                 int x0, int y0, int x1, int y1)
{
    if (x1 <= x0 || y1 <= y0)
        return;

    if (!rect->valid) {
        rect->x0 = x0;
        rect->y0 = y0;
        rect->x1 = x1;
        rect->y1 = y1;
        rect->valid = true;
        return;
    }

    rect->x0 = FFMIN(rect->x0, x0);
    rect->y0 = FFMIN(rect->y0, y0);
    rect->x1 = FFMAX(rect->x1, x1);
    rect->y1 = FFMAX(rect->y1, y1);
}

static void update_mangetsu_rect_from_bitmap(GradientRect *rect,
                                             const CombinedBitmapInfo *info,
                                             const Bitmap *bm)
{
    if (!bm)
        return;

    int logical_w = bm->logical_w > 0 ? bm->logical_w : bm->w;
    int logical_h = bm->logical_h > 0 ? bm->logical_h : bm->h;
    update_mangetsu_rect(rect,
                         info->x + bm->left,
                         info->y + bm->top,
                         info->x + bm->left + logical_w,
                         info->y + bm->top + logical_h);
}

static MangetsuGradientLayer *mangetsu_gradient_layer_at(
    MangetsuGradientState *state, MangetsuGradientTarget target, int layer)
{
    if (target == MANGETSU_GRADIENT_TARGET_BORDER) {
        if (layer < 0 || layer >= MANGETSU_GRADIENT_BORDER_LAYERS)
            return NULL;
        return &state->border[layer];
    }
    if (target == MANGETSU_GRADIENT_TARGET_BORDER_ALPHA) {
        if (layer < 0 || layer >= MANGETSU_GRADIENT_BORDER_LAYERS)
            return NULL;
        return &state->border_alpha[layer];
    }
    if (target == MANGETSU_GRADIENT_TARGET_ALPHA) {
        if (layer < 0 || layer >= MANGETSU_GRADIENT_LAYERS)
            return NULL;
        return &state->alpha[layer];
    }

    if (layer < 0 || layer >= MANGETSU_GRADIENT_LAYERS)
        return NULL;
    return &state->layer[layer];
}

static const MangetsuGradientLayer *mangetsu_gradient_const_layer_at(
    const MangetsuGradientState *state, MangetsuGradientTarget target, int layer)
{
    if (target == MANGETSU_GRADIENT_TARGET_BORDER) {
        if (layer < 0 || layer >= MANGETSU_GRADIENT_BORDER_LAYERS)
            return NULL;
        return &state->border[layer];
    }
    if (target == MANGETSU_GRADIENT_TARGET_BORDER_ALPHA) {
        if (layer < 0 || layer >= MANGETSU_GRADIENT_BORDER_LAYERS)
            return NULL;
        return &state->border_alpha[layer];
    }
    if (target == MANGETSU_GRADIENT_TARGET_ALPHA) {
        if (layer < 0 || layer >= MANGETSU_GRADIENT_LAYERS)
            return NULL;
        return &state->alpha[layer];
    }

    if (layer < 0 || layer >= MANGETSU_GRADIENT_LAYERS)
        return NULL;
    return &state->layer[layer];
}

static void compute_mangetsu_gradient_rect_for_layer(
    RenderContext *state, MangetsuGradientTarget target, int layer_index)
{
    TextInfo *text_info = &state->text_info;
    ASS_Renderer *render_priv = state->renderer;
    for (unsigned i = 0; i < text_info->n_bitmaps; i++) {
        CombinedBitmapInfo *info = &text_info->combined_bitmaps[i];
        MangetsuGradientLayer *layer =
            mangetsu_gradient_layer_at(&info->mangetsu_gradient, target,
                                       layer_index);
        if (!layer)
            continue;
        if (layer->coordinate_mode == MANGETSU_GRADIENT_POSITIONED_RECT) {
            double scroll = info->scroll_id ?
                state->scroll_contexts[info->scroll_id - 1].displacement : 0.0;
            ass_mangetsu_gradient_prepare_positioned(
                layer, x2scr_pos_scaled(render_priv, layer->script_x1),
                y2scr_pos(render_priv, layer->script_y1) - scroll,
                x2scr_pos_scaled(render_priv, layer->script_x2),
                y2scr_pos(render_priv, layer->script_y2) - scroll);
        } else {
            layer->rect = (GradientRect) {0};
        }
    }

    for (unsigned i = 0; i < text_info->n_bitmaps; i++) {
        CombinedBitmapInfo *info = &text_info->combined_bitmaps[i];
        MangetsuGradientLayer *layer =
            mangetsu_gradient_layer_at(&info->mangetsu_gradient, target,
                                       layer_index);
        if (!layer || !layer->active || layer->coordinate_mode !=
                MANGETSU_GRADIENT_ATTACHED)
            continue;

        GradientRect rect = {0};
        for (unsigned j = 0; j < text_info->n_bitmaps; j++) {
            CombinedBitmapInfo *other = &text_info->combined_bitmaps[j];
            const MangetsuGradientLayer *other_layer =
                mangetsu_gradient_const_layer_at(&other->mangetsu_gradient,
                                                 target, layer_index);
            if (!other_layer->active || other_layer->coordinate_mode !=
                    MANGETSU_GRADIENT_ATTACHED ||
                    other_layer->segment_id != layer->segment_id ||
                    other->scroll_id != info->scroll_id)
                continue;
            update_mangetsu_rect_from_bitmap(&rect, other, other->bm);
        }
        layer->rect = rect;
    }
}

static void compute_mangetsu_gradient_rects(RenderContext *state)
{
    for (int layer = 0; layer < MANGETSU_GRADIENT_LAYERS; layer++)
        compute_mangetsu_gradient_rect_for_layer(
            state, MANGETSU_GRADIENT_TARGET_COLOR, layer);
    for (int layer = 0; layer < MANGETSU_GRADIENT_BORDER_LAYERS; layer++)
        compute_mangetsu_gradient_rect_for_layer(
            state, MANGETSU_GRADIENT_TARGET_BORDER, layer);
    for (int layer = 0; layer < MANGETSU_GRADIENT_LAYERS; layer++)
        compute_mangetsu_gradient_rect_for_layer(
            state, MANGETSU_GRADIENT_TARGET_ALPHA, layer);
    for (int layer = 0; layer < MANGETSU_GRADIENT_BORDER_LAYERS; layer++)
        compute_mangetsu_gradient_rect_for_layer(
            state, MANGETSU_GRADIENT_TARGET_BORDER_ALPHA, layer);

    TextInfo *text_info = &state->text_info;
    for (unsigned i = 0; i < text_info->n_bitmaps; i++) {
        MangetsuGradientLayer *layer =
            &text_info->combined_bitmaps[i].secondary_outline.gradient;
        layer->rect = (GradientRect) {0};
    }
    for (unsigned i = 0; i < text_info->n_bitmaps; i++) {
        CombinedBitmapInfo *info = &text_info->combined_bitmaps[i];
        MangetsuGradientLayer *layer =
            &text_info->combined_bitmaps[i].secondary_outline.gradient;
        if (!layer->active || layer->coordinate_mode !=
                MANGETSU_GRADIENT_ATTACHED)
            continue;
        GradientRect rect = {0};
        for (unsigned j = 0; j < text_info->n_bitmaps; j++) {
            CombinedBitmapInfo *other = &text_info->combined_bitmaps[j];
            const MangetsuGradientLayer *other_layer =
                &other->secondary_outline.gradient;
            if (other_layer->active && other_layer->coordinate_mode ==
                    MANGETSU_GRADIENT_ATTACHED &&
                    other_layer->segment_id == layer->segment_id &&
                    other->scroll_id == info->scroll_id)
                update_mangetsu_rect_from_bitmap(&rect, other, other->bm);
        }
        layer->rect = rect;
    }
}

static bool curved_d6_value(double value)
{
    return isfinite(value) && value >= INT32_MIN / 64.0 &&
           value <= INT32_MAX / 64.0;
}

static bool transform_curved_cluster(GlyphInfo *root,
                                     ASS_DVector point,
                                     ASS_DVector tangent,
                                     bool apply)
{
    if (!root)
        return false;

    double length = hypot(tangent.x, tangent.y);
    if (!(length > 0) || !isfinite(length))
        return false;
    double c = tangent.x / length;
    double s = tangent.y / length;
    double baseline_x = d6_to_double(root->pos.x) -
                        d6_to_double(root->offset.x);
    double baseline_y = d6_to_double(root->pos.y) -
                        d6_to_double(root->offset.y) -
                        d6_to_double(root->vshift);
    double ass_angle = -atan2(s, c) * 180.0 / ASS_PI;
    bool karaoke = root->effect_type == EF_KARAOKE_KF;
    double frontier = root->curved_effect_timing;

    for (GlyphInfo *info = root; info; info = info->next) {
        double local_x = d6_to_double(info->pos.x) - baseline_x;
        double local_y = d6_to_double(info->pos.y) - baseline_y;
        double advance_x = d6_to_double(info->advance.x);
        double advance_y = d6_to_double(info->advance.y);
        double x = point.x + local_x * c - local_y * s;
        double y = point.y + local_x * s + local_y * c;
        double ax = advance_x * c - advance_y * s;
        double ay = advance_x * s + advance_y * c;
        if (!curved_d6_value(x) || !curved_d6_value(y) ||
                !curved_d6_value(ax) || !curved_d6_value(ay) ||
                (karaoke && !curved_d6_value(frontier / 64 - local_x)))
            return false;
        if (apply) {
            if (karaoke) {
                /* The timing pass measured from the shaped cluster pen.
                 * Include each member's shaping offset in the same frame. */
                info->curved_effect_timing = ass_lrint(frontier - local_x * 64);
                info->effect_type = EF_KARAOKE_KF;
                info->effect_timing = info->curved_effect_timing;
                info->curved_karaoke = true;
                if (info == root && info->karaoke_reverse) {
                    uint32_t color = info->c[0];
                    info->c[0] = info->c[1];
                    info->c[1] = color;
                }
                info->karaoke_reverse = false;
            }
            info->pos.x = double_to_d6(x);
            info->pos.y = double_to_d6(y);
            info->curved_angle = ass_angle;
            info->advance.x = double_to_d6(ax);
            info->advance.y = double_to_d6(ay);
        }
    }
    return true;
}

bool ass_curved_text_transform_cluster(GlyphInfo *root,
                                       ASS_DVector point,
                                       ASS_DVector tangent)
{
    return transform_curved_cluster(root, point, tangent, false) &&
           transform_curved_cluster(root, point, tangent, true);
}

static int effective_curved_alignment(RenderContext *state)
{
    return state->curved_text_align ?
        numpad2align(state->curved_text_align) : state->alignment;
}

static bool apply_curved_text(RenderContext *state,
                              ASS_DVector *attachment,
                              ASS_DVector *attachment_tangent)
{
    TextInfo *text_info = &state->text_info;
    if (state->native_vertical || !state->curved_path_outline ||
            text_info->n_lines < 1 ||
            text_info->n_furi_groups || state->column_event ||
            (state->evt_type & (EVENT_HSCROLL | EVENT_VSCROLL)))
        return false;

    double object_scale = state->object_scale;
    ASS_CurvedPath path;
    if (!ass_curved_path_flatten(&path,
            &state->curved_path_outline->outline[0],
            x2scr_offset(state, object_scale),
            y2scr_offset(state, object_scale)))
        return false;

    FriBidiStrIndex *cmap = ass_shaper_get_reorder_map(state->shaper);
    if (!cmap) {
        ass_curved_path_free(&path);
        return false;
    }

    /* The single-line case needs no allocation.  For wrapped text, keep one
     * shaped advance, path cursor and normal baseline offset per visual line. */
    double single_line[3] = {0};
    double *line_data = single_line;
    size_t n_lines = text_info->n_lines;
    if (n_lines > 1) {
        if (n_lines > SIZE_MAX / (3 * sizeof(*line_data))) {
            ass_curved_path_free(&path);
            return false;
        }
        line_data = calloc(3 * n_lines, sizeof(*line_data));
        if (!line_data) {
            ass_curved_path_free(&path);
            return false;
        }
    }
    double *line_advance = line_data;
    double *line_cursor = line_data + n_lines;
    double *line_baseline = line_data + 2 * n_lines;
    compute_line_baselines(state, line_baseline);

    for (int i = 0; i < text_info->length; i++) {
        GlyphInfo *root = text_info->glyphs + cmap[i];
        if (root->line < 0 || (size_t) root->line >= n_lines)
            goto fail;
        if (!root->skip && root->symbol != '\n')
            line_advance[root->line] +=
                d6_to_double(root->cluster_advance.x);
    }
    /* Shaped advances define the common block width before line alignment.
     * The final anchor is measured only after all clusters are curved. */
    double max_advance = 0.0;
    for (size_t line = 0; line < n_lines; line++) {
        if (!isfinite(line_advance[line]) ||
                !isfinite(line_baseline[line]))
            goto fail;
        max_advance = FFMAX(max_advance, line_advance[line]);
    }

    int alignment = effective_curved_alignment(state);
    int halign = alignment & 3;
    int line_alignment = state->line_alignment ?
        state->line_alignment : state->text_alignment & 3;
    double path_offset =
        x2scr_offset(state, state->curved_text_x * object_scale);
    double normal_offset =
        y2scr_offset(state, state->curved_text_y * object_scale);
    double attach_distance = halign == HALIGN_CENTER ? path.length * 0.5 :
        halign == HALIGN_RIGHT ? path.length : 0.0;
    ASS_DVector path_point, path_tangent;
    if (!ass_curved_path_sample(&path, attach_distance + path_offset,
                                &path_point, &path_tangent))
        goto fail;
    path_point.x -= path_tangent.y * normal_offset;
    path_point.y += path_tangent.x * normal_offset;

    /* Validate every transformed cluster before mutating any of them.  This
     * keeps pathological coordinates on the normal-rendering fallback path. */
    for (int pass = 0; pass < 2; pass++) {
        for (size_t line = 0; line < n_lines; line++) {
            line_cursor[line] = halign == HALIGN_CENTER ?
                (path.length - max_advance) * 0.5 :
                halign == HALIGN_RIGHT ? path.length - max_advance : 0.0;
            double spare = max_advance - line_advance[line];
            line_cursor[line] += line_alignment == HALIGN_CENTER ?
                spare * 0.5 :
                line_alignment == HALIGN_RIGHT ? spare : 0.0;
            line_cursor[line] += path_offset;
        }
        for (int i = 0; i < text_info->length; i++) {
            GlyphInfo *root = text_info->glyphs + cmap[i];
            if (root->skip || root->symbol == '\n')
                continue;

            ASS_DVector point, tangent;
            double advance = d6_to_double(root->cluster_advance.x);
            int line = root->line;
            if (!ass_curved_path_sample(&path, line_cursor[line],
                                        &point, &tangent))
                goto fail;
            double offset = normal_offset + line_baseline[line];
            point.x += -tangent.y * offset;
            point.y +=  tangent.x * offset;
            double advance_x = advance * tangent.x;
            double advance_y = advance * tangent.y;
            if (!transform_curved_cluster(root, point, tangent, pass != 0) ||
                    !curved_d6_value(advance_x) ||
                    !curved_d6_value(advance_y))
                goto fail;
            if (pass) {
                root->cluster_advance.x = double_to_d6(advance_x);
                root->cluster_advance.y = double_to_d6(advance_y);
            }
            line_cursor[line] += advance;
        }
    }

    if (line_data != single_line)
        free(line_data);
    *attachment = path_point;
    *attachment_tangent = path_tangent;
    ass_curved_path_free(&path);
    return true;

fail:
    if (line_data != single_line)
        free(line_data);
    ass_curved_path_free(&path);
    return false;
}

static void compute_curved_string_bbox(TextInfo *text_info, ASS_DRect *bbox,
                                       ASS_DRect *path_bbox,
                                       ASS_DVector tangent)
{
    bool seen = false;
    bbox->x_min = bbox->y_min = 0.0;
    bbox->x_max = bbox->y_max = 0.0;
    *path_bbox = *bbox;
    for (int i = 0; i < text_info->length; i++) {
        GlyphInfo *root = text_info->glyphs + i;
        if (root->skip)
            continue;
        for (GlyphInfo *info = root; info; info = info->next) {
            double angle = -info->curved_angle * ASS_PI / 180.0;
            double c = cos(angle), s = sin(angle);
            double px = d6_to_double(info->pos.x);
            double py = d6_to_double(info->pos.y);
            double xs[2] = {d6_to_double(info->bbox.x_min),
                            d6_to_double(info->bbox.x_max)};
            double ys[2] = {d6_to_double(info->bbox.y_min),
                            d6_to_double(info->bbox.y_max)};
            for (int xi = 0; xi < 2; xi++) {
                for (int yi = 0; yi < 2; yi++) {
                    double x = px + xs[xi] * c - ys[yi] * s;
                    double y = py + xs[xi] * s + ys[yi] * c;
                    double along = x * tangent.x + y * tangent.y;
                    double normal = -x * tangent.y + y * tangent.x;
                    if (!seen) {
                        bbox->x_min = bbox->x_max = x;
                        bbox->y_min = bbox->y_max = y;
                        path_bbox->x_min = path_bbox->x_max = along;
                        path_bbox->y_min = path_bbox->y_max = normal;
                        seen = true;
                    } else {
                        bbox->x_min = FFMIN(bbox->x_min, x);
                        bbox->x_max = FFMAX(bbox->x_max, x);
                        bbox->y_min = FFMIN(bbox->y_min, y);
                        bbox->y_max = FFMAX(bbox->y_max, y);
                        path_bbox->x_min = FFMIN(path_bbox->x_min, along);
                        path_bbox->x_max = FFMAX(path_bbox->x_max, along);
                        path_bbox->y_min = FFMIN(path_bbox->y_min, normal);
                        path_bbox->y_max = FFMAX(path_bbox->y_max, normal);
                    }
                }
            }
        }
    }
}

static MangetsuGradientDebugSegment *find_mangetsu_debug_segment(
    MangetsuGradientDebugState *debug, int segment_id)
{
    for (int i = 0; i < debug->n_segments; i++)
        if (debug->segments[i].segment_id == segment_id)
            return &debug->segments[i];
    return NULL;
}

static void collect_mangetsu_gradient_debug_for_layer(
    TextInfo *text_info, MangetsuGradientDebugState *debug,
    MangetsuGradientTarget target, int layer_index)
{
    for (unsigned i = 0; i < text_info->n_bitmaps; i++) {
        CombinedBitmapInfo *info = &text_info->combined_bitmaps[i];
        const MangetsuGradientLayer *layer =
            mangetsu_gradient_const_layer_at(&info->mangetsu_gradient,
                                             target, layer_index);
        if (!layer || !layer->active)
            continue;

        MangetsuGradientDebugSegment *segment =
            find_mangetsu_debug_segment(debug, layer->segment_id);
        if (!segment) {
            if (debug->n_segments >= MANGETSU_GRADIENT_DEBUG_MAX_SEGMENTS)
                continue;
            segment = &debug->segments[debug->n_segments++];
            memset(segment, 0, sizeof(*segment));
            segment->active = true;
            segment->target = target;
            segment->layer = layer_index;
            segment->type = layer->type;
            segment->coordinate_mode = layer->coordinate_mode;
            segment->segment_id = layer->segment_id;
            segment->angle = layer->angle;
            segment->n_stops = layer->n_stops;
            segment->script_x1 = layer->script_x1;
            segment->script_y1 = layer->script_y1;
            segment->script_x2 = layer->script_x2;
            segment->script_y2 = layer->script_y2;
            memcpy(segment->stops, layer->stops,
                   layer->n_stops * sizeof(layer->stops[0]));
        }
        segment->bitmap_count++;
        segment->rect_valid |= layer->rect.valid;
        segment->positioned_rect_valid |= layer->positioned_rect.valid;
    }
}

static void collect_mangetsu_gradient_debug(RenderContext *state)
{
    TextInfo *text_info = &state->text_info;
    MangetsuGradientDebugState *debug =
        &state->renderer->mangetsu_gradient_debug;

    for (int layer = 0; layer < MANGETSU_GRADIENT_LAYERS; layer++)
        collect_mangetsu_gradient_debug_for_layer(
            text_info, debug, MANGETSU_GRADIENT_TARGET_COLOR, layer);
    for (int layer = 0; layer < MANGETSU_GRADIENT_BORDER_LAYERS; layer++)
        collect_mangetsu_gradient_debug_for_layer(
            text_info, debug, MANGETSU_GRADIENT_TARGET_BORDER, layer);
    for (int layer = 0; layer < MANGETSU_GRADIENT_LAYERS; layer++)
        collect_mangetsu_gradient_debug_for_layer(
            text_info, debug, MANGETSU_GRADIENT_TARGET_ALPHA, layer);
    for (int layer = 0; layer < MANGETSU_GRADIENT_BORDER_LAYERS; layer++)
        collect_mangetsu_gradient_debug_for_layer(
            text_info, debug, MANGETSU_GRADIENT_TARGET_BORDER_ALPHA, layer);
}

static bool text_needs_rgba(const TextInfo *text_info)
{
    for (unsigned i = 0; i < text_info->n_bitmaps; i++) {
        const CombinedBitmapInfo *info = &text_info->combined_bitmaps[i];
        bool has_bitmap = info->bm || info->bm_o || info->bm_s;
        for (int layer = 0;
             layer < ASS_BORDER_LAYERS_MAX - 1 && !has_bitmap;
             layer++)
            has_bitmap = info->bm_border[layer] != NULL;
        if (!info->bitmap_count || !has_bitmap)
            continue;
        if (info->blend_mode != ASS_BLEND_NORMAL)
            return true;
        if (info->fade_color.active && info->fade_color.amount > 0)
            return true;
        if (!info->from_drawing) {
            const TextPatternPaint *pattern = &info->pattern;
            if (pattern->propagate_polka)
                return true;
            for (int layer = 0; layer < 3; layer++)
                if (pattern->polka_face[layer].has_color &&
                        pattern->polka_face[layer].size > 0)
                    return true;
            for (int layer = 1; layer < ASS_BORDER_LAYERS_MAX; layer++)
                if (pattern->polka_border[layer].has_color &&
                        pattern->polka_border[layer].size > 0)
                    return true;
        }
        for (int layer = 0; layer < 4; layer++) {
            if (info->image_fill.layer[layer].enabled)
                return true;
            const GradientValues *vals = &info->gradient.layer[layer];
            if (vals->color_enabled || vals->alpha_enabled)
                return true;
            if (info->mangetsu_gradient.layer[layer].active)
                return true;
            if (info->mangetsu_gradient.alpha[layer].active)
                return true;
        }
        if (info->bm_o && info->mangetsu_gradient.border[0].active)
            return true;
        if (info->bm_o && info->mangetsu_gradient.border_alpha[0].active)
            return true;
        if (info->bm_o &&
                (info->secondary_outline.type == KARAOKE_OUTLINE_VECTOR ||
                 info->secondary_outline.type == KARAOKE_OUTLINE_GRADIENT))
            return true;
        for (int layer = 1; layer < ASS_BORDER_LAYERS_MAX; layer++) {
            if (!info->bm_border[layer - 1])
                continue;
            const GradientValues *vals = &info->border_layers[layer].gradient;
            if (vals->color_enabled || vals->alpha_enabled)
                return true;
            if (info->mangetsu_gradient.border[layer].active)
                return true;
            if (info->mangetsu_gradient.border_alpha[layer].active)
                return true;
        }
    }

    return false;
}

static void position_glyph_list_for_render(GlyphInfo *glyphs, int length,
                                           double device_x, double device_y,
                                           double par_scale_x)
{
    for (int i = 0; i < length; i++) {
        for (GlyphInfo *info = glyphs + i; info; info = info->next) {
            double jitter_dx = info->has_jitter ? info->jitter_dx : 0.0;
            double jitter_dy = info->has_jitter ? info->jitter_dy : 0.0;
            info->pos.x = double_to_d6(device_x + jitter_dx +
                                       d6_to_double(info->pos.x) * par_scale_x);
            info->pos.y = double_to_d6(device_y + jitter_dy) + info->pos.y;
        }
    }
}

static void position_glyphs_for_render(RenderContext *state,
                                       double device_x, double device_y)
{
    ASS_Renderer *render_priv = state->renderer;
    int left = render_priv->settings.left_margin;
    device_x = (device_x - left) * render_priv->par_scale_x + left;
    TextInfo *text_info = &state->text_info;
    position_glyph_list_for_render(text_info->glyphs, text_info->length,
                                   device_x, device_y,
                                   render_priv->par_scale_x);
    for (int i = 0; i < text_info->n_furi_groups; i++) {
        FuriGroup *group = &text_info->furi_groups[i];
        position_glyph_list_for_render(group->glyphs, group->length,
                                       device_x, device_y,
                                       render_priv->par_scale_x);
    }
}

/* Transform the local inline frontier with the same matrix as the glyph.
 * A projective transform maps a straight wipe to a screen-space half-plane;
 * this also covers event rotation, shear, PAR and ordinary perspective. */
static bool curved_karaoke_wipe(RenderContext *state, GlyphInfo *info,
                                double wipe[3])
{
    if (!info->curved_karaoke)
        return false;
    if (info->effect_timing < -50000000 || info->effect_timing > 50000000) {
        wipe[0] = wipe[1] = 0;
        wipe[2] = info->effect_timing < 0 ? 1 : -1;
        return true;
    }
    double m[3][3], p[3];
    calc_transform_matrix(state, info, m, NULL);
    for (int i = 0; i < 3; i++)
        p[i] = m[i][0] * info->effect_timing + m[i][2];
    double a = m[1][1] * p[2] - m[2][1] * p[1];
    double b = m[2][1] * p[0] - m[0][1] * p[2];
    double c = m[0][1] * p[1] - m[1][1] * p[0];
    double length = hypot(a, b);
    double w = p[2] - 64 * m[2][0];
    if (!(length > 0) || !isfinite(length) || !isfinite(c) ||
            !isfinite(w) || w == 0)
        return false;
    double side = -(a * m[0][0] + b * m[1][0] + c * m[2][0]) / w;
    if (!isfinite(side) || side == 0)
        return false;
    if (side > 0)
        length = -length;
    wipe[0] = a / length;
    wipe[1] = b / length;
    wipe[2] = c / (64 * length);
    return true;
}

static void render_glyph_list_to_bitmaps(RenderContext *state,
                                         GlyphInfo *glyphs, int length,
                                         unsigned *nb_bitmaps,
                                         CombinedBitmapInfo **combined_info)
{
    ASS_Renderer *render_priv = state->renderer;
    TextInfo *text_info = &state->text_info;
    bool new_run = true;
    CombinedBitmapInfo *current_info = NULL;
    ASS_DVector offset;

    for (int i = 0; i < length; i++) {
        GlyphInfo *info = glyphs + i;
        if (info->starts_new_run)
            new_run = true;
        if (current_info && info->chat_part != current_info->chat_part)
            new_run = true;
        if (current_info && info->scroll_id != current_info->scroll_id)
            new_run = true;
        if (info->skip)
            continue;

        for (; info; info = info->next) {
            /* Curved progressive glyphs have independent local wipe frames.
             * Raster and composite caches remain independent of progress. */
            if (info->curved_karaoke)
                new_run = true;
            /* Different shaped glyphs can carry different cycle colors even
             * within one HarfBuzz cluster. Never union their paint masks. */
            if (info->pattern.has_cycle)
                new_run = true;
            int flags = 0;
            if (info->border_style == 3)
                flags |= FILTER_BORDER_STYLE_3;
            if (glyph_border_max_x(info) || glyph_border_max_y(info))
                flags |= FILTER_NONZERO_BORDER;
            if ((has_multi_border_layers(info->border_layers) ||
                 layer1_filter_differs(info)) &&
                    info->border_style != 3)
                flags |= FILTER_MULTI_BORDER;
            if (info->shadow_x || info->shadow_y)
                flags |= FILTER_NONZERO_SHADOW;
            if (flags & FILTER_NONZERO_SHADOW &&
                (info->furi_base_karaoke ||
                 info->effect_type == EF_KARAOKE_KF ||
                 info->effect_type == EF_KARAOKE_KO ||
                 info->effect_type == EF_KARAOKE_REVEAL ||
                 _a(info->c[0]) != 0xFF ||
                 info->border_style == 3))
                flags |= FILTER_FILL_IN_SHADOW;
            if (!(flags & FILTER_NONZERO_BORDER) &&
                !(flags & FILTER_FILL_IN_SHADOW))
                flags &= ~FILTER_NONZERO_SHADOW;
            if ((flags & FILTER_NONZERO_BORDER &&
                 _a(info->c[0]) == 0 &&
                 _a(info->c[1]) == 0 &&
                 info->fade == 0) ||
                info->border_style == 3)
                flags |= FILTER_FILL_IN_BORDER;

            if (new_run) {
                if (*nb_bitmaps >= text_info->max_bitmaps) {
                    size_t new_size = 2 * text_info->max_bitmaps;
                    if (!ASS_REALLOC_ARRAY(text_info->combined_bitmaps, new_size))
                        continue;

                    text_info->max_bitmaps = new_size;
                    *combined_info = text_info->combined_bitmaps;
                }
                current_info = &(*combined_info)[*nb_bitmaps];

                current_info->chat_part = info->chat_part;
                current_info->scroll_id = info->scroll_id;

                memcpy(&current_info->c, &info->c, sizeof(info->c));
                memcpy(&current_info->base_c, &info->c, sizeof(info->c));
                current_info->gradient = info->gradient;
                current_info->mangetsu_gradient = info->mangetsu_gradient;
                current_info->pattern = info->pattern;
                current_info->secondary_outline = info->secondary_outline;
                current_info->image_fill = info->image_fill;
                current_info->blend_mode = info->blend_mode;
                memcpy(current_info->border_layers, info->border_layers,
                       sizeof(current_info->border_layers));
                current_info->fade = info->fade;
                current_info->fade_color = info->fade_color;
                current_info->line = info->line;
                current_info->from_drawing = info->drawing_text.str != NULL;
                current_info->draw_sub_x = 0;
                current_info->draw_sub_y = 0;
                for (int j = 0; j < 4; j++)
                    ass_apply_fade(&current_info->c[j], info->fade);

                current_info->effect_type = info->effect_type;
                current_info->native_vertical = info->native_vertical;
                current_info->karaoke_origin_y = info->pos.y;
                current_info->karaoke_reverse = info->karaoke_reverse;
                current_info->effect_timing = info->effect_timing;
                current_info->curved_karaoke = curved_karaoke_wipe(
                    state, info, current_info->karaoke_wipe);
                current_info->furi_base_karaoke = info->furi_base_karaoke;
                current_info->furi_base_reverse = info->furi_base_reverse;
                current_info->furi_group = info->furi_group;
                current_info->furi_base_start = info->furi_base_start;
                current_info->furi_base_end = info->furi_base_end;
                current_info->leftmost_x = OUTLINE_MAX;

                FilterDesc *filter = &current_info->filter;
                filter->flags = flags;
                filter->be = info->be;

                int32_t shadow_mask_x, shadow_mask_y;
                double blur_radius_scale = 2 / sqrt(log(256));
                double blur_scale_x = state->blur_scale_x * blur_radius_scale;
                double blur_scale_y = state->blur_scale_y * blur_radius_scale;
                filter->blur_x = quantize_blur(info->blur_x * blur_scale_x, &shadow_mask_x);
                filter->blur_y = quantize_blur(info->blur_y * blur_scale_y, &shadow_mask_y);
                set_border_filters(filter, info->border_layers,
                                   blur_scale_x, blur_scale_y);
                if (flags & FILTER_NONZERO_SHADOW) {
                    int32_t x = double_to_d6(info->shadow_x * state->border_scale_x);
                    int32_t y = double_to_d6(info->shadow_y * state->border_scale_y);
                    filter->shadow.x = (x + (shadow_mask_x >> 1)) & ~shadow_mask_x;
                    filter->shadow.y = (y + (shadow_mask_y >> 1)) & ~shadow_mask_y;
                } else {
                    filter->shadow.x = filter->shadow.y = 0;
                }

                current_info->x = current_info->y = INT_MAX;
                current_info->bm = current_info->bm_o = current_info->bm_s = NULL;
                for (int j = 0; j < ASS_BORDER_LAYERS_MAX - 1; j++)
                    current_info->bm_border[j] = NULL;
                current_info->image = NULL;

                current_info->bitmap_count = current_info->max_bitmap_count = 0;
                current_info->bitmaps = malloc(MAX_SUB_BITMAPS_INITIAL * sizeof(BitmapRef));
                if (!current_info->bitmaps)
                    continue;

                current_info->max_bitmap_count = MAX_SUB_BITMAPS_INITIAL;
                current_info->has_distortion = false;
                current_info->temp_image = NULL;
                current_info->owned_source_image = NULL;

                (*nb_bitmaps)++;
                new_run = false;
            }
            assert(current_info);

            ASS_Vector pos, pos_o;
            if (current_info->from_drawing && !current_info->bitmap_count) {
                current_info->draw_sub_x = (uint8_t) ((info->pos.x >> 3) & 7);
                current_info->draw_sub_y = (uint8_t) ((info->pos.y >> 3) & 7);
            }
            get_bitmap_glyph(state, info, current_info->curved_karaoke ? NULL :
                             &current_info->leftmost_x, &pos, &pos_o,
                             &offset, !current_info->bitmap_count, flags);

            if (info->has_distort_bitmap || info->distorted_outline)
                current_info->has_distortion = true;

            bool has_bitmap = info->bm || info->bm_o;
            for (int j = 0; j < ASS_BORDER_LAYERS_MAX - 1 && !has_bitmap; j++)
                has_bitmap = info->bm_border[j] != NULL;
            if (!has_bitmap)
                continue;

            if (current_info->bitmap_count >= current_info->max_bitmap_count) {
                size_t new_size = 2 * current_info->max_bitmap_count;
                if (!ASS_REALLOC_ARRAY(current_info->bitmaps, new_size))
                    continue;

                current_info->max_bitmap_count = new_size;
            }
            BitmapRef *ref = &current_info->bitmaps[current_info->bitmap_count];
            memset(ref, 0, sizeof(*ref));
            ref->bm   = info->bm;
            ref->bm_o = info->bm_o;
            ref->pos   = pos;
            ref->pos_o = pos_o;
            for (int j = 0; j < ASS_BORDER_LAYERS_MAX - 1; j++) {
                ref->bm_border[j] = info->bm_border[j];
                ref->pos_border[j] = info->pos_border[j];
            }
            current_info->bitmap_count++;

            current_info->x = FFMIN(current_info->x, pos.x);
            current_info->y = FFMIN(current_info->y, pos.y);
        }
    }
}

static int decoration_filter_flags(const GlyphInfo *info)
{
    int flags = 0;
    if (info->border_style == 3)
        flags |= FILTER_BORDER_STYLE_3;
    if (glyph_border_max_x(info) || glyph_border_max_y(info))
        flags |= FILTER_NONZERO_BORDER;
    if ((has_multi_border_layers(info->border_layers) ||
         layer1_filter_differs(info)) &&
            info->border_style != 3)
        flags |= FILTER_MULTI_BORDER;
    if (info->shadow_x || info->shadow_y)
        flags |= FILTER_NONZERO_SHADOW;
    if (flags & FILTER_NONZERO_SHADOW &&
        (info->furi_base_karaoke ||
         info->effect_type == EF_KARAOKE_KF ||
         info->effect_type == EF_KARAOKE_KO ||
         info->effect_type == EF_KARAOKE_REVEAL ||
         _a(info->c[0]) != 0xFF ||
         info->border_style == 3))
        flags |= FILTER_FILL_IN_SHADOW;
    if (!(flags & FILTER_NONZERO_BORDER) &&
        !(flags & FILTER_FILL_IN_SHADOW))
        flags &= ~FILTER_NONZERO_SHADOW;
    if ((flags & FILTER_NONZERO_BORDER &&
         _a(info->c[0]) == 0 &&
         _a(info->c[1]) == 0 &&
         info->fade == 0) ||
        info->border_style == 3)
        flags |= FILTER_FILL_IN_BORDER;
    return flags;
}

static Bitmap *decoration_owned_bitmap(const GlyphInfo *deco,
                                       CompositeHashValue *owned,
                                       Bitmap *bitmap)
{
    if (bitmap == &deco->distort_bitmap)
        return &owned->bm;
    if (bitmap == &deco->distort_bitmap_o)
        return &owned->bm_o;
    for (int i = 0; i < ASS_BORDER_LAYERS_MAX - 1; i++)
        if (bitmap == &deco->distort_bitmap_border[i])
            return &owned->bm_border[i];
    return bitmap;
}

static void move_decoration_distort_bitmaps(CompositeHashValue *owned,
                                            GlyphInfo *deco)
{
    owned->bm = deco->distort_bitmap;
    owned->bm_o = deco->distort_bitmap_o;
    for (int i = 0; i < ASS_BORDER_LAYERS_MAX - 1; i++)
        owned->bm_border[i] = deco->distort_bitmap_border[i];
    ass_aligned_retag(owned->bm.buffer, ASS_ALIGNED_ALLOC_GLYPH_BITMAP,
                      &owned->bm, "owned decoration fill");
    ass_aligned_retag(owned->bm_o.buffer, ASS_ALIGNED_ALLOC_GLYPH_BITMAP,
                      &owned->bm_o, "owned decoration border");
    for (int i = 0; i < ASS_BORDER_LAYERS_MAX - 1; i++)
        ass_aligned_retag(owned->bm_border[i].buffer,
                          ASS_ALIGNED_ALLOC_GLYPH_BITMAP,
                          &owned->bm_border[i], "owned decoration border layer");
    deco->distort_bitmap = (Bitmap) {0};
    deco->distort_bitmap_o = (Bitmap) {0};
    for (int i = 0; i < ASS_BORDER_LAYERS_MAX - 1; i++)
        deco->distort_bitmap_border[i] = (Bitmap) {0};
    deco->has_distort_bitmap = false;
}

static bool append_decoration_bitmap_info(RenderContext *state,
                                          GlyphInfo *info,
                                          unsigned *nb_bitmaps,
                                          CombinedBitmapInfo **combined_info)
{
    TextInfo *text_info = &state->text_info;
    GlyphInfo deco = *info;
    deco.next = NULL;
    deco.flags = info->decoration_flags | DECO_ONLY;
    memcpy(deco.c, info->decoration_c, sizeof(deco.c));
    MangetsuGradientLayer decoration_gradient =
        deco.mangetsu_gradient.layer[4];
    MangetsuGradientLayer decoration_alpha_gradient =
        deco.mangetsu_gradient.alpha[4];
    ass_mangetsu_gradient_state_reset(&deco.mangetsu_gradient);
    if (decoration_gradient.active)
        deco.mangetsu_gradient.layer[0] = decoration_gradient;
    if (decoration_alpha_gradient.active)
        deco.mangetsu_gradient.alpha[0] = decoration_alpha_gradient;
    deco.outline = NULL;
    deco.distorted_outline = NULL;
    deco.has_distort_bitmap = false;
    deco.has_distort_outline = false;
    deco.bm = deco.bm_o = NULL;
    for (int j = 0; j < ASS_BORDER_LAYERS_MAX - 1; j++)
        deco.bm_border[j] = NULL;

    get_outline_glyph(state, &deco);
    if (!deco.outline)
        return true;

    int flags = decoration_filter_flags(&deco);
    ASS_Vector pos, pos_o;
    ASS_DVector offset;
    int32_t leftmost_x = OUTLINE_MAX;
    double wipe[3];
    bool curved_karaoke = curved_karaoke_wipe(state, &deco, wipe);
    get_bitmap_glyph(state, &deco, curved_karaoke ? NULL : &leftmost_x, &pos, &pos_o,
                     &offset, true, flags);

    bool has_bitmap = deco.bm || deco.bm_o;
    for (int j = 0; j < ASS_BORDER_LAYERS_MAX - 1 && !has_bitmap; j++)
        has_bitmap = deco.bm_border[j] != NULL;
    if (!has_bitmap) {
        ass_free_glyph_render_resources(&deco);
        return true;
    }

    if (*nb_bitmaps >= text_info->max_bitmaps) {
        size_t new_size = 2 * text_info->max_bitmaps;
        if (!ASS_REALLOC_ARRAY(text_info->combined_bitmaps, new_size)) {
            ass_free_glyph_render_resources(&deco);
            return false;
        }
        text_info->max_bitmaps = new_size;
        *combined_info = text_info->combined_bitmaps;
    }

    CombinedBitmapInfo *current_info = &(*combined_info)[(*nb_bitmaps)++];
    memset(current_info, 0, sizeof(*current_info));
    memcpy(&current_info->c, &deco.c, sizeof(deco.c));
    memcpy(&current_info->base_c, &deco.c, sizeof(deco.c));
    current_info->gradient = deco.gradient;
    current_info->mangetsu_gradient = deco.mangetsu_gradient;
    current_info->pattern = (TextPatternPaint) {0};
    current_info->secondary_outline = deco.secondary_outline;
    current_info->image_fill = deco.image_fill;
    current_info->blend_mode = deco.blend_mode;
    memcpy(current_info->border_layers, deco.border_layers,
           sizeof(current_info->border_layers));
    current_info->fade = deco.fade;
    current_info->chat_part = deco.chat_part;
    current_info->scroll_id = deco.scroll_id;
    current_info->fade_color = deco.fade_color;
    current_info->line = deco.line;
    for (int j = 0; j < 4; j++)
        ass_apply_fade(&current_info->c[j], deco.fade);
    current_info->effect_type = deco.effect_type;
    current_info->karaoke_reverse = deco.karaoke_reverse;
    current_info->effect_timing = deco.effect_timing;
    current_info->curved_karaoke = curved_karaoke;
    if (curved_karaoke)
        memcpy(current_info->karaoke_wipe, wipe, sizeof(wipe));
    current_info->furi_base_karaoke = deco.furi_base_karaoke;
    current_info->furi_base_reverse = deco.furi_base_reverse;
    current_info->furi_group = deco.furi_group;
    current_info->furi_base_start = deco.furi_base_start;
    current_info->furi_base_end = deco.furi_base_end;
    current_info->leftmost_x = leftmost_x;
    current_info->filter.flags = flags;
    current_info->filter.be = deco.be;

    int32_t shadow_mask_x, shadow_mask_y;
    double blur_radius_scale = 2 / sqrt(log(256));
    double blur_scale_x = state->blur_scale_x * blur_radius_scale;
    double blur_scale_y = state->blur_scale_y * blur_radius_scale;
    current_info->filter.blur_x =
        quantize_blur(deco.blur_x * blur_scale_x, &shadow_mask_x);
    current_info->filter.blur_y =
        quantize_blur(deco.blur_y * blur_scale_y, &shadow_mask_y);
    set_border_filters(&current_info->filter, deco.border_layers,
                       blur_scale_x, blur_scale_y);
    if (flags & FILTER_NONZERO_SHADOW) {
        int32_t x = double_to_d6(deco.shadow_x * state->border_scale_x);
        int32_t y = double_to_d6(deco.shadow_y * state->border_scale_y);
        current_info->filter.shadow.x =
            (x + (shadow_mask_x >> 1)) & ~shadow_mask_x;
        current_info->filter.shadow.y =
            (y + (shadow_mask_y >> 1)) & ~shadow_mask_y;
    }

    current_info->x = pos.x;
    current_info->y = pos.y;
    current_info->bitmaps = malloc(sizeof(BitmapRef));
    if (!current_info->bitmaps) {
        ass_free_glyph_render_resources(&deco);
        current_info->bitmap_count = current_info->max_bitmap_count = 0;
        return false;
    }
    current_info->bitmap_count = current_info->max_bitmap_count = 1;

    BitmapRef *ref = current_info->bitmaps;
    memset(ref, 0, sizeof(*ref));
    if (deco.has_distort_bitmap) {
        CompositeHashValue *owned = calloc(1, sizeof(*owned));
        if (!owned) {
            ass_free_glyph_render_resources(&deco);
            free(current_info->bitmaps);
            current_info->bitmaps = NULL;
            current_info->bitmap_count = current_info->max_bitmap_count = 0;
            return false;
        }
        current_info->owned_source_image = owned;
        current_info->has_distortion = true;
        ref->bm = decoration_owned_bitmap(&deco, owned, deco.bm);
        ref->bm_o = decoration_owned_bitmap(&deco, owned, deco.bm_o);
        for (int j = 0; j < ASS_BORDER_LAYERS_MAX - 1; j++)
            ref->bm_border[j] = decoration_owned_bitmap(
                &deco, owned, deco.bm_border[j]);
        move_decoration_distort_bitmaps(owned, &deco);
    } else {
        ref->bm = deco.bm;
        ref->bm_o = deco.bm_o;
        for (int j = 0; j < ASS_BORDER_LAYERS_MAX - 1; j++)
            ref->bm_border[j] = deco.bm_border[j];
    }
    ref->pos = pos;
    ref->pos_o = pos_o;
    for (int j = 0; j < ASS_BORDER_LAYERS_MAX - 1; j++) {
        ref->pos_border[j] = deco.pos_border[j];
    }
    return true;
}

static void render_decoration_list_to_bitmaps(RenderContext *state,
                                              GlyphInfo *glyphs, int length,
                                              unsigned *nb_bitmaps,
                                              CombinedBitmapInfo **combined_info)
{
    for (int i = 0; i < length; i++) {
        for (GlyphInfo *info = glyphs + i; info; info = info->next) {
            if (!info->has_custom_decoration || info->skip)
                continue;
            append_decoration_bitmap_info(state, info, nb_bitmaps,
                                          combined_info);
        }
    }
}

// Convert glyphs to bitmaps, combine them, apply blur, generate shadows.
static void render_and_combine_glyphs(RenderContext *state)
{
    ASS_Renderer *render_priv = state->renderer;
    render_priv->repeated_event_stats.geometry++;
    TextInfo *text_info = &state->text_info;
    unsigned nb_bitmaps = 0;
    CombinedBitmapInfo *combined_info = text_info->combined_bitmaps;
    render_glyph_list_to_bitmaps(state, text_info->glyphs, text_info->length,
                                 &nb_bitmaps, &combined_info);
    for (int i = 0; i < text_info->n_furi_groups; i++) {
        FuriGroup *group = &text_info->furi_groups[i];
        render_glyph_list_to_bitmaps(state, group->glyphs, group->length,
                                     &nb_bitmaps, &combined_info);
    }
    render_decoration_list_to_bitmaps(state, text_info->glyphs,
                                      text_info->length, &nb_bitmaps,
                                      &combined_info);
    for (int i = 0; i < text_info->n_furi_groups; i++) {
        FuriGroup *group = &text_info->furi_groups[i];
        render_decoration_list_to_bitmaps(state, group->glyphs, group->length,
                                          &nb_bitmaps, &combined_info);
    }

    for (int i = 0; i < nb_bitmaps; i++) {
        CombinedBitmapInfo *info = &combined_info[i];
        if (!info->bitmap_count) {
            free(info->bitmaps);
            continue;
        }

        if (info->effect_type == EF_KARAOKE_KF && !info->curved_karaoke) {
            if (info->native_vertical)
                info->effect_timing = lround(
                    d6_to_double(info->karaoke_origin_y) +
                    d6_to_double(info->effect_timing));
            else
                info->effect_timing = lround(d6_to_double(info->leftmost_x) +
                    d6_to_double(info->effect_timing) * render_priv->par_scale_x);
        }
        for (int j = 0; j < info->bitmap_count; j++) {
            info->bitmaps[j].pos.x -= info->x;
            info->bitmaps[j].pos.y -= info->y;
            info->bitmaps[j].pos_o.x -= info->x;
            info->bitmaps[j].pos_o.y -= info->y;
            for (int layer = 0; layer < ASS_BORDER_LAYERS_MAX - 1; layer++) {
                info->bitmaps[j].pos_border[layer].x -= info->x;
                info->bitmaps[j].pos_border[layer].y -= info->y;
            }
        }

        if (info->has_distortion) {
            // Distortion depends on per-word bounding boxes, so reuse via the composite cache is unsafe.
            CompositeHashKey key;
            key.filter = info->filter;
            key.bitmap_count = info->bitmap_count;
            key.bitmaps = info->bitmaps;
            CompositeHashValue *val = calloc(1, sizeof(*val));
            if (!val) {
                free(info->bitmaps);
                info->bitmaps = NULL;
                free_owned_source_image(info);
                continue;
            }
            if (!ass_composite_construct(&key, val, render_priv)) {
                free(info->bitmaps);
                free_temporary_composite(val);
                free(val);
                info->bitmaps = NULL;
                free_owned_source_image(info);
                continue;
            }
            info->bm = val->bm.buffer ? &val->bm : NULL;
            info->bm_o = val->bm_o.buffer ? &val->bm_o : NULL;
            for (int layer = 0; layer < ASS_BORDER_LAYERS_MAX - 1; layer++)
                info->bm_border[layer] = val->bm_border[layer].buffer ?
                    &val->bm_border[layer] : NULL;
            info->bm_s = val->bm_s.buffer ? &val->bm_s : NULL;
            info->image = NULL;
            info->temp_image = val;

            free(info->bitmaps);
            info->bitmaps = NULL;
            free_owned_source_image(info);
            continue;
        }

        CompositeHashKey key;
        key.filter = info->filter;
        key.bitmap_count = info->bitmap_count;
        key.bitmaps = info->bitmaps;
        render_priv->repeated_event_stats.composite_lookups++;
        CompositeHashValue *val = ass_cache_get(render_priv->cache.composite_cache, &key, render_priv);
        if (!val)
            continue;

        if (val->bm.buffer)
            info->bm = &val->bm;
        if (val->bm_o.buffer)
            info->bm_o = &val->bm_o;
        for (int layer = 0; layer < ASS_BORDER_LAYERS_MAX - 1; layer++)
            if (val->bm_border[layer].buffer)
                info->bm_border[layer] = &val->bm_border[layer];
        if (val->bm_s.buffer)
            info->bm_s = &val->bm_s;
        info->image = val;
        continue;
    }

    text_info->n_bitmaps = nb_bitmaps;
}

static inline void rectangle_combine(ASS_Rect *rect, const Bitmap *bm, ASS_Vector pos)
{
    pos.x += bm->left;
    pos.y += bm->top;
    rectangle_update(rect, pos.x, pos.y, pos.x + bm->w, pos.y + bm->h);
}

static void bitmap_max_into(Bitmap *dst, const Bitmap *src)
{
    if (!dst->buffer || !src->buffer)
        return;

    int32_t l = FFMAX(dst->left, src->left);
    int32_t t = FFMAX(dst->top,  src->top);
    int32_t r = FFMIN(dst->left + dst->w, src->left + src->w);
    int32_t b = FFMIN(dst->top  + dst->h, src->top  + src->h);
    if (l >= r || t >= b)
        return;

    uint8_t *d = dst->buffer + (t - dst->top) * dst->stride + (l - dst->left);
    const uint8_t *s = src->buffer + (t - src->top) * src->stride + (l - src->left);
    for (int32_t y = 0; y < b - t; y++) {
        for (int32_t x = 0; x < r - l; x++)
            d[x] = FFMAX(d[x], s[x]);
        d += dst->stride;
        s += src->stride;
    }
}

static void ring_subtract_covered(Bitmap *ring, Bitmap *covered)
{
    if (!ring->buffer || !covered->buffer)
        return;

    int32_t l = FFMAX(ring->left, covered->left);
    int32_t t = FFMAX(ring->top,  covered->top);
    int32_t r = FFMIN(ring->left + ring->w, covered->left + covered->w);
    int32_t b = FFMIN(ring->top  + ring->h, covered->top  + covered->h);
    if (l >= r || t >= b)
        return;

    uint8_t *rb = ring->buffer + (t - ring->top) * ring->stride + (l - ring->left);
    uint8_t *cb = covered->buffer + (t - covered->top) * covered->stride + (l - covered->left);
    for (int32_t y = 0; y < b - t; y++) {
        for (int32_t x = 0; x < r - l; x++) {
            uint8_t orig = rb[x];
            uint8_t cov = cb[x];
            rb[x] = orig > cov ? orig - cov : 0;
            cb[x] = FFMAX(cov, orig);
        }
        rb += ring->stride;
        cb += covered->stride;
    }
}

/*
 * To find these values, simulate blur on the border between two
 * half-planes, one zero-filled (background) and the other filled
 * with the maximum supported value (foreground). Keep incrementing
 * the \be argument. The necessary padding is the distance by which
 * the blurred foreground image extends beyond the original border
 * and into the background. Initially it increases along with \be,
 * but very soon it grinds to a halt. At some point, the blurred
 * image actually reaches a stationary point and stays unchanged
 * forever after, simply _shifting_ by one pixel for each \be
 * step--moving in the direction of the non-zero half-plane and
 * thus decreasing the necessary padding (although the large
 * padding is still needed for intermediate results). In practice,
 * images are finite rather than infinite like half-planes, but
 * this can only decrease the required padding. Half-planes filled
 * with extreme values are the theoretical limit of the worst case.
 * Make sure to use the right pixel value range in the simulation!
 */
int ass_be_padding(int be)
{
    if (be <= 3)
        return be;
    if (be <= 7)
        return 4;
    return 5;
}


size_t ass_composite_construct(void *key, void *value, void *priv)
{
    ((ASS_Renderer *) priv)->repeated_event_stats.composite_constructions++;
    ASS_Renderer *render_priv = priv;
    CompositeHashKey *k = key;
    CompositeHashValue *v = value;
    memset(v, 0, sizeof(*v));

    ASS_Rect rect, rect_o;
    ASS_Rect rect_l, rect_o_l;
    rectangle_reset(&rect);
    rectangle_reset(&rect_o);
    rectangle_reset(&rect_l);
    rectangle_reset(&rect_o_l);

    size_t n_bm = 0;
    size_t n_bm_o[ASS_BORDER_LAYERS_MAX] = {0};
    BitmapRef *last = NULL;
    BitmapRef *last_o[ASS_BORDER_LAYERS_MAX] = {0};
    for (int i = 0; i < k->bitmap_count; i++) {
        BitmapRef *ref = &k->bitmaps[i];
        if (ref->bm) {
            rectangle_combine(&rect, ref->bm, ref->pos);
            int32_t lw = ref->bm->logical_w > 0 ? ref->bm->logical_w : ref->bm->w;
            int32_t lh = ref->bm->logical_h > 0 ? ref->bm->logical_h : ref->bm->h;
            rectangle_update(&rect_l,
                             ref->pos.x + ref->bm->left,
                             ref->pos.y + ref->bm->top,
                             ref->pos.x + ref->bm->left + lw,
                             ref->pos.y + ref->bm->top + lh);
            last = ref;
            n_bm++;
        }
        for (int layer = 0; layer < ASS_BORDER_LAYERS_MAX; layer++) {
            Bitmap *bm_o = bitmap_ref_border_bitmap(ref, layer);
            if (!bm_o)
                continue;
            ASS_Vector pos_o = bitmap_ref_border_pos(ref, layer);
            rectangle_combine(&rect_o, bm_o, pos_o);
            int32_t lw = bm_o->logical_w > 0 ? bm_o->logical_w : bm_o->w;
            int32_t lh = bm_o->logical_h > 0 ? bm_o->logical_h : bm_o->h;
            rectangle_update(&rect_o_l,
                             pos_o.x + bm_o->left,
                             pos_o.y + bm_o->top,
                             pos_o.x + bm_o->left + lw,
                             pos_o.y + bm_o->top + lh);
            last_o[layer] = ref;
            n_bm_o[layer]++;
        }
    }

    int bord = ass_be_padding(k->filter.be);
    if (k->filter.flags & FILTER_MULTI_BORDER)
        for (int layer = 0; layer < ASS_BORDER_LAYERS_MAX; layer++)
            if (n_bm_o[layer])
                bord = FFMAX(bord,
                             ass_be_padding(k->filter.border_be[layer]));
    if (!bord && n_bm == 1) {
        ass_copy_bitmap(&render_priv->engine, &v->bm, last->bm);
        v->bm.left += last->pos.x;
        v->bm.top  += last->pos.y;
    } else if (n_bm && ass_alloc_bitmap(&render_priv->engine, &v->bm,
                                        rect.x_max - rect.x_min + 2 * bord,
                                        rect.y_max - rect.y_min + 2 * bord,
                                        true)) {
        Bitmap *dst = &v->bm;
        bool have_subpix = false;
        dst->left = rect.x_min - bord;
        dst->top  = rect.y_min - bord;
        dst->logical_w = rect_l.x_max - rect_l.x_min + 2 * bord;
        dst->logical_h = rect_l.y_max - rect_l.y_min + 2 * bord;
        for (int i = 0; i < k->bitmap_count; i++) {
            Bitmap *src = k->bitmaps[i].bm;
            if (!src)
                continue;
            if (!have_subpix) {
                dst->sub_x = src->sub_x;
                dst->sub_y = src->sub_y;
                have_subpix = true;
            }
            int x = k->bitmaps[i].pos.x + src->left - dst->left;
            int y = k->bitmaps[i].pos.y + src->top  - dst->top;
            assert(x >= 0 && x + src->w <= dst->w);
            assert(y >= 0 && y + src->h <= dst->h);
            unsigned char *buf = dst->buffer + y * dst->stride + x;
            render_priv->engine.add_bitmaps(buf, dst->stride,
                                            src->buffer, src->stride,
                                            src->w, src->h);
        }
    }
    int flags = k->filter.flags;
    bool multi_border = flags & FILTER_MULTI_BORDER;
    if (!multi_border && !bord && n_bm_o[0] == 1) {
        ass_copy_bitmap(&render_priv->engine, &v->bm_o, last_o[0]->bm_o);
        v->bm_o.left += last_o[0]->pos_o.x;
        v->bm_o.top  += last_o[0]->pos_o.y;
    } else if (!multi_border && n_bm_o[0] &&
               ass_alloc_bitmap(&render_priv->engine, &v->bm_o,
                                rect_o.x_max - rect_o.x_min + 2 * bord,
                                rect_o.y_max - rect_o.y_min + 2 * bord,
                                true)) {
        Bitmap *dst = &v->bm_o;
        bool have_subpix = false;
        dst->left = rect_o.x_min - bord;
        dst->top  = rect_o.y_min - bord;
        dst->logical_w = rect_o_l.x_max - rect_o_l.x_min + 2 * bord;
        dst->logical_h = rect_o_l.y_max - rect_o_l.y_min + 2 * bord;
        for (int i = 0; i < k->bitmap_count; i++) {
            Bitmap *src = k->bitmaps[i].bm_o;
            if (!src)
                continue;
            if (!have_subpix) {
                dst->sub_x = src->sub_x;
                dst->sub_y = src->sub_y;
                have_subpix = true;
            }
            int x = k->bitmaps[i].pos_o.x + src->left - dst->left;
            int y = k->bitmaps[i].pos_o.y + src->top  - dst->top;
            assert(x >= 0 && x + src->w <= dst->w);
            assert(y >= 0 && y + src->h <= dst->h);
            unsigned char *buf = dst->buffer + y * dst->stride + x;
            render_priv->engine.add_bitmaps(buf, dst->stride,
                                            src->buffer, src->stride,
                                            src->w, src->h);
        }
    } else if (multi_border) {
        for (int layer = 0; layer < ASS_BORDER_LAYERS_MAX; layer++) {
            if (!n_bm_o[layer])
                continue;
            Bitmap *dst = composite_border_bitmap(v, layer);
            if (!ass_alloc_bitmap(&render_priv->engine, dst,
                                  rect_o.x_max - rect_o.x_min + 2 * bord,
                                  rect_o.y_max - rect_o.y_min + 2 * bord,
                                  true))
                continue;
            bool have_subpix = false;
            dst->left = rect_o.x_min - bord;
            dst->top  = rect_o.y_min - bord;
            dst->logical_w = rect_o_l.x_max - rect_o_l.x_min + 2 * bord;
            dst->logical_h = rect_o_l.y_max - rect_o_l.y_min + 2 * bord;
            for (int i = 0; i < k->bitmap_count; i++) {
                BitmapRef *ref = &k->bitmaps[i];
                Bitmap *src = bitmap_ref_border_bitmap(ref, layer);
                if (!src)
                    continue;
                ASS_Vector pos_o = bitmap_ref_border_pos(ref, layer);
                if (!have_subpix) {
                    dst->sub_x = src->sub_x;
                    dst->sub_y = src->sub_y;
                    have_subpix = true;
                }
                int x = pos_o.x + src->left - dst->left;
                int y = pos_o.y + src->top  - dst->top;
                assert(x >= 0 && x + src->w <= dst->w);
                assert(y >= 0 && y + src->h <= dst->h);
                unsigned char *buf = dst->buffer + y * dst->stride + x;
                render_priv->engine.add_bitmaps(buf, dst->stride,
                                                src->buffer, src->stride,
                                                src->w, src->h);
            }
        }
    }

    double r2x = restore_blur(k->filter.blur_x);
    double r2y = restore_blur(k->filter.blur_y);
    if (multi_border) {
        /*
         * The shadow uses the outer cumulative geometry with the global
         * filter. It must not inherit any visible border's own blur.
         */
        if ((flags & FILTER_NONZERO_SHADOW) &&
                (flags & FILTER_NONZERO_BORDER)) {
            for (int layer = ASS_BORDER_LAYERS_MAX - 1; layer >= 0; layer--) {
                Bitmap *source = composite_border_bitmap(v, layer);
                if (source->buffer) {
                    ass_copy_bitmap(&render_priv->engine, &v->bm_s, source);
                    break;
                }
            }
        }
        /*
         * Separate geometric rings before filtering. Differently blurred
         * cumulative masks cannot be subtracted without corrupting rings.
         */
        Bitmap covered = {0};
        if (rect_o.x_min <= rect_o.x_max && rect_o.y_min <= rect_o.y_max &&
                ass_alloc_bitmap(&render_priv->engine, &covered,
                                 rect_o.x_max - rect_o.x_min + 2 * bord,
                                 rect_o.y_max - rect_o.y_min + 2 * bord,
                                 true)) {
            covered.left = rect_o.x_min - bord;
            covered.top  = rect_o.y_min - bord;
            covered.logical_w = rect_o_l.x_max - rect_o_l.x_min + 2 * bord;
            covered.logical_h = rect_o_l.y_max - rect_o_l.y_min + 2 * bord;
            bitmap_max_into(&covered, &v->bm);
            for (int layer = 0; layer < ASS_BORDER_LAYERS_MAX; layer++)
                ring_subtract_covered(composite_border_bitmap(v, layer),
                                      &covered);
            ass_free_bitmap(&covered);
        }
        ass_synth_blur(&render_priv->engine, &v->bm_s,
                       k->filter.be, r2x, r2y);
        ass_synth_blur(&render_priv->engine, &v->bm,
                       k->filter.be, r2x, r2y);
        for (int layer = 0; layer < ASS_BORDER_LAYERS_MAX; layer++) {
            Bitmap *ring = composite_border_bitmap(v, layer);
            if (!ring->buffer)
                continue;
            int blur_x = k->filter.border_blur_x[layer];
            int blur_y = k->filter.border_blur_y[layer];
            ass_synth_blur(&render_priv->engine, ring,
                           k->filter.border_be[layer],
                           blur_x == k->filter.blur_x ?
                               r2x : restore_blur(blur_x),
                           blur_y == k->filter.blur_y ?
                               r2y : restore_blur(blur_y));
        }
    } else {
        if (!(flags & FILTER_NONZERO_BORDER) || (flags & FILTER_BORDER_STYLE_3))
            ass_synth_blur(&render_priv->engine, &v->bm, k->filter.be, r2x, r2y);
        ass_synth_blur(&render_priv->engine, &v->bm_o, k->filter.be, r2x, r2y);
        if (!(flags & FILTER_FILL_IN_BORDER) &&
                !(flags & FILTER_FILL_IN_SHADOW))
            ass_fix_outline(&v->bm, &v->bm_o);
    }

    if (flags & FILTER_NONZERO_SHADOW) {
        if (flags & FILTER_NONZERO_BORDER) {
            if (!multi_border)
                ass_copy_bitmap(&render_priv->engine, &v->bm_s, &v->bm_o);
            if (!multi_border && (flags & FILTER_FILL_IN_BORDER) &&
                    !(flags & FILTER_FILL_IN_SHADOW))
                ass_fix_outline(&v->bm, &v->bm_s);
        } else if (flags & FILTER_BORDER_STYLE_3) {
            v->bm_s = v->bm_o;
            memset(&v->bm_o, 0, sizeof(v->bm_o));
        } else {
            ass_copy_bitmap(&render_priv->engine, &v->bm_s, &v->bm);
        }

        // Works right even for negative offsets
        // '>>' rounds toward negative infinity, '&' returns correct remainder
        v->bm_s.left += k->filter.shadow.x >> 6;
        v->bm_s.top  += k->filter.shadow.y >> 6;
        int shift_x = k->filter.shadow.x & SUBPIXEL_MASK;
        int shift_y = k->filter.shadow.y & SUBPIXEL_MASK;
        ass_shift_bitmap(&v->bm_s, shift_x, shift_y);
        v->bm_s.sub_x = (uint8_t) ((v->bm_s.sub_x + ((shift_x + 4) >> 3)) & 7);
        v->bm_s.sub_y = (uint8_t) ((v->bm_s.sub_y + ((shift_y + 4) >> 3)) & 7);
    }

    if (!multi_border && (flags & FILTER_FILL_IN_SHADOW) &&
            !(flags & FILTER_FILL_IN_BORDER))
        ass_fix_outline(&v->bm, &v->bm_o);

    return sizeof(CompositeHashKey) + sizeof(CompositeHashValue) +
        k->bitmap_count * sizeof(BitmapRef) +
        bitmap_size(&v->bm) + bitmap_size(&v->bm_o) + bitmap_size(&v->bm_s) +
        bitmap_size(&v->bm_border[0]) + bitmap_size(&v->bm_border[1]) +
        bitmap_size(&v->bm_border[2]) + bitmap_size(&v->bm_border[3]) +
        bitmap_size(&v->bm_border[4]) + bitmap_size(&v->bm_border[5]) +
        bitmap_size(&v->bm_border[6]) + bitmap_size(&v->bm_border[7]) +
        bitmap_size(&v->bm_border[8]);
}

typedef struct {
    int inner_x, inner_y;
    int outer_x, outer_y;
    uint32_t color;
} BoxBorderRenderLayer;

typedef struct {
    int32_t source_size;
    int32_t radius_x;
    int32_t radius_y;
} BS4BoxShape;

/*
 * BorderStyle=4 is one event-level object even though normal rendering keeps
 * geometry on individual glyphs.  Keep a snapshot of one glyph's evaluated
 * geometry for the box, but deliberately retain paint state in RenderContext:
 * box mode, colour, padding and box-border tags have always been event-level
 * final-state values.
 */
typedef struct {
    bool valid;
    ASS_DRect layout_bounds;
    GlyphInfo geometry;
    double device_x, device_y;
} BS4BoxGeometry;

static bool bs4_glyph_color_visible(const GlyphInfo *info, int layer)
{
    uint32_t color = info->c[layer];
    ass_apply_fade(&color, info->fade);
    return _a(color) != 0xFF;
}

static bool karaoke_reveal_hidden(RenderContext *state, const GlyphInfo *info)
{
    if (!info->furi_base_karaoke)
        return info->effect_type == EF_KARAOKE_REVEAL &&
               info->effect_timing <= 0;

    if (info->furi_group < 0 ||
            info->furi_group >= state->text_info.n_furi_groups)
        return false;
    FuriGroup *group = &state->text_info.furi_groups[info->furi_group];
    int64_t now = state->renderer->time - state->event->Start;
    bool found = false;
    for (int i = 0; i < group->n_karaoke_regions; i++) {
        int index = group->karaoke_regions[i].segment;
        if (index < 0 || index >= state->text_info.n_karaoke_segments)
            return false;
        KaraokeSegment *segment = &state->text_info.karaoke_segments[index];
        if (segment->effect_type != EF_KARAOKE_REVEAL)
            return false;
        found = true;
        if (now >= segment->start)
            return false;
    }
    return found;
}

static bool bs4_glyph_has_visible_paint(RenderContext *state,
                                        const GlyphInfo *info)
{
    if (karaoke_reveal_hidden(state, info))
        return false;

    if (bs4_glyph_color_visible(info, 0))
        return true;

    if ((info->furi_base_karaoke ||
         info->effect_type == EF_KARAOKE ||
         info->effect_type == EF_KARAOKE_KO ||
         info->effect_type == EF_KARAOKE_KF) &&
        bs4_glyph_color_visible(info, 1))
        return true;

    if ((glyph_border_max_x(info) || glyph_border_max_y(info) ||
         has_multi_border_layers(info->border_layers)) &&
        bs4_glyph_color_visible(info, 2))
        return true;

    return false;
}

static bool bs4_visible_glyph(RenderContext *state, const GlyphInfo *info)
{
    if (info->skip || !bs4_glyph_has_visible_paint(state, info))
        return false;

    if (info->drawing_text.str)
        return true;

    OutlineHashValue *outline = info->distorted_outline ?
        info->distorted_outline : info->outline;
    return outline && outline->outline[0].n_points;
}

static GlyphInfo *find_bs4_geometry_in_list(RenderContext *state,
                                             GlyphInfo *glyphs, int length,
                                             GlyphInfo **fallback)
{
    for (int i = 0; i < length; i++) {
        for (GlyphInfo *info = glyphs + i; info; info = info->next) {
            if (info->skip)
                continue;
            if (!*fallback)
                *fallback = info;
            if (bs4_visible_glyph(state, info))
                return info;
        }
    }
    return NULL;
}

/*
 * The first visible glyph/drawing defines the deterministic geometry state of
 * an event box.  Later per-glyph geometry does not split the one BS4 box.
 * This runs after origin/jitter evaluation but before glyph rendering turns
 * GlyphInfo.pos into screen coordinates.
 */
static void capture_bs4_box_geometry(RenderContext *state,
                                     BS4BoxGeometry *box,
                                     const ASS_DRect *layout_bounds,
                                     double device_x, double device_y)
{
    *box = (BS4BoxGeometry) {0};
    box->layout_bounds = *layout_bounds;
    box->device_x = device_x;
    box->device_y = device_y;

    TextInfo *text_info = &state->text_info;
    GlyphInfo *fallback = NULL;
    GlyphInfo *info = find_bs4_geometry_in_list(state, text_info->glyphs,
                                                text_info->length,
                                                &fallback);
    for (int i = 0; !info && i < text_info->n_furi_groups; i++) {
        FuriGroup *group = &text_info->furi_groups[i];
        info = find_bs4_geometry_in_list(state, group->glyphs, group->length,
                                         &fallback);
    }
    if (!info && fallback && !karaoke_reveal_hidden(state, fallback))
        info = fallback;
    if (!info)
        return;

    box->geometry = *info;
    box->valid = true;
}

static BS4BoxGeometry *capture_scroll_boxes(RenderContext *state,
                                            double device_x, double device_y,
                                            int *count)
{
    TextInfo *text = &state->text_info;
    *count = 1;
    for (int i = 1; i < text->length; i++)
        *count += text->glyphs[i].scroll_id != text->glyphs[i - 1].scroll_id;
    BS4BoxGeometry *boxes = ass_realloc_array(NULL, *count, sizeof(*boxes));
    if (!boxes)
        return NULL;
    int index = 0;
    for (int start = 0; start < text->length;) {
        int end = start + 1;
        while (end < text->length &&
               text->glyphs[end].scroll_id == text->glyphs[start].scroll_id)
            end++;
        BS4BoxGeometry *box = &boxes[index++];
        *box = (BS4BoxGeometry) { .device_x = device_x, .device_y = device_y };
        GlyphInfo *fallback = NULL;
        GlyphInfo *info = find_bs4_geometry_in_list(state, text->glyphs + start,
                                                   end - start, &fallback);
        if (!info && fallback && !karaoke_reveal_hidden(state, fallback))
            info = fallback;
        if (info) {
            box->geometry = *info;
            box->valid = true;
            box->layout_bounds = (ASS_DRect) {DBL_MAX, DBL_MAX, -DBL_MAX, -DBL_MAX};
            for (int j = start; j < end; j++) {
                GlyphInfo *g = &text->glyphs[j];
                if (g->skip) continue;
                double x = d6_to_double(g->pos.x), y = d6_to_double(g->pos.y);
                box->layout_bounds.x_min = FFMIN(box->layout_bounds.x_min, x);
                box->layout_bounds.x_max = FFMAX(box->layout_bounds.x_max,
                                                x + d6_to_double(g->cluster_advance.x));
                box->layout_bounds.y_min = FFMIN(box->layout_bounds.y_min,
                                                y - text->lines[g->line].asc);
                box->layout_bounds.y_max = FFMAX(box->layout_bounds.y_max,
                                                y + text->lines[g->line].desc);
            }
            for (int j = 0; j < text->n_furi_groups; j++) {
                FuriGroup *group = &text->furi_groups[j];
                if (group->base_start >= start && group->base_start < end)
                    add_glyph_list_visual_bbox(group->glyphs, group->length,
                                               &box->layout_bounds);
            }
            if (info->scroll_id)
                box->device_y -= state->scroll_contexts[info->scroll_id - 1].displacement;
        }
        start = end;
    }
    return boxes;
}

static uint32_t box_border_layer_color(RenderContext *state,
                                       const BorderLayerState *layer)
{
    uint32_t box = state->c[3];
    uint32_t color = layer->has_color ? (layer->color & 0xFFFFFF00u) :
                     (box & 0xFFFFFF00u);
    color |= layer->has_alpha ? _a(layer->color) : _a(box);
    ass_apply_fades(&color, state->fade, state->fade_color);
    return color;
}

static bool bs4_double_to_d6(double value, int32_t *out)
{
    if (!isfinite(value) || fabs(value) > INT_MAX / 64.0)
        return false;
    *out = ass_lrint(value * 64.0);
    return true;
}

static bool bs4_effective_scales(const BS4BoxGeometry *box,
                                 double *scale_x, double *scale_y)
{
    double x = box->geometry.scale_x * box->geometry.scale_fix;
    double y = box->geometry.scale_y * box->geometry.scale_fix;
    if (box->geometry.soft_scale != 1.0) {
        if (!isfinite(box->geometry.soft_scale) || box->geometry.soft_scale <= 0.0)
            return false;
        x /= box->geometry.soft_scale;
        y /= box->geometry.soft_scale;
    }
    if (!isfinite(x) || !isfinite(y) || x == 0.0 || y == 0.0)
        return false;
    *scale_x = fabs(x);
    *scale_y = fabs(y);
    return true;
}

static double bs4_clamp_box_radius(double radius, double left, double top,
                                   double right, double bottom)
{
    if (!isfinite(radius) || radius <= 0.0 ||
        !(left < right) || !(top < bottom))
        return 0.0;

    double limit = 0.5 * FFMIN(right - left, bottom - top);
    return isfinite(limit) && limit > 0.0 ? FFMIN(radius, limit) : 0.0;
}

/*
 * The original square outline spans 0..64.  A larger source square gives
 * rounded corners enough fixed-point precision without changing radius-zero
 * boxes or their cached transform path.
 */
static BS4BoxShape bs4_box_shape(double left, double top,
                                 double right, double bottom, double radius)
{
    BS4BoxShape shape = { .source_size = 64 };
    radius = bs4_clamp_box_radius(radius, left, top, right, bottom);
    if (radius <= 0.0)
        return shape;

    double width = right - left;
    double height = bottom - top;
    int32_t radius_x = ass_lrint(radius / width * BS4_ROUNDED_BOX_SCALE);
    int32_t radius_y = ass_lrint(radius / height * BS4_ROUNDED_BOX_SCALE);
    if (radius_x <= 0 || radius_y <= 0)
        return shape;

    shape.source_size = BS4_ROUNDED_BOX_SCALE;
    shape.radius_x = FFMIN(radius_x, BS4_ROUNDED_BOX_SCALE / 2);
    shape.radius_y = FFMIN(radius_y, BS4_ROUNDED_BOX_SCALE / 2);
    return shape;
}

/* Build the m1 glyph matrix in the event-local layout coordinate plane. */
static bool bs4_event_transform_matrix(RenderContext *state,
                                       const BS4BoxGeometry *box,
                                       double matrix[3][3])
{
    GlyphInfo info = box->geometry;
    if (!isfinite(info.scale_x) || !isfinite(info.scale_y) ||
        info.scale_x == 0.0 || info.scale_y == 0.0)
        return false;

    ASS_Renderer *render_priv = state->renderer;
    double jitter_x = info.has_jitter ? info.jitter_dx : 0.0;
    double jitter_y = info.has_jitter ? info.jitter_dy : 0.0;
    double screen_x = (box->device_x - render_priv->settings.left_margin) *
        render_priv->par_scale_x + render_priv->settings.left_margin + jitter_x;
    double screen_y = box->device_y + jitter_y;
    if (!bs4_double_to_d6(screen_x, &info.pos.x) ||
        !bs4_double_to_d6(screen_y, &info.pos.y))
        return false;

    int64_t shift_x = (int64_t) box->geometry.shift.x -
        box->geometry.pos.x;
    int64_t shift_y = (int64_t) box->geometry.shift.y -
        box->geometry.pos.y;
    if (shift_x < INT_MIN || shift_x > INT_MAX ||
        shift_y < INT_MIN || shift_y > INT_MAX)
        return false;
    info.shift.x = (int32_t) shift_x;
    info.shift.y = (int32_t) shift_y;

    calc_transform_matrix(state, &info, matrix, NULL);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            if (!isfinite(matrix[i][j]))
                return false;
    return true;
}

static bool bs4_multiply_matrix(double result[3][3],
                                const double left[3][3],
                                const double right[3][3])
{
    double temp[3][3] = {{0}};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            for (int k = 0; k < 3; k++)
                temp[i][j] += left[i][k] * right[k][j];
            if (!isfinite(temp[i][j]))
                return false;
        }
    }
    memcpy(result, temp, sizeof(temp));
    return true;
}

/*
 * Make a homography from the cached unit square (0..64) to the supplied
 * quadrilateral.  Rendering the unit square through that matrix keeps a BS4
 * rectangle a real four-sided path through rotation and projection.
 */
static bool bs4_unit_square_to_quad(const ASS_DVector points[4],
                                    int32_t source_size,
                                    double matrix[3][3])
{
    if (source_size <= 0)
        return false;
    const double x0 = points[0].x, y0 = points[0].y;
    const double x1 = points[1].x, y1 = points[1].y;
    const double x2 = points[2].x, y2 = points[2].y;
    const double x3 = points[3].x, y3 = points[3].y;
    for (int i = 0; i < 4; i++)
        if (!isfinite(points[i].x) || !isfinite(points[i].y))
            return false;

    double dx1 = x1 - x2;
    double dx2 = x3 - x2;
    double dx3 = x0 - x1 + x2 - x3;
    double dy1 = y1 - y2;
    double dy2 = y3 - y2;
    double dy3 = y0 - y1 + y2 - y3;
    double den = dx1 * dy2 - dx2 * dy1;
    if (!isfinite(den) || fabs(den) < 1e-12)
        return false;

    double g = (dx3 * dy2 - dx2 * dy3) / den;
    double h = (dx1 * dy3 - dx3 * dy1) / den;
    double a = x1 * (g + 1.0) - x0;
    double b = x3 * (h + 1.0) - x0;
    double d = y1 * (g + 1.0) - y0;
    double e = y3 * (h + 1.0) - y0;
    double values[] = {a, b, x0, d, e, y0, g, h};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
        if (!isfinite(values[i]))
            return false;

    matrix[0][0] = a / source_size;
    matrix[0][1] = b / source_size;
    matrix[0][2] = x0;
    matrix[1][0] = d / source_size;
    matrix[1][1] = e / source_size;
    matrix[1][2] = y0;
    matrix[2][0] = g / source_size;
    matrix[2][1] = h / source_size;
    matrix[2][2] = 1.0;
    return true;
}

/*
 * Map a local rectangle through the canonical event distortion first, then
 * through the normal event transform.  All box-border rectangles share the
 * same distortion domain so their common edges stay attached instead of being
 * independently re-normalised: the padded fill rectangle for legacy distortion,
 * or the canonical glyph's source bbox for extended distortion.
 */
static bool bs4_box_matrix(RenderContext *state, const BS4BoxGeometry *box,
                           double left, double top, double right, double bottom,
                           double domain_left, double domain_top,
                           double domain_right, double domain_bottom,
                           const BS4BoxShape *shape,
                           double matrix[3][3])
{
    if (!shape || !(left < right) || !(top < bottom) ||
        !(domain_left < domain_right) || !(domain_top < domain_bottom))
        return false;

    ASS_DVector points[4] = {
        { left * 64.0,  top * 64.0 },
        { right * 64.0, top * 64.0 },
        { right * 64.0, bottom * 64.0 },
        { left * 64.0,  bottom * 64.0 },
    };
    if (box->geometry.distort_enabled) {
        // Six-slot boxes retain their padded fill domain. Extended boxes use
        // the canonical glyph's shared source domain, including all its lines,
        // so padding and box borders continue the same bilinear deformation.
        bool shared = box->geometry.distort_extended &&
                      box->geometry.has_distort_outline;
        const ASS_DRect *bbox = &box->geometry.distort_bbox;
        const double x0 = shared ? bbox->x_min : domain_left * 64.0;
        const double y0 = shared ? bbox->y_min : domain_top * 64.0;
        const double x1 = shared ? bbox->x_max : domain_right * 64.0;
        const double y1 = shared ? bbox->y_max : domain_bottom * 64.0;
        for (int i = 0; i < 4; i++)
            points[i] = ass_distort_map_point(&box->geometry.distort, x0, y0, x1, y1,
                                              points[i].x, points[i].y);
    }

    double local_matrix[3][3];
    double event_matrix[3][3];
    return bs4_unit_square_to_quad(points, shape->source_size, local_matrix) &&
        bs4_event_transform_matrix(state, box, event_matrix) &&
        bs4_multiply_matrix(matrix, event_matrix, local_matrix);
}

/* Avoid rasterizing a completely off-frame box before normal clipping. */
static bool bs4_bitmap_may_be_visible(RenderContext *state,
                                      const double matrix[3][3],
                                      int32_t source_size)
{
    if (source_size <= 0)
        return false;
    ASS_Renderer *render_priv = state->renderer;
    double x_min = DBL_MAX, y_min = DBL_MAX;
    double x_max = -DBL_MAX, y_max = -DBL_MAX;
    for (int y = 0; y <= source_size; y += source_size) {
        for (int x = 0; x <= source_size; x += source_size) {
            double z = matrix[2][0] * x + matrix[2][1] * y + matrix[2][2];
            if (!isfinite(z) || z <= 0.0)
                return true;
            double px = (matrix[0][0] * x + matrix[0][1] * y + matrix[0][2]) /
                z / 64.0;
            double py = (matrix[1][0] * x + matrix[1][1] * y + matrix[1][2]) /
                z / 64.0;
            if (!isfinite(px) || !isfinite(py))
                return true;
            x_min = FFMIN(x_min, px);
            y_min = FFMIN(y_min, py);
            x_max = FFMAX(x_max, px);
            y_max = FFMAX(y_max, py);
        }
    }

    int clip_x0 = 0, clip_y0 = 0;
    int clip_x1 = render_priv->width, clip_y1 = render_priv->height;
    if (!state->clip_mode) {
        clip_x0 = FFMINMAX(state->clip_x0, 0, render_priv->width);
        clip_y0 = FFMINMAX(state->clip_y0, 0, render_priv->height);
        clip_x1 = FFMINMAX(state->clip_x1, 0, render_priv->width);
        clip_y1 = FFMINMAX(state->clip_y1, 0, render_priv->height);
    }
    if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1)
        return false;

    /* Keep a pixel of AA guard band; render_glyph performs the exact clip. */
    return x_max >= clip_x0 - 1.0 && x_min <= clip_x1 + 1.0 &&
        y_max >= clip_y0 - 1.0 && y_min <= clip_y1 + 1.0;
}

static bool bs4_get_bitmap(RenderContext *state, const double matrix[3][3],
                           const BS4BoxShape *shape,
                           ASS_Vector *pos, Bitmap **bitmap)
{
    if (!shape || !bs4_bitmap_may_be_visible(state, matrix,
                                              shape->source_size))
        return false;

    ASS_Renderer *render_priv = state->renderer;
    OutlineHashKey outline_key = {0};
    if (shape->radius_x > 0 && shape->radius_y > 0) {
        outline_key.type = OUTLINE_ROUNDED_BOX;
        outline_key.u.rounded_box.radius_x = shape->radius_x;
        outline_key.u.rounded_box.radius_y = shape->radius_y;
    } else {
        /* Preserve the exact pre-\boxr cached square and matrix path. */
        outline_key.type = OUTLINE_BOX;
    }
    OutlineHashValue *outline = ass_cache_get(render_priv->cache.outline_cache,
                                              &outline_key, render_priv);
    if (!outline || !outline->valid)
        return false;

    BitmapHashKey key = {0};
    key.outline = outline;
    double transform[3][3];
    memcpy(transform, matrix, sizeof(transform));
    if (!quantize_transform(transform, pos, NULL, true, &key))
        return false;

    *bitmap = ass_cache_get(render_priv->cache.bitmap_cache, &key, state);
    return *bitmap && (*bitmap)->buffer && (*bitmap)->w && (*bitmap)->h;
}

static bool bs4_mask_has_coverage(const uint8_t *mask, int width, int height)
{
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
            if (mask[(size_t) y * width + x])
                return true;
    return false;
}

static uint8_t *bs4_copy_bitmap_mask(const Bitmap *bitmap)
{
    if (!bitmap || !bitmap->buffer || bitmap->w < 1 || bitmap->h < 1 ||
        (size_t) bitmap->w > SIZE_MAX / (size_t) bitmap->h)
        return NULL;

    size_t size = (size_t) bitmap->w * bitmap->h;
    uint8_t *mask = ass_aligned_alloc_tagged(
        1, size, false, ASS_ALIGNED_ALLOC_BS4_MASK, bitmap);
    if (!mask)
        return NULL;
    for (int y = 0; y < bitmap->h; y++)
        memcpy(mask + (size_t) y * bitmap->w,
               bitmap->buffer + (ptrdiff_t) y * bitmap->stride, bitmap->w);
    return mask;
}

/* Subtract anti-aliased inner coverage from the outer transformed rectangle. */
static void bs4_subtract_inner_mask(uint8_t *outer_mask,
                                    const Bitmap *outer, ASS_Vector outer_pos,
                                    const Bitmap *inner, ASS_Vector inner_pos)
{
    if (!outer_mask || !outer || !inner || !inner->buffer)
        return;

    int64_t ox = (int64_t) outer_pos.x + outer->left;
    int64_t oy = (int64_t) outer_pos.y + outer->top;
    int64_t ix = (int64_t) inner_pos.x + inner->left;
    int64_t iy = (int64_t) inner_pos.y + inner->top;
    int64_t left = FFMAX(ox, ix);
    int64_t top = FFMAX(oy, iy);
    int64_t right = FFMIN(ox + outer->w, ix + inner->w);
    int64_t bottom = FFMIN(oy + outer->h, iy + inner->h);
    if (left >= right || top >= bottom)
        return;

    for (int64_t y = top; y < bottom; y++) {
        uint8_t *dst = outer_mask + (size_t) (y - oy) * outer->w + (left - ox);
        const uint8_t *src = inner->buffer + (y - iy) * inner->stride +
            (left - ix);
        for (int64_t x = left; x < right; x++) {
            uint8_t coverage = *src++;
            *dst = *dst > coverage ? *dst - coverage : 0;
            dst++;
        }
    }
}

static ASS_Image **append_bs4_bitmap(RenderContext *state, Bitmap *bitmap,
                                     ASS_Vector pos, uint32_t color,
                                     unsigned type, ASS_Image **tail,
                                     ASS_ImageRGBA ***rgba_tail)
{
    /* render_bitmap_rgba() expects a CombinedBitmapInfo even for flat masks. */
    CombinedBitmapInfo paint = {0};
    paint.base_c[0] = color;
    paint.c[0] = color;
    paint.blend_mode = state->blend_mode;
    return render_glyph(state, &paint, bitmap, pos.x, pos.y, color, 0,
                        1000000, tail, type, NULL, 0, 0, rgba_tail);
}

static int collect_box_border_render_layers(RenderContext *state,
                                             BoxBorderRenderLayer *layers)
{
    int count = 0;
    int outer_x = 0;
    int outer_y = 0;
    /* Match native multi-border semantics: each layer stores its own
     * thickness, while its rendered rectangle starts at the cumulative
     * extent of all earlier enabled layers. */
    for (int i = 0; i < ASS_BORDER_LAYERS_MAX; i++) {
        BorderLayerState *layer = &state->box_border_layers[i];
        if (!border_layer_has_size(layer))
            continue;

        int thickness_x = layer->size_x > 0 ?
            lround(layer->size_x * state->border_scale_x) : 0;
        int thickness_y = layer->size_y > 0 ?
            lround(layer->size_y * state->border_scale_y) : 0;
        if (thickness_x < 1 && thickness_y < 1)
            continue;

        int inner_x = outer_x;
        int inner_y = outer_y;
        outer_x += thickness_x;
        outer_y += thickness_y;
        layers[count++] = (BoxBorderRenderLayer) {
            .inner_x = inner_x,
            .inner_y = inner_y,
            .outer_x = outer_x,
            .outer_y = outer_y,
            .color = box_border_layer_color(state, layer),
        };
    }
    return count;
}

static void add_background(RenderContext *state, EventImages *event_images,
                           ASS_ImageRGBA **rgba_head,
                           const BS4BoxGeometry *geometry)
{
    if (!geometry->valid)
        return;

    ASS_Renderer *render_priv = state->renderer;
    double scale_x, scale_y;
    if (!bs4_effective_scales(geometry, &scale_x, &scale_y))
        return;

    /*
     * These are the old BS4 extents, moved from final screen-space into the
     * event-local plane.  Padding and box-border widths are expanded here,
     * before all rotation/projection/distortion.
     */
    double par = render_priv->par_scale_x;
    if (!isfinite(par) || par <= 0.0)
        return;
    double shadow_x = state->shadow_x > 0.0 ?
        state->shadow_x * state->border_scale_x * scale_x / par : 0.0;
    double shadow_y = state->shadow_y > 0.0 ?
        state->shadow_y * state->border_scale_y * scale_y : 0.0;
    double extra_x = FFMAX(0.0, state->box_extra_x) *
        state->border_scale_x * scale_x / par;
    double extra_y = FFMAX(0.0, state->box_extra_y) *
        state->border_scale_y * scale_y;

    TextInfo *text_info = &state->text_info;
    double fill_left = geometry->layout_bounds.x_min -
        text_info->border_x / par - shadow_x - extra_x;
    double fill_right = geometry->layout_bounds.x_max +
        text_info->border_x / par + shadow_x + extra_x;
    double fill_top = geometry->layout_bounds.y_min - text_info->border_top -
        shadow_y - extra_y;
    double fill_bottom = geometry->layout_bounds.y_max + text_info->border_bottom +
        shadow_y + extra_y;
    if (!isfinite(fill_left) || !isfinite(fill_top) ||
        !isfinite(fill_right) || !isfinite(fill_bottom) ||
        !(fill_left < fill_right) || !(fill_top < fill_bottom))
        return;
    double fill_radius = bs4_clamp_box_radius(state->box_corner_radius,
                                              fill_left, fill_top,
                                              fill_right, fill_bottom);

    ASS_Image *box_head = NULL;
    ASS_Image **box_tail = &box_head;
    ASS_ImageRGBA *box_rgba_head = NULL;
    ASS_ImageRGBA **box_rgba_tail = &box_rgba_head;
    ASS_ImageRGBA ***rgba_tail = rgba_head ? &box_rgba_tail : NULL;

    BoxBorderRenderLayer layers[ASS_BORDER_LAYERS_MAX];
    int n_layers = collect_box_border_render_layers(state, layers);
    for (int i = n_layers - 1; i >= 0; i--) {
        const BoxBorderRenderLayer *layer = &layers[i];
        double outer_x = layer->outer_x * scale_x / par;
        double outer_y = layer->outer_y * scale_y;
        double inner_x = layer->inner_x * scale_x / par;
        double inner_y = layer->inner_y * scale_y;
        double outer_matrix[3][3], inner_matrix[3][3];
        ASS_Vector outer_pos, inner_pos;
        Bitmap *outer = NULL, *inner = NULL;
        BS4BoxShape outer_shape = bs4_box_shape(
            fill_left - outer_x, fill_top - outer_y,
            fill_right + outer_x, fill_bottom + outer_y,
            fill_radius + FFMIN(outer_x, outer_y));
        BS4BoxShape inner_shape = bs4_box_shape(
            fill_left - inner_x, fill_top - inner_y,
            fill_right + inner_x, fill_bottom + inner_y,
            fill_radius + FFMIN(inner_x, inner_y));
        if (!bs4_box_matrix(state, geometry,
                            fill_left - outer_x, fill_top - outer_y,
                            fill_right + outer_x, fill_bottom + outer_y,
                            fill_left, fill_top, fill_right, fill_bottom,
                            &outer_shape,
                            outer_matrix) ||
            !bs4_box_matrix(state, geometry,
                            fill_left - inner_x, fill_top - inner_y,
                            fill_right + inner_x, fill_bottom + inner_y,
                            fill_left, fill_top, fill_right, fill_bottom,
                            &inner_shape,
                            inner_matrix) ||
            !bs4_get_bitmap(state, outer_matrix, &outer_shape,
                            &outer_pos, &outer))
            continue;

        /* An off-frame inner box is an empty hole, not a reason to drop
         * the visible outward ring. */
        if (bs4_bitmap_may_be_visible(state, inner_matrix,
                                      inner_shape.source_size) &&
            !bs4_get_bitmap(state, inner_matrix, &inner_shape,
                            &inner_pos, &inner))
            continue;

        uint8_t *mask = bs4_copy_bitmap_mask(outer);
        if (!mask)
            continue;
        if (inner)
            bs4_subtract_inner_mask(mask, outer, outer_pos, inner, inner_pos);
        if (bs4_mask_has_coverage(mask, outer->w, outer->h)) {
            Bitmap ring = {
                .left = outer->left,
                .top = outer->top,
                .w = outer->w,
                .h = outer->h,
                .logical_w = outer->logical_w,
                .logical_h = outer->logical_h,
                .sub_x = outer->sub_x,
                .sub_y = outer->sub_y,
                .stride = outer->w,
                .buffer = mask,
            };
            box_tail = append_bs4_bitmap(state, &ring, outer_pos, layer->color,
                                         IMAGE_TYPE_OUTLINE, box_tail, rgba_tail);
        }
        ass_aligned_free_tagged(mask, ASS_ALIGNED_ALLOC_BS4_MASK, outer);
    }

    double fill_matrix[3][3];
    ASS_Vector fill_pos;
    Bitmap *fill = NULL;
    BS4BoxShape fill_shape = bs4_box_shape(fill_left, fill_top,
                                            fill_right, fill_bottom,
                                            fill_radius);
    if (bs4_box_matrix(state, geometry, fill_left, fill_top,
                       fill_right, fill_bottom,
                       fill_left, fill_top, fill_right, fill_bottom,
                       &fill_shape,
                       fill_matrix) &&
        bs4_get_bitmap(state, fill_matrix, &fill_shape, &fill_pos, &fill)) {
        uint32_t color = state->c[3];
        ass_apply_fades(&color, state->fade, state->fade_color);
        box_tail = append_bs4_bitmap(state, fill, fill_pos, color,
                                     IMAGE_TYPE_SHADOW, box_tail, rgba_tail);
    }

    /* render_text() already clipped text, so only clip the new box images. */
    *box_tail = NULL;
    *box_rgba_tail = NULL;
    if (box_head)
        blend_vector_clip(state, box_head);
    if (box_rgba_head)
        blend_vector_clip_rgba(state, box_rgba_head);

    if (box_head) {
        *box_tail = event_images->imgs;
        event_images->imgs = box_head;
    }
    if (rgba_head && box_rgba_head) {
        *box_rgba_tail = *rgba_head;
        *rgba_head = box_rgba_head;
        event_images->imgs_rgba = box_rgba_head;
    }
}

/* Chat dimensions are screen-scaled PlayRes ratios and one font-based unit.
 * The 75% bubble cap is intentional: the other side must remain visible. */
static double chat_natural_width(const TextInfo *info, int start, int end)
{
    double line = 0.0, widest = 0.0;
    for (int i = start; i < end; i++) {
        const GlyphInfo *glyph = &info->glyphs[i];
        if (glyph->symbol == '\n') {
            widest = FFMAX(widest, line);
            line = 0.0;
        } else if (!glyph->skip) {
            line += fabs(d6_to_double(glyph->cluster_advance.x));
        }
    }
    return FFMAX(widest, line);
}

static const GlyphInfo *chat_message_style_glyph(const TextInfo *info,
                                                  const ChatRange *range)
{
    if (range->start >= range->end)
        return NULL;
    int body = range->name_end < range->end ? range->name_end : range->start;
    return &info->glyphs[body];
}

/* Chat geometry is measured before PAR scaling; convert ASS border units once
 * here and use the same extents for padding and the rendered stroke. */
static void chat_stroke_extent(RenderContext *state, double size,
                               double *stroke_x, double *stroke_y)
{
    double par = state->renderer->par_scale_x;
    *stroke_x = par > 0 ? FFMAX(0.0, size * state->border_scale_x / par) : 0;
    *stroke_y = FFMAX(0.0, size * state->border_scale_y);
}

static ChatMetrics chat_choose_metrics(RenderContext *state,
                                       const ASS_ChatScene *chat,
                                       const ChatRange *ranges,
                                       const ChatRange *title)
{
    ASS_Renderer *priv = state->renderer;
    const double video_width = priv->frame_content_width;
    const double video_height = priv->frame_content_height;
    double unit = fabs(state->style->FontSize * state->screen_scale_y *
                       state->object_scale);
    unit = FFMINMAX(unit, 6.0, FFMAX(8.0, video_height / 9.0));
    ChatMetrics m = {
        .panel_pad = unit * 0.28,
        .bubble_pad = unit * 0.22,
        .bubble_gap = unit * 0.10,
        .message_gap = unit * 0.16,
        .header_pad = unit * 0.20,
        .radius = unit * 0.22,
        .viewport_max = video_height * 0.68,
        .viewport_min = video_height * 0.28,
    };
    double natural = 0.0;
    double stroke_max = 0.0;
    for (int i = 0; i < chat->count; i++) {
        natural = FFMAX(natural, chat_natural_width(&state->text_info,
                          ranges[i].start, ranges[i].end));
        const GlyphInfo *glyph = chat_message_style_glyph(&state->text_info,
                                                           &ranges[i]);
        if (glyph || ranges[i].has_preset) {
            double stroke_x, stroke_y;
            double size = glyph ? glyph->chat_bubble.border_size :
                ranges[i].empty_bubble.border_size;
            chat_stroke_extent(state, size,
                               &stroke_x, &stroke_y);
            stroke_max = FFMAX(stroke_max, FFMAX(stroke_x, stroke_y));
        }
    }
    double pad_limit = FFMAX(m.bubble_pad, video_width * 0.56 * 0.75 / 4);
    m.bubble_pad = FFMIN(pad_limit,
                         FFMAX(m.bubble_pad, stroke_max + m.bubble_gap));
    double title_width = chat_natural_width(&state->text_info,
                                            title->start, title->end);
    double wanted = FFMAX((natural + 2 * m.bubble_pad) / 0.75 +
                          2 * m.panel_pad,
                          title_width + 2 * m.header_pad + 2 * m.panel_pad);
    /* A pathological line cannot expand the phone across the whole video. */
    m.panel_width = FFMINMAX(wanted, video_width * 0.30,
                             video_width * 0.56);
    double inner = m.panel_width - 2 * m.panel_pad;
    m.max_text_width = FFMAX(8.0, inner * 0.75 - 2 * m.bubble_pad);
    return m;
}

static void chat_measure_range(const TextInfo *info, ChatRange *range,
                               double fallback_height)
{
    ASS_DRect b = {DBL_MAX, DBL_MAX, -DBL_MAX, -DBL_MAX};
    for (int i = range->start; i < range->end; i++) {
        const GlyphInfo *root = &info->glyphs[i];
        if (root->skip || root->symbol == '\n')
            continue;
        for (const GlyphInfo *glyph = root; glyph; glyph = glyph->next) {
            double x = d6_to_double(glyph->pos.x);
            double y = d6_to_double(glyph->pos.y);
            b.x_min = FFMIN(b.x_min, x + d6_to_double(glyph->bbox.x_min));
            b.x_max = FFMAX(b.x_max, x + FFMAX(
                d6_to_double(glyph->bbox.x_max),
                d6_to_double(glyph->cluster_advance.x)));
            b.y_min = FFMIN(b.y_min, y + d6_to_double(glyph->bbox.y_min));
            b.y_max = FFMAX(b.y_max, y + d6_to_double(glyph->bbox.y_max));
        }
    }
    for (int i = 0; i < info->n_furi_groups; i++) {
        const FuriGroup *group = &info->furi_groups[i];
        if (group->base_start >= range->start &&
            group->base_start < range->end)
            add_glyph_list_visual_bbox(group->glyphs, group->length, &b);
    }
    if (b.x_min == DBL_MAX) {
        b.x_min = b.x_max = 0.0;
        b.y_min = 0.0;
        b.y_max = fallback_height;
    }
    range->bounds = b;
    range->width = FFMAX(0.0, b.x_max - b.x_min);
    range->height = FFMAX(fallback_height, b.y_max - b.y_min);
}

static void chat_shift_range(TextInfo *info, const ChatRange *range,
                             double x, double y)
{
    int32_t dx = double_to_d6(x - range->bounds.x_min);
    int32_t dy = double_to_d6(y - range->bounds.y_min);
    for (int i = range->start; i < range->end; i++)
        for (GlyphInfo *glyph = &info->glyphs[i]; glyph; glyph = glyph->next) {
            glyph->pos.x += dx;
            glyph->pos.y += dy;
        }
    for (int i = 0; i < info->n_furi_groups; i++) {
        FuriGroup *group = &info->furi_groups[i];
        if (group->base_start < range->start ||
            group->base_start >= range->end)
            continue;
        for (int j = 0; j < group->length; j++)
            for (GlyphInfo *glyph = &group->glyphs[j]; glyph;
                 glyph = glyph->next) {
                glyph->pos.x += dx;
                glyph->pos.y += dy;
            }
    }
}

static void chat_set_part(TextInfo *info, const ChatRange *range, int part)
{
    for (int i = range->start; i < range->end; i++)
        for (GlyphInfo *glyph = &info->glyphs[i]; glyph; glyph = glyph->next)
            glyph->chat_part = part;
    for (int i = 0; i < info->n_furi_groups; i++) {
        FuriGroup *group = &info->furi_groups[i];
        if (group->base_start < range->start ||
            group->base_start >= range->end)
            continue;
        for (int j = 0; j < group->length; j++)
            for (GlyphInfo *glyph = &group->glyphs[j]; glyph;
                 glyph = glyph->next)
                glyph->chat_part = part;
    }
}

static void chat_hide_range(TextInfo *info, const ChatRange *range)
{
    for (int i = range->start; i < range->end; i++)
        for (GlyphInfo *glyph = &info->glyphs[i]; glyph; glyph = glyph->next)
            glyph->skip = true;
    for (int i = 0; i < info->n_furi_groups; i++) {
        FuriGroup *group = &info->furi_groups[i];
        if (group->base_start < range->start ||
            group->base_start >= range->end)
            continue;
        for (int j = 0; j < group->length; j++)
            for (GlyphInfo *glyph = &group->glyphs[j]; glyph;
                 glyph = glyph->next)
                glyph->skip = true;
    }
}

static void resolve_event_clip(RenderContext *state)
{
    ASS_Renderer *priv = state->renderer;
    if (state->explicit || !priv->settings.use_margins) {
        ASS_DRect clip = clip_rectangle(state);
        state->clip_x0 = clip_screen_coord(state, x2scr_pos_scaled(priv, clip.x_min));
        state->clip_x1 = clip_screen_coord(state, x2scr_pos_scaled(priv, clip.x_max));
        state->clip_y0 = clip_screen_coord(state, y2scr_pos(priv, clip.y_min));
        state->clip_y1 = clip_screen_coord(state, y2scr_pos(priv, clip.y_max));
        if (state->explicit) {
            int zx = priv->settings.left_margin;
            int zy = priv->settings.top_margin;
            state->clip_x0 = FFMAX(state->clip_x0, zx);
            state->clip_y0 = FFMAX(state->clip_y0, zy);
            state->clip_x1 = FFMIN(state->clip_x1,
                                   zx + priv->frame_content_width);
            state->clip_y1 = FFMIN(state->clip_y1,
                                   zy + priv->frame_content_height);
        }
    } else {
        state->clip_x0 = state->clip_y0 = 0;
        state->clip_x1 = priv->settings.frame_width;
        state->clip_y1 = priv->settings.frame_height;
    }
}

static Bitmap *chat_box_bitmap(RenderContext *state, double x, double y,
                               double width, double height, double radius,
                               ASS_Vector *pos)
{
    if (width <= 0 || height <= 0)
        return NULL;
    double par = state->renderer->par_scale_x;
    double left_margin = state->renderer->settings.left_margin;
    x = (x - left_margin) * par + left_margin;
    width *= par;
    radius = FFMIN(radius * par, radius);
    BS4BoxShape shape = bs4_box_shape(x, y, x + width, y + height, radius);
    double scale = 64.0 / shape.source_size;
    double matrix[3][3] = {
        {width * scale, 0.0, x * 64.0},
        {0.0, height * scale, y * 64.0},
        {0.0, 0.0, 1.0},
    };
    Bitmap *bitmap;
    if (!bs4_get_bitmap(state, matrix, &shape, pos, &bitmap))
        return NULL;
    return bitmap;
}

static ASS_Image **append_chat_box(RenderContext *state, double x, double y,
                                   double width, double height, double radius,
                                   uint32_t color, ASS_Image **tail,
                                   ASS_ImageRGBA ***rgba_tail)
{
    ASS_Vector pos;
    Bitmap *bitmap = chat_box_bitmap(state, x, y, width, height,
                                      radius, &pos);
    if (!bitmap)
        return tail;
    ass_apply_fades(&color, state->fade, state->fade_color);
    return append_bs4_bitmap(state, bitmap, pos, color,
                             IMAGE_TYPE_SHADOW, tail, rgba_tail);
}

/* A bubble stroke occupies the inside of its measured rounded rectangle.
 * Subtracting the inner mask keeps fill and border alpha from overlapping. */
static ASS_Image **append_chat_bubble(RenderContext *state,
                                      double x, double y, double width,
                                      double height, double radius,
                                      double stroke_x, double stroke_y,
                                      ChatBubbleStyle style,
                                      ASS_Image **tail,
                                      ASS_ImageRGBA ***rgba_tail)
{
    if (stroke_x <= 0 || stroke_y <= 0)
        return append_chat_box(state, x, y, width, height, radius,
                               style.fill, tail, rgba_tail);
    ASS_Vector outer_pos, inner_pos;
    Bitmap *outer = chat_box_bitmap(state, x, y, width, height,
                                    radius, &outer_pos);
    Bitmap *inner = chat_box_bitmap(state, x + stroke_x, y + stroke_y,
                                    width - 2 * stroke_x,
                                    height - 2 * stroke_y,
                                    FFMAX(0.0, radius - FFMAX(stroke_x, stroke_y)),
                                    &inner_pos);
    if (!outer)
        return tail;
    uint8_t *mask = bs4_copy_bitmap_mask(outer);
    if (mask) {
        if (inner)
            bs4_subtract_inner_mask(mask, outer, outer_pos, inner, inner_pos);
        if (bs4_mask_has_coverage(mask, outer->w, outer->h)) {
            Bitmap ring = {
                .left = outer->left,
                .top = outer->top,
                .w = outer->w,
                .h = outer->h,
                .logical_w = outer->logical_w,
                .logical_h = outer->logical_h,
                .sub_x = outer->sub_x,
                .sub_y = outer->sub_y,
                .stride = outer->w,
                .buffer = mask,
            };
            uint32_t border = style.border;
            ass_apply_fades(&border, state->fade, state->fade_color);
            tail = append_bs4_bitmap(state, &ring, outer_pos, border,
                                     IMAGE_TYPE_OUTLINE, tail, rgba_tail);
        }
        ass_aligned_free_tagged(mask, ASS_ALIGNED_ALLOC_BS4_MASK, outer);
    }
    if (inner) {
        uint32_t fill = style.fill;
        ass_apply_fades(&fill, state->fade, state->fade_color);
        tail = append_bs4_bitmap(state, inner, inner_pos, fill,
                                 IMAGE_TYPE_SHADOW, tail, rgba_tail);
    }
    return tail;
}

/* A native check is two cached, transformed square masks. It has no font,
 * text-stream entry or wrapping advance, and uses the bubble's existing clips
 * and the same ASS/RGBA bitmap output as the rounded UI surfaces. */
static ASS_Image **append_chat_check_stroke(RenderContext *state,
                                            double x1, double y1,
                                            double x2, double y2,
                                            double thickness, uint32_t color,
                                            ASS_Image **tail,
                                            ASS_ImageRGBA ***rgba_tail)
{
    double dx = x2 - x1, dy = y2 - y1;
    double length = hypot(dx, dy);
    if (length <= 0)
        return tail;
    double nx = -dy * thickness / length;
    double ny = dx * thickness / length;
    double par = state->renderer->par_scale_x;
    double margin = state->renderer->settings.left_margin;
    double matrix[3][3] = {
        {dx * par, nx * par, ((x1 - nx / 2 - margin) * par + margin) * 64},
        {dy, ny, (y1 - ny / 2) * 64},
        {0, 0, 1},
    };
    BS4BoxShape shape = {.source_size = 64};
    ASS_Vector pos;
    Bitmap *bitmap;
    if (!bs4_get_bitmap(state, matrix, &shape, &pos, &bitmap))
        return tail;
    return append_bs4_bitmap(state, bitmap, pos, color, IMAGE_TYPE_CHARACTER,
                             tail, rgba_tail);
}

static ASS_Image **append_chat_receipt(RenderContext *state, int mark,
                                       double right, double bottom,
                                       double padding, double available_width,
                                       uint32_t color, ASS_Image **tail,
                                       ASS_ImageRGBA ***rgba_tail)
{
    double height = FFMIN(padding * 0.65, available_width / 3);
    if (height < 1)
        return tail;
    double width = height * 1.5;
    double spacing = height * 0.65;
    double thickness = FFMAX(0.8, height * 0.18);
    double x = right - width - (mark == 2 ? spacing : 0);
    double y = bottom - height;
    ass_apply_fades(&color, state->fade, state->fade_color);
    for (int i = 0; i < mark; i++) {
        double left = x + i * spacing;
        tail = append_chat_check_stroke(state, left, y + height * 0.55,
            left + width * 0.30, y + height, thickness, color, tail, rgba_tail);
        tail = append_chat_check_stroke(state, left + width * 0.30, y + height,
            left + width, y, thickness, color, tail, rgba_tail);
    }
    return tail;
}

static bool render_chat_scene(RenderContext *state, ASS_Event *event,
                              EventImages *event_images,
                              ASS_ImageRGBA **rgba_out,
                              const ASS_ChatScene *chat, ChatRange *ranges,
                              ChatRange *title, const ChatMetrics *m)
{
    ASS_Renderer *priv = state->renderer;
    TextInfo *info = &state->text_info;
    double unit = FFMAX(8.0, m->bubble_pad * 3.0);
    chat_measure_range(info, title, unit);
    double bubble_cap = (m->panel_width - 2 * m->panel_pad) * 0.75;
    double total = 0.0, tallest = 0.0;
    for (int i = 0; i < chat->count; i++) {
        chat_measure_range(info, &ranges[i], unit);
        ranges[i].stack_top = total;
        total += ranges[i].height + 2 * m->bubble_pad;
        if (i + 1 < chat->count)
            total += m->message_gap;
        tallest = FFMAX(tallest, ranges[i].height + 2 * m->bubble_pad);
    }
    int initial = chat->has_time ? chat->start_count : chat->count;
    double initial_height = initial ? ranges[initial - 1].stack_top +
        ranges[initial - 1].height + 2 * m->bubble_pad : 0.0;
    double viewport_height = FFMIN(m->viewport_max,
        FFMAX(m->viewport_min, FFMAX(initial_height, tallest)));
    double header_height = title->end > title->start ?
        title->height + 2 * m->header_pad : 0.0;
    /* Panel/header geometry is fixed for the entire event. Only bubbles
     * move in the clipped viewport below the header. */
    double panel_height = 2 * m->panel_pad + header_height + viewport_height;

    int margin_l = event->MarginL ? event->MarginL : state->style->MarginL;
    int margin_r = event->MarginR ? event->MarginR : state->style->MarginR;
    int margin_v = event->MarginV ? event->MarginV : state->style->MarginV;
    double anchor_x, anchor_y;
    if (state->evt_type & EVENT_POSITIONED) {
        anchor_x = x2scr_pos(priv, state->pos_x);
        anchor_y = y2scr_pos(priv, state->pos_y);
    } else {
        int halign = state->alignment & 3;
        if (halign == HALIGN_LEFT)
            anchor_x = x2scr_left(state, margin_l);
        else if (halign == HALIGN_RIGHT)
            anchor_x = x2scr_right(state, priv->track->PlayResX - margin_r);
        else
            anchor_x = (x2scr_left(state, margin_l) +
                x2scr_right(state, priv->track->PlayResX - margin_r)) / 2.0;
        int valign = state->alignment & 12;
        if (valign == VALIGN_TOP)
            anchor_y = y2scr_top(state, margin_v);
        else if (valign == VALIGN_CENTER)
            anchor_y = y2scr(state, priv->track->PlayResY / 2.0);
        else {
            double line_pos = state->explicit ? 0 : priv->settings.line_position;
            double bottom = y2scr_sub(state, priv->track->PlayResY - margin_v);
            anchor_y = bottom + (y2scr_top(state, 0) - bottom) *
                line_pos / 100.0;
        }
    }
    int halign = state->alignment & 3;
    int valign = state->alignment & 12;
    double panel_x = anchor_x - (halign == HALIGN_RIGHT ? m->panel_width :
                                halign == HALIGN_CENTER ? m->panel_width / 2 : 0);
    double panel_y = anchor_y - (valign == VALIGN_SUB ? panel_height :
                                valign == VALIGN_CENTER ? panel_height / 2 : 0);
    double viewport_top = panel_y + m->panel_pad + header_height;
    double viewport_bottom = viewport_top + viewport_height;

    int previous;
    double progress;
    int visible = ass_chat_visible(chat, priv->time - event->Start,
                                   &previous, &progress);
    double visible_height = visible ? ranges[visible - 1].stack_top +
        ranges[visible - 1].height + 2 * m->bubble_pad : 0.0;
    double previous_height = previous ? ranges[previous - 1].stack_top +
        ranges[previous - 1].height + 2 * m->bubble_pad : 0.0;
    if ((size_t) chat->count + 1 > SIZE_MAX / sizeof(ASS_ChatClip))
        return false;
    ASS_ChatClip *clips = calloc((size_t) chat->count + 1, sizeof(*clips));
    if (!clips)
        return false;

    int first_body = chat->count && ranges[0].name_end < ranges[0].end ?
        ranges[0].name_end : chat->count ? ranges[0].start : 0;
    uint32_t panel_color = chat->count && ranges[0].start < ranges[0].end ?
        info->glyphs[first_body].c[3] : state->c[3];
    unsigned luminance = (((panel_color >> 24) & 255) * 299 +
        ((panel_color >> 16) & 255) * 587 +
        ((panel_color >> 8) & 255) * 114) / 1000;
    uint32_t header_color = luminance < 128 ? 0xFFFFFF00u : 0x00000000u;
    uint32_t title_color = luminance < 128 ? 0x00000000u : 0xFFFFFF00u;
    /* Header colors are scene-wide and use the final sequential tag state. */
    if (state->chat_title.has_background)
        header_color = state->chat_title.background;
    if (state->chat_title.has_text)
        title_color = state->chat_title.text;
    for (int i = title->start; i < title->end; i++)
        for (GlyphInfo *glyph = &info->glyphs[i]; glyph; glyph = glyph->next) {
            glyph->c[0] = title_color;
            glyph->gradient = (GradientState) {0};
            ass_mangetsu_gradient_state_reset(&glyph->mangetsu_gradient);
            glyph->pattern = (TextPatternPaint) {0};
            glyph->image_fill = (ImageFillState) {0};
            glyph->blend_mode = ASS_BLEND_NORMAL;
        }
    for (int i = 0; i < chat->count; i++) {
        for (int j = ranges[i].start; j < ranges[i].name_end; j++)
            for (GlyphInfo *glyph = &info->glyphs[j]; glyph;
                 glyph = glyph->next)
                glyph->c[0] = glyph->c[1];
        chat_set_part(info, &ranges[i], i + 1);
    }

    if (title->end > title->start)
        chat_shift_range(info, title,
            panel_x + (m->panel_width - title->width) / 2.0,
            panel_y + m->panel_pad + m->header_pad);
    for (int i = 0; i < chat->count; i++) {
        double final_top = viewport_bottom - visible_height + ranges[i].stack_top;
        double top;
        if (i < previous)
            top = viewport_bottom - previous_height + ranges[i].stack_top +
                  progress * (previous_height - visible_height);
        else
            top = final_top + (1.0 - progress) *
                  (ranges[i].height + 2 * m->bubble_pad);
        ranges[i].stack_top = top;
        double bubble_width = FFMIN(bubble_cap,
                                    ranges[i].width + 2 * m->bubble_pad);
        double bubble_x = chat->messages[i].side ?
            panel_x + m->panel_width - m->panel_pad - bubble_width :
            panel_x + m->panel_pad;
        double physical_x = (bubble_x - priv->settings.left_margin) *
            priv->par_scale_x + priv->settings.left_margin;
        clips[i + 1].x0 = (int) ceil(physical_x);
        clips[i + 1].x1 = (int) floor(physical_x +
                                     bubble_width * priv->par_scale_x);
        chat_shift_range(info, &ranges[i], bubble_x + m->bubble_pad,
                         top + m->bubble_pad);
        if (i >= visible || top + ranges[i].height +
            2 * m->bubble_pad <= viewport_top || top >= viewport_bottom)
            chat_hide_range(info, &ranges[i]);
    }

    resolve_event_clip(state);
    state->chat_clip_y0 = (int) ceil(viewport_top);
    state->chat_clip_y1 = (int) floor(viewport_bottom);
    state->chat_visible = visible;
    state->chat_clips = clips;
    state->chat_clip_active = false;
    state->detect_collisions = 0;

    position_glyphs_for_render(state, 0.0, 0.0);
    render_and_combine_glyphs(state);
    compute_line_gradient_rects(state);
    compute_mangetsu_gradient_rects(state);
    state->needs_rgba = text_needs_rgba(info);

    memset(event_images, 0, sizeof(*event_images));
    event_images->event = event;
    event_images->left = lround((panel_x - priv->settings.left_margin) *
        priv->par_scale_x + priv->settings.left_margin);
    event_images->top = lround(panel_y);
    event_images->width = lround(m->panel_width * priv->par_scale_x);
    event_images->height = lround(panel_height);
    event_images->detect_collisions = 0;
    event_images->needs_rgba = state->needs_rgba;

    ASS_Image *box_head = NULL;
    ASS_Image **box_tail = &box_head;
    ASS_ImageRGBA *box_rgba_head = NULL;
    ASS_ImageRGBA **box_rgba_tail = &box_rgba_head;
    ASS_ImageRGBA ***rgba_tail = rgba_out ? &box_rgba_tail : NULL;
    box_tail = append_chat_box(state, panel_x, panel_y, m->panel_width,
                               panel_height, m->radius * 1.6, panel_color,
                               box_tail, rgba_tail);
    if (header_height > 0)
        box_tail = append_chat_box(state, panel_x + m->panel_pad,
            panel_y + m->panel_pad, m->panel_width - 2 * m->panel_pad,
            header_height, m->radius, header_color, box_tail, rgba_tail);
    for (int i = 0; i < visible; i++) {
        double bubble_width = FFMIN(bubble_cap,
                                    ranges[i].width + 2 * m->bubble_pad);
        double bubble_height = ranges[i].height + 2 * m->bubble_pad;
        double bubble_x = chat->messages[i].side ?
            panel_x + m->panel_width - m->panel_pad - bubble_width :
            panel_x + m->panel_pad;
        const GlyphInfo *glyph = chat_message_style_glyph(info, &ranges[i]);
        ChatBubbleStyle style = glyph ? glyph->chat_bubble :
            ranges[i].has_preset ? ranges[i].empty_bubble : state->chat_bubble;
        double stroke_x, stroke_y;
        chat_stroke_extent(state, style.border_size, &stroke_x, &stroke_y);
        double stroke_limit = FFMAX(0.0, FFMIN(m->bubble_pad - m->bubble_gap,
            FFMIN(bubble_width, bubble_height) / 4.0));
        stroke_x = FFMIN(stroke_x, stroke_limit);
        stroke_y = FFMIN(stroke_y, stroke_limit);
        state->chat_clip_active = true;
        /* Leave one raster pixel for the rounded edge's antialiasing. */
        state->chat_clip_x0 = clips[i + 1].x0 - 1;
        state->chat_clip_x1 = clips[i + 1].x1 + 1;
        box_tail = append_chat_bubble(state, bubble_x, ranges[i].stack_top,
            bubble_width, bubble_height, m->radius, stroke_x, stroke_y,
            style, box_tail, rgba_tail);
        int mark = ranges[i].receipt.enabled && ass_chat_is_outgoing(chat, i) ?
            ass_chat_receipt(chat, i, ranges[i].receipt,
                             priv->time - event->Start) : 0;
        if (mark) {
            double inset = FFMAX(1.0, m->bubble_gap * 0.4);
            double padding = FFMAX(0.0, m->bubble_pad - stroke_y - inset);
            box_tail = append_chat_receipt(state, mark,
                bubble_x + bubble_width - stroke_x - inset,
                ranges[i].stack_top + bubble_height - stroke_y - inset,
                padding, FFMAX(0.0, bubble_width - 2 * (stroke_x + inset)),
                glyph ? glyph->c[0] : ranges[i].receipt_color, box_tail, rgba_tail);
        }
    }
    state->chat_clip_active = false;
    *box_tail = NULL;
    *box_rgba_tail = NULL;
    if (box_head)
        blend_vector_clip(state, box_head);
    if (box_rgba_head)
        blend_vector_clip_rgba(state, box_rgba_head);

    ASS_ImageRGBA *text_rgba = NULL;
    ASS_Image *text = render_text(state, rgba_out ? &text_rgba : NULL);
    *box_tail = text;
    event_images->imgs = box_head ? box_head : text;
    if (box_rgba_head) {
        *box_rgba_tail = text_rgba;
        event_images->imgs_rgba = box_rgba_head;
    } else
        event_images->imgs_rgba = text_rgba;
    if (rgba_out)
        *rgba_out = event_images->imgs_rgba;
    state->chat_clips = NULL;
    free(clips);
    return true;
}

/* Syntax depends on source text alone. Keep a small per-renderer cache so a
 * persistent chat event is tokenized once; shaping still uses normal caches. */
static ASS_ChatScene *get_chat_scene(ASS_Renderer *priv, const char *source,
                                     bool *cached)
{
    *cached = false;
    if (!strstr(source, "\\chatmode"))
        return NULL;
    for (int i = 0; i < 8; i++) {
        if (priv->chat_cache[i].source &&
            !strcmp(priv->chat_cache[i].source, source)) {
            *cached = true;
            return priv->chat_cache[i].scene;
        }
    }
    ASS_ChatScene *scene = ass_chat_parse(source);
    if (!scene)
        return NULL;
    size_t length = strlen(source);
    if (length > 65536 || scene->count > 256)
        return scene;
    char *copy = malloc(length + 1);
    if (!copy)
        return scene;
    memcpy(copy, source, length + 1);
    unsigned slot = priv->chat_cache_next++ % 8;
    free(priv->chat_cache[slot].source);
    ass_chat_free(priv->chat_cache[slot].scene);
    priv->chat_cache[slot].source = copy;
    priv->chat_cache[slot].scene = scene;
    *cached = true;
    return scene;
}

static void release_chat_scene(ASS_ChatScene *scene, bool cached)
{
    if (!cached)
        ass_chat_free(scene);
}

#include "ass_event_reuse.h"

/**
 * \brief Main ass rendering function, glues everything together
 * \param event event to render
 * \param event_images struct containing resulting images, will also be initialized
 * Process event, appending resulting ASS_Image's to images_root.
 */
bool
ass_render_event(RenderContext *state, ASS_Event *event,
                 EventImages *event_images, ASS_ImageRGBA **rgba_out)
{
    ASS_Renderer *render_priv = state->renderer;
    if (event->Style >= render_priv->track->n_styles) {
        ass_msg(render_priv->library, MSGL_WARN, "No style found");
        return false;
    }
    if (!event->Text) {
        ass_msg(render_priv->library, MSGL_WARN, "Empty event");
        return false;
    }

    bool chat_cached = false;
    ASS_ChatScene *chat = get_chat_scene(render_priv, event->Text,
                                         &chat_cached);
    free_render_context(state);
    init_render_context(state, event, chat != NULL);
    ChatRange *ranges = chat && chat->count ?
        calloc(chat->count, sizeof(*ranges)) : NULL;
    ChatRange title = {0};
    if (chat && chat->count && !ranges) {
        release_chat_scene(chat, chat_cached);
        free_render_context(state);
        return false;
    }
    if (!parse_events(state, event, chat, ranges, &title)) {
        release_chat_scene(chat, chat_cached);
        free(ranges);
        return false;
    }

    TextInfo *text_info = &state->text_info;
    BS4BoxGeometry bs4_box_geometry = {0};
    if (text_info->length == 0) {
        // no valid symbols in the event; this can be smth like {comment}
        free_render_context(state);
        release_chat_scene(chat, chat_cached);
        free(ranges);
        return false;
    }

    bool automatic_position =
        (state->motion.relative_x && !state->pos_override_x) ||
        (state->motion.relative_y && !state->pos_override_y);
    if (state->motion.type == MOTION_NONE)
        for (int i = 0; i < state->n_pos_transforms; i++)
            automatic_position |= state->pos_transforms[i].relative_x ||
                                  state->pos_transforms[i].relative_y;
    if (!automatic_position &&
            (state->motion.type != MOTION_NONE || state->n_pos_transforms)) {
        ASS_DVector pos = evaluate_animated_position(state);
        state->pos_x = pos.x;
        state->pos_y = pos.y;
        if (state->n_pos_transforms) {
            state->evt_type |= EVENT_POSITIONED;
            state->detect_collisions = 0;
        }
    }

    ass_vertical_prepare(state);
    split_style_runs(state);
    ass_vertical_mark_syllables(state);

    if (repeated_geometry_lookup(state)) {
        resolve_event_clip(state);
        repeated_geometry_render(state, event_images, rgba_out);
        free_render_context(state);
        release_chat_scene(chat, chat_cached);
        free(ranges);
        return true;
    }

    // Find shape runs and shape text
    ass_shaper_set_base_direction(state->shaper,
            ass_resolve_base_direction(state->font_encoding));
    ass_shaper_find_runs(state->shaper, render_priv, text_info->glyphs,
            text_info->length);
    render_priv->repeated_event_stats.shapes++;
    if (!ass_shaper_shape(state->shaper, text_info)) {
        ass_msg(render_priv->library, MSGL_ERR, "Failed to shape text");
        ass_shaper_cleanup(state->shaper, text_info);
        free_render_context(state);
        release_chat_scene(chat, chat_cached);
        free(ranges);
        return false;
    }

    retrieve_glyphs(state);

    if (!prepare_furi_groups(state)) {
        ass_msg(render_priv->library, MSGL_ERR, "Failed to shape furi text");
        ass_shaper_cleanup(state->shaper, text_info);
        free_render_context(state);
        release_chat_scene(chat, chat_cached);
        free(ranges);
        return false;
    }

    apply_cycle_paint(state);

    preliminary_layout(state);

    ChatMetrics chat_metrics = {0};
    if (chat)
        chat_metrics = chat_choose_metrics(state, chat, ranges, &title);

    int valign = state->alignment & 12;

    int MarginL =
        (event->MarginL) ? event->MarginL : state->style->MarginL;
    int MarginR =
        (event->MarginR) ? event->MarginR : state->style->MarginR;
    int MarginV =
        (event->MarginV) ? event->MarginV : state->style->MarginV;

    // calculate max length of a line
    double max_text_width =
        x2scr_right(state, render_priv->track->PlayResX - MarginR) -
        x2scr_left(state, MarginL);
    if (chat)
        max_text_width = chat_metrics.max_text_width;

    if (state->native_vertical) {
        double max_height = y2scr_sub(state,
            render_priv->track->PlayResY - MarginV) -
            y2scr_top(state, MarginV);
        if (!ass_vertical_layout(state, max_height)) {
            ass_shaper_cleanup(state->shaper, text_info);
            free_render_context(state);
            release_chat_scene(chat, chat_cached);
            free(ranges);
            return false;
        }
        /* Later effect passes use the shaper's logical-to-visual map even
         * though native vertical placement has already positioned units. */
        if (!ass_shaper_reorder(state->shaper, text_info)) {
            ass_shaper_cleanup(state->shaper, text_info);
            free_render_context(state);
            release_chat_scene(chat, chat_cached);
            free(ranges);
            return false;
        }
        ass_process_karaoke_effects(state);
    } else {
        // Horizontal wrapping and placement retain their existing path.
        wrap_lines_smart(state, max_text_width);
        ass_process_karaoke_effects(state);
        reorder_text(state);
        if (text_info->n_furi_groups)
            resolve_furi_group_collisions(state);
        align_lines(state, max_text_width);
        apply_column_layout(state);
    }

    if (text_info->n_furi_groups)
        position_furi_groups(state);

    if (!expand_furi_line_metrics(state)) {
        ass_msg(render_priv->library, MSGL_ERR, "Failed to expand furi line metrics");
        ass_shaper_cleanup(state->shaper, text_info);
        free_render_context(state);
        release_chat_scene(chat, chat_cached);
        free(ranges);
        return false;
    }

    if (chat) {
        bool ok = render_chat_scene(state, event, event_images, rgba_out,
                                    chat, ranges, &title, &chat_metrics);
        ass_shaper_cleanup(state->shaper, text_info);
        free_render_context(state);
        release_chat_scene(chat, chat_cached);
        free(ranges);
        return ok;
    }

    /* Keep the box's source rectangle in untransformed event-local layout. */
    ASS_DRect bs4_layout_bbox;
    compute_string_bbox(text_info, &bs4_layout_bbox);
    add_furi_to_bbox(text_info, &bs4_layout_bbox);

    bool distorted_text = apply_distortion(state);

    // determine text bounding box
    ASS_DRect bbox;
    compute_string_bbox(text_info, &bbox);
    ASS_DRect bbox_origin = bbox;
    double origin_x = 0.0;
    double origin_y = 0.0;
    bool rotate_baseline = false;
    for (int i = 0; i < text_info->length; i++) {
        if (text_info->glyphs[i].frs != 0.0) {
            rotate_baseline = true;
            break;
        }
    }
    if (rotate_baseline)
        get_base_point(&bbox_origin, state->alignment,
                       &origin_x, &origin_y);

    apply_baseline_shear(state);

    ASS_DVector curved_attachment = {0};
    ASS_DVector curved_tangent = {0};
    ASS_DRect curved_bbox;
    bool curved_text = apply_curved_text(state, &curved_attachment,
                                         &curved_tangent);
    if (curved_text) {
        /* The path and completed text remain local to the event anchor. */
        if (rotate_baseline)
            origin_x = origin_y = 0.0;
        compute_curved_string_bbox(text_info, &bbox, &curved_bbox,
                                   curved_tangent);
        bbox_origin = bbox;
    }

    if (rotate_baseline) {
        apply_baseline_rotation(state, origin_x, origin_y);
        if (curved_text)
            compute_curved_string_bbox(text_info, &bbox, &curved_bbox,
                                       curved_tangent);
        else
            compute_string_bbox(text_info, &bbox);
    }
    if (text_info->n_furi_groups)
        position_furi_groups(state);

    ASS_DRect render_bbox = bbox;
    add_furi_to_bbox(text_info, &render_bbox);
    ASS_DRect warp_text_bbox;
    bool warp_text_anchor = distorted_text && !curved_text &&
        compute_warp_text_bbox(text_info, &warp_text_bbox);
    int warp_alignment = state->warp_text_alignment ?
        numpad2align(state->warp_text_alignment) : state->alignment;
    bool reserve_furi = furi_reserves_vertical_space(text_info);
    // Ruby always contributes to horizontal alignment and occupied width.
    // furichangepos controls only vertical line/block reservation.
    ASS_DRect placement_bounds = render_bbox;
    if (!reserve_furi) {
        placement_bounds.y_min = bbox.y_min;
        placement_bounds.y_max = bbox.y_max;
    }
    ASS_DRect *placement_bbox = &placement_bounds;
    ASS_DRect *bbox_for_origin = rotate_baseline ? &bbox_origin : placement_bbox;
    ASS_DRect *bbox_for_position = placement_bbox;
    ASS_DVector object_anchor = {0};

    // determine device coordinates for text
    double device_x = 0;
    double device_y = 0;

    if (curved_text) {
        if (state->evt_type & EVENT_POSITIONED) {
            object_anchor.x = x2scr_pos(render_priv, state->pos_x);
            object_anchor.y = y2scr_pos(render_priv, state->pos_y);
        } else {
            int halign = state->alignment & 3;
            if (halign == HALIGN_LEFT)
                object_anchor.x = x2scr_left(state, MarginL);
            else if (halign == HALIGN_RIGHT)
                object_anchor.x = x2scr_right(state,
                    render_priv->track->PlayResX - MarginR);
            else
                object_anchor.x = (x2scr_left(state, MarginL) +
                    x2scr_right(state,
                        render_priv->track->PlayResX - MarginR)) * 0.5;

            if (valign == VALIGN_TOP) {
                object_anchor.y = y2scr_top(state, MarginV);
            } else if (valign == VALIGN_CENTER) {
                object_anchor.y = y2scr(state,
                    render_priv->track->PlayResY / 2.0);
            } else {
                double line_pos = state->explicit ?
                    0 : render_priv->settings.line_position;
                double scr_bottom = y2scr_sub(state,
                    render_priv->track->PlayResY - MarginV);
                double scr_top = y2scr_top(state, 0);
                object_anchor.y = scr_bottom +
                    (scr_top - scr_bottom) * line_pos / 100.0;
            }
        }
    } else if (state->evt_type & EVENT_POSITIONED) {
        // A non-curved event retains the ordinary bbox-relative placement.
        double base_x = 0;
        double base_y = 0;
        get_base_point(bbox_for_position, state->alignment, &base_x, &base_y);
        device_x =
            x2scr_pos(render_priv, state->pos_x) - base_x;
        device_y =
            y2scr_pos(render_priv, state->pos_y) - base_y;
    }

    // x coordinate
    if (!curved_text && (state->evt_type & EVENT_HSCROLL)) {
        if (state->scroll_direction == SCROLL_RL)
            device_x =
                x2scr_pos(render_priv,
                      render_priv->track->PlayResX -
                      state->scroll_shift);
        else if (state->scroll_direction == SCROLL_LR)
            device_x =
                x2scr_pos(render_priv, state->scroll_shift) -
                (bbox_for_position->x_max - bbox_for_position->x_min);
    } else if (!curved_text && !(state->evt_type & EVENT_POSITIONED)) {
        device_x = x2scr_left(state, MarginL);
    }

    // y coordinate
    if (!curved_text && (state->evt_type & EVENT_VSCROLL)) {
        if (state->scroll_direction == SCROLL_TB)
            device_y =
                y2scr(state,
                      state->scroll_y0 +
                      state->scroll_shift) -
                bbox_for_position->y_max;
        else if (state->scroll_direction == SCROLL_BT)
            device_y =
                y2scr(state,
                      state->scroll_y1 -
                      state->scroll_shift) -
                bbox_for_position->y_min;
    } else if (!curved_text && !(state->evt_type & EVENT_POSITIONED)) {
        if (valign == VALIGN_TOP) {     // toptitle
            device_y =
                y2scr_top(state,
                          MarginV) + text_info->lines[0].asc;
        } else if (valign == VALIGN_CENTER) {   // midtitle
            double scr_y =
                y2scr(state, render_priv->track->PlayResY / 2.0);
            device_y = scr_y -
                (bbox_for_position->y_max + bbox_for_position->y_min) / 2.0;
        } else {                // subtitle
            double line_pos = state->explicit ?
                0 : render_priv->settings.line_position;
            double scr_top, scr_bottom, scr_y0;
            if (valign != VALIGN_SUB)
                ass_msg(render_priv->library, MSGL_V,
                       "Invalid valign, assuming 0 (subtitle)");
            scr_bottom =
                y2scr_sub(state,
                          render_priv->track->PlayResY - MarginV);
            scr_top = y2scr_top(state, 0); //xxx not always 0?
            device_y = scr_bottom + (scr_top - scr_bottom) * line_pos / 100.0;
            device_y -= text_info->height;
            device_y += text_info->lines[0].asc;
            // clip to top to avoid confusion if line_position is very high,
            // turning the subtitle into a toptitle
            // also, don't change behavior if line_position is not used
            scr_y0 = scr_top + text_info->lines[0].asc;
            if (device_y < scr_y0 && line_pos > 0) {
                device_y = scr_y0;
            }
        }
    }

    if (state->native_vertical && !curved_text) {
        /* Native columns have a completed block bbox.  Use its ASS base
         * point for ordinary margin placement as well as explicit \pos. */
        double base_x = 0.0, base_y = 0.0;
        get_base_point(bbox_for_position, state->alignment,
                       &base_x, &base_y);
        if (!(state->evt_type & (EVENT_POSITIONED | EVENT_HSCROLL))) {
            int halign = state->alignment & 3;
            double left = x2scr_left(state, MarginL);
            double right = x2scr_right(state,
                render_priv->track->PlayResX - MarginR);
            double anchor_x = halign == HALIGN_LEFT ? left :
                halign == HALIGN_RIGHT ? right : (left + right) * 0.5;
            device_x = anchor_x - base_x;
        }
        if (!(state->evt_type & (EVENT_POSITIONED | EVENT_VSCROLL))) {
            double anchor_y;
            if (valign == VALIGN_TOP)
                anchor_y = y2scr_top(state, MarginV);
            else if (valign == VALIGN_CENTER)
                anchor_y = y2scr(state,
                    render_priv->track->PlayResY / 2.0);
            else {
                double line_pos = state->explicit ?
                    0 : render_priv->settings.line_position;
                double bottom = y2scr_sub(state,
                    render_priv->track->PlayResY - MarginV);
                double top = y2scr_top(state, 0);
                anchor_y = bottom + (top - bottom) * line_pos / 100.0;
            }
            device_y = anchor_y - base_y;
        }
    }

    double object_base_x = 0.0;
    double object_base_y = 0.0;
    double text_base_x = 0.0;
    double text_base_y = 0.0;
    if (curved_text) {
        /* Measure the final curved block in the tangent/normal frame at the
         * selected path point. No visual line has anchoring authority here. */
        int curved_alignment = effective_curved_alignment(state);
        double along = 0.0, normal = 0.0;
        get_base_point(&curved_bbox, curved_alignment, &along, &normal);
        text_base_x = along * curved_tangent.x - normal * curved_tangent.y -
                      curved_attachment.x;
        text_base_y = along * curved_tangent.y + normal * curved_tangent.x -
                      curved_attachment.y;
    } else {
        get_base_point(bbox_for_position, state->alignment,
                       &object_base_x, &object_base_y);
        get_base_point(warp_text_anchor ? &warp_text_bbox : bbox_for_position,
                       warp_text_anchor ? warp_alignment :
                           state->native_vertical ? state->alignment :
                           state->text_alignment,
                       &text_base_x, &text_base_y);
        object_anchor.x = device_x + object_base_x;
        object_anchor.y = device_y + object_base_y;
    }
    if (automatic_position) {
        // Layout has now resolved Style margins, alignment and glyph metrics.
        // Convert that anchor back to script units before applying operands.
        double x0 = x2scr_pos(render_priv, 0);
        double y0 = y2scr_pos(render_priv, 0);
        double sx = x2scr_pos(render_priv, 1) - x0;
        double sy = y2scr_pos(render_priv, 1) - y0;
        state->pos_x = sx != 0 ? (object_anchor.x - x0) / sx : 0;
        state->pos_y = sy != 0 ? (object_anchor.y - y0) / sy : 0;
        ASS_DVector pos = evaluate_animated_position(state);
        state->pos_x = pos.x;
        state->pos_y = pos.y;
        object_anchor.x = x2scr_pos(render_priv, pos.x);
        object_anchor.y = y2scr_pos(render_priv, pos.y);
        state->evt_type |= EVENT_POSITIONED;
        state->detect_collisions = 0;
    }
    device_x = object_anchor.x - text_base_x;
    device_y = object_anchor.y - text_base_y;

    update_glyph_jitter_offsets(state);

    resolve_event_clip(state);

    if (state->evt_type & EVENT_VSCROLL) {
        int y0 = lround(y2scr_pos(render_priv, state->scroll_y0));
        int y1 = lround(y2scr_pos(render_priv, state->scroll_y1));

        state->clip_y0 = FFMAX(state->clip_y0, y0);
        state->clip_y1 = FFMIN(state->clip_y1, y1);
    }

    if (!prepare_scroll_layout(state, device_y)) {
        ass_shaper_cleanup(state->shaper, text_info);
        free_render_context(state);
        release_chat_scene(chat, chat_cached);
        free(ranges);
        return false;
    }

    calculate_rotation_params(state, bbox_for_origin, device_x, device_y,
                              &object_anchor);

    int n_scroll_boxes = 0;
    BS4BoxGeometry *scroll_boxes = NULL;
    if (state->bs4_box_mode)
        capture_bs4_box_geometry(state, &bs4_box_geometry, &bs4_layout_bbox,
                                 device_x, device_y);
    if (state->bs4_box_mode && state->n_scroll_contexts) {
        scroll_boxes = capture_scroll_boxes(state, device_x, device_y, &n_scroll_boxes);
        if (!scroll_boxes) {
            ass_shaper_cleanup(state->shaper, text_info);
            free_render_context(state);
            release_chat_scene(chat, chat_cached);
            free(ranges);
            return false;
        }
    }

    position_glyphs_for_render(state, device_x, device_y);
    /* Rotation origins and perspective are resolved from normal layout first.
     * Translating the glyph position now moves the complete raster result
     * vertically without rotating the scroll vector or changing \org. */
    displace_scroll_glyphs(state);

    render_and_combine_glyphs(state);
    compute_line_gradient_rects(state);
    compute_mangetsu_gradient_rects(state);
    collect_mangetsu_gradient_debug(state);
    state->needs_rgba = text_needs_rgba(text_info);

    memset(event_images, 0, sizeof(*event_images));
    // VSFilter does *not* shift lines with a border > margin to be within the
    // frame, so negative values for top and left may occur
    if (curved_text || reserve_furi) {
        event_images->top = device_y + render_bbox.y_min - text_info->border_top;
        event_images->height =
            render_bbox.y_max - render_bbox.y_min +
            text_info->border_bottom + text_info->border_top + 0.5;
    } else {
        event_images->top = device_y - text_info->lines[0].asc - text_info->border_top;
        event_images->height =
            text_info->height + text_info->border_bottom + text_info->border_top;
    }
    event_images->left =
        (device_x + placement_bbox->x_min) * render_priv->par_scale_x - text_info->border_x + 0.5;
    event_images->width =
        (placement_bbox->x_max - placement_bbox->x_min) * render_priv->par_scale_x
        + 2 * text_info->border_x + 0.5;
    event_images->detect_collisions = state->detect_collisions;
    event_images->shift_direction = (valign == VALIGN_SUB) ? -1 : 1;
    event_images->event = event;
    bool want_rgba = rgba_out != NULL;
    event_images->needs_rgba = state->needs_rgba;
    event_images->imgs_rgba = NULL;
    repeated_geometry_store(state, event_images);
    ASS_ImageRGBA **rgba_ptr = want_rgba ? &event_images->imgs_rgba : NULL;
    event_images->imgs = render_text(state, rgba_ptr);

    if (state->bs4_box_mode && !scroll_boxes)
        add_background(state, event_images,
                       rgba_out ? &event_images->imgs_rgba : NULL,
                       &bs4_box_geometry);
    for (int i = n_scroll_boxes - 1; i >= 0; i--) {
        select_scroll_clip(state, scroll_boxes[i].geometry.scroll_id);
        add_background(state, event_images,
                       rgba_out ? &event_images->imgs_rgba : NULL, &scroll_boxes[i]);
    }
    state->scroll_clip_active = false;
    free(scroll_boxes);

    if (rgba_out)
        *rgba_out = event_images->imgs_rgba;

    ass_shaper_cleanup(state->shaper, text_info);
    free_render_context(state);

    release_chat_scene(chat, chat_cached);
    free(ranges);

    return true;
}

/**
 * \brief Check cache limits and reset cache if they are exceeded
 */
static void check_cache_limits(ASS_Renderer *priv, CacheStore *cache)
{
    ass_cache_cut(cache->composite_cache, cache->composite_max_size);
    ass_cache_cut(cache->bitmap_cache, cache->bitmap_max_size);
    ass_cache_cut(cache->outline_cache, cache->glyph_max);
}

static void setup_shaper(ASS_Shaper *shaper, ASS_Renderer *render_priv)
{
    ASS_Track *track = render_priv->track;

    ass_shaper_set_kerning(shaper, track->Kerning);
    ass_shaper_set_language(shaper, track->Language);
    ass_shaper_set_level(shaper, render_priv->settings.shaper);
#ifdef USE_FRIBIDI_EX_API
    ass_shaper_set_bidi_brackets(shaper,
            track->parser_priv->feature_flags & FEATURE_MASK(ASS_FEATURE_BIDI_BRACKETS));
#endif
    ass_shaper_set_whole_text_layout(shaper,
            track->parser_priv->feature_flags & FEATURE_MASK(ASS_FEATURE_WHOLE_TEXT_LAYOUT));
}

/**
 * \brief Start a new frame
 */
bool
ass_start_frame(ASS_Renderer *render_priv, ASS_Track *track,
                long long now)
{
    if (!render_priv->settings.frame_width
        && !render_priv->settings.frame_height)
        return false;               // library not initialized

    if (!render_priv->fontselect)
        return false;

    if (render_priv->library != track->library)
        return false;

    render_priv->track = track;
    ass_clear_repeated_geometry(render_priv);
    memset(&render_priv->repeated_event_stats, 0,
           sizeof(render_priv->repeated_event_stats));
    render_priv->time = now;
    render_priv->frame_needs_rgba = false;
    render_priv->rgba_output_limit_hit = false;
    render_priv->rgba_output_size = 0;
    render_priv->mangetsu_gradient_debug = (MangetsuGradientDebugState) {0};

    ass_lazy_track_init(render_priv->library, render_priv->track);

    if (render_priv->library->num_fontdata != render_priv->num_emfonts) {
        assert(render_priv->library->num_fontdata > render_priv->num_emfonts);
        render_priv->num_emfonts = ass_update_embedded_fonts(
            render_priv->fontselect, render_priv->num_emfonts);
    }

    setup_shaper(render_priv->state.shaper, render_priv);
    setup_shaper(render_priv->state.furi_shaper, render_priv);

    // PAR correction
    double par = render_priv->settings.par;
    bool lr_track = track->LayoutResX > 0 && track->LayoutResY > 0;
    if (par == 0. || lr_track) {
        if (render_priv->frame_content_width && render_priv->frame_content_height && (lr_track ||
                (render_priv->settings.storage_width && render_priv->settings.storage_height))) {
            double dar = ((double) render_priv->frame_content_width) /
                         render_priv->frame_content_height;
            ASS_Vector layout_res = ass_layout_res(render_priv);
            double sar = ((double) layout_res.x) / layout_res.y;
            par = dar / sar;
        } else
            par = 1.0;
    }
    render_priv->par_scale_x = par;

    render_priv->prev_images_root = render_priv->images_root;
    render_priv->images_root = NULL;

    check_cache_limits(render_priv, &render_priv->cache);

    return true;
}

/* Keep the old array intact if growing it fails. Both legacy and RGBA frame
 * paths use this storage, so allocation failure cannot become an invalid
 * EventImages write through a lost realloc pointer. */
bool ass_ensure_event_images(ASS_Renderer *render_priv, int count)
{
    if (count < render_priv->eimg_size)
        return true;
    if (render_priv->eimg_size > INT_MAX - 100) {
        ass_msg(render_priv->library, MSGL_ERR,
                "Too many active subtitle events");
        return false;
    }

    int new_size = render_priv->eimg_size + 100;
    EventImages *images = ass_realloc_array(render_priv->eimg, new_size,
                                             sizeof(*images));
    if (!images) {
        ass_msg(render_priv->library, MSGL_ERR,
                "Could not allocate active event list");
        return false;
    }
    render_priv->eimg = images;
    render_priv->eimg_size = new_size;
    return true;
}

int ass_cmp_event_layer(const void *p1, const void *p2)
{
    ASS_Event *e1 = ((EventImages *) p1)->event;
    ASS_Event *e2 = ((EventImages *) p2)->event;
    if (e1->Layer < e2->Layer)
        return -1;
    if (e1->Layer > e2->Layer)
        return 1;
    if (e1->ReadOrder < e2->ReadOrder)
        return -1;
    if (e1->ReadOrder > e2->ReadOrder)
        return 1;
    return 0;
}

static ASS_RenderPriv *get_render_priv(ASS_Renderer *render_priv,
                                       ASS_Event *event)
{
    if (!event->render_priv) {
        event->render_priv = calloc(1, sizeof(ASS_RenderPriv));
        if (!event->render_priv)
            return NULL;
    }
    if (render_priv->render_id != event->render_priv->render_id) {
        memset(event->render_priv, 0, sizeof(ASS_RenderPriv));
        event->render_priv->render_id = render_priv->render_id;
    }

    return event->render_priv;
}

static int overlap(Rect *s1, Rect *s2)
{
    if (s1->y0 >= s2->y1 || s2->y0 >= s1->y1 ||
        s1->x0 >= s2->x1 || s2->x0 >= s1->x1)
        return 0;
    return 1;
}

static int cmp_rect_y0(const void *p1, const void *p2)
{
    return ((Rect *) p1)->y0 - ((Rect *) p2)->y0;
}

static void
shift_event(ASS_Renderer *render_priv, EventImages *ei, int shift)
{
    ASS_Image *cur = ei->imgs;
    while (cur) {
        cur->dst_y += shift;
        // clip top and bottom
        if (cur->dst_y < 0) {
            int clip = -cur->dst_y;
            cur->h -= clip;
            cur->bitmap += clip * cur->stride;
            cur->dst_y = 0;
        }
        if (cur->dst_y + cur->h >= render_priv->height) {
            int clip = cur->dst_y + cur->h - render_priv->height;
            cur->h -= clip;
        }
        if (cur->h <= 0) {
            cur->h = 0;
            cur->dst_y = 0;
        }
        cur = cur->next;
    }
    ASS_ImageRGBA *rcur = ei->imgs_rgba;
    while (rcur) {
        int64_t shifted_y = (int64_t) rcur->dst_y + shift;
        if (shifted_y < INT_MIN || shifted_y > INT_MAX) {
            rcur->h = 0;
            rcur->dst_y = 0;
            rcur = rcur->next;
            continue;
        }
        rcur->dst_y = (int) shifted_y;
        if (rcur->dst_y < 0) {
            int64_t clip64 = -(int64_t) rcur->dst_y;
            if (clip64 >= rcur->h) {
                rcur->h = 0;
            } else {
                int clip = (int) clip64;
                rcur->h -= clip;
                rcur->rgba += (size_t) clip * rcur->stride;
            }
            rcur->dst_y = 0;
        }
        if (rcur->dst_y >= render_priv->height)
            rcur->h = 0;
        else if ((int64_t) rcur->dst_y + rcur->h > render_priv->height)
            rcur->h = render_priv->height - rcur->dst_y;
        if (rcur->h <= 0) {
            rcur->h = 0;
            rcur->dst_y = 0;
        }
        rcur = rcur->next;
    }
    ei->top += shift;
}

// dir: 1 - move down
//      -1 - move up
static int fit_rect(Rect *s, Rect *fixed, int *cnt, int dir)
{
    int i;
    int shift = 0;

    if (dir == 1)               // move down
        for (i = 0; i < *cnt; ++i) {
            if (s->y1 + shift <= fixed[i].y0 || s->y0 + shift >= fixed[i].y1 ||
                s->x1 <= fixed[i].x0 || s->x0 >= fixed[i].x1)
                continue;
            shift = fixed[i].y1 - s->y0;
    } else                      // dir == -1, move up
        for (i = *cnt - 1; i >= 0; --i) {
            if (s->y1 + shift <= fixed[i].y0 || s->y0 + shift >= fixed[i].y1 ||
                s->x1 <= fixed[i].x0 || s->x0 >= fixed[i].x1)
                continue;
            shift = fixed[i].y0 - s->y1;
        }

    fixed[*cnt].y0 = s->y0 + shift;
    fixed[*cnt].y1 = s->y1 + shift;
    fixed[*cnt].x0 = s->x0;
    fixed[*cnt].x1 = s->x1;
    (*cnt)++;
    qsort(fixed, *cnt, sizeof(*fixed), cmp_rect_y0);

    return shift;
}

void
ass_fix_collisions(ASS_Renderer *render_priv, EventImages *imgs, int cnt)
{
    Rect *used = ass_realloc_array(NULL, cnt, sizeof(*used));
    int cnt_used = 0;
    int i, j;

    if (!used)
        return;

    // fill used[] with fixed events
    for (i = 0; i < cnt; ++i) {
        ASS_RenderPriv *priv;
        // VSFilter considers events colliding if their intersections area is non-zero,
        // zero-area events are therefore effectively fixed as well
        if (!imgs[i].detect_collisions || !imgs[i].height  || !imgs[i].width)
            continue;
        priv = get_render_priv(render_priv, imgs[i].event);
        if (priv && priv->height > 0) { // it's a fixed event
            Rect s;
            s.y0 = priv->top;
            s.y1 = priv->top + priv->height;
            s.x0 = priv->left;
            s.x1 = priv->left + priv->width;
            if (priv->height != imgs[i].height) {       // no, it's not
                ass_msg(render_priv->library, MSGL_WARN,
                        "Event height has changed");
                priv->top = 0;
                priv->height = 0;
                priv->left = 0;
                priv->width = 0;
            }
            for (j = 0; j < cnt_used; ++j)
                if (overlap(&s, used + j)) {    // no, it's not
                    priv->top = 0;
                    priv->height = 0;
                    priv->left = 0;
                    priv->width = 0;
                }
            if (priv->height > 0) {     // still a fixed event
                used[cnt_used].y0 = priv->top;
                used[cnt_used].y1 = priv->top + priv->height;
                used[cnt_used].x0 = priv->left;
                used[cnt_used].x1 = priv->left + priv->width;
                cnt_used++;
                shift_event(render_priv, imgs + i, priv->top - imgs[i].top);
            }
        }
    }
    qsort(used, cnt_used, sizeof(*used), cmp_rect_y0);

    // try to fit other events in free spaces
    for (i = 0; i < cnt; ++i) {
        ASS_RenderPriv *priv;
        if (!imgs[i].detect_collisions || !imgs[i].height  || !imgs[i].width)
            continue;
        priv = get_render_priv(render_priv, imgs[i].event);
        if (priv && priv->height == 0) {        // not a fixed event
            int shift;
            Rect s;
            s.y0 = imgs[i].top;
            s.y1 = imgs[i].top + imgs[i].height;
            s.x0 = imgs[i].left;
            s.x1 = imgs[i].left + imgs[i].width;
            shift = fit_rect(&s, used, &cnt_used, imgs[i].shift_direction);
            if (shift)
                shift_event(render_priv, imgs + i, shift);
            // make it fixed
            priv->top = imgs[i].top;
            priv->height = imgs[i].height;
            priv->left = imgs[i].left;
            priv->width = imgs[i].width;
        }

    }

    free(used);
}

/**
 * \brief compare two images
 * \param i1 first image
 * \param i2 second image
 * \return 0 if identical, 1 if different positions, 2 if different content
 */
static int ass_image_compare(ASS_Image *i1, ASS_Image *i2)
{
    if (i1->w != i2->w)
        return 2;
    if (i1->h != i2->h)
        return 2;
    if (i1->stride != i2->stride)
        return 2;
    if (i1->color != i2->color)
        return 2;
    if (i1->bitmap != i2->bitmap)
        return 2;
    if (i1->dst_x != i2->dst_x)
        return 1;
    if (i1->dst_y != i2->dst_y)
        return 1;
    return 0;
}

/**
 * \brief compare current and previous image list
 * \param priv library handle
 * \return 0 if identical, 1 if different positions, 2 if different content
 */
int ass_detect_change(ASS_Renderer *priv)
{
    ASS_Image *img, *img2;
    int diff;

    img = priv->prev_images_root;
    img2 = priv->images_root;
    diff = 0;
    while (img && diff < 2) {
        ASS_Image *next, *next2;
        next = img->next;
        if (img2) {
            int d = ass_image_compare(img, img2);
            if (d > diff)
                diff = d;
            next2 = img2->next;
        } else {
            // previous list is shorter
            diff = 2;
            break;
        }
        img = next;
        img2 = next2;
    }

    // is the previous list longer?
    if (img2)
        diff = 2;

    return diff;
}

/**
 * \brief free a single image.
 * \param img image as returned by ass_render_frame().
 *        Only img will be freed, not img->next.
 * \return img->next
 * Should not be called outside of ass_frame_unref
 * once the image list's refcounting is set up.
 */
static ASS_Image *ass_free_image(ASS_Image *img) {
    ASS_Image *next = img->next;

    ASS_ImagePriv *priv = (ASS_ImagePriv *) img;
    ass_cache_dec_ref(priv->source);
    ass_aligned_free_tagged(
        priv->buffer, ASS_ALIGNED_ALLOC_LEGACY_IMAGE, priv);
    free(priv);

    return next;
}

/**
 * \brief render a frame
 * \param priv library handle
 * \param track track
 * \param now current video timestamp (ms)
 * \param detect_change a value describing how the new images differ from the previous ones will be written here:
 *        0 if identical, 1 if different positions, 2 if different content.
 *        Can be NULL, in that case no detection is performed.
 */
ASS_Image *ass_render_frame(ASS_Renderer *priv, ASS_Track *track,
                            long long now, int *detect_change)
{
    // init frame
    if (!ass_start_frame(priv, track, now)) {
        if (detect_change)
            *detect_change = 2;
        return NULL;
    }

    // render events separately
    int cnt = 0;
    for (int i = 0; i < track->n_events; i++) {
        ASS_Event *event = track->events + i;
        if ((event->Start <= now)
            && (now < (event->Start + event->Duration))) {
            if (!ass_ensure_event_images(priv, cnt))
                break;
            memset(priv->eimg + cnt, 0, sizeof(*priv->eimg));
            if (ass_render_event(&priv->state, event, priv->eimg + cnt, NULL)) {
                priv->frame_needs_rgba |= priv->eimg[cnt].needs_rgba;
                cnt++;
            }
        }
    }

    // sort by layer
    if (cnt > 0)
        qsort(priv->eimg, cnt, sizeof(EventImages), ass_cmp_event_layer);

    // call fix_collisions for each group of events with the same layer
    EventImages *last = priv->eimg;
    for (int i = 1; i < cnt; i++)
        if (last->event->Layer != priv->eimg[i].event->Layer) {
            ass_fix_collisions(priv, last, priv->eimg + i - last);
            last = priv->eimg + i;
        }
    if (cnt > 0)
        ass_fix_collisions(priv, last, priv->eimg + cnt - last);

    // concat lists, removing fully transparent bitmaps
    ASS_Image **tail = &priv->images_root;
    for (int i = 0; i < cnt; i++) {
        ASS_Image *cur = priv->eimg[i].imgs;
        while (cur) {
            if (_a(cur->color) == 0xFF) {
                cur = ass_free_image(cur);
                continue;
            }

            *tail = cur;
            tail = &cur->next;
            cur = cur->next;
        }
    }

    // If the last image was skipped in the above loop, *tail may not be NULL and needs to be set to NULL.
    *tail = NULL;

    ass_frame_ref(priv->images_root);

    if (detect_change)
        *detect_change = ass_detect_change(priv);

    // free the previous image list
    ass_frame_unref(priv->prev_images_root);
    priv->prev_images_root = NULL;

    if (track->parser_priv->prune_delay >= 0)
        ass_prune_events(track, now - track->parser_priv->prune_delay);

    return priv->images_root;
}

/**
 * \brief Add reference to a frame image list.
 * \param image_list image list returned by ass_render_frame()
 */
void ass_frame_ref(ASS_Image *img)
{
    if (!img)
        return;
    ((ASS_ImagePriv *) img)->ref_count++;
}

/**
 * \brief Release reference to a frame image list.
 * \param image_list image list returned by ass_render_frame()
 */
void ass_frame_unref(ASS_Image *img)
{
    if (!img || --((ASS_ImagePriv *) img)->ref_count)
        return;
    do {
        img = ass_free_image(img);
    } while (img);
}

void ass_debug_fail_next_owned_image_allocation(ASS_Renderer *priv)
{
    if (priv)
        priv->debug_fail_next_owned_image_allocation = true;
}
