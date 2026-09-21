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

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef NDEBUG
#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#endif
#endif

#include "ass_render.h"
#include "ass_utils.h"
#include "ass_priv.h"

#define _r(c)   ((c) >> 24)
#define _g(c)   (((c) >> 16) & 0xFF)
#define _b(c)   (((c) >> 8) & 0xFF)
#define _a(c)   ((c) & 0xFF)

#ifndef NDEBUG

#define ASS_RGBA_DEBUG_MAGIC UINT64_C(0x4153535f52474241)

typedef struct rgba_debug_allocation {
    uint8_t *base;
    size_t size;
    ASS_ImageRGBAPriv *image;
    uint64_t id;
    const char *creation_site;
    const char *last_operation;
    struct rgba_debug_allocation *next;
} RgbaDebugAllocation;

static RgbaDebugAllocation *rgba_debug_allocations;
static uint64_t rgba_debug_next_id = 1;
static size_t rgba_debug_allocation_count;
static uint64_t rgba_debug_registry_scans;
static uint64_t rgba_debug_registry_scan_steps;

#ifdef _WIN32
static volatile LONG rgba_debug_lock_state;

static void rgba_debug_lock(void)
{
    while (InterlockedCompareExchange(&rgba_debug_lock_state, 1, 0) != 0)
        Sleep(0);
}

static void rgba_debug_unlock(void)
{
    InterlockedExchange(&rgba_debug_lock_state, 0);
}
#else
static volatile int rgba_debug_lock_state;

static void rgba_debug_lock(void)
{
    while (__sync_lock_test_and_set(&rgba_debug_lock_state, 1)) {
        while (rgba_debug_lock_state)
            sched_yield();
    }
}

static void rgba_debug_unlock(void)
{
    __sync_lock_release(&rgba_debug_lock_state);
}
#endif

static const char *rgba_owner_name(ASS_RGBAOwner owner)
{
    switch (owner) {
    case ASS_RGBA_OWNER_NEW:          return "new";
    case ASS_RGBA_OWNER_EVENT:        return "event";
    case ASS_RGBA_OWNER_FRAME_RESULT: return "frame result";
    case ASS_RGBA_OWNER_CALLER:       return "caller";
    case ASS_RGBA_OWNER_FREED:        return "freed";
    }
    return "unknown";
}

static void rgba_debug_fail(const char *operation, ASS_ImageRGBA *img,
                            const RgbaDebugAllocation *allocation)
{
    fprintf(stderr,
            "Invalid RGBA aligned free/ownership operation: pointer=%p "
            "image=%p allocation_id=%" PRIu64 " creation_site=%s "
            "last_owner=%s last_operation=%s operation=%s\n",
            allocation ? (void *) allocation->base : NULL, (void *) img,
            allocation ? allocation->id : 0,
            allocation && allocation->creation_site ?
                allocation->creation_site : "unknown",
            allocation && allocation->image ?
                rgba_owner_name(allocation->image->owner) : "unowned",
            allocation && allocation->last_operation ?
                allocation->last_operation : "unknown",
            operation ? operation : "unknown");
    abort();
}

static RgbaDebugAllocation *rgba_debug_find_buffer(uint8_t *buffer)
{
    rgba_debug_lock();
    rgba_debug_registry_scans++;
    for (RgbaDebugAllocation *cur = rgba_debug_allocations; cur; cur = cur->next) {
        rgba_debug_registry_scan_steps++;
        if (cur->base == buffer) {
            rgba_debug_unlock();
            return cur;
        }
    }
    rgba_debug_unlock();
    return NULL;
}

static RgbaDebugAllocation *rgba_debug_find_image(ASS_ImageRGBA *img)
{
    rgba_debug_lock();
    rgba_debug_registry_scans++;
    for (RgbaDebugAllocation *cur = rgba_debug_allocations; cur; cur = cur->next) {
        rgba_debug_registry_scan_steps++;
        if ((ASS_ImageRGBA *) cur->image == img) {
            rgba_debug_unlock();
            return cur;
        }
    }
    rgba_debug_unlock();
    return NULL;
}

