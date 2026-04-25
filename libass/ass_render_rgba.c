/*
 * Copyright (C) 2025 libass contributors
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

#include <stdlib.h>

#include "ass_render.h"
#include "ass_utils.h"
#include "ass_priv.h"

#define _r(c)   ((c) >> 24)
#define _g(c)   (((c) >> 16) & 0xFF)
#define _b(c)   (((c) >> 8) & 0xFF)
#define _a(c)   ((c) & 0xFF)

static bool rgba_alloc_size(int stride, int h, unsigned align, size_t *size)
{
    if (stride <= 0 || h <= 0)
        return false;
    if ((size_t) stride > (SIZE_MAX - align) / (size_t) h)
        return false;
    if ((size_t) stride > (size_t) (INT_MAX - align) / (size_t) h)
        return false;
    *size = (size_t) stride * h + align;
    return true;
}

static bool rgba_stride_size(ASS_Renderer *priv, int w, int h,
                             int *stride, size_t *size)
{
    if (!priv || w <= 0 || h <= 0 || w > INT_MAX / 4)
        return false;

    unsigned align = 1U << priv->engine.align_order;
    size_t raw_stride = (size_t) w * 4;
    size_t aligned_stride = ass_align(align, raw_stride);
    if (aligned_stride < raw_stride || aligned_stride > INT_MAX)
        return false;
    if (!rgba_alloc_size((int) aligned_stride, h, align, size))
        return false;

    *stride = (int) aligned_stride;
    return true;
}

static bool rgba_reserve(ASS_Renderer *priv, size_t replace_size,
                         size_t alloc_size)
{
    size_t used = priv->rgba_output_size;
    size_t kept = used > replace_size ? used - replace_size : 0;
    size_t max_size = priv->rgba_output_max_size;

    if (alloc_size > SIZE_MAX - kept ||
        (max_size && (kept > max_size || alloc_size > max_size - kept))) {
        if (!priv->rgba_output_limit_hit) {
            ass_msg(priv->library, MSGL_WARN,
                    "RGBA output memory limit exceeded; dropping RGBA tiles");
            priv->rgba_output_limit_hit = true;
        }
        return false;
    }

    priv->rgba_output_size = kept + alloc_size;
    return true;
}

uint8_t *ass_rgba_alloc_buffer_stride(ASS_Renderer *priv, int stride, int h,
                                      size_t replace_size,
                                      size_t *alloc_size)
{
    if (!priv)
        return NULL;

    unsigned align = 1U << priv->engine.align_order;
    size_t size;
    if (!rgba_alloc_size(stride, h, align, &size))
        return NULL;

    size_t old_used = priv->rgba_output_size;
    if (!rgba_reserve(priv, replace_size, size))
        return NULL;

    uint8_t *buffer = ass_aligned_alloc(align, size, false);
    if (!buffer) {
        priv->rgba_output_size = old_used;
        return NULL;
    }

    if (alloc_size)
        *alloc_size = size;
    return buffer;
}

uint8_t *ass_rgba_alloc_buffer(ASS_Renderer *priv, int w, int h,
                               size_t replace_size, int *stride,
                               size_t *alloc_size)
{
    int rgba_stride;
    size_t size;
    if (!rgba_stride_size(priv, w, h, &rgba_stride, &size))
        return NULL;

    size_t old_used = priv->rgba_output_size;
    if (!rgba_reserve(priv, replace_size, size))
        return NULL;

    unsigned align = 1U << priv->engine.align_order;
    uint8_t *buffer = ass_aligned_alloc(align, size, false);
    if (!buffer) {
        priv->rgba_output_size = old_used;
        return NULL;
    }

    if (stride)
        *stride = rgba_stride;
    if (alloc_size)
        *alloc_size = size;
    return buffer;
}

ASS_ImageRGBA *ass_rgba_image_alloc(ASS_Renderer *priv, int w, int h,
                                    int dst_x, int dst_y, int type)
{
    int stride;
    size_t alloc_size;
    uint8_t *buffer =
        ass_rgba_alloc_buffer(priv, w, h, 0, &stride, &alloc_size);
    if (!buffer)
        return NULL;

    ASS_ImageRGBAPriv *img = malloc(sizeof(*img));
    if (!img) {
        if (priv && priv->rgba_output_size >= alloc_size)
            priv->rgba_output_size -= alloc_size;
        ass_aligned_free(buffer);
        return NULL;
    }

    img->result.w = w;
    img->result.h = h;
    img->result.stride = stride;
    img->result.rgba = buffer;
    img->result.dst_x = dst_x;
    img->result.dst_y = dst_y;
    img->result.type = type;
    img->result.next = NULL;
    img->buffer = buffer;
    img->alloc_size = alloc_size;
    return &img->result;
}

void ass_rgba_image_replace_buffer(ASS_ImageRGBA *img, uint8_t *buffer,
                                   size_t alloc_size, int w, int h,
                                   int stride)
{
    ASS_ImageRGBAPriv *priv = (ASS_ImageRGBAPriv *) img;
    ass_aligned_free(priv->buffer);
    priv->buffer = buffer;
    priv->alloc_size = alloc_size;
    img->w = w;
    img->h = h;
    img->stride = stride;
    img->rgba = buffer;
}

void ass_rgba_image_free(ASS_Renderer *priv, ASS_ImageRGBA *img)
{
    if (!img)
        return;

    ASS_ImageRGBAPriv *rgba_priv = (ASS_ImageRGBAPriv *) img;
    if (priv) {
        if (priv->rgba_output_size >= rgba_priv->alloc_size)
            priv->rgba_output_size -= rgba_priv->alloc_size;
        else
            priv->rgba_output_size = 0;
    }
    ass_aligned_free(rgba_priv->buffer);
    free(rgba_priv);
}

static bool clip_rgba_to_frame(ASS_Renderer *priv, ASS_ImageRGBA *img)
{
    if (!priv || !img || !img->rgba || img->w <= 0 || img->h <= 0 ||
        img->stride <= 0)
        return false;

    int64_t x0 = img->dst_x;
    int64_t y0 = img->dst_y;
    int64_t x1 = x0 + img->w;
    int64_t y1 = y0 + img->h;
    if (x1 <= 0 || y1 <= 0 || x0 >= priv->width || y0 >= priv->height)
        return false;

    if (x0 < 0) {
        int clip = (int) -x0;
        img->rgba += (size_t) clip * 4;
        img->w -= clip;
        img->dst_x = 0;
    }
    if (y0 < 0) {
        int clip = (int) -y0;
        img->rgba += (size_t) clip * img->stride;
        img->h -= clip;
        img->dst_y = 0;
    }
    if ((int64_t) img->dst_x + img->w > priv->width)
        img->w = priv->width - img->dst_x;
    if ((int64_t) img->dst_y + img->h > priv->height)
        img->h = priv->height - img->dst_y;

    if (img->w <= 0 || img->h <= 0)
        return false;
    for (int y = 0; y < img->h; y++) {
        uint8_t *row = img->rgba + (size_t) y * img->stride;
        for (int x = 0; x < img->w; x++)
            if (row[4 * x + 3])
                return true;
    }

    return false;
}

static ASS_ImageRGBA *convert_images_to_rgba(ASS_Renderer *priv, ASS_Image *imgs)
{
    ASS_ImageRGBA *head = NULL;
    ASS_ImageRGBA **tail = &head;
    for (ASS_Image *cur = imgs; cur; cur = cur->next) {
        if (cur->w <= 0 || cur->h <= 0 || cur->stride <= 0 || !cur->bitmap)
            continue;
        ASS_ImageRGBA *node = ass_rgba_image_alloc(priv, cur->w, cur->h,
                                                   cur->dst_x, cur->dst_y,
                                                   cur->type);
        if (!node)
            continue;
        int stride = node->stride;
        uint8_t *rgba = node->rgba;
        uint32_t color = cur->color;
        uint8_t base_alpha = 255 - _a(color);
        for (int y = 0; y < cur->h; y++) {
            const uint8_t *src = cur->bitmap + y * cur->stride;
            uint8_t *dst = rgba + y * stride;
            for (int x = 0; x < cur->w; x++) {
                uint8_t cov = src[x];
                uint8_t A = (uint8_t) ((cov * base_alpha + 127) / 255);
                dst[4 * x + 0] = (uint8_t) ((_r(color) * A + 127) / 255);
                dst[4 * x + 1] = (uint8_t) ((_g(color) * A + 127) / 255);
                dst[4 * x + 2] = (uint8_t) ((_b(color) * A + 127) / 255);
                dst[4 * x + 3] = A;
            }
        }
        if (!clip_rgba_to_frame(priv, node)) {
            ass_rgba_image_free(priv, node);
            continue;
        }
        *tail = node;
        tail = &node->next;
    }
    return head;
}

ASS_ImageRGBA *ass_render_frame_rgba(ASS_Renderer *priv, ASS_Track *track,
                                     long long now, int *detect_change)
{
    ASS_ImageRGBA *rgba_root = NULL;
    ASS_ImageRGBA **rgba_tail = &rgba_root;

    if (!ass_start_frame(priv, track, now)) {
        if (detect_change)
            *detect_change = 2;
        return NULL;
    }

    int cnt = 0;
    for (int i = 0; i < track->n_events; i++) {
        ASS_Event *event = track->events + i;
        if ((event->Start <= now) && (now < (event->Start + event->Duration))) {
            if (cnt >= priv->eimg_size) {
                priv->eimg_size += 100;
                priv->eimg = realloc(priv->eimg,
                                     priv->eimg_size * sizeof(EventImages));
            }
            if (ass_render_event(&priv->state, event, priv->eimg + cnt,
                                 &priv->eimg[cnt].imgs_rgba)) {
                priv->frame_needs_rgba |= priv->eimg[cnt].needs_rgba;
                cnt++;
            }
        }
    }

    if (cnt > 0)
        qsort(priv->eimg, cnt, sizeof(EventImages), ass_cmp_event_layer);

    EventImages *last = priv->eimg;
    for (int i = 1; i < cnt; i++)
        if (last->event->Layer != priv->eimg[i].event->Layer) {
            ass_fix_collisions(priv, last, priv->eimg + i - last);
            last = priv->eimg + i;
        }
    if (cnt > 0)
        ass_fix_collisions(priv, last, priv->eimg + cnt - last);

    ASS_Image **tail = &priv->images_root;
    for (int i = 0; i < cnt; i++) {
        ASS_Image *cur = priv->eimg[i].imgs;
        while (cur) {
            *tail = cur;
            tail = &cur->next;
            cur = cur->next;
        }
        ASS_ImageRGBA *rcur = priv->eimg[i].imgs_rgba;
        while (rcur) {
            ASS_ImageRGBA *next = rcur->next;
            rcur->next = NULL;
            if (clip_rgba_to_frame(priv, rcur)) {
                *rgba_tail = rcur;
                rgba_tail = &rcur->next;
            } else {
                ass_rgba_image_free(priv, rcur);
            }
            rcur = next;
        }
        priv->eimg[i].imgs_rgba = NULL;
    }

    ass_frame_ref(priv->images_root);

    if (detect_change)
        *detect_change = ass_detect_change(priv);

    ass_frame_unref(priv->prev_images_root);
    priv->prev_images_root = NULL;

    if (!rgba_root && priv->images_root && !priv->frame_needs_rgba)
        rgba_root = convert_images_to_rgba(priv, priv->images_root);

    if (track->parser_priv->prune_delay >= 0)
        ass_prune_events(track, now - track->parser_priv->prune_delay);

    return rgba_root;
}

void ass_free_images_rgba(ASS_ImageRGBA *img)
{
    while (img) {
        ASS_ImageRGBA *next = img->next;
        ass_rgba_image_free(NULL, img);
        img = next;
    }
}
