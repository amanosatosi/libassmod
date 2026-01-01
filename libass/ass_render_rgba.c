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
                                 &priv->eimg[cnt].imgs_rgba))
                cnt++;
        }
    }

    if (cnt > 0)
        qsort(priv->eimg, cnt, sizeof(EventImages), cmp_event_layer);

    EventImages *last = priv->eimg;
    for (int i = 1; i < cnt; i++)
        if (last->event->Layer != priv->eimg[i].event->Layer) {
            fix_collisions(priv, last, priv->eimg + i - last);
            last = priv->eimg + i;
        }
    if (cnt > 0)
        fix_collisions(priv, last, priv->eimg + cnt - last);

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
            *rgba_tail = rcur;
            rgba_tail = &rcur->next;
            rcur = rcur->next;
        }
        priv->eimg[i].imgs_rgba = NULL;
    }

    ass_frame_ref(priv->images_root);

    if (detect_change)
        *detect_change = ass_detect_change(priv);

    ass_frame_unref(priv->prev_images_root);
    priv->prev_images_root = NULL;

    if (track->parser_priv->prune_delay >= 0)
        ass_prune_events(track, now - track->parser_priv->prune_delay);

    return rgba_root;
}

void ass_free_images_rgba(ASS_ImageRGBA *img)
{
    while (img) {
        ASS_ImageRGBA *next = img->next;
        ass_aligned_free(img->rgba);
        free(img);
        img = next;
    }
}