static void rgba_debug_track_buffer(uint8_t *buffer, size_t size,
                                    const char *creation_site)
{
    RgbaDebugAllocation *allocation = malloc(sizeof(*allocation));
    if (!allocation) {
        fprintf(stderr, "Could not allocate RGBA ownership registry entry\n");
        abort();
    }
    *allocation = (RgbaDebugAllocation) {
        .base = buffer,
        .size = size,
        .creation_site = creation_site,
        .last_operation = "allocated",
    };
    rgba_debug_lock();
    allocation->id = rgba_debug_next_id++;
    if (!allocation->id)
        allocation->id = rgba_debug_next_id++;
    allocation->next = rgba_debug_allocations;
    rgba_debug_allocations = allocation;
    rgba_debug_allocation_count++;
    rgba_debug_unlock();
}

static void rgba_debug_remove(RgbaDebugAllocation *allocation)
{
    rgba_debug_lock();
    RgbaDebugAllocation **link = &rgba_debug_allocations;
    rgba_debug_registry_scans++;
    while (*link && *link != allocation) {
        rgba_debug_registry_scan_steps++;
        link = &(*link)->next;
    }
    if (*link)
        rgba_debug_registry_scan_steps++;
    if (!*link)
        rgba_debug_fail("remove allocation", NULL, allocation);
    *link = allocation->next;
    rgba_debug_allocation_count--;
    rgba_debug_unlock();
    free(allocation);
}

static ASS_ImageRGBAPriv *rgba_debug_validate_image(ASS_ImageRGBA *img,
                                                     const char *operation,
                                                     RgbaDebugAllocation **out)
{
    RgbaDebugAllocation *allocation = rgba_debug_find_image(img);
    if (!allocation)
        rgba_debug_fail(operation, img, NULL);

    ASS_ImageRGBAPriv *priv = allocation->image;
    if (priv->magic != ASS_RGBA_DEBUG_MAGIC || !priv->alive ||
        priv->allocation_id != allocation->id || priv->buffer != allocation->base ||
        allocation->size != priv->alloc_size)
        rgba_debug_fail(operation, img, allocation);
    if (out)
        *out = allocation;
    return priv;
}

static void rgba_debug_claim_buffer(ASS_ImageRGBAPriv *image,
                                    uint8_t *buffer, size_t size,
                                    ASS_RGBAOwner owner,
                                    const char *operation)
{
    RgbaDebugAllocation *allocation = rgba_debug_find_buffer(buffer);
    if (!allocation || allocation->image || allocation->size != size)
        rgba_debug_fail(operation, &image->result, allocation);

    allocation->image = image;
    allocation->last_operation = operation;
    image->magic = ASS_RGBA_DEBUG_MAGIC;
    image->allocation_id = allocation->id;
    image->owner = owner;
    image->alive = true;
}

static void rgba_debug_release_unowned_buffer(uint8_t *buffer,
                                              const char *operation)
{
    RgbaDebugAllocation *allocation = rgba_debug_find_buffer(buffer);
    if (!allocation || allocation->image)
        rgba_debug_fail(operation, NULL, allocation);
    rgba_debug_remove(allocation);
}

#else

#define rgba_debug_track_buffer(buffer, size, creation_site) ((void) 0)
#define rgba_debug_release_unowned_buffer(buffer, operation) ((void) 0)

#endif

size_t ass_rgba_debug_live_allocation_count(void)
{
#ifndef NDEBUG
    rgba_debug_lock();
    size_t count = rgba_debug_allocation_count;
    rgba_debug_unlock();
    return count;
#else
    return 0;
#endif
}

void ass_rgba_debug_allocation_stats(size_t *live_allocations,
                                     uint64_t *registry_scans,
                                     uint64_t *registry_scan_steps)
{
#ifndef NDEBUG
    rgba_debug_lock();
    if (live_allocations)
        *live_allocations = rgba_debug_allocation_count;
    if (registry_scans)
        *registry_scans = rgba_debug_registry_scans;
    if (registry_scan_steps)
        *registry_scan_steps = rgba_debug_registry_scan_steps;
    rgba_debug_unlock();
#else
    if (live_allocations)
        *live_allocations = 0;
    if (registry_scans)
        *registry_scans = 0;
    if (registry_scan_steps)
        *registry_scan_steps = 0;
#endif
}

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
                                      size_t *alloc_size,
                                      const char *allocation_site)
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

    uint8_t *buffer = ass_aligned_alloc_tagged(
        align, size, false, ASS_ALIGNED_ALLOC_RGBA_IMAGE, priv);
    if (!buffer) {
        priv->rgba_output_size = old_used;
        return NULL;
    }
    rgba_debug_track_buffer(buffer, size, allocation_site);

    if (alloc_size)
        *alloc_size = size;
    return buffer;
}

