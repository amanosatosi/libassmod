/*
 * Copyright (C) 2009 Grigori Goronzy <greg@geekmind.org>
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

#ifndef LIBASS_PARSE_H
#define LIBASS_PARSE_H

#include <string.h>

#include "ass_render.h"

#define BLUR_MAX_RADIUS 100.0

#define _r(c)   ((c) >> 24)
#define _g(c)   (((c) >> 16) & 0xFF)
#define _b(c)   (((c) >> 8) & 0xFF)
#define _a(c)   ((c) & 0xFF)

static inline uint32_t mult_alpha(uint32_t a, uint32_t b)
{
    return a - ((uint64_t) a * b + 0x7F) / 0xFF + b;
}

void ass_update_font(RenderContext *state);
void ass_apply_transition_effects(RenderContext *state);
void ass_process_karaoke_effects(RenderContext *state);
typedef struct ass_override_text {
    struct ass_override_text *next;
    char text[];
} ASS_OverrideText;

// Read the same byte stream as ass_prepare_override_block without a copy.
// Bounds must belong to one override block, never to the whole event text.
static inline unsigned char ass_override_peek(char **p, char *end)
{
    while (*p < end) {
        if (**p == '[') {
            char *close = memchr(*p + 1, ']', end - (*p + 1));
            *p = close ? close + 1 : end;
        } else if (**p == '\\' && *p + 1 < end && (*p)[1] == 'N') {
            *p += 2;
        } else {
            return (unsigned char) **p;
        }
    }
    return 0;
}

static inline unsigned char ass_override_next(char **p, char *end)
{
    unsigned char c = ass_override_peek(p, end);
    if (*p < end)
        ++*p;
    return c;
}

static inline void ass_override_spaces(char **p, char *end)
{
    unsigned char c;
    while ((c = ass_override_peek(p, end)) == ' ' || c == '\t')
        ++*p;
}

static inline bool ass_override_prefix(char **p, char *end, const char *name)
{
    char *next = *p;
    while (*name)
        if (ass_override_next(&next, end) != (unsigned char) *name++)
            return false;
    *p = next;
    return true;
}

// A non-destructive lexical view of one bounded override block. The caller
// owns *storage, or it is NULL when the original source can be used directly.
bool ass_prepare_override_block(char **start, char **end, ASS_OverrideText **storage);
char *ass_parse_override_block(RenderContext *state, char *start, char *end);
void ass_free_override_buffers(RenderContext *state);
unsigned ass_get_next_char(RenderContext *state, char **str);
char *ass_parse_tags(RenderContext *state, char *p, char *end, double pwr,
                     bool nested);
int ass_event_has_hard_overrides(char *str);
void ass_apply_fade(uint32_t *clr, int fade);
void ass_apply_fade_color(uint32_t *clr, FadeColorState fade_color);
void ass_apply_fades(uint32_t *clr, int fade, FadeColorState fade_color);


#endif /* LIBASS_PARSE_H */
