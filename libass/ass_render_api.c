/*
 * Copyright (C) 2006 Evgeniy Stepanov <eugeni.stepanov@gmail.com>
 * Copyright (C) 2010 Grigori Goronzy <greg@geekmind.org>
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

#include <limits.h>
#include <stdint.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "ass_render.h"
#include "ass_utils.h"

static void ass_reconfigure(ASS_Renderer *priv)
{
    ASS_Settings *settings = &priv->settings;

    priv->render_id++;
    ass_cache_empty(priv->cache.composite_cache);
    ass_cache_empty(priv->cache.bitmap_cache);
    ass_cache_empty(priv->cache.outline_cache);

    priv->width = settings->frame_width;
    priv->height = settings->frame_height;
    priv->frame_content_width = settings->frame_width - settings->left_margin -
        settings->right_margin;
    priv->frame_content_height = settings->frame_height - settings->top_margin -
        settings->bottom_margin;
    priv->fit_width =
        (long long) priv->frame_content_width * priv->height >=
        (long long) priv->frame_content_height * priv->width ?
            priv->width :
            (double) priv->frame_content_width * priv->height / priv->frame_content_height;
    priv->fit_height =
        (long long) priv->frame_content_width * priv->height <=
        (long long) priv->frame_content_height * priv->width ?
            priv->height :
            (double) priv->frame_content_height * priv->width / priv->frame_content_width;
}

static bool ass_tag_image_format_supported(ASS_TagImageFormat format)
{
    return format == ASS_TAG_IMAGE_FORMAT_PNG ||
           format == ASS_TAG_IMAGE_FORMAT_JPEG ||
           format == ASS_TAG_IMAGE_FORMAT_WEBP;
}

static char *ass_normalize_tag_image_path(const char *path)
{
    if (!path || !*path)
        return NULL;

    const char *start = path;
    const char *end = path + strlen(path);

    while (start < end && isspace((unsigned char) *start))
        start++;
    while (end > start && isspace((unsigned char) end[-1]))
        end--;

    if (end - start >= 2) {
        char first = start[0];
        char last = end[-1];
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            start++;
            end--;
        }
    }

    while (start < end && isspace((unsigned char) *start))
        start++;
    while (end > start && isspace((unsigned char) end[-1]))
        end--;

    size_t len = end - start;
    if (!len)
        return NULL;

    char *norm = malloc(len + 1);
    if (!norm)
        return NULL;

    for (size_t i = 0; i < len; i++) {
        char c = start[i];
        norm[i] = (c == '\\') ? '/' : c;
    }
    norm[len] = '\0';

    while (norm[0] == '.' && norm[1] == '/')
        memmove(norm, norm + 2, strlen(norm + 2) + 1);

    if (!norm[0]) {
        free(norm);
        return NULL;
    }

    return norm;
}

static bool ass_tag_image_path_absolute(const char *path)
{
    if (!path || !*path)
        return false;
    if (path[0] == '/')
        return true;
    size_t len = strlen(path);
    if (len >= 3 && isalpha((unsigned char) path[0]) &&
        path[1] == ':' && path[2] == '/')
        return true;
    return false;
}

static ASS_TagImageEntry *ass_find_tag_image_mutable(ASS_Renderer *priv,
                                                     const char *norm_path)
{
    for (ASS_TagImageEntry *cur = priv ? priv->tag_images : NULL; cur; cur = cur->next) {
        if (!strcmp(cur->key, norm_path))
            return cur;
    }
    return NULL;
}

static const ASS_TagImageEntry *ass_find_tag_image(const ASS_Renderer *priv,
                                                   const char *norm_path)
{
    for (const ASS_TagImageEntry *cur = priv ? priv->tag_images : NULL; cur; cur = cur->next) {
        if (!strcmp(cur->key, norm_path))
            return cur;
    }
    return NULL;
}

static char *ass_track_base_dir(const ASS_Track *track)
{
    if (!track || !track->name)
        return NULL;

    char *norm = ass_normalize_tag_image_path(track->name);
    if (!norm)
        return NULL;

    char *last_sep = strrchr(norm, '/');
    if (!last_sep) {
        free(norm);
        return NULL;
    }

    last_sep[1] = '\0';
    return norm;
}

void ass_clear_tag_images_internal(ASS_Renderer *priv)
{
    if (!priv)
        return;

    ASS_TagImageEntry *cur = priv->tag_images;
    while (cur) {
        ASS_TagImageEntry *next = cur->next;
        free(cur->key);
        free(cur->rgba);
        free(cur);
        cur = next;
    }
    priv->tag_images = NULL;
}

void ass_clear_tag_images(ASS_Renderer *priv)
{
    ass_clear_tag_images_internal(priv);
}

int ass_set_tag_image_rgba(ASS_Renderer *priv, const char *path,
                           ASS_TagImageFormat format, int width, int height,
                           int stride, const uint8_t *rgba)
{
    if (!priv || !path || !rgba || width <= 0 || height <= 0 ||
        width > INT_MAX / 4 ||
        !ass_tag_image_format_supported(format))
        return -1;
    if (stride < width * 4)
        return -1;

    if (height > SIZE_MAX / (size_t) stride)
        return -1;
    size_t size = (size_t) stride * height;
    if (!size)
        return -1;

    char *norm_path = ass_normalize_tag_image_path(path);
    if (!norm_path)
        return -1;

    uint8_t *copy = malloc(size);
    if (!copy) {
        free(norm_path);
        return -1;
    }
    memcpy(copy, rgba, size);

    ASS_TagImageEntry *entry = ass_find_tag_image_mutable(priv, norm_path);
    if (!entry) {
        entry = calloc(1, sizeof(*entry));
        if (!entry) {
            free(copy);
            free(norm_path);
            return -1;
        }
        entry->key = norm_path;
        entry->next = priv->tag_images;
        priv->tag_images = entry;
    } else {
        free(norm_path);
        free(entry->rgba);
    }

    entry->format = format;
    entry->width = width;
    entry->height = height;
    entry->stride = stride;
    entry->rgba = copy;
    return 0;
}

const ASS_TagImageEntry *ass_lookup_tag_image(ASS_Renderer *priv,
                                              ASS_Track *track,
                                              ASS_StringView path)
{
    if (!priv || !path.str || !path.len)
        return NULL;

    char *raw = ass_copy_string(path);
    if (!raw)
        return NULL;

    char *norm = ass_normalize_tag_image_path(raw);
    free(raw);
    if (!norm)
        return NULL;

    const ASS_TagImageEntry *entry = ass_find_tag_image(priv, norm);
    if (!entry && !ass_tag_image_path_absolute(norm)) {
        char *base = ass_track_base_dir(track);
        if (base) {
            size_t len_base = strlen(base);
            size_t len_norm = strlen(norm);
            if (len_norm <= SIZE_MAX - len_base - 1) {
                char *joined = malloc(len_base + len_norm + 1);
                if (joined) {
                    memcpy(joined, base, len_base);
                    memcpy(joined + len_base, norm, len_norm + 1);
                    entry = ass_find_tag_image(priv, joined);
                    free(joined);
                }
            }
            free(base);
        }
    }

    if (!entry) {
        free(norm);
        return NULL;
    }

    if (track && (entry->width > track->PlayResX || entry->height > track->PlayResY)) {
        ass_msg(priv->library, MSGL_WARN,
                "Ignoring \\img '%s': %dx%d exceeds script resolution %dx%d",
                norm, entry->width, entry->height, track->PlayResX, track->PlayResY);
        free(norm);
        return NULL;
    }

    free(norm);
    return entry;
}

void ass_set_frame_size(ASS_Renderer *priv, int w, int h)
{
    if (w <= 0 || h <= 0 || w > FFMIN(INT_MAX, SIZE_MAX) / h)
        w = h = 0;
    if (priv->settings.frame_width != w || priv->settings.frame_height != h) {
        priv->settings.frame_width = w;
        priv->settings.frame_height = h;
        ass_reconfigure(priv);
    }
}

void ass_set_storage_size(ASS_Renderer *priv, int w, int h)
{
    if (w <= 0 || h <= 0 || w > FFMIN(INT_MAX, SIZE_MAX) / h)
        w = h = 0;
    if (priv->settings.storage_width != w ||
        priv->settings.storage_height != h) {
        priv->settings.storage_width = w;
        priv->settings.storage_height = h;
        ass_reconfigure(priv);
    }
}

void ass_set_shaper(ASS_Renderer *priv, ASS_ShapingLevel level)
{
    // select the complex shaper for illegal values
    if (level == ASS_SHAPING_SIMPLE || level == ASS_SHAPING_COMPLEX)
        priv->settings.shaper = level;
    else
        priv->settings.shaper = ASS_SHAPING_COMPLEX;
}

void ass_set_margins(ASS_Renderer *priv, int t, int b, int l, int r)
{
    if (priv->settings.left_margin != l || priv->settings.right_margin != r ||
        priv->settings.top_margin != t || priv->settings.bottom_margin != b) {
        priv->settings.left_margin = l;
        priv->settings.right_margin = r;
        priv->settings.top_margin = t;
        priv->settings.bottom_margin = b;
        ass_reconfigure(priv);
    }
}

void ass_set_use_margins(ASS_Renderer *priv, int use)
{
    priv->settings.use_margins = use;
}

void ass_set_aspect_ratio(ASS_Renderer *priv, double dar, double sar)
{
    ass_set_pixel_aspect(priv, dar / sar);
}

void ass_set_pixel_aspect(ASS_Renderer *priv, double par)
{
    if (par < 0) par = 0;
    if (priv->settings.par != par) {
        priv->settings.par = par;
        ass_reconfigure(priv);
    }
}

void ass_set_font_scale(ASS_Renderer *priv, double font_scale)
{
    if (priv->settings.font_size_coeff != font_scale) {
        priv->settings.font_size_coeff = font_scale;
        ass_reconfigure(priv);
    }
}

void ass_set_hinting(ASS_Renderer *priv, ASS_Hinting ht)
{
    if (priv->settings.hinting != ht) {
        priv->settings.hinting = ht;
        ass_reconfigure(priv);
    }
}

void ass_set_line_spacing(ASS_Renderer *priv, double line_spacing)
{
    priv->settings.line_spacing = line_spacing;
}

void ass_set_line_position(ASS_Renderer *priv, double line_position)
{
    if (priv->settings.line_position != line_position) {
        priv->settings.line_position = line_position;
        ass_reconfigure(priv);
    }
}

void ass_set_fonts(ASS_Renderer *priv, const char *default_font,
                   const char *default_family, int dfp,
                   const char *config, int update)
{
    free(priv->settings.default_font);
    free(priv->settings.default_family);
    priv->settings.default_font = default_font ? strdup(default_font) : 0;
    priv->settings.default_family =
        default_family ? strdup(default_family) : 0;

    ass_reconfigure(priv);

    ass_cache_empty(priv->cache.font_cache);
    ass_cache_empty(priv->cache.metrics_cache);

    if (priv->fontselect)
        ass_fontselect_free(priv->fontselect);
    priv->fontselect = ass_fontselect_init(priv->library, priv->ftlibrary,
            &priv->num_emfonts, default_family, default_font, config, dfp);
}

void ass_set_selective_style_override_enabled(ASS_Renderer *priv, int bits)
{
    if (priv->settings.selective_style_overrides != bits) {
        priv->settings.selective_style_overrides = bits;
        ass_reconfigure(priv);
    }
}

void ass_set_selective_style_override(ASS_Renderer *priv, ASS_Style *style)
{
    ASS_Style *user_style = &priv->user_override_style;
    free(user_style->FontName);
    *user_style = *style;
    user_style->FontName = strdup(user_style->FontName);
    ass_reconfigure(priv);
}

int ass_track_has_rgba(ASS_Track *track)
{
    return track && track->has_rgba;
}

ASS_RenderResult ass_render_frame_auto(ASS_Renderer *priv, ASS_Track *track,
                                       long long now, int *detect_change)
{
    ASS_RenderResult res = {0};

    ASS_ImageRGBA *rgba = ass_render_frame_rgba(priv, track, now, detect_change);
    res.imgs = priv ? priv->images_root : NULL;
    res.imgs_rgba = NULL;
    res.use_rgba = 0;

    if (priv && priv->frame_needs_rgba) {
        res.use_rgba = 1;
        res.imgs_rgba = rgba;
    } else {
        ass_free_images_rgba(rgba);
    }

    return res;
}

int ass_frame_needs_rgba(ASS_Renderer *priv)
{
    return priv && priv->frame_needs_rgba;
}

int ass_fonts_update(ASS_Renderer *render_priv)
{
    // This is just a stub now!
    return 1;
}

void ass_set_cache_limits(ASS_Renderer *render_priv, int glyph_max,
                          int bitmap_max)
{
    render_priv->cache.glyph_max = glyph_max ? glyph_max : GLYPH_CACHE_MAX;

    size_t bitmap_cache, composite_cache;
    if (bitmap_max) {
        bitmap_cache = MEGABYTE * (size_t) bitmap_max;
        composite_cache = bitmap_cache / (COMPOSITE_CACHE_RATIO + 1);
        bitmap_cache -= composite_cache;
    } else {
        bitmap_cache = BITMAP_CACHE_MAX_SIZE;
        composite_cache = COMPOSITE_CACHE_MAX_SIZE;
    }
    render_priv->cache.bitmap_max_size = bitmap_cache;
    render_priv->cache.composite_max_size = composite_cache;
}

ASS_FontProvider *
ass_create_font_provider(ASS_Renderer *priv, ASS_FontProviderFuncs *funcs,
                         void *data)
{
    return ass_font_provider_new(priv->fontselect, funcs, data);
}