uint8_t *ass_rgba_alloc_buffer(ASS_Renderer *priv, int w, int h,
                               size_t replace_size, int *stride,
                               size_t *alloc_size,
                               const char *allocation_site)
{
    int rgba_stride;
    size_t size;
    if (!rgba_stride_size(priv, w, h, &rgba_stride, &size))
        return NULL;

    size_t old_used = priv->rgba_output_size;
    if (!rgba_reserve(priv, replace_size, size))
        return NULL;

    unsigned align = 1U << priv->engine.align_order;
    uint8_t *buffer = ass_aligned_alloc_tagged(
        align, size, false, ASS_ALIGNED_ALLOC_RGBA_IMAGE, priv);
    if (!buffer) {
        priv->rgba_output_size = old_used;
        return NULL;
    }
    rgba_debug_track_buffer(buffer, size, allocation_site);

    if (stride)
        *stride = rgba_stride;
    if (alloc_size)
        *alloc_size = size;
    return buffer;
}

ASS_ImageRGBA *ass_rgba_image_alloc(ASS_Renderer *priv, int w, int h,
                                    int dst_x, int dst_y, int type,
                                    ASS_RGBAOwner owner,
                                    const char *allocation_site)
{
    int stride;
    size_t alloc_size;
    uint8_t *buffer =
        ass_rgba_alloc_buffer(priv, w, h, 0, &stride, &alloc_size,
                              allocation_site);
    if (!buffer)
        return NULL;

    ASS_ImageRGBAPriv *img = malloc(sizeof(*img));
    if (!img) {
        if (priv && priv->rgba_output_size >= alloc_size)
            priv->rgba_output_size -= alloc_size;
        else if (priv)
            priv->rgba_output_size = 0;
        rgba_debug_release_unowned_buffer(buffer, "image object allocation failure");
        ass_aligned_free_tagged(
            buffer, ASS_ALIGNED_ALLOC_RGBA_IMAGE, priv);
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
    img->blend_rgb = NULL;
    img->blend_alloc_size = 0;
    img->blend_stride = 0;
    img->blend_mode = ASS_BLEND_NORMAL;
    ass_aligned_retag(buffer, ASS_ALIGNED_ALLOC_RGBA_IMAGE, img,
                      allocation_site);
#ifndef NDEBUG
    rgba_debug_claim_buffer(img, buffer, alloc_size, owner, "image allocation");
#else
    (void) owner;
#endif
    return &img->result;
}

void ass_rgba_image_replace_buffer(ASS_ImageRGBA *img, uint8_t *buffer,
                                   size_t alloc_size, int w, int h,
                                   int stride)
{
    if (!img || !buffer || !alloc_size || w <= 0 || h <= 0 || stride <= 0)
        return;

#ifndef NDEBUG
    RgbaDebugAllocation *old_allocation;
    ASS_ImageRGBAPriv *priv = rgba_debug_validate_image(
        img, "replace buffer", &old_allocation);
    RgbaDebugAllocation *new_allocation = rgba_debug_find_buffer(buffer);
    if (!new_allocation || new_allocation->image ||
        new_allocation->size != alloc_size)
        rgba_debug_fail("replace buffer", img, new_allocation);

    new_allocation->image = priv;
    new_allocation->last_operation = "replace buffer";
    priv->allocation_id = new_allocation->id;
    rgba_debug_remove(old_allocation);
#else
    ASS_ImageRGBAPriv *priv = ass_rgba_image_private(img, "replace buffer");
#endif
    if (!priv)
        return;
    ass_aligned_free_tagged(
        priv->buffer, ASS_ALIGNED_ALLOC_RGBA_IMAGE, priv);
    priv->buffer = buffer;
    priv->alloc_size = alloc_size;
    ass_aligned_retag(buffer, ASS_ALIGNED_ALLOC_RGBA_IMAGE, priv,
                      "RGBA buffer replacement");
    img->w = w;
    img->h = h;
    img->stride = stride;
    img->rgba = buffer;
}

void ass_rgba_image_free(ASS_Renderer *priv, ASS_ImageRGBA *img)
{
    if (!img)
        return;

#ifndef NDEBUG
    RgbaDebugAllocation *allocation;
    ASS_ImageRGBAPriv *rgba_priv = rgba_debug_validate_image(
        img, "image destruction", &allocation);
#else
    ASS_ImageRGBAPriv *rgba_priv = ass_rgba_image_private(img,
                                                            "image destruction");
#endif
    if (!rgba_priv)
        return;
    if (priv) {
        if (priv->rgba_output_size >= rgba_priv->alloc_size)
            priv->rgba_output_size -= rgba_priv->alloc_size;
        else
            priv->rgba_output_size = 0;
    }
#ifndef NDEBUG
    rgba_priv->owner = ASS_RGBA_OWNER_FREED;
    rgba_priv->alive = false;
    allocation->last_operation = "image destruction";
    rgba_debug_remove(allocation);
#endif
    ass_aligned_free_tagged(
        rgba_priv->buffer, ASS_ALIGNED_ALLOC_RGBA_IMAGE, rgba_priv);
    free(rgba_priv->blend_rgb);
    free(rgba_priv);
}

ASS_ImageRGBAPriv *ass_rgba_image_private(ASS_ImageRGBA *img,
                                           const char *operation)
{
    if (!img)
        return NULL;
#ifndef NDEBUG
    return rgba_debug_validate_image(img, operation, NULL);
#else
    (void) operation;
    return (ASS_ImageRGBAPriv *) img;
#endif
}

bool ass_rgba_image_view_valid(ASS_ImageRGBA *img, const char *operation)
{
    ASS_ImageRGBAPriv *priv = ass_rgba_image_private(img, operation);
    if (!priv || !img->rgba || img->w <= 0 || img->h <= 0 || img->stride <= 0)
        return false;

    size_t row_size = 0;
    uintptr_t base = (uintptr_t) priv->buffer;
    uintptr_t view = (uintptr_t) img->rgba;
    bool valid = (size_t) img->w <= SIZE_MAX / 4;
    if (valid)
        row_size = (size_t) img->w * 4;
    valid = valid && row_size <= (size_t) img->stride && view >= base;
    size_t offset = valid ? (size_t) (view - base) : 0;
    if (valid && (offset > priv->alloc_size || row_size > priv->alloc_size - offset))
        valid = false;
    if (valid && img->h > 1 &&
        (size_t) img->stride > (priv->alloc_size - offset - row_size) /
                              (size_t) (img->h - 1))
        valid = false;

#ifndef NDEBUG
    if (!valid)
        rgba_debug_fail(operation, img, rgba_debug_find_image(img));
#endif
    return valid;
}

bool ass_rgba_image_alloc_blend_rgb(ASS_ImageRGBA *img,
                                    ASS_BlendMode blend_mode)
{
    ASS_ImageRGBAPriv *priv = ass_rgba_image_private(img, "blend source allocation");
    if (!priv || blend_mode == ASS_BLEND_NORMAL || img->w <= 0 || img->h <= 0 ||
            img->stride <= 0 || (size_t) img->stride > SIZE_MAX / (size_t) img->h)
        return false;

    size_t size = (size_t) img->stride * (size_t) img->h;
    uint8_t *buffer = malloc(size);
    if (!buffer)
        return false;
    free(priv->blend_rgb);
    priv->blend_rgb = buffer;
    priv->blend_alloc_size = size;
    priv->blend_stride = img->stride;
    priv->blend_mode = blend_mode;
    return true;
}

bool ass_rgba_image_crop_blend_rgb(ASS_ImageRGBA *img, int x, int y,
                                   int w, int h, int stride)
{
    ASS_ImageRGBAPriv *priv = ass_rgba_image_private(img, "blend source crop");
    if (!priv || !priv->blend_rgb)
        return true;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || w > INT_MAX / 4 ||
            stride < w * 4 ||
            (size_t) stride > SIZE_MAX / (size_t) h) {
        free(priv->blend_rgb);
        priv->blend_rgb = NULL;
        priv->blend_alloc_size = 0;
        priv->blend_stride = 0;
        priv->blend_mode = ASS_BLEND_NORMAL;
        return false;
    }

    size_t public_offset = (size_t) (img->rgba - priv->buffer);
    size_t view_y = public_offset / (size_t) img->stride;
    size_t view_x_bytes = public_offset % (size_t) img->stride;
    size_t src_y = view_y + (size_t) y;
    size_t src_x_bytes = view_x_bytes + (size_t) x * 4;
    size_t row_bytes = (size_t) w * 4;
    bool source_valid = src_x_bytes + row_bytes <=
        (size_t) priv->blend_stride;
    size_t source_start = 0;
    if (source_valid && src_y <= SIZE_MAX / (size_t) priv->blend_stride) {
        source_start = src_y * (size_t) priv->blend_stride;
        source_valid = source_start <= SIZE_MAX - src_x_bytes;
        source_start += src_x_bytes;
    } else {
        source_valid = false;
    }
    size_t last_row = (size_t) (h - 1) * (size_t) priv->blend_stride;
    source_valid = source_valid && source_start <= priv->blend_alloc_size &&
        last_row <= priv->blend_alloc_size - source_start &&
        row_bytes <= priv->blend_alloc_size - source_start - last_row;
    if (!source_valid) {
        free(priv->blend_rgb);
        priv->blend_rgb = NULL;
        priv->blend_alloc_size = 0;
        priv->blend_stride = 0;
        priv->blend_mode = ASS_BLEND_NORMAL;
        return false;
    }

    size_t size = (size_t) stride * (size_t) h;
    uint8_t *buffer = malloc(size);
    if (!buffer) {
        free(priv->blend_rgb);
        priv->blend_rgb = NULL;
        priv->blend_alloc_size = 0;
        priv->blend_stride = 0;
        priv->blend_mode = ASS_BLEND_NORMAL;
        return false;
    }
    const uint8_t *source = priv->blend_rgb + source_start;
    for (int row = 0; row < h; row++)
        memcpy(buffer + (size_t) row * stride,
               source + (size_t) row * priv->blend_stride, row_bytes);
    free(priv->blend_rgb);
    priv->blend_rgb = buffer;
    priv->blend_alloc_size = size;
    priv->blend_stride = stride;
    return true;
}

