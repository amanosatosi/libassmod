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

#ifdef CONFIG_UNIBREAK
#include <linebreak.h>
#endif

#include "ass.h"
#include "ass_outline.h"
#include "ass_render.h"
#include "ass_parse.h"
#include "ass_priv.h"
#include "ass_distort.h"
#include "ass_shaper.h"

size_t ass_bitmap_construct(void *key, void *value, void *priv);
size_t ass_composite_construct(void *key, void *value, void *priv);
static void apply_rnd_offsets(const BitmapHashKey *k, ASS_Outline *outline,
                              ASS_Library *lib);
static bool build_rnd_bitmaps(RenderContext *state, GlyphInfo *info,
                              OutlineHashValue *outline_src,
                              const double m[3][3],
                              ASS_Vector *pos, ASS_Vector *pos_o,
                              bool need_border, int flags);
static inline bool border_layer_has_size(const BorderLayerState *layer);
static bool border_layers_state_equal(const BorderLayerState *a,
                                      const BorderLayerState *b);
static bool has_multi_border_layers(const BorderLayerState *layers);
static void sync_glyph_layer1_border(GlyphInfo *info);
static double glyph_border_max_x(const GlyphInfo *info);
static double glyph_border_max_y(const GlyphInfo *info);
static Bitmap *combined_border_bitmap(CombinedBitmapInfo *info, int layer);
static Bitmap *composite_border_bitmap(CompositeHashValue *value, int layer);
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

// Temporary scale for debugging / visual calibration of rnd* magnitude.
#ifndef ASS_RND_SCALE
#define ASS_RND_SCALE 0.1
#endif

/* Define ASS_RND_DEBUG to enable verbose rnd* logging for debugging.
 * Disabled by default to avoid noisy builds. */
/* #define ASS_RND_DEBUG */


static void free_glyph_list_chains(GlyphInfo *glyphs, int length)
{
    for (int i = 0; i < length; i++) {
        GlyphInfo *info = glyphs[i].next;
        glyphs[i].next = NULL;
        while (info) {
            GlyphInfo *next = info->next;
            if (info->has_distort_bitmap) {
                ass_free_bitmap(&info->distort_bitmap);
                ass_free_bitmap(&info->distort_bitmap_o);
                for (int i = 0; i < ASS_BORDER_LAYERS_MAX - 1; i++)
                    ass_free_bitmap(&info->distort_bitmap_border[i]);
            }
            if (info->has_distort_outline && info->distorted_outline) {
                ass_outline_free(&info->distorted_outline->outline[0]);
                ass_outline_free(&info->distorted_outline->outline[1]);
                free(info->distorted_outline);
            }
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
            if (info->has_distort_bitmap) {
                ass_free_bitmap(&info->distort_bitmap);
                ass_free_bitmap(&info->distort_bitmap_o);
                for (int k = 0; k < ASS_BORDER_LAYERS_MAX - 1; k++)
                    ass_free_bitmap(&info->distort_bitmap_border[k]);
            }
            if (info->has_distort_outline && info->distorted_outline) {
                ass_outline_free(&info->distorted_outline->outline[0]);
                ass_outline_free(&info->distorted_outline->outline[1]);
                free(info->distorted_outline);
            }
        }
        free(group->glyphs);
        free(group->event_text);
    }
    free(text_info->furi_groups);
    text_info->furi_groups = NULL;
    text_info->n_furi_groups = 0;
    text_info->max_furi_groups = 0;
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
    ass_rasterizer_done(&state->rasterizer);

    if (state->shaper)
        ass_shaper_free(state->shaper);
    if (state->furi_shaper)
        ass_shaper_free(state->furi_shaper);

    text_info_done(&state->text_info);
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

    free(render_priv->settings.default_font);
    free(render_priv->settings.default_family);

    free(render_priv->user_override_style.FontName);

    free(render_priv);
}

/**
 * \brief Create a new ASS_Image
 * Parameters are the same as ASS_Image fields.
 */
static ASS_Image *my_draw_bitmap(unsigned char *bitmap, int bitmap_w,
                                 int bitmap_h, int stride, int dst_x,
                                 int dst_y, uint32_t color,
                                 CompositeHashValue *source)
{
    ASS_ImagePriv *img = malloc(sizeof(ASS_ImagePriv));
    if (!img) {
        if (!source)
            ass_aligned_free(bitmap);
        return NULL;
    }

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

    return &img->result;
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
        ass_rgba_image_alloc(render_priv, w, h, dst_x, dst_y, type);
    if (!img)
        return NULL;
    int rgba_stride = img->stride;
    uint8_t *rgba = img->rgba;

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

    const GradientValues *vals = &info->gradient.layer[layer];
    const ImageFillLayer *image_fill = &info->image_fill.layer[layer];
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

    uint32_t base_color = info->base_c[layer];
    uint8_t base_alpha = _a(base_color);
    uint8_t fade = info->fade;
    uint8_t style_alpha = base_alpha;
    if (fade > 0)
        style_alpha = mult_alpha(style_alpha, fade);
    uint8_t style_opacity = 255 - style_alpha;

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
                uint8_t layer_opacity = (uint8_t) ((sa * style_opacity + 127) / 255);
                uint8_t A = draw_img_compat ?
                    (uint8_t) ((cov64 * layer_opacity) >> 6) :
                    (uint8_t) ((cov * layer_opacity + 127) / 255);
                row[4 * x + 0] = (uint8_t) ((sr * A + 127) / 255);
                row[4 * x + 1] = (uint8_t) ((sg * A + 127) / 255);
                row[4 * x + 2] = (uint8_t) ((sb * A + 127) / 255);
                row[4 * x + 3] = A;
                continue;
            }
            int32_t uf = 0;
            if (denom_w > 0) {
                int64_t num_u = ((int64_t) (src_x + x)) << 16;
                uf = (int32_t) (num_u / denom_w);
            }
            uint32_t color = (vals->color_enabled) ?
                ass_gradient_sample_color_fixed(vals, uf, vf) : base_color;
            uint8_t alpha = (vals->alpha_enabled) ?
                ass_gradient_sample_alpha_fixed(vals, uf, vf) : base_alpha;
            if (fade > 0)
                alpha = mult_alpha(alpha, fade);
            uint8_t A = (uint8_t) ((cov * (255 - alpha)) / 255);
            row[4 * x + 0] = (uint8_t) ((_r(color) * A) / 255);
            row[4 * x + 1] = (uint8_t) ((_g(color) * A) / 255);
            row[4 * x + 2] = (uint8_t) ((_b(color) * A) / 255);
            row[4 * x + 3] = A;
        }
    }

    return img;
}