void ass_rgba_images_set_owner(ASS_ImageRGBA *img, ASS_RGBAOwner owner,
                               const char *operation)
{
    while (img) {
#ifndef NDEBUG
        RgbaDebugAllocation *allocation;
        ASS_ImageRGBAPriv *rgba_priv = rgba_debug_validate_image(
            img, operation, &allocation);
        ASS_ImageRGBA *next = rgba_priv->result.next;
        rgba_priv->owner = owner;
        allocation->last_operation = operation;
#else
        ASS_ImageRGBA *next = img->next;
        (void) owner;
        (void) operation;
#endif
        img = next;
    }
}

bool ass_rgba_image_clip_to_frame(ASS_Renderer *priv, ASS_ImageRGBA *img)
{
    if (!priv || !img)
        return false;
    if (!ass_rgba_image_view_valid(img, "clip to frame"))
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
    if (!ass_rgba_image_view_valid(img, "clip result"))
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
        ASS_ImageRGBA *node = ass_rgba_image_alloc(
            priv, cur->w, cur->h, cur->dst_x, cur->dst_y, cur->type,
            ASS_RGBA_OWNER_FRAME_RESULT, "legacy image conversion");
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
        if (!ass_rgba_image_clip_to_frame(priv, node)) {
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
            if (!ass_ensure_event_images(priv, cnt))
                break;
            memset(priv->eimg + cnt, 0, sizeof(*priv->eimg));
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
        /* Transfer the entire event-local list before exposing any node in
         * the frame result. This prevents two list heads owning one node. */
        priv->eimg[i].imgs_rgba = NULL;
        while (rcur) {
            ASS_ImageRGBA *next = rcur->next;
            rcur->next = NULL;
            if (ass_rgba_image_clip_to_frame(priv, rcur)) {
                ass_rgba_images_set_owner(rcur, ASS_RGBA_OWNER_FRAME_RESULT,
                                          "event-to-frame transfer");
                *rgba_tail = rcur;
                rgba_tail = &rcur->next;
            } else {
                ass_rgba_image_free(priv, rcur);
            }
            rcur = next;
        }
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

    ass_rgba_images_set_owner(rgba_root, ASS_RGBA_OWNER_CALLER,
                              "frame result return");
    return rgba_root;
}

void ass_free_images_rgba(ASS_ImageRGBA *img)
{
    while (img) {
#ifndef NDEBUG
        ASS_ImageRGBAPriv *rgba_priv = rgba_debug_validate_image(
            img, "frame result cleanup", NULL);
        ASS_ImageRGBA *next = rgba_priv->result.next;
#else
        ASS_ImageRGBA *next = img->next;
#endif
        ass_rgba_image_free(NULL, img);
        img = next;
    }
}

int ass_composite_images_bgra(ASS_ImageRGBA *img, uint8_t *dst,
                              int width, int height, int stride)
{
    int64_t stride_abs = stride < 0 ? -(int64_t) stride : stride;
    if (!dst || width <= 0 || height <= 0 || !stride ||
            (int64_t) width * 4 > stride_abs)
        return -1;

    /* Validate the complete list before modifying dst so callers may safely
     * fall back to their normal compositor on error. */
    for (ASS_ImageRGBA *cur = img; cur; cur = cur->next)
        if (!ass_rgba_image_private(cur, "BGRA composition preflight") ||
                !ass_rgba_image_view_valid(cur, "BGRA composition preflight"))
            return -1;

    for (ASS_ImageRGBA *cur = img; cur; cur = cur->next) {
        ASS_ImageRGBAPriv *priv = ass_rgba_image_private(cur, "BGRA composition");

        int64_t tile_left = cur->dst_x;
        int64_t tile_top = cur->dst_y;
        int x0 = tile_left <= 0 ? 0 :
            tile_left >= width ? width : (int) tile_left;
        int y0 = tile_top <= 0 ? 0 :
            tile_top >= height ? height : (int) tile_top;
        int64_t tile_right = tile_left + cur->w;
        int64_t tile_bottom = tile_top + cur->h;
        int x1 = tile_right <= 0 ? 0 :
            tile_right >= width ? width : (int) tile_right;
        int y1 = tile_bottom <= 0 ? 0 :
            tile_bottom >= height ? height : (int) tile_bottom;
        if (x0 >= x1 || y0 >= y1)
            continue;
        int source_x = x0 - cur->dst_x;
        int source_y = y0 - cur->dst_y;
        const uint8_t *rgba = cur->rgba + (size_t) source_y * cur->stride +
            (size_t) source_x * 4;

        const uint8_t *blend_rgb = NULL;
        ASS_BlendMode mode = priv->blend_mode;
        if (mode < ASS_BLEND_NORMAL || mode > ASS_BLEND_SUBSTRACT_INVERSE)
            mode = ASS_BLEND_NORMAL;
        if (mode != ASS_BLEND_NORMAL && priv->blend_rgb) {
            size_t offset = (size_t) (cur->rgba - priv->buffer);
            size_t blend_y = offset / (size_t) cur->stride + (size_t) source_y;
            size_t blend_x = offset % (size_t) cur->stride +
                (size_t) source_x * 4;
            size_t row_bytes = (size_t) (x1 - x0) * 4;
            bool blend_valid = blend_x + row_bytes <=
                (size_t) priv->blend_stride &&
                blend_y <= SIZE_MAX / (size_t) priv->blend_stride;
            size_t blend_start = blend_valid ?
                blend_y * (size_t) priv->blend_stride : 0;
            blend_valid = blend_valid && blend_start <= SIZE_MAX - blend_x;
            blend_start += blend_valid ? blend_x : 0;
            size_t last_row = (size_t) (y1 - y0 - 1) *
                (size_t) priv->blend_stride;
            blend_valid = blend_valid &&
                blend_start <= priv->blend_alloc_size &&
                last_row <= priv->blend_alloc_size - blend_start &&
                row_bytes <= priv->blend_alloc_size - blend_start - last_row;
            if (blend_valid)
                blend_rgb = priv->blend_rgb + blend_start;
            else
                mode = ASS_BLEND_NORMAL;
        } else {
            mode = ASS_BLEND_NORMAL;
        }

        for (int y = y0; y < y1; y++) {
            const uint8_t *src = rgba + (size_t) (y - y0) * cur->stride;
            const uint8_t *straight = blend_rgb ?
                blend_rgb + (size_t) (y - y0) * priv->blend_stride : NULL;
            uint8_t *out = dst + (ptrdiff_t) y * stride + (size_t) x0 * 4;
            for (int x = 0; x < x1 - x0; x++) {
                unsigned alpha = src[4 * x + 3];
                if (mode == ASS_BLEND_NORMAL) {
                    unsigned inv_alpha = 255 - alpha;
                    out[4 * x + 0] = (uint8_t) (src[4 * x + 2] +
                        out[4 * x + 0] * inv_alpha / 255);
                    out[4 * x + 1] = (uint8_t) (src[4 * x + 1] +
                        out[4 * x + 1] * inv_alpha / 255);
                    out[4 * x + 2] = (uint8_t) (src[4 * x + 0] +
                        out[4 * x + 2] * inv_alpha / 255);
                } else if (alpha) {
                    for (int c = 0; c < 3; c++) {
                        int source_channel = 2 - c;
                        out[4 * x + c] = ass_blend_compose_channel(
                            mode, straight[4 * x + source_channel],
                            out[4 * x + c], (uint8_t) alpha);
                    }
                }
                out[4 * x + 3] = 0;
            }
        }
    }
    return 0;
}