static unsigned char *copy_bitmap_region(const Bitmap *bm, int x0, int y0,
                                         int w, int h, int align,
                                         int *stride_out)
{
    if (w <= 0 || h <= 0)
        return NULL;

    int stride = ass_align(align, w);
    unsigned char *buf = ass_aligned_alloc(align, stride * h + align, false);
    if (!buf)
        return NULL;

    for (int y = 0; y < h; y++) {
        unsigned char *dst = buf + y * stride;
        unsigned char *src = bm->buffer + (y0 + y) * bm->stride + x0;
        memcpy(dst, src, w);
    }

    *stride_out = stride;
    return buf;
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
    brk -= dst_x;

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
    }

        // draw the rectangles
        for (j = 0; j < i; j++) {
            int lbrk = brk;
            // kick out rectangles that are invalid now
            if (r[j].x1 <= r[j].x0 || r[j].y1 <= r[j].y0)
                continue;
            // split up into left and right for karaoke, if needed
            if (lbrk > r[j].x0) {
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
                uint32_t legacy_color = color;
                if (rgba_tail && combined &&
                    combined->image_fill.layer[layer1].enabled)
                    legacy_color = (legacy_color & 0xFFFFFF00u) | 0xFFu;
                img = my_draw_bitmap(sub_buf, sub_w, sub_h, sub_stride,
                                     dst_x + r[j].x0, dst_y + r[j].y0, legacy_color, source);
                if (!img) {
                    if (!source)
                        ass_aligned_free(sub_buf);
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
            if (lbrk < r[j].x1) {
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
                uint32_t legacy_color = color2;
                if (rgba_tail && combined &&
                    combined->image_fill.layer[layer2].enabled)
                    legacy_color = (legacy_color & 0xFFFFFF00u) | 0xFFu;
                img = my_draw_bitmap(sub_buf, sub_w, sub_h, sub_stride,
                                     dst_x + lbrk, dst_y + r[j].y0, legacy_color, source);
                if (!img) {
                    if (!source)
                        ass_aligned_free(sub_buf);
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
    brk -= dst_x;

    // clipping
    clip_x0 = FFMINMAX(state->clip_x0, 0, render_priv->width);
    clip_y0 = FFMINMAX(state->clip_y0, 0, render_priv->height);
    clip_x1 = FFMINMAX(state->clip_x1, 0, render_priv->width);
    clip_y1 = FFMINMAX(state->clip_y1, 0, render_priv->height);
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

    if (brk > b_x0) {           // draw left part
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
        uint32_t legacy_color = color;
        if (rgba_tail && combined &&
            combined->image_fill.layer[layer1].enabled)
            legacy_color = (legacy_color & 0xFFFFFF00u) | 0xFFu;
        img = my_draw_bitmap(sub_buf, sub_w, sub_h, sub_stride,
                             dst_x + b_x0, dst_y + b_y0, legacy_color, source);
        if (!img) {
            if (!source)
                ass_aligned_free(sub_buf);
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
    if (brk < b_x1) {           // draw right part
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
        uint32_t legacy_color = color2;
        if (rgba_tail && combined &&
            combined->image_fill.layer[layer2].enabled)
            legacy_color = (legacy_color & 0xFFFFFF00u) | 0xFFu;
        img = my_draw_bitmap(sub_buf, sub_w, sub_h, sub_stride,
                             dst_x + brk, dst_y + b_y0, legacy_color, source);
        if (!img) {
            if (!source)
                ass_aligned_free(sub_buf);
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

static ASS_Image **render_border_layer(RenderContext *state,
                                       CombinedBitmapInfo *info,
                                       int layer, ASS_Image **tail,
                                       ASS_ImageRGBA ***rgba_tail)
{
    Bitmap *bm = combined_border_bitmap(info, layer);
    if (!bm)
        return tail;

    if ((info->effect_type == EF_KARAOKE_KO) && (info->effect_timing <= 0))
        return tail;

    if (layer == 0) {
        return render_glyph(state, info, bm, info->x, info->y, info->c[2],
                            0, 1000000, tail, IMAGE_TYPE_OUTLINE, info->image,
                            2, 2, rgba_tail);
    }

    uint32_t color = info->border_layers[layer].color;
    ass_apply_fade(&color, info->fade);

    uint32_t saved_base = info->base_c[2];
    GradientValues saved_gradient = info->gradient.layer[2];
    ImageFillLayer saved_image = info->image_fill.layer[2];
    info->base_c[2] = info->border_layers[layer].color;
    memset(&info->gradient.layer[2], 0, sizeof(info->gradient.layer[2]));
    clear_image_fill_layer(&info->image_fill.layer[2]);

    tail = render_glyph(state, info, bm, info->x, info->y, color,
                        0, 1000000, tail, IMAGE_TYPE_OUTLINE, info->image,
                        2, 2, rgba_tail);

    info->base_c[2] = saved_base;
    info->gradient.layer[2] = saved_gradient;
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

static double motion_progress(RenderContext *state, const MotionState *motion)
{
    int32_t t1, t2;
    motion_timing(motion, state, &t1, &t2);

    int t = state->renderer->time - state->event->Start;
    if (t <= t1)
        return 0.;
    if (t >= t2)
        return 1.;

    int32_t delta_t = (uint32_t) t2 - t1;
    if (!delta_t)
        return 1.;

    return ((double) (int32_t) ((uint32_t) t - t1)) / delta_t;
}

static ASS_DVector evaluate_motion(RenderContext *state)
{
    MotionState *m = &state->motion;
    switch (m->type) {
    case MOTION_POS:
        return (ASS_DVector) {m->x1, m->y1};
    case MOTION_MOVE: {
        double k = motion_progress(state, m);
        double x = m->x1 + (m->x2 - m->x1) * k;
        double y = m->y1 + (m->y2 - m->y1) * k;
        return (ASS_DVector) {x, y};
    }
    case MOTION_MOVER: {
        double k = motion_progress(state, m);
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
        double k = motion_progress(state, m);
        double inv = 1 - k;
        double x = inv * inv * m->x1 + 2 * inv * k * m->x2 + k * k * m->x3;
        double y = inv * inv * m->y1 + 2 * inv * k * m->y2 + k * k * m->y3;
        return (ASS_DVector) {x, y};
    }
    case MOTION_MOVES4: {
        double k = motion_progress(state, m);
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

/**
 * Iterate through a list of bitmaps and blend with clip vector, if
 * applicable. The blended bitmaps are added to a free list which is freed
 * at the start of a new frame.
 */
static void blend_vector_clip(RenderContext *state, ASS_Image *head)
{
    if (!state->clip_drawing_text.str)
        return;

    ASS_Renderer *render_priv = state->renderer;

    OutlineHashKey ol_key;
    ol_key.type = OUTLINE_DRAWING;
    ol_key.u.drawing.text = state->clip_drawing_text;

    double m[3][3] = {{0}};
    int32_t scale_base = lshiftwrapi(1, state->clip_drawing_scale - 1);
    double w = scale_base > 0 ? (1.0 / scale_base) : 0;
    m[0][0] = state->screen_scale_x * w;
    m[1][1] = state->screen_scale_y * w;
    m[2][2] = 1;

    m[0][2] = int_to_d6(render_priv->settings.left_margin);
    m[1][2] = int_to_d6(render_priv->settings.top_margin);
    ASS_DVector mvc = movevc_offset(state);
    if (mvc.x || mvc.y) {
        m[0][2] += mvc.x * state->screen_scale_x * 64;
        m[1][2] += mvc.y * state->screen_scale_y * 64;
    }

    ASS_Vector pos;
    BitmapHashKey key = {0};
    key.outline = ass_cache_get(render_priv->cache.outline_cache, &ol_key, render_priv);
    if (!key.outline || !key.outline->valid ||
            !quantize_transform(m, &pos, NULL, true, &key))
        return;

    Bitmap *clip_bm = ass_cache_get(render_priv->cache.bitmap_cache, &key, state);
    if (!clip_bm || !clip_bm->buffer || !clip_bm->w || !clip_bm->h)
        return;

    // Iterate through bitmaps and blend/clip them
    for (ASS_Image *cur = head; cur; cur = cur->next) {
        int left, top, right, bottom, w, h;
        int ax, ay, aw, ah, as;
        int bx, by, bw, bh, bs;
        int aleft, atop, bleft, btop;
        unsigned char *abuffer, *bbuffer, *nbuffer;

        abuffer = cur->bitmap;
        bbuffer = clip_bm->buffer;
        ax = cur->dst_x;
        ay = cur->dst_y;
        aw = cur->w;
        ah = cur->h;
        as = cur->stride;
        bx = pos.x + clip_bm->left;
        by = pos.y + clip_bm->top;
        bw = clip_bm->w;
        bh = clip_bm->h;
        bs = clip_bm->stride;

        // Calculate overlap coordinates
        left = (ax > bx) ? ax : bx;
        top = (ay > by) ? ay : by;
        right = ((ax + aw) < (bx + bw)) ? (ax + aw) : (bx + bw);
        bottom = ((ay + ah) < (by + bh)) ? (ay + ah) : (by + bh);
        aleft = left - ax;
        atop = top - ay;
        w = right - left;
        h = bottom - top;
        bleft = left - bx;
        btop = top - by;

        unsigned align = 1 << render_priv->engine.align_order;
        if (state->clip_drawing_mode) {
            // Inverse clip
            if (ax + aw < bx || ay + ah < by || ax > bx + bw ||
                ay > by + bh || !h || !w) {
                continue;
            }

            // Allocate new buffer and add to free list
            nbuffer = ass_aligned_alloc(align, as * ah + align, false);
            if (!nbuffer)
                break;

            // Blend together
            memcpy(nbuffer, abuffer, ((ah - 1) * as) + aw);
            render_priv->engine.imul_bitmaps(nbuffer + atop * as + aleft, as,
                                             bbuffer + btop * bs + bleft, bs,
                                             w, h);
        } else {
            // Regular clip
            if (ax + aw < bx || ay + ah < by || ax > bx + bw ||
                ay > by + bh || !h || !w) {
                cur->w = cur->h = cur->stride = 0;
                continue;
            }

            // Allocate new buffer and add to free list
            unsigned ns = ass_align(align, w);
            nbuffer = ass_aligned_alloc(align, ns * h + align, false);
            if (!nbuffer)
                break;

            // Blend together
            render_priv->engine.mul_bitmaps(nbuffer, ns,
                                            abuffer + atop * as + aleft, as,
                                            bbuffer + btop * bs + bleft, bs,
                                            w, h);
            cur->dst_x += aleft;
            cur->dst_y += atop;
            cur->w = w;
            cur->h = h;
            cur->stride = ns;
        }

        ASS_ImagePriv *priv = (ASS_ImagePriv *) cur;
        priv->buffer = cur->bitmap = nbuffer;
        ass_cache_dec_ref(priv->source);
        priv->source = NULL;
    }
}

static void blend_vector_clip_rgba(RenderContext *state, ASS_ImageRGBA *head)
{
    if (!head || !state->clip_drawing_text.str)
        return;

    ASS_Renderer *render_priv = state->renderer;

    OutlineHashKey ol_key;
    ol_key.type = OUTLINE_DRAWING;
    ol_key.u.drawing.text = state->clip_drawing_text;

    double m[3][3] = {{0}};
    int32_t scale_base = lshiftwrapi(1, state->clip_drawing_scale - 1);
    double w = scale_base > 0 ? (1.0 / scale_base) : 0;
    m[0][0] = state->screen_scale_x * w;
    m[1][1] = state->screen_scale_y * w;
    m[2][2] = 1;

    m[0][2] = int_to_d6(render_priv->settings.left_margin);
    m[1][2] = int_to_d6(render_priv->settings.top_margin);
    ASS_DVector mvc = movevc_offset(state);
    if (mvc.x || mvc.y) {
        m[0][2] += mvc.x * state->screen_scale_x * 64;
        m[1][2] += mvc.y * state->screen_scale_y * 64;
    }

    ASS_Vector pos;
    BitmapHashKey key = {0};
    key.outline = ass_cache_get(render_priv->cache.outline_cache, &ol_key, render_priv);
    if (!key.outline || !key.outline->valid ||
            !quantize_transform(m, &pos, NULL, true, &key))
        return;

    Bitmap *clip_bm = ass_cache_get(render_priv->cache.bitmap_cache, &key, state);
    if (!clip_bm || !clip_bm->buffer || !clip_bm->w || !clip_bm->h)
        return;

    for (ASS_ImageRGBA *cur = head; cur; cur = cur->next) {
        int left, top, right, bottom, aw, ah, as;
        int ax, ay, bx, by, bw, bh, bs;
        int aleft, atop, bleft, btop;
        ASS_ImageRGBAPriv *rgba_priv = (ASS_ImageRGBAPriv *) cur;
        uint8_t *abuffer = cur->rgba;
        uint8_t *bbuffer = clip_bm->buffer;
        ax = cur->dst_x;
        ay = cur->dst_y;
        aw = cur->w;
        ah = cur->h;
        as = cur->stride;
        bx = pos.x + clip_bm->left;
        by = pos.y + clip_bm->top;
        bw = clip_bm->w;
        bh = clip_bm->h;
        bs = clip_bm->stride;

        left = (ax > bx) ? ax : bx;
        top = (ay > by) ? ay : by;
        right = ((ax + aw) < (bx + bw)) ? (ax + aw) : (bx + bw);
        bottom = ((ay + ah) < (by + bh)) ? (ay + ah) : (by + bh);
        aleft = left - ax;
        atop = top - ay;
        int wclip = right - left;
        int hclip = bottom - top;
        bleft = left - bx;
        btop = top - by;

        if (state->clip_drawing_mode) {
            if (ax + aw < bx || ay + ah < by || ax > bx + bw ||
                ay > by + bh || !hclip || !wclip) {
                continue;
            }

            size_t alloc_size;
            uint8_t *nbuffer =
                ass_rgba_alloc_buffer_stride(render_priv, as, ah,
                                             rgba_priv->alloc_size,
                                             &alloc_size);
            if (!nbuffer)
                break;
            memcpy(nbuffer, abuffer, as * ah);
            for (int y = 0; y < hclip; y++) {
                uint8_t *dst = nbuffer + (atop + y) * as + (aleft * 4);
                uint8_t *src_mask = bbuffer + (btop + y) * bs + bleft;
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
            if (ax + aw < bx || ay + ah < by || ax > bx + bw ||
                ay > by + bh || !hclip || !wclip) {
                cur->w = cur->h = 0;
                continue;
            }

            int ns;
            size_t alloc_size;
            uint8_t *nbuffer =
                ass_rgba_alloc_buffer(render_priv, wclip, hclip,
                                      rgba_priv->alloc_size, &ns,
                                      &alloc_size);
            if (!nbuffer)
                break;
            for (int y = 0; y < hclip; y++) {
                uint8_t *dst = nbuffer + y * ns;
                uint8_t *src = abuffer + (atop + y) * as + aleft * 4;
                uint8_t *src_mask = bbuffer + (btop + y) * bs + bleft;
                for (int x = 0; x < wclip; x++) {
                    uint8_t mval = src_mask[x];
                    for (int c = 0; c < 4; c++) {
                        dst[4 * x + c] =
                            (uint8_t) ((src[4 * x + c] * mval + 127) / 255);
                    }
                }
            }
            cur->dst_x += aleft;
            cur->dst_y += atop;
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

    for (unsigned i = 0; i < n_bitmaps; i++) {
        CombinedBitmapInfo *info = &bitmaps[i];
        if (!info->bm_s || state->border_style == 4)
            continue;

        tail =
            render_glyph(state, info, info->bm_s, info->x, info->y, info->c[3], 0,
                         1000000, tail, IMAGE_TYPE_SHADOW, info->image,
                         3, 3, out_rgba ? &rgba_tail : NULL);
    }

    for (unsigned i = 0; i < n_bitmaps; i++) {
        CombinedBitmapInfo *info = &bitmaps[i];
        for (int layer = ASS_BORDER_LAYERS_MAX - 1; layer >= 0; layer--)
            tail = render_border_layer(state, info, layer, tail,
                                       out_rgba ? &rgba_tail : NULL);
    }

    for (unsigned i = 0; i < n_bitmaps; i++) {
        CombinedBitmapInfo *info = &bitmaps[i];
        if (!info->bm)
            continue;

        if ((info->effect_type == EF_KARAOKE)
                || (info->effect_type == EF_KARAOKE_KO)) {
            if (info->effect_timing > 0)
                tail =
                    render_glyph(state, info, info->bm, info->x, info->y,
                                 info->c[0], 0, 1000000, tail,
                                 IMAGE_TYPE_CHARACTER, info->image, 0, 0,
                                 out_rgba ? &rgba_tail : NULL);
            else
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
    blend_vector_clip(state, head);
    if (out_rgba) {
        blend_vector_clip_rgba(state, rgba_head);
        *out_rgba = rgba_head;
    }
    return head;
}

static void compute_string_bbox(TextInfo *text, ASS_DRect *bbox)
{
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

/**
 * \brief partially reset render_context to style values
 * Works like {\r}: resets some style overrides
 */
void ass_reset_render_context(RenderContext *state, ASS_Style *style)
{
    style = handle_selective_style_overrides(state, style);

    init_font_scale(state);

    state->c[0] = style->PrimaryColour;
    state->c[1] = style->SecondaryColour;
    state->c[2] = style->OutlineColour;
    state->c[3] = style->BackColour;
    ass_gradient_state_reset(&state->gradient, state->c);
    for (int i = 0; i < 4; i++)
        clear_image_fill_layer(&state->image_fill.layer[i]);
    state->flags =
        (style->Underline ? DECO_UNDERLINE : 0) |
        (style->StrikeOut ? DECO_STRIKETHROUGH : 0);
    state->font_size = style->FontSize;

    state->family.str = style->FontName;
    state->family.len = strlen(style->FontName);
    state->treat_family_as_pattern = style->treat_fontname_as_pattern;
    state->bold = style->Bold;
    state->italic = style->Italic;
    ass_update_font(state);

    state->border_style = style->BorderStyle;
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
    for (int i = 1; i < ASS_BORDER_LAYERS_MAX; i++) {
        state->border_layers[i] = (BorderLayerState) {
            .enabled = false,
            .has_color = false,
            .has_alpha = false,
            .size_x = 0,
            .size_y = 0,
            .color = style->OutlineColour,
        };
    }
    state->scale_x = style->ScaleX;
    state->scale_y = style->ScaleY;
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
    state->rnd_x = 0.0;
    state->rnd_y = 0.0;
    state->rnd_z = 0.0;
    state->needs_rgba = false;
    state->distort_enabled = false;
    state->distort_u1 = 1.0;
    state->distort_v1 = 0.0;
    state->distort_u2 = 1.0;
    state->distort_v2 = 1.0;
    state->distort_u3 = 0.0;
    state->distort_v3 = 1.0;
}

/**
 * \brief Start new event. Reset state.
 */
static void
init_render_context(RenderContext *state, ASS_Event *event)
{
    ASS_Renderer *render_priv = state->renderer;

    state->event = event;
    state->parsed_tags = 0;
    state->evt_type = EVENT_NORMAL;

    state->wrap_style = render_priv->track->WrapStyle;

    state->pos_x = 0;
    state->pos_y = 0;
    state->org_x = 0;
    state->org_y = 0;
    state->have_origin = 0;
    state->clip_x0 = 0;
    state->clip_y0 = 0;
    state->clip_x1 = render_priv->track->PlayResX;
    state->clip_y1 = render_priv->track->PlayResY;
    state->clip_mode = 0;
    state->detect_collisions = 1;
    state->fade = 0;
    state->drawing_scale = 0;
    state->pbo = 0;
    state->movevc = (MoveVCState) {0};
    state->motion = (MotionState) {0};
    state->motion.type = MOTION_NONE;
    state->jitter = ass_jitter_default_state();
    state->rnd_x = state->rnd_y = state->rnd_z = 0.0;
    state->rnd_seed_base = (uint64_t) event->ReadOrder;
    state->effect_type = EF_NONE;
    state->effect_timing = 0;
    state->effect_skip_timing = 0;
    state->reset_effect = false;
    state->distort_enabled = false;
    state->distort_u1 = 1.0;
    state->distort_v1 = 0.0;
    state->distort_u2 = 1.0;
    state->distort_v2 = 1.0;
    state->distort_u3 = 0.0;
    state->distort_v3 = 1.0;

    ass_apply_transition_effects(state);
    state->explicit = state->evt_type != EVENT_NORMAL ||
                      ass_event_has_hard_overrides(event->Text);

    ass_reset_render_context(state, NULL);
    state->alignment = state->style->Alignment;
    state->justify = state->style->Justify;
}

static void free_distortion_resources(RenderContext *state)
{
    TextInfo *text_info = &state->text_info;
    for (int i = 0; i < text_info->length; i++) {
        for (GlyphInfo *info = text_info->glyphs + i; info; info = info->next) {
            if (info->has_distort_bitmap) {
                ass_free_bitmap(&info->distort_bitmap);
                ass_free_bitmap(&info->distort_bitmap_o);
                for (int j = 0; j < ASS_BORDER_LAYERS_MAX - 1; j++)
                    ass_free_bitmap(&info->distort_bitmap_border[j]);
                info->bm = NULL;
                info->bm_o = NULL;
                for (int j = 0; j < ASS_BORDER_LAYERS_MAX - 1; j++)
                    info->bm_border[j] = NULL;
                info->has_distort_bitmap = false;
            }
            if (info->has_distort_outline && info->distorted_outline) {
                ass_outline_free(&info->distorted_outline->outline[0]);
                ass_outline_free(&info->distorted_outline->outline[1]);
                free(info->distorted_outline);
                info->distorted_outline = NULL;
                info->has_distort_outline = false;
            }
        }
    }

    for (unsigned i = 0; i < text_info->n_bitmaps; i++) {
        CombinedBitmapInfo *info = &text_info->combined_bitmaps[i];
        if (info->temp_image) {
            ass_free_bitmap(&info->temp_image->bm);
            ass_free_bitmap(&info->temp_image->bm_o);
            ass_free_bitmap(&info->temp_image->bm_s);
            for (int j = 0; j < ASS_BORDER_LAYERS_MAX - 1; j++)
                ass_free_bitmap(&info->temp_image->bm_border[j]);
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
    free_distortion_resources(state);
    free_furi_groups(&state->text_info);
    state->font = NULL;
    state->family.str = NULL;
    state->family.len = 0;
    state->clip_drawing_text.str = NULL;
    state->clip_drawing_text.len = 0;
    state->text_info.length = 0;
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
            ass_face_set_size(k->font->faces[k->face_index], k->size);
            if (!ass_font_get_glyph(k->font, k->face_index, k->glyph_index,
                                    render_priv->settings.hinting))
                return 1;
            if (!ass_get_glyph_outline(&v->outline[0], &v->advance,
                                       k->font->faces[k->face_index],
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
                                    STROKER_PRECISION)) {
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
                                  GlyphInfo *info, double m[3][3])
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

    OutlineHashValue *outline = info->distorted_outline ? info->distorted_outline : info->outline;
    bool distorted = info->distorted_outline && info->distort_enabled;
    info->has_distort_bitmap = false;

    if (!outline || info->symbol == '\n' || info->symbol == 0 || info->skip)
        return;

    double m1[3][3], m2[3][3], m[3][3];
    const ASS_Transform *tr = &info->transform;
    calc_transform_matrix(state, info, m1);
    for (int i = 0; i < 3; i++) {
        m2[i][0] = m1[i][0] * tr->scale.x;
        m2[i][1] = m1[i][1] * tr->scale.y;
        m2[i][2] = m1[i][0] * tr->offset.x + m1[i][1] * tr->offset.y + m1[i][2];
    }
    memcpy(m, m2, sizeof(m));

    if (info->effect_type == EF_KARAOKE_KF)
        ass_outline_update_min_transformed_x(&outline->outline[0], m, leftmost_x);

    BitmapHashKey key = {0};
    key.rnd_x = info->rnd_x;
    key.rnd_y = info->rnd_y;
    key.rnd_z = info->rnd_z;
    key.rnd_seed = info->rnd_seed;
    key.outline = outline;
    if (!quantize_transform(m, pos, offset, first, &key))
        return;

    *pos_o = *pos;

    bool rnd_active = (info->rnd_x != 0.0) || (info->rnd_y != 0.0) || (info->rnd_z != 0.0);
    if (rnd_active && !(flags & (FILTER_BORDER_STYLE_3 | FILTER_MULTI_BORDER))) {
        bool ok = build_rnd_bitmaps(state, info, outline, m, pos, pos_o,
                                    (flags & FILTER_NONZERO_BORDER), flags);
        if (ok)
            return;
        // Fall through to cached path if rnd build failed
    }

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
        ASS_DVector offset = { -bord.x, -bord.y - info->asc };
        for (int i = 0; i < 3; i++) {
            m[i][0] = m1[i][0] * scale.x;
            m[i][1] = m1[i][1] * scale.y;
            m[i][2] = m1[i][0] * offset.x + m1[i][1] * offset.y + m1[i][2];
        }

        if (load_border_bitmap(state, info, &key, &ol_key, m, pos_o, &offset,
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

    double prev_x = 0;
    double prev_y = 0;
    for (int layer = 0; layer < ASS_BORDER_LAYERS_MAX; layer++) {
        const BorderLayerState *border = &info->border_layers[layer];
        double size_x = layer == 0 ? info->border_x : border->size_x;
        double size_y = layer == 0 ? info->border_y : border->size_y;
        bool has_size = layer == 0 ? size_x > 0 || size_y > 0 :
                                      border_layer_has_size(border);
        if (!has_size)
            continue;
        if (size_x <= prev_x && size_y <= prev_y)
            continue;

        OutlineHashKey ol_key;
        double border_m[3][3];
        bool zero_border = false;
        if (!setup_border_outline_key(state, info, outline, m, m2,
                                      size_x, size_y, &ol_key, border_m,
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
            prev_x = size_x;
            prev_y = size_y;
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
            prev_x = size_x;
            prev_y = size_y;
        } else if (layer == 0) {
            *pos_o = *pos;
        }
    }
}

static inline size_t outline_size(const ASS_Outline* outline)
{
    return sizeof(ASS_Vector) * outline->n_points + outline->n_segments;
}

static bool build_rnd_bitmaps(RenderContext *state, GlyphInfo *info,
                              OutlineHashValue *outline_src,
                              const double m[3][3],
                              ASS_Vector *pos, ASS_Vector *pos_o,
                              bool need_border, int flags)
{
    ASS_Renderer *render_priv = state->renderer;
    Bitmap *bm_fill = &info->distort_bitmap;
    Bitmap *bm_border = &info->distort_bitmap_o;
    memset(bm_fill, 0, sizeof(*bm_fill));
    memset(bm_border, 0, sizeof(*bm_border));

    ASS_Outline outline_fill[2] = {{0}};
    ASS_Outline outline_border[2] = {{0}};
    bool ok = false;

    // Transform base outline to screen space
    if (m[2][0] || m[2][1]) {
        if (!ass_outline_transform_3d(&outline_fill[0], &outline_src->outline[0], m) ||
            !ass_outline_transform_3d(&outline_fill[1], &outline_src->outline[1], m))
            goto done;
    } else {
        if (!ass_outline_transform_2d(&outline_fill[0], &outline_src->outline[0], m) ||
            !ass_outline_transform_2d(&outline_fill[1], &outline_src->outline[1], m))
            goto done;
    }

    BitmapHashKey temp_key = {0};
    temp_key.rnd_x = info->rnd_x;
    temp_key.rnd_y = info->rnd_y;
    temp_key.rnd_z = info->rnd_z;
    temp_key.rnd_seed = info->rnd_seed;
    temp_key.matrix_z.x = m[2][0];
    temp_key.matrix_z.y = m[2][1];

#ifdef ASS_RND_DEBUG
    ass_msg(render_priv->library, MSGL_V,
            "rnd apply: rnd_x=%.3f rnd_y=%.3f rnd_z=%.3f seed=%llu",
            temp_key.rnd_x, temp_key.rnd_y, temp_key.rnd_z,
            (unsigned long long) temp_key.rnd_seed);
#endif

        apply_rnd_offsets(&temp_key, &outline_fill[0], render_priv->library);
        apply_rnd_offsets(&temp_key, &outline_fill[1], render_priv->library);

    if (!ass_outline_to_bitmap(state, bm_fill, &outline_fill[0], &outline_fill[1]))
        goto done;
    info->bm = bm_fill;
    info->bm_o = NULL;

    if (need_border) {
        double bord_x =
            64 * state->border_scale_x * info->border_x / info->transform.scale.x /
                render_priv->par_scale_x;
        double bord_y =
            64 * state->border_scale_y * info->border_y / info->transform.scale.y;

        if (bord_x > 0 || bord_y > 0) {
            if (!ass_outline_stroke(&outline_border[0], &outline_border[1],
                                    &outline_fill[0],
                                    bord_x * STROKER_PRECISION,
                                    bord_y * STROKER_PRECISION,
                                    STROKER_PRECISION))
                goto done;

            if (!ass_outline_to_bitmap(state, bm_border, &outline_border[0], &outline_border[1]))
                goto done;
            info->bm_o = bm_border;
        } else {
            info->bm_o = info->bm;
        }
    }

    info->has_distort_bitmap = true;
    ok = true;

done:
    ass_outline_free(&outline_fill[0]);
    ass_outline_free(&outline_fill[1]);
    ass_outline_free(&outline_border[0]);
    ass_outline_free(&outline_border[1]);
    return ok;
}

static inline uint64_t rnd_mix64(uint64_t x)
{
    // SplitMix64 scramble for deterministic, per-point seeds
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static inline double rnd_pm1(uint64_t seed)
{
    // Generate uniform [-1, 1] from 53 random bits
    double u01 = (rnd_mix64(seed) >> 11) * (1.0 / 9007199254740992.0);
    return u01 * 2.0 - 1.0;
}

static void apply_rnd_offsets(const BitmapHashKey *k, ASS_Outline *outline,
                              ASS_Library *lib)
{
    double mag_x = FFMIN(fabs(k->rnd_x), ASS_RND_MAX_PX) * ASS_RND_SCALE;
    double mag_y = FFMIN(fabs(k->rnd_y), ASS_RND_MAX_PX) * ASS_RND_SCALE;
    double mag_z = FFMIN(fabs(k->rnd_z), ASS_RND_MAX_PX) * ASS_RND_SCALE;
    bool has_perspective = k->matrix_z.x || k->matrix_z.y;
    if (!(mag_x || mag_y || (mag_z && has_perspective)))
        return;

    static bool eff_logged = false;
    if (lib && !eff_logged) {
        eff_logged = true;
        ass_msg(lib, MSGL_V, "rnd eff (scaled): eff_x=%.3f eff_y=%.3f eff_z=%.3f",
                mag_x, mag_y, mag_z);
    }

    double max_dx_px = 0.0, max_dy_px = 0.0;
    double max_dx_raw = 0.0, max_dy_raw = 0.0;
    int32_t min_x = INT32_MAX, min_y = INT32_MAX;
    int32_t max_x = INT32_MIN, max_y = INT32_MIN;
    for (size_t i = 0; i < outline->n_points; i++) {
        uint64_t seed = k->rnd_seed ^ (uint64_t) i;
        double dx = mag_x ? rnd_pm1(seed ^ 0x5851f42d4c957f2dULL) * mag_x : 0.0;
        double dy = mag_y ? rnd_pm1(seed ^ 0x14057b7ef767814fULL) * mag_y : 0.0;
        if (mag_z && has_perspective)
            dy += rnd_pm1(seed ^ 0x94d049bb133111ebULL) * mag_z;

        double raw_dx = dx * 64.0;
        double raw_dy = dy * 64.0;
        max_dx_px = FFMAX(max_dx_px, fabs(dx));
        max_dy_px = FFMAX(max_dy_px, fabs(dy));
        max_dx_raw = FFMAX(max_dx_raw, fabs(raw_dx));
        max_dy_raw = FFMAX(max_dy_raw, fabs(raw_dy));

        outline->points[i].x = ass_lrint(outline->points[i].x + raw_dx);
        outline->points[i].y = ass_lrint(outline->points[i].y + raw_dy);
        min_x = FFMIN(min_x, outline->points[i].x);
        min_y = FFMIN(min_y, outline->points[i].y);
        max_x = FFMAX(max_x, outline->points[i].x);
        max_y = FFMAX(max_y, outline->points[i].y);
    }
    if (lib) {
#ifdef ASS_RND_DEBUG
        double w_raw = (max_x > min_x) ? (max_x - min_x) : 0;
        double h_raw = (max_y > min_y) ? (max_y - min_y) : 0;
        ass_msg(lib, MSGL_V,
                "rnd apply: rnd_x=%.3f rnd_y=%.3f rnd_z=%.3f max_raw_dx=%.2f max_raw_dy=%.2f max_px_dx=%.2f max_px_dy=%.2f bbox_raw=%.2fx%.2f px=%.2fx%.2f",
                k->rnd_x, k->rnd_y, k->rnd_z,
                max_dx_raw, max_dy_raw,
                max_dx_px, max_dy_px,
                w_raw, h_raw, w_raw / 64.0, h_raw / 64.0);
#endif
    }
    if (mag_x) assert(max_dx_px <= mag_x + 0.5);
    if (mag_y) assert(max_dy_px <= mag_y + 0.5);
}

size_t ass_bitmap_construct(void *key, void *value, void *priv)
{
    RenderContext *state = priv;
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

    if (k->rnd_x || k->rnd_y || k->rnd_z) {
        apply_rnd_offsets(k, &outline[0], state->renderer->library);
        apply_rnd_offsets(k, &outline[1], state->renderer->library);
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
           state->fshp * state->screen_scale_y;
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
    for (i = 0; i < ti->length; ++i) {
        cur = ti->glyphs + i;
        if (cur->linebreak) {
            // Mark whitespace before
            j = i - 1;
            cur = ti->glyphs + j;
            while (j && IS_WHITESPACE(cur)) {
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
            if (break_at >= 0)
                ass_msg(render_priv->library, MSGL_DBG2, "line break at %d",
                        break_at);
        }
        if (ALLOWBREAK(cur->symbol, i)) {
            last_breakable = i;
        }

        if (break_at != -1) {
            // need to use one more line
            // marking break_at+1 as start of a new line
            int lead = break_at + 1;    // the first symbol of the new line
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
            if (lead < text_info->length) {
                text_info->glyphs[lead].linebreak = break_type;
                last_breakable = -1;
                s1 = text_info->glyphs + lead;
                text_info->n_lines++;
            }
        }
    }
}

/*
 * Rewind from a linestart position back to the first non-whitespace (0x20)
 * character. Trailing ASCII whitespace gets trimmed in rendering.
 * Assumes both arguments are part of the same array.
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
    double pen_shift_x = d6_to_double(-text_info->glyphs[i].pos.x);
    double pen_shift_y = 0.;

    for (i = 0; i < text_info->length; ++i) {
        GlyphInfo *cur = text_info->glyphs + i;
        if (cur->linebreak) {
            while (i < text_info->length && cur->skip && !FORCEBREAK(cur->symbol, i))
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
static void split_style_runs_list(GlyphInfo *glyphs, int length)
{
    if (length <= 0)
        return;

    Effect last_effect_type = glyphs[0].effect_type;
    glyphs[0].starts_new_run = true;
    for (int i = 1; i < length; i++) {
        GlyphInfo *info = glyphs + i;
        GlyphInfo *last = glyphs + (i - 1);
        Effect effect_type = info->effect_type;
        info->starts_new_run =
            info->effect_timing ||  // but ignore effect_skip_timing
            (effect_type != EF_NONE && effect_type != last_effect_type) ||
            info->drawing_text.str ||
            last->drawing_text.str ||
            !ass_string_equal(last->font->desc.family, info->font->desc.family) ||
            last->font->desc.vertical != info->font->desc.vertical ||
            last->font_size != info->font_size ||
            last->c[0] != info->c[0] ||
            last->c[1] != info->c[1] ||
            last->c[2] != info->c[2] ||
            last->c[3] != info->c[3] ||
            !ass_gradient_equal(&last->gradient, &info->gradient) ||
            !image_fill_state_equal(&last->image_fill, &info->image_fill) ||
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
        if (effect_type != EF_NONE)
            last_effect_type = effect_type;
    }
}

static void split_style_runs(RenderContext *state)
{
    split_style_runs_list(state->text_info.glyphs, state->text_info.length);
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
        if ((p[1] == 'N') || ((p[1] == 'n') &&
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

    GlyphInfo *info = &(*glyphs)[*length];
    memset(info, 0, sizeof(GlyphInfo));

    if (drawing_text.str) {
        info->drawing_text = drawing_text;
        info->drawing_scale = state->drawing_scale;
        info->drawing_pbo = state->pbo;
    }

    double scale_x = state->scale_x;
    double scale_y = state->scale_y;
    double hspacing = state->hspacing;
    if (is_furi) {
        scale_x *= state->furi_scale_x / 100.0;
        scale_y *= state->furi_scale_y / 100.0;
        hspacing = state->furi_hspacing;
    }

    info->symbol = code;
    info->font = state->font;
    for (int i = 0; i < 4; i++)
        info->c[i] = state->c[i];
    info->gradient = state->gradient;
    info->image_fill = state->image_fill;
    info->line = 0;

    info->effect_type = state->effect_type;
    info->effect_timing = state->effect_timing;
    info->effect_skip_timing = state->effect_skip_timing;
    info->reset_effect = state->reset_effect;
    info->font_size = fabs(state->font_size * state->screen_scale_y);
    info->be = state->be;
    info->blur_x = state->blur_x;
    info->blur_y = state->blur_y;
    info->shadow_x = state->shadow_x;
    info->shadow_y = state->shadow_y;
    info->scale_x = scale_x;
    info->scale_y = scale_y;
    info->border_style = state->border_style;
    info->border_x = state->border_x;
    info->border_y = state->border_y;
    memcpy(info->border_layers, state->border_layers, sizeof(info->border_layers));
    sync_glyph_layer1_border(info);
    info->hspacing = hspacing;
    info->bold = state->bold;
    info->italic = state->italic;
    info->flags = state->flags;
    if (info->font->desc.vertical && code >= VERTICAL_LOWER_BOUND)
        info->flags |= DECO_ROTATE;
    info->frx = state->frx;
    info->fry = state->fry;
    info->frs = state->frs;
    info->frz = state->frz + info->frs;
    info->z = state->z;
    info->ortho = state->ortho;
    info->fax = state->fax;
    info->fay = state->fay;
    info->fade = state->fade;
    info->vshift = -double_to_d6(state->fsvp * state->screen_scale_y);
    if (state->jitter.enabled) {
        info->has_jitter = true;
        info->jitter = state->jitter;
    }
    info->has_rnd = state->rnd_x || state->rnd_y || state->rnd_z;
    uint64_t glyph_index = (uint64_t) *length;
    if (is_furi)
        glyph_index ^= (uint64_t) (furi_group + 1) << 48;
    info->rnd_seed = state->rnd_seed_base ^ (glyph_index << 32) ^ (uint64_t) info->glyph_index;
    info->rnd_x = x2scr_offset(state, state->rnd_x);
    info->rnd_y = y2scr_offset(state, state->rnd_y);
    info->rnd_z = y2scr_offset(state, state->rnd_z);
#ifdef ASS_RND_DEBUG
    if (info->has_rnd) {
        ass_msg(render_priv->library, MSGL_WARN,
                "glyph rnd (screen units): x=%g y=%g z=%g has_rnd=%d",
                info->rnd_x, info->rnd_y, info->rnd_z, info->has_rnd);
    }
#endif
    info->distort_enabled = state->distort_enabled;
    info->distort_u1 = state->distort_u1;
    info->distort_v1 = state->distort_v1;
    info->distort_u2 = state->distort_u2;
    info->distort_v2 = state->distort_v2;
    info->distort_u3 = state->distort_u3;
    info->distort_v3 = state->distort_v3;
    info->distorted_outline = NULL;
    info->has_distort_bitmap = false;
    info->has_distort_outline = false;
    info->is_furi = is_furi;
    info->furi_group = furi_group;

    info->hspacing_scaled = 0;
    info->scale_fix = 1;

    if (!drawing_text.str) {
        info->hspacing_scaled = double_to_d6(info->hspacing *
                state->screen_scale_x / render_priv->par_scale_x *
                info->scale_x);
        fix_glyph_scaling(render_priv, info);
    }

    (*length)++;
    return true;
}

static bool append_text_segment(RenderContext *state, char *start, char *end,
                                GlyphInfo **glyphs,
                                FriBidiChar **event_text,
                                char **breaks,
                                int *length,
                                int *max_glyphs,
                                bool is_furi,
                                int furi_group)
{
    char *p = start;
    while (p < end) {
        unsigned code = get_next_char_bounded(state, &p, end);
        if (!code)
            break;
        if (!append_glyph_to_target(state, glyphs, event_text, breaks,
                                    length, max_glyphs, code,
                                    (ASS_StringView) {NULL, 0},
                                    is_furi, furi_group))
            return false;

        if (!is_furi) {
            state->effect_type = EF_NONE;
            state->effect_timing = 0;
            state->effect_skip_timing = 0;
            state->reset_effect = false;
        }
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
           a->size_x == b->size_x &&
           a->size_y == b->size_y &&
           a->color == b->color;
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
    double prev_x = 0;
    double prev_y = 0;
    for (int i = 0; i < ASS_BORDER_LAYERS_MAX; i++) {
        if (!border_layer_has_size(&layers[i]))
            continue;
        if (layers[i].size_x <= prev_x && layers[i].size_y <= prev_y)
            continue;
        if (i > 0)
            return true;
        prev_x = layers[i].size_x;
        prev_y = layers[i].size_y;
    }
    return false;
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
}

static double glyph_border_max_x(const GlyphInfo *info)
{
    double max = info->border_x > 0 ? info->border_x : 0;
    for (int i = 1; i < ASS_BORDER_LAYERS_MAX; i++)
        if (border_layer_has_size(&info->border_layers[i]))
            max = FFMAX(max, info->border_layers[i].size_x);
    return max;
}

static double glyph_border_max_y(const GlyphInfo *info)
{
    double max = info->border_y > 0 ? info->border_y : 0;
    for (int i = 1; i < ASS_BORDER_LAYERS_MAX; i++)
        if (border_layer_has_size(&info->border_layers[i]))
            max = FFMAX(max, info->border_layers[i].size_y);
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
    char *end;
} FuriCandidate;

typedef enum {
    FURI_CANDIDATE_NONE = 0,
    FURI_CANDIDATE_LITERAL,
    FURI_CANDIDATE_GROUP,
} FuriCandidateType;

static FuriCandidateType parse_furi_candidate(char *p, FuriCandidate *candidate)
{
    if (*p != '<')
        return FURI_CANDIDATE_NONE;

    char *pipe = NULL;
    char *q = p + 1;
    bool malformed = false;
    while (*q) {
        if (*q == '\\' && furi_escapes_char(q[1])) {
            q += 2;
            continue;
        }
        // Override blocks inside a furi group are reserved for a later
        // implementation; literalize them instead of half-parsing them.
        if (*q == '<' || *q == '{' || *q == '}')
            malformed = true;
        if (*q == '|') {
            if (!pipe)
                pipe = q;
            else
                malformed = true;
        } else if (*q == '>') {
            candidate->base_start = p + 1;
            candidate->base_end = pipe ? pipe : q;
            candidate->furi_start = pipe ? pipe + 1 : q;
            candidate->furi_end = q;
            candidate->end = q + 1;

            if (!pipe)
                return FURI_CANDIDATE_LITERAL;
            if (candidate->base_start == candidate->base_end ||
                    candidate->furi_start == candidate->furi_end ||
                    malformed)
                return FURI_CANDIDATE_LITERAL;
            return FURI_CANDIDATE_GROUP;
        }
        q++;
    }

    if (pipe) {
        candidate->base_start = p;
        candidate->base_end = q;
        candidate->furi_start = q;
        candidate->furi_end = q;
        candidate->end = q;
        return FURI_CANDIDATE_LITERAL;
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
    group->style = state->furi_style;
    group->scale_x = state->furi_scale_x;
    group->scale_y = state->furi_scale_y;
    group->hspacing = state->furi_hspacing;
    group->offset_x = state->furi_offset_x;
    group->offset_y = state->furi_offset_y;

    if (!append_text_segment(state, candidate->base_start, candidate->base_end,
                             &text_info->glyphs, &text_info->event_text,
                             &text_info->breaks, &text_info->length,
                             &text_info->max_glyphs, false, group_id))
        return false;

    group->base_len = text_info->length - group->base_start;
    if (group->base_len <= 0)
        return false;
    for (int i = 0; i < group->base_len; i++) {
        GlyphInfo *info = &text_info->glyphs[group->base_start + i];
        info->is_furi_base = true;
        info->furi_group = group_id;
    }

    if (!append_text_segment(state, candidate->furi_start, candidate->furi_end,
                             &group->glyphs, &group->event_text, NULL,
                             &group->length, &group->max_glyphs,
                             true, group_id))
        return false;

    return group->length > 0;
}

// Parse event text.
// Fill render_priv->text_info.
static bool parse_events(RenderContext *state, ASS_Event *event)
{
    TextInfo *text_info = &state->text_info;

    char *p = event->Text, *q;

    // Event parsing.
    while (true) {
        ASS_StringView drawing_text = {NULL, 0};

        // get next char, executing style override
        // this affects render_context
        unsigned code = 0;
        while (*p) {
            if ((*p == '{') && (q = strchr(p, '}'))) {
                p = ass_parse_tags(state, p, q, 1., false);
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
            } else {
                if (state->furi_enabled && *p == '<') {
                    FuriCandidate candidate;
                    FuriCandidateType type = parse_furi_candidate(p, &candidate);
                    if (type == FURI_CANDIDATE_GROUP) {
                        if (!append_furi_group(state, &candidate))
                            goto fail;
                        p = candidate.end;
                        code = 0;
                        break;
                    } else if (type == FURI_CANDIDATE_LITERAL) {
                        if (!append_text_segment(state, p, candidate.end,
                                                 &text_info->glyphs,
                                                 &text_info->event_text,
                                                 &text_info->breaks,
                                                 &text_info->length,
                                                 &text_info->max_glyphs,
                                                 false, -1))
                            goto fail;
                        p = candidate.end;
                        code = 0;
                        break;
                    }
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

        state->effect_type = EF_NONE;
        state->effect_timing = 0;
        state->effect_skip_timing = 0;
        state->reset_effect = false;
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
            info->distort_u1 = root->distort_u1;
            info->distort_v1 = root->distort_v1;
            info->distort_u2 = root->distort_u2;
            info->distort_v2 = root->distort_v2;
            info->distort_u3 = root->distort_u3;
            info->distort_v3 = root->distort_v3;
            get_outline_glyph(state, info);
            if (info->has_rnd) {
                // Pad metrics so bbox/collision/clipping include rnd jitter plus stroke/shadow
                double rnd_pad_x = FFMIN(fabs(info->rnd_x), ASS_RND_MAX_PX) * ASS_RND_SCALE;
                double rnd_pad_y = FFMIN(fabs(info->rnd_y), ASS_RND_MAX_PX) * ASS_RND_SCALE;
                double rnd_pad_z = FFMIN(fabs(info->rnd_z), ASS_RND_MAX_PX) * ASS_RND_SCALE;
                double rnd_pad = FFMAX(rnd_pad_x, rnd_pad_y);
                if (info->frx != 0.0 || info->fry != 0.0)
                    rnd_pad = FFMAX(rnd_pad, rnd_pad_z);

                double border_pad_x =
                    glyph_border_max_x(info) *
                    state->border_scale_x / state->renderer->par_scale_x;
                double border_pad_y =
                    glyph_border_max_y(info) *
                    state->border_scale_y;
                double border_pad = FFMAX(border_pad_x, border_pad_y);

                double shadow_pad =
                    FFMAX(fabs(info->shadow_x), fabs(info->shadow_y));

                rnd_pad = ceil(rnd_pad + border_pad + shadow_pad);
                int32_t rnd_pad_d6 = double_to_d6(rnd_pad);
                info->bbox.x_min -= rnd_pad_d6;
                info->bbox.x_max += rnd_pad_d6;
                info->bbox.y_min -= rnd_pad_d6;
                info->bbox.y_max += rnd_pad_d6;
                info->asc += rnd_pad_d6;
                info->desc += rnd_pad_d6;
#ifdef ASS_RND_DEBUG
                ass_msg(state->renderer->library, MSGL_V,
                        "rnd pad: rnd_x=%.3f rnd_y=%.3f rnd_z=%.3f pad_px=%.3f",
                        info->rnd_x, info->rnd_y, info->rnd_z, rnd_pad);
#endif
            }
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

static void shift_furi_base(RenderContext *state, FuriGroup *group,
                            int32_t shift)
{
    TextInfo *text_info = &state->text_info;
    if (!shift)
        return;

    for (int i = 0; i < group->base_len; i++) {
        GlyphInfo *root = &text_info->glyphs[group->base_start + i];
        for (GlyphInfo *info = root; info; info = info->next)
            info->offset.x += shift;
    }
}

static void apply_furi_group_layout(RenderContext *state, FuriGroup *group)
{
    TextInfo *text_info = &state->text_info;
    int32_t base_width = furi_base_advance_width(state, group);
    int32_t furi_width = furi_text_advance_width(group);
    group->layout_width = base_width;
    group->base_shift = 0;

    if (group->style == 2) {
        if (base_width > 0 && furi_width > base_width) {
            double scale = (double) base_width / furi_width;
            scale_furi_group_x(group, scale);
        }
        return;
    }

    int32_t group_width = FFMAX(base_width, furi_width);
    int32_t extra = group_width - base_width;
    if (extra <= 0)
        return;

    int last = group->base_start + group->base_len - 1;
    for (; last >= group->base_start; last--) {
        if (!text_info->glyphs[last].skip)
            break;
    }
    if (last < group->base_start)
        return;

    group->layout_width = group_width;
    group->base_shift = extra / 2;
    shift_furi_base(state, group, group->base_shift);
    text_info->glyphs[last].cluster_advance.x += extra;
}

static bool furi_base_metrics(RenderContext *state, FuriGroup *group,
                              double *left, double *right,
                              double *top, int *line)
{
    TextInfo *text_info = &state->text_info;
    bool have = false;
    *left = DBL_MAX;
    *right = -DBL_MAX;
    *top = DBL_MAX;
    *line = 0;

    GlyphInfo *first = &text_info->glyphs[group->base_start];
    double group_left = d6_to_double(first->pos.x - first->offset.x -
                                     group->base_shift);
    if (group->layout_width > 0) {
        *left = group_left;
        *right = group_left + d6_to_double(group->layout_width);
    }

    for (int i = 0; i < group->base_len; i++) {
        GlyphInfo *root = &text_info->glyphs[group->base_start + i];
        double x0 = d6_to_double(root->pos.x);
        double x1 = x0 + d6_to_double(root->cluster_advance.x);
        if (group->layout_width <= 0) {
            *left = FFMIN(*left, FFMIN(x0, x1));
            *right = FFMAX(*right, FFMAX(x0, x1));
        }
        if (!have)
            *line = root->line;

        for (GlyphInfo *info = root; info; info = info->next) {
            double y0 = d6_to_double(info->pos.y + info->bbox.y_min);
            *top = FFMIN(*top, y0);
        }
        have = true;
    }

    return have && *left < *right;
}

static bool furi_text_metrics(FuriGroup *group, double *left,
                              double *right, double *bottom)
{
    bool have = false;
    *left = DBL_MAX;
    *right = -DBL_MAX;
    *bottom = -DBL_MAX;

    for (int i = 0; i < group->length; i++) {
        GlyphInfo *root = &group->glyphs[i];
        if (root->skip)
            continue;

        double x0 = d6_to_double(root->pos.x);
        // Center against inter-glyph spacing, not a trailing \furifsp pad.
        int32_t advance = root->cluster_advance.x - root->hspacing_scaled;
        double x1 = x0 + d6_to_double(advance);
        *left = FFMIN(*left, FFMIN(x0, x1));
        *right = FFMAX(*right, FFMAX(x0, x1));

        for (GlyphInfo *info = root; info; info = info->next) {
            double y1 = d6_to_double(info->pos.y + info->bbox.y_max);
            *bottom = FFMAX(*bottom, y1);
        }
        have = true;
    }

    return have && *left < *right;
}

static void position_furi_group(RenderContext *state, FuriGroup *group)
{
    double base_left, base_right, base_top;
    double furi_left, furi_right, furi_bottom;
    int line;
    if (!furi_base_metrics(state, group, &base_left, &base_right,
                           &base_top, &line))
        return;
    if (!furi_text_metrics(group, &furi_left, &furi_right, &furi_bottom))
        return;

    double base_width = base_right - base_left;
    double furi_width = furi_right - furi_left;
    double target_left = base_left + (base_width - furi_width) / 2.0;

    double dx = target_left - furi_left + x2scr_offset(state, group->offset_x);
    double dy = base_top - furi_bottom - y2scr_offset(state, group->offset_y);
    int32_t shift_x = double_to_d6(dx);
    int32_t shift_y = double_to_d6(dy);

    for (int i = 0; i < group->length; i++) {
        for (GlyphInfo *info = &group->glyphs[i]; info; info = info->next) {
            info->pos.x += shift_x;
            info->pos.y += shift_y;
            info->line = line;
        }
    }
}

static void position_furi_groups(RenderContext *state)
{
    TextInfo *text_info = &state->text_info;
    for (int i = 0; i < text_info->n_furi_groups; i++)
        position_furi_group(state, &text_info->furi_groups[i]);
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

static bool prepare_furi_groups(RenderContext *state)
{
    TextInfo *text_info = &state->text_info;
    if (!text_info->n_furi_groups)
        return true;

    ass_shaper_set_base_direction(state->furi_shaper,
            ass_resolve_base_direction(state->font_encoding));

    for (int i = 0; i < text_info->n_furi_groups; i++) {
        FuriGroup *group = &text_info->furi_groups[i];
        TextInfo furi_text = furi_group_text_info(group);

        split_style_runs_list(group->glyphs, group->length);
        ass_shaper_find_runs(state->furi_shaper, state->renderer,
                             group->glyphs, group->length);
        if (!ass_shaper_shape(state->furi_shaper, &furi_text))
            return false;

        retrieve_glyphs_from_list(state, group->glyphs, group->length);
        apply_furi_group_layout(state, group);
        if (!reorder_furi_group(state, group))
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
    if (!text_info->n_furi_groups)
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
        int line = 0;
        double base_left, base_right, base_top;
        double furi_top, furi_bottom;
        if (!furi_base_metrics(state, group, &base_left, &base_right,
                               &base_top, &line))
            continue;
        if (line < 0 || line >= text_info->n_lines)
            continue;
        if (!furi_group_visual_bbox(group, &furi_top, &furi_bottom))
            continue;

        // Per visual line, reserve the maximum furi overhang relative to
        // the base text top. Do not accumulate multiple groups on a line.
        above[line] = FFMAX(above[line], FFMAX(0.0, base_top - furi_top));
        below[line] = FFMAX(below[line], FFMAX(0.0, furi_bottom - base_top));
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

static bool glyph_is_separator(const GlyphInfo *info)
{
    return info->symbol == ' ' || info->symbol == NBSP || info->symbol == '\n';
}

static bool distort_params_match(const GlyphInfo *a, const GlyphInfo *b)
{
    if (!a->distort_enabled || !b->distort_enabled)
        return false;
    return a->distort_u1 == b->distort_u1 && a->distort_v1 == b->distort_v1 &&
           a->distort_u2 == b->distort_u2 && a->distort_v2 == b->distort_v2 &&
           a->distort_u3 == b->distort_u3 && a->distort_v3 == b->distort_v3;
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

    // Corner pins: P0 fixed at (0,0); P1=(u1,v1) top-right; P2=(u2,v2) bottom-right; P3=(u3,v3) bottom-left.
    ASS_DistortParams params = {
        .u1 = info->distort_u1, .v1 = info->distort_v1,
        .u2 = info->distort_u2, .v2 = info->distort_v2,
        .u3 = info->distort_u3, .v3 = info->distort_v3,
    };
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

            ASS_DVector mapped = ass_distort_map_point(&params, min_x, min_y,
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
    info->bbox.x_min = ass_lrint(distorted->cbox.x_min * scale_x + off_x);
    info->bbox.y_min = ass_lrint(distorted->cbox.y_min * scale_y + off_y);
    info->bbox.x_max = ass_lrint(distorted->cbox.x_max * scale_x + off_x);
    info->bbox.y_max = ass_lrint(distorted->cbox.y_max * scale_y + off_y);

    return true;
}

static void apply_distortion(RenderContext *state)
{
    TextInfo *text_info = &state->text_info;
    FriBidiStrIndex *cmap = ass_shaper_get_reorder_map(state->shaper);
    if (!cmap)
        return;

    for (int i = 0; i < text_info->length; i++) {
        GlyphInfo *root = text_info->glyphs + cmap[i];
        if (glyph_is_separator(root) || !root->distort_enabled)
            continue;

        double min_x = DBL_MAX, min_y = DBL_MAX;
        double max_x = -DBL_MAX, max_y = -DBL_MAX;
        bool has_bbox = false;

        int end = i;
        while (end < text_info->length) {
            GlyphInfo *cur = text_info->glyphs + cmap[end];
            if (cur->linebreak && end != i)
                break;
            if (glyph_is_separator(cur))
                break;
            if (!distort_params_match(root, cur))
                break;

            for (GlyphInfo *g = cur; g; g = g->next) {
                if (!g->outline || !g->outline->outline[0].n_points)
                    continue;
                double pen_x = g->pos.x;
                double pen_y = g->pos.y;
                double gminx = (double) g->bbox.x_min + pen_x;
                double gmaxx = (double) g->bbox.x_max + pen_x;
                double gminy = (double) g->bbox.y_min + pen_y;
                double gmaxy = (double) g->bbox.y_max + pen_y;
                // Include advance span so bounding box covers inter-glyph spacing,
                // matching VSFilter's broader per-word box.
                double adv_x = pen_x + g->advance.x;
                double adv_y = pen_y + g->advance.y;
                gminx = FFMIN(gminx, adv_x);
                gmaxx = FFMAX(gmaxx, adv_x);
                gminy = FFMIN(gminy, adv_y);
                gmaxy = FFMAX(gmaxy, adv_y);
                min_x = FFMIN(min_x, gminx);
                max_x = FFMAX(max_x, gmaxx);
                min_y = FFMIN(min_y, gminy);
                max_y = FFMAX(max_y, gmaxy);
                has_bbox = true;
            }

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
            if (cur->skip || !cur->outline)
                continue;
            distort_warp_glyph(cur, min_x, min_y, max_x, max_y);
        }

        i = end - 1;
    }
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
    int halign = state->alignment & 3;
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
            info->shift.x = info->pos.x + double_to_d6(device_x + jitter_dx - center.x +
                    info->shadow_x * state->border_scale_x /
                    render_priv->par_scale_x);
            info->shift.y = info->pos.y + double_to_d6(device_y + jitter_dy - center.y +
                    info->shadow_y * state->border_scale_y);
            info = info->next;
        }
    }
}

static void calculate_rotation_params(RenderContext *state, ASS_DRect *bbox,
                                      double device_x, double device_y)
{
    ASS_Renderer *render_priv = state->renderer;
    TextInfo *text_info = &state->text_info;
    ASS_DVector center;
    if (state->have_origin) {
        center.x = x2scr_pos(render_priv, state->org_x);
        center.y = y2scr_pos(render_priv, state->org_y);
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

    // For BorderStyle=4, the "shadow" is the opaque background box. If there
    // is no separate shadow bitmap, anchor its gradient to the character box
    // so the box uses the same line-space rectangle.
    if (state->border_style == 4) {
        for (int i = 0; i < text_info->n_lines; i++) {
            LineInfo *ln = &text_info->lines[i];
            if (!ln->grad_shadow_valid && ln->grad_char_valid) {
                ln->grad_shadow = ln->grad_char;
                ln->grad_shadow_valid = true;
            }
        }
    }
}

static bool text_needs_rgba(const TextInfo *text_info)
{
    for (unsigned i = 0; i < text_info->n_bitmaps; i++) {
        const CombinedBitmapInfo *info = &text_info->combined_bitmaps[i];
        if (!info->bitmap_count || (!info->bm && !info->bm_o && !info->bm_s))
            continue;
        for (int layer = 0; layer < 4; layer++) {
            if (info->image_fill.layer[layer].enabled)
                return true;
            const GradientValues *vals = &info->gradient.layer[layer];
            if (vals->color_enabled || vals->alpha_enabled)
                return true;
        }
    }

    return false;
}

static void render_glyph_list_to_bitmaps(RenderContext *state,
                                         GlyphInfo *glyphs, int length,
                                         double device_x, double device_y,
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
        if (info->skip)
            continue;

        for (; info; info = info->next) {
            int flags = 0;
            if (info->border_style == 3)
                flags |= FILTER_BORDER_STYLE_3;
            if (glyph_border_max_x(info) || glyph_border_max_y(info))
                flags |= FILTER_NONZERO_BORDER;
            if (has_multi_border_layers(info->border_layers) &&
                    info->border_style != 3)
                flags |= FILTER_MULTI_BORDER;
            if (info->shadow_x || info->shadow_y)
                flags |= FILTER_NONZERO_SHADOW;
            if (flags & FILTER_NONZERO_SHADOW &&
                (info->effect_type == EF_KARAOKE_KF ||
                 info->effect_type == EF_KARAOKE_KO ||
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

                memcpy(&current_info->c, &info->c, sizeof(info->c));
                memcpy(&current_info->base_c, &info->c, sizeof(info->c));
                current_info->gradient = info->gradient;
                current_info->image_fill = info->image_fill;
                memcpy(current_info->border_layers, info->border_layers,
                       sizeof(current_info->border_layers));
                current_info->fade = info->fade;
                current_info->line = info->line;
                current_info->from_drawing = info->drawing_text.str != NULL;
                current_info->draw_sub_x = 0;
                current_info->draw_sub_y = 0;
                for (int j = 0; j < 4; j++)
                    ass_apply_fade(&current_info->c[j], info->fade);

                current_info->effect_type = info->effect_type;
                current_info->effect_timing = info->effect_timing;
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

                (*nb_bitmaps)++;
                new_run = false;
            }
            assert(current_info);

            ASS_Vector pos, pos_o;
            double jitter_dx = info->has_jitter ? info->jitter_dx : 0.0;
            double jitter_dy = info->has_jitter ? info->jitter_dy : 0.0;
            info->pos.x = double_to_d6(device_x + jitter_dx +
                                       d6_to_double(info->pos.x) * render_priv->par_scale_x);
            info->pos.y = double_to_d6(device_y + jitter_dy) + info->pos.y;
            if (current_info->from_drawing && !current_info->bitmap_count) {
                current_info->draw_sub_x = (uint8_t) ((info->pos.x >> 3) & 7);
                current_info->draw_sub_y = (uint8_t) ((info->pos.y >> 3) & 7);
            }
            get_bitmap_glyph(state, info, &current_info->leftmost_x, &pos, &pos_o,
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

// Convert glyphs to bitmaps, combine them, apply blur, generate shadows.
static void render_and_combine_glyphs(RenderContext *state,
                                      double device_x, double device_y)
{
    ASS_Renderer *render_priv = state->renderer;
    TextInfo *text_info = &state->text_info;
    int left = render_priv->settings.left_margin;
    device_x = (device_x - left) * render_priv->par_scale_x + left;
    unsigned nb_bitmaps = 0;
    CombinedBitmapInfo *combined_info = text_info->combined_bitmaps;
    render_glyph_list_to_bitmaps(state, text_info->glyphs, text_info->length,
                                 device_x, device_y, &nb_bitmaps,
                                 &combined_info);
    for (int i = 0; i < text_info->n_furi_groups; i++) {
        FuriGroup *group = &text_info->furi_groups[i];
        render_glyph_list_to_bitmaps(state, group->glyphs, group->length,
                                     device_x, device_y, &nb_bitmaps,
                                     &combined_info);
    }

    for (int i = 0; i < nb_bitmaps; i++) {
        CombinedBitmapInfo *info = &combined_info[i];
        if (!info->bitmap_count) {
            free(info->bitmaps);
            continue;
        }

        if (info->effect_type == EF_KARAOKE_KF)
            info->effect_timing = lround(d6_to_double(info->leftmost_x) +
                d6_to_double(info->effect_timing) * render_priv->par_scale_x);

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
                continue;
            }
            if (!ass_composite_construct(&key, val, render_priv)) {
                free(info->bitmaps);
                free(val);
                info->bitmaps = NULL;
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
            continue;
        }

        CompositeHashKey key;
        key.filter = info->filter;
        key.bitmap_count = info->bitmap_count;
        key.bitmaps = info->bitmaps;
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
    if (!(flags & FILTER_NONZERO_BORDER) || (flags & FILTER_BORDER_STYLE_3))
        ass_synth_blur(&render_priv->engine, &v->bm, k->filter.be, r2x, r2y);
    ass_synth_blur(&render_priv->engine, &v->bm_o, k->filter.be, r2x, r2y);
    for (int layer = 0; layer < ASS_BORDER_LAYERS_MAX - 1; layer++)
        ass_synth_blur(&render_priv->engine, &v->bm_border[layer],
                       k->filter.be, r2x, r2y);

    if (multi_border) {
        // Multi-border uses the existing global blur/be treatment on each
        // expanded mask, then cuts rings from the blurred masks. This keeps
        // transparent outer borders from compositing under inner borders
        // without introducing per-layer blur state.
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
    } else if (!(flags & FILTER_FILL_IN_BORDER) && !(flags & FILTER_FILL_IN_SHADOW)) {
        ass_fix_outline(&v->bm, &v->bm_o);
    }

    if (flags & FILTER_NONZERO_SHADOW) {
        if (flags & FILTER_NONZERO_BORDER) {
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

static void add_background(RenderContext *state, EventImages *event_images,
                           ASS_ImageRGBA **rgba_head)
{
    ASS_Renderer *render_priv = state->renderer;
    int size_x = state->shadow_x > 0 ?
        lround(state->shadow_x * state->border_scale_x) : 0;
    int size_y = state->shadow_y > 0 ?
        lround(state->shadow_y * state->border_scale_y) : 0;
    int left    = event_images->left - size_x;
    int top     = event_images->top  - size_y;
    int right   = event_images->left + event_images->width  + size_x;
    int bottom  = event_images->top  + event_images->height + size_y;
    left        = FFMINMAX(left,   0, render_priv->width);
    top         = FFMINMAX(top,    0, render_priv->height);
    right       = FFMINMAX(right,  0, render_priv->width);
    bottom      = FFMINMAX(bottom, 0, render_priv->height);
    int w = right - left;
    int h = bottom - top;
    if (w < 1 || h < 1)
        return;
    void *nbuffer = ass_aligned_alloc(1, w * h, false);
    if (!nbuffer)
        return;
    memset(nbuffer, 0xFF, w * h);
    uint32_t clr = state->c[3];
    ass_apply_fade(&clr, state->fade);
    ASS_Image *img = my_draw_bitmap(nbuffer, w, h, w, left, top,
                                    clr, NULL);
    if (img) {
        img->next = event_images->imgs;
        event_images->imgs = img;
    }
    if (rgba_head) {
        uint8_t alpha = 255 - _a(clr);
        ASS_ImageRGBA *rimg =
            ass_rgba_image_alloc(render_priv, w, h, left, top,
                                 IMAGE_TYPE_SHADOW);
        if (rimg) {
            int stride = rimg->stride;
            uint8_t *rgba = rimg->rgba;
            uint8_t pr = (uint8_t) ((_r(clr) * alpha + 127) / 255);
            uint8_t pg = (uint8_t) ((_g(clr) * alpha + 127) / 255);
            uint8_t pb = (uint8_t) ((_b(clr) * alpha + 127) / 255);
            for (int y = 0; y < h; y++) {
                uint8_t *row = rgba + y * stride;
                for (int x = 0; x < w; x++) {
                    row[4 * x + 0] = pr;
                    row[4 * x + 1] = pg;
                    row[4 * x + 2] = pb;
                    row[4 * x + 3] = alpha;
                }
            }
            rimg->next = *rgba_head;
            *rgba_head = rimg;
            event_images->imgs_rgba = rimg;
        }
    }
}

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

    free_render_context(state);
    init_render_context(state, event);

    if (!parse_events(state, event))
        return false;

    TextInfo *text_info = &state->text_info;
    if (text_info->length == 0) {
        // no valid symbols in the event; this can be smth like {comment}
        free_render_context(state);
        return false;
    }

    if (state->motion.type != MOTION_NONE) {
        ASS_DVector pos = evaluate_motion(state);
        state->pos_x = pos.x;
        state->pos_y = pos.y;
    }

    split_style_runs(state);

    // Find shape runs and shape text
    ass_shaper_set_base_direction(state->shaper,
            ass_resolve_base_direction(state->font_encoding));
    ass_shaper_find_runs(state->shaper, render_priv, text_info->glyphs,
            text_info->length);
    if (!ass_shaper_shape(state->shaper, text_info)) {
        ass_msg(render_priv->library, MSGL_ERR, "Failed to shape text");
        free_render_context(state);
        return false;
    }

    retrieve_glyphs(state);

    if (!prepare_furi_groups(state)) {
        ass_msg(render_priv->library, MSGL_ERR, "Failed to shape furi text");
        ass_shaper_cleanup(state->shaper, text_info);
        free_render_context(state);
        return false;
    }

    preliminary_layout(state);

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

    // wrap lines
    wrap_lines_smart(state, max_text_width);

    // depends on glyph x coordinates being monotonous within runs, so it should be done before reorder
    ass_process_karaoke_effects(state);

    reorder_text(state);

    align_lines(state, max_text_width);

    if (text_info->n_furi_groups)
        position_furi_groups(state);

    if (!expand_furi_line_metrics(state)) {
        ass_msg(render_priv->library, MSGL_ERR, "Failed to expand furi line metrics");
        ass_shaper_cleanup(state->shaper, text_info);
        free_render_context(state);
        return false;
    }

    apply_distortion(state);

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
        get_base_point(&bbox_origin, state->alignment, &origin_x, &origin_y);

    apply_baseline_shear(state);

    if (rotate_baseline) {
        apply_baseline_rotation(state, origin_x, origin_y);
        compute_string_bbox(text_info, &bbox);
    }
    if (text_info->n_furi_groups)
        position_furi_groups(state);

    ASS_DRect render_bbox = bbox;
    add_furi_to_bbox(text_info, &render_bbox);
    ASS_DRect *bbox_for_origin = rotate_baseline ? &bbox_origin : &render_bbox;
    ASS_DRect *bbox_for_position = &render_bbox;

    // determine device coordinates for text
    double device_x = 0;
    double device_y = 0;

    // handle positioned events first: an event can be both positioned and
    // scrolling, and the scrolling effect overrides the position on one axis
    if (state->evt_type & EVENT_POSITIONED) {
        double base_x = 0;
        double base_y = 0;
        get_base_point(bbox_for_position, state->alignment, &base_x, &base_y);
        device_x =
            x2scr_pos(render_priv, state->pos_x) - base_x;
        device_y =
            y2scr_pos(render_priv, state->pos_y) - base_y;
    }

    // x coordinate
    if (state->evt_type & EVENT_HSCROLL) {
        if (state->scroll_direction == SCROLL_RL)
            device_x =
                x2scr_pos(render_priv,
                      render_priv->track->PlayResX -
                      state->scroll_shift);
        else if (state->scroll_direction == SCROLL_LR)
            device_x =
                x2scr_pos(render_priv, state->scroll_shift) -
                (bbox_for_position->x_max - bbox_for_position->x_min);
    } else if (!(state->evt_type & EVENT_POSITIONED)) {
        device_x = x2scr_left(state, MarginL);
    }

    // y coordinate
    if (state->evt_type & EVENT_VSCROLL) {
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
    } else if (!(state->evt_type & EVENT_POSITIONED)) {
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

    update_glyph_jitter_offsets(state);

    // fix clip coordinates
    if (state->explicit || !render_priv->settings.use_margins) {
        state->clip_x0 =
            lround(x2scr_pos_scaled(render_priv, state->clip_x0));
        state->clip_x1 =
            lround(x2scr_pos_scaled(render_priv, state->clip_x1));
        state->clip_y0 =
            lround(y2scr_pos(render_priv, state->clip_y0));
        state->clip_y1 =
            lround(y2scr_pos(render_priv, state->clip_y1));

        if (state->explicit) {
            // we still need to clip against screen boundaries
            int zx = render_priv->settings.left_margin;
            int zy = render_priv->settings.top_margin;
            int sx = zx + render_priv->frame_content_width;
            int sy = zy + render_priv->frame_content_height;

            state->clip_x0 = FFMAX(state->clip_x0, zx);
            state->clip_y0 = FFMAX(state->clip_y0, zy);
            state->clip_x1 = FFMIN(state->clip_x1, sx);
            state->clip_y1 = FFMIN(state->clip_y1, sy);
        }
    } else {
        // no \clip (explicit==0) and use_margins => only clip to screen with margins
        state->clip_x0 = 0;
        state->clip_y0 = 0;
        state->clip_x1 = render_priv->settings.frame_width;
        state->clip_y1 = render_priv->settings.frame_height;
    }

    if (state->evt_type & EVENT_VSCROLL) {
        int y0 = lround(y2scr_pos(render_priv, state->scroll_y0));
        int y1 = lround(y2scr_pos(render_priv, state->scroll_y1));

        state->clip_y0 = FFMAX(state->clip_y0, y0);
        state->clip_y1 = FFMIN(state->clip_y1, y1);
    }

    calculate_rotation_params(state, bbox_for_origin, device_x, device_y);

    render_and_combine_glyphs(state, device_x, device_y);
    compute_line_gradient_rects(state);
    state->needs_rgba = text_needs_rgba(text_info);

    memset(event_images, 0, sizeof(*event_images));
    // VSFilter does *not* shift lines with a border > margin to be within the
    // frame, so negative values for top and left may occur
    if (text_info->n_furi_groups) {
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
        (device_x + render_bbox.x_min) * render_priv->par_scale_x - text_info->border_x + 0.5;
    event_images->width =
        (render_bbox.x_max - render_bbox.x_min) * render_priv->par_scale_x
        + 2 * text_info->border_x + 0.5;
    event_images->detect_collisions = state->detect_collisions;
    event_images->shift_direction = (valign == VALIGN_SUB) ? -1 : 1;
    event_images->event = event;
    bool want_rgba = rgba_out != NULL;
    event_images->needs_rgba = state->needs_rgba;
    event_images->imgs_rgba = NULL;
    ASS_ImageRGBA **rgba_ptr = want_rgba ? &event_images->imgs_rgba : NULL;
    event_images->imgs = render_text(state, rgba_ptr);

    if (state->border_style == 4)
        add_background(state, event_images,
                       rgba_out ? &event_images->imgs_rgba : NULL);

    if (rgba_out)
        *rgba_out = event_images->imgs_rgba;

    ass_shaper_cleanup(state->shaper, text_info);
    free_render_context(state);

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
    render_priv->time = now;
    render_priv->frame_needs_rgba = false;
    render_priv->rgba_output_limit_hit = false;
    render_priv->rgba_output_size = 0;

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
            if (cnt >= priv->eimg_size) {
                priv->eimg_size += 100;
                priv->eimg =
                    realloc(priv->eimg,
                            priv->eimg_size * sizeof(EventImages));
            }
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

    // concat lists
    ASS_Image **tail = &priv->images_root;
    for (int i = 0; i < cnt; i++) {
        ASS_Image *cur = priv->eimg[i].imgs;
        while (cur) {
            *tail = cur;
            tail = &cur->next;
            cur = cur->next;
        }
    }
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
        ASS_ImagePriv *priv = (ASS_ImagePriv *) img;
        img = img->next;
        ass_cache_dec_ref(priv->source);
        ass_aligned_free(priv->buffer);
        free(priv);
    } while (img);
}
