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

#include "config.h"
#include "ass_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#include "ass_library.h"
#include "ass_render.h"
#include "ass_parse.h"

#define MAX_VALID_NARGS 10
#define MAX_BE 127
#define NBSP 0xa0   // unicode non-breaking space character

struct arg {
    char *start, *end;
};

static bool parse_int32_arg_strict(struct arg arg, int32_t *out);

static inline int32_t argtoi32(struct arg arg)
{
    int32_t value;
    mystrtoi32(&arg.start, 10, &value);
    return value;
}

static inline double argtod(struct arg arg)
{
    double value;
    mystrtod(&arg.start, &value);
    return value;
}

static inline void push_arg(struct arg *args, int *nargs, char *start, char *end)
{
    if (*nargs <= MAX_VALID_NARGS) {
        rskip_spaces(&end, start);
        if (end > start) {
            args[*nargs] = (struct arg) {start, end};
            ++*nargs;
        }
    }
}

/**
 * \brief Check if starting part of (*p) matches sample.
 * If true, shift p to the first symbol after the matching part.
 */
static inline int mystrcmp(char **p, const char *sample)
{
    char *p2;
    for (p2 = *p; *sample != 0 && *p2 == *sample; p2++, sample++)
        ;
    if (*sample == 0) {
        *p = p2;
        return 1;
    }
    return 0;
}

static inline bool rnd_numeric_start(char c)
{
    return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

/**
 * \brief Change current font, using setting from render_priv->state.
 */
void ass_update_font(RenderContext *state)
{
    unsigned val;
    ASS_FontDesc desc;

    desc.family = state->family;
    if (!desc.family.str)
        return;
    if (desc.family.len && desc.family.str[0] == '@') {
        desc.vertical = 1;
        desc.family.str++;
        desc.family.len--;
    } else {
        desc.vertical = 0;
    }

    val = state->bold;
    // 0 = normal, 1 = bold, >1 = exact weight
    if (val == 1 || val == -1)
        val = 700;               // bold
    else if (val <= 0)
        val = 400;               // normal
    desc.bold = val;

    val = state->italic;
    if (val == 1)
        val = 100;              // italic
    else if (val <= 0)
        val = 0;                // normal
    desc.italic = val;

    state->font = ass_font_new(state->renderer, &desc);
}

/**
 * \brief Convert double to int32_t without UB
 * on out-of-range values; match x86 behavior
 */
static inline int32_t dtoi32(double val)
{
    if (ass_isnan(val) || val <= INT32_MIN || val >= INT32_MAX + 1LL)
        return INT32_MIN;
    return val;
}

static double calc_anim(double new, double old, double pwr)
{
   return (1 - pwr) * old + new * pwr;
}

static int32_t calc_anim_int32(uint32_t new, uint32_t old, double pwr)
{
    return dtoi32(calc_anim(new, old, pwr));
}

static void apply_motion(RenderContext *state, MotionState motion,
                         double pwr, bool allow_override)
{
    if (motion.type == MOTION_NONE || pwr <= 0)
        return;

    if ((state->evt_type & EVENT_POSITIONED) && !allow_override)
        return;

    MotionState *dst = &state->motion;
    MotionState old = *dst;

    dst->type = motion.type;
    dst->x1 = calc_anim(motion.x1, old.x1, pwr);
    dst->y1 = calc_anim(motion.y1, old.y1, pwr);
    dst->x2 = calc_anim(motion.x2, old.x2, pwr);
    dst->y2 = calc_anim(motion.y2, old.y2, pwr);
    dst->x3 = calc_anim(motion.x3, old.x3, pwr);
    dst->y3 = calc_anim(motion.y3, old.y3, pwr);
    dst->x4 = calc_anim(motion.x4, old.x4, pwr);
    dst->y4 = calc_anim(motion.y4, old.y4, pwr);
    dst->angle1 = calc_anim(motion.angle1, old.angle1, pwr);
    dst->angle2 = calc_anim(motion.angle2, old.angle2, pwr);
    dst->radius1 = calc_anim(motion.radius1, old.radius1, pwr);
    dst->radius2 = calc_anim(motion.radius2, old.radius2, pwr);
    dst->has_timing = motion.has_timing;
    dst->t1 = dtoi32(calc_anim(motion.t1, old.t1, pwr));
    dst->t2 = dtoi32(calc_anim(motion.t2, old.t2, pwr));

    if (!(state->evt_type & EVENT_POSITIONED)) {
        state->evt_type |= EVENT_POSITIONED;
        state->detect_collisions = 0;
    }
}

static void apply_jitter(RenderContext *state, JitterState jitter, double pwr)
{
    if (pwr <= 0)
        return;

    JitterState *dst = &state->jitter;

    if (!jitter.enabled) {
        if (pwr >= 1)
            *dst = ass_jitter_default_state();
        return;
    }

    dst->left = calc_anim(jitter.left, dst->left, pwr);
    dst->right = calc_anim(jitter.right, dst->right, pwr);
    dst->up = calc_anim(jitter.up, dst->up, pwr);
    dst->down = calc_anim(jitter.down, dst->down, pwr);
    dst->enabled = true;

    if (jitter.has_period) {
        dst->period = calc_anim(jitter.period, dst->period, pwr);
        dst->has_period = true;
    }

    if (jitter.has_seed && (pwr >= 1 || !dst->has_seed)) {
        dst->seed = jitter.seed;
        dst->has_seed = true;
    }
}

static double jitter_extent_from_arg(struct arg arg)
{
    int32_t raw = argtoi32(arg);
    int64_t extent = raw;
    if (extent < 0)
        extent = -extent;
    if (extent > INT32_MAX / 8)
        extent = INT32_MAX / 8;
    return (double) extent * 8.0;
}

static void normalize_motion_timing(MotionState *motion)
{
    if (motion->has_timing && motion->t1 > motion->t2) {
        int32_t tmp = motion->t2;
        motion->t2 = motion->t1;
        motion->t1 = tmp;
    }
}
/**
 * \brief Calculate a weighted average of two colors
 * calculates c1*(1-a) + c2*a, but separately for each component except alpha
 */
static void change_color(uint32_t *var, uint32_t new, double pwr)
{
    uint32_t co = ass_bswap32(*var);
    uint32_t cn = ass_bswap32(new);

    uint32_t cc = (calc_anim_int32(cn & 0xff0000, co & 0xff0000, pwr) & 0xff0000) |
                  (calc_anim_int32(cn & 0x00ff00, co & 0x00ff00, pwr) & 0x00ff00) |
                  (calc_anim_int32(cn & 0x0000ff, co & 0x0000ff, pwr) & 0x0000ff);

    (*var) = (ass_bswap32(cc & 0xffffff)) | _a(*var);
}

// like change_color, but for alpha component only
static inline void change_alpha(uint32_t *var, int32_t new, double pwr)
{
    *var = (*var & 0xFFFFFF00) | (uint8_t)calc_anim_int32(new, _a(*var), pwr);
}

void ass_apply_fade(uint32_t *clr, int fade)
{
    // VSFilter compatibility: apply fade only when it's positive
    if (fade > 0)
        change_alpha(clr, mult_alpha(_a(*clr), fade), 1);
}

void ass_apply_fade_color(uint32_t *clr, FadeColorState fade_color)
{
    if (!fade_color.active || fade_color.amount <= 0)
        return;

    if (fade_color.amount >= 1) {
        *clr = (fade_color.color & 0xFFFFFF00u) | _a(*clr);
        return;
    }

    uint32_t out = *clr;
    change_color(&out, fade_color.color, fade_color.amount);
    *clr = out;
}

void ass_apply_fades(uint32_t *clr, int fade, FadeColorState fade_color)
{
    ass_apply_fade(clr, fade);
    ass_apply_fade_color(clr, fade_color);
}

static void disable_image_fill_layer(RenderContext *state, int layer)
{
    if (layer < 0 || layer > 3)
        return;
    state->image_fill.layer[layer].enabled = false;
    state->image_fill.layer[layer].path = (ASS_StringView) {NULL, 0};
    state->image_fill.layer[layer].xoffset = 0;
    state->image_fill.layer[layer].yoffset = 0;
}

static inline bool ass_inline_isspace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '\f' || c == '\v';
}

static inline void trim_arg_inline(struct arg *arg)
{
    while (arg->start < arg->end && ass_inline_isspace(*arg->start))
        arg->start++;
    while (arg->end > arg->start && ass_inline_isspace(arg->end[-1]))
        arg->end--;
}

static bool split_img_inline_args(struct arg raw, struct arg *path_arg,
                                  struct arg *x_arg, struct arg *y_arg)
{
    *path_arg = raw;
    x_arg->start = x_arg->end = NULL;
    y_arg->start = y_arg->end = NULL;
    trim_arg_inline(path_arg);
    if (path_arg->end <= path_arg->start)
        return false;

    char *start = path_arg->start;
    char *end = path_arg->end;

    if (*start == '"' || *start == '\'') {
        char quote = *start;
        char *q = start + 1;
        while (q < end && *q != quote)
            q++;
        if (q < end) {
            path_arg->start = start + 1;
            path_arg->end = q;
            trim_arg_inline(path_arg);

            char *rest = q + 1;
            while (rest < end && ass_inline_isspace(*rest))
                rest++;
            if (rest < end && *rest == ',') {
                rest++;
                char *mid = rest;
                while (mid < end && *mid != ',')
                    mid++;
                x_arg->start = rest;
                x_arg->end = mid;
                trim_arg_inline(x_arg);
                if (mid < end && *mid == ',') {
                    y_arg->start = mid + 1;
                    y_arg->end = end;
                    trim_arg_inline(y_arg);
                    return y_arg->end > y_arg->start;
                }
            }
            return false;
        }
    }

    char *comma = start;
    while (comma < end && *comma != ',')
        comma++;
    path_arg->start = start;
    path_arg->end = comma;
    trim_arg_inline(path_arg);

    if (comma < end && *comma == ',') {
        char *mid = comma + 1;
        while (mid < end && *mid != ',')
            mid++;
        x_arg->start = comma + 1;
        x_arg->end = mid;
        trim_arg_inline(x_arg);
        if (mid < end && *mid == ',') {
            y_arg->start = mid + 1;
            y_arg->end = end;
            trim_arg_inline(y_arg);
            return y_arg->end > y_arg->start;
        }
    }

    return false;
}

static void apply_img_tag(RenderContext *state, int layer,
                          const struct arg *args, int nargs, double pwr)
{
    if (layer < 0 || layer > 3 || nargs < 1)
        return;

    struct arg path_arg = args[0];
    struct arg x_arg = {NULL, NULL};
    struct arg y_arg = {NULL, NULL};
    bool have_offsets = false;

    if (nargs >= 3) {
        x_arg = args[1];
        y_arg = args[2];
        trim_arg_inline(&path_arg);
        trim_arg_inline(&x_arg);
        trim_arg_inline(&y_arg);
        if (path_arg.end - path_arg.start >= 2) {
            char first = path_arg.start[0];
            char last = path_arg.end[-1];
            if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
                path_arg.start++;
                path_arg.end--;
            }
        }
        have_offsets = true;
    } else {
        have_offsets = split_img_inline_args(args[0], &path_arg, &x_arg, &y_arg);
    }

    if (path_arg.end > path_arg.start)
        state->renderer->track->has_rgba = 1;

    if (pwr >= 1.0 && path_arg.end > path_arg.start) {
        state->image_fill.layer[layer].enabled = true;
        state->image_fill.layer[layer].path.str = path_arg.start;
        state->image_fill.layer[layer].path.len = path_arg.end - path_arg.start;
        state->image_fill.layer[layer].xoffset = 0;
        state->image_fill.layer[layer].yoffset = 0;
        state->needs_rgba = true;
    }

    if (have_offsets) {
        state->image_fill.layer[layer].xoffset =
            dtoi32(calc_anim(argtoi32(x_arg),
                             state->image_fill.layer[layer].xoffset, pwr));
        state->image_fill.layer[layer].yoffset =
            dtoi32(calc_anim(argtoi32(y_arg),
                             state->image_fill.layer[layer].yoffset, pwr));
    }
}

/**
 * \brief Calculate alpha value by piecewise linear function
 * Used for \fad, \fade implementation.
 */
static int
interpolate_alpha(long long now, int32_t t1, int32_t t2, int32_t t3,
                  int32_t t4, int a1, int a2, int a3)
{
    int a;
    double cf;

    if (now < t1) {
        a = a1;
    } else if (now < t2) {
        cf = ((double) (int32_t) ((uint32_t) now - t1)) /
                (int32_t) ((uint32_t) t2 - t1);
        a = a1 * (1 - cf) + a2 * cf;
    } else if (now < t3) {
        a = a2;
    } else if (now < t4) {
        cf = ((double) (int32_t) ((uint32_t) now - t3)) /
                (int32_t) ((uint32_t) t4 - t3);
        a = a2 * (1 - cf) + a3 * cf;
    } else {                    // now >= t4
        a = a3;
    }

    return a;
}

static double interpolate_fad_color_amount(long long now, int32_t t1,
                                           int32_t t2, int32_t t3,
                                           int32_t t4, bool *fade_in)
{
    if (now < t1) {
        *fade_in = true;
        return 1.0;
    } else if (now < t2) {
        double cf = ((double) (int32_t) ((uint32_t) now - t1)) /
                    (int32_t) ((uint32_t) t2 - t1);
        *fade_in = true;
        return 1.0 - cf;
    } else if (now < t3) {
        *fade_in = false;
        return 0.0;
    } else if (now < t4) {
        double cf = ((double) (int32_t) ((uint32_t) now - t3)) /
                    (int32_t) ((uint32_t) t4 - t3);
        *fade_in = false;
        return cf;
    } else {
        *fade_in = false;
        return 1.0;
    }
}

enum ClipType {
    CLIP_INVALID = 0,
    CLIP_RECTANGLE,
    CLIP_VECTOR,
};

typedef struct ClipParseResult {
    enum ClipType type;
    int32_t x0, y0, x1, y1;
    int scale;
    struct arg drawing;
    const char *reason;
} ClipParseResult;

#define MAX_CLIP_TOKENS 8

static int split_clip_args(char *start, char *end,
                           struct arg *tokens, bool *has_empty)
{
    int count = 0;
    *has_empty = false;

    if (!start || !end || start > end)
        return 0;

    for (char *cursor = start; cursor <= end;) {
        char *next = memchr(cursor, ',', end - cursor);
        char *tok_start = cursor;
        char *tok_end = next ? next : end;

        skip_spaces(&tok_start);
        rskip_spaces(&tok_end, tok_start);

        if (tok_end > tok_start) {
            if (count < MAX_CLIP_TOKENS)
                tokens[count] = (struct arg) { tok_start, tok_end };
        } else {
            *has_empty = true;
        }

        ++count;
        if (!next)
            break;
        cursor = next + 1;
    }

    return count;
}

static bool parse_clip_rectangle_coord(RenderContext *state, struct arg token,
                                       int idx, int32_t *value)
{
    char *ptr = token.start;
    double parsed;

    if (!mystrtod(&ptr, &parsed) || ptr != token.end) {
        ass_msg(state->renderer->library, MSGL_DBG2,
                "PARSE clip rectangle coord[%d] rejected: '%.*s'",
                idx, (int) (token.end - token.start), token.start);
        return false;
    }

    if (ass_isnan(parsed) || parsed < (double) INT32_MIN ||
            parsed > (double) INT32_MAX) {
        ass_msg(state->renderer->library, MSGL_DBG2,
                "PARSE clip rectangle coord[%d] rejected (range): %g",
                idx, parsed);
        return false;
    }

    // Decimal clip coordinates are converted by truncating toward zero.
    *value = (int32_t) parsed;
    return true;
}

static bool parse_clip_scale(RenderContext *state, struct arg token, int *scale)
{
    char *ptr = token.start;
    errno = 0;
    int32_t parsed;
    bool ok = mystrtoi32(&ptr, 10, &parsed);

    if (!ok || ptr != token.end || errno == ERANGE ||
            parsed < 1 || parsed > 31) {
        ass_msg(state->renderer->library, MSGL_DBG2,
                "PARSE clip vector scale rejected: '%.*s'",
                (int) (token.end - token.start), token.start);
        return false;
    }

    *scale = (int) parsed;
    return true;
}

static bool validate_clip_drawing_token(struct arg token)
{
    if (token.start >= token.end)
        return false;

    switch (*token.start) {
    case 'm':
    case 'n':
    case 'l':
    case 'b':
    case 's':
    case 'p':
    case 'c':
        return true;
    default:
        return false;
    }
}

static ClipParseResult parse_clip_tag(RenderContext *state, const char *tag_name,
                                      char *name_end, char *q, char *end)
{
    ClipParseResult result = {
        .type = CLIP_INVALID,
        .scale = 1,
        .reason = "invalid clip arguments",
    };

    if (name_end >= end || *name_end != '(') {
        result.reason = "missing parenthesized arguments";
        return result;
    }

    char *raw_start = name_end + 1;
    char *raw_end = q;
    if (raw_end > raw_start && raw_end[-1] == ')')
        --raw_end;
    if (raw_start > raw_end) {
        result.reason = "empty clip argument list";
        return result;
    }

    struct arg tokens[MAX_CLIP_TOKENS];
    for (int i = 0; i < MAX_CLIP_TOKENS; i++)
        tokens[i] = (struct arg) { NULL, NULL };

    bool has_empty = false;
    int count = split_clip_args(raw_start, raw_end, tokens, &has_empty);
    if (count <= 0 || has_empty) {
        result.reason = has_empty ? "empty clip argument token"
                                  : "missing clip arguments";
        return result;
    }

    if (count == 4) {
        if (!parse_clip_rectangle_coord(state, tokens[0], 0, &result.x0) ||
            !parse_clip_rectangle_coord(state, tokens[1], 1, &result.y0) ||
            !parse_clip_rectangle_coord(state, tokens[2], 2, &result.x1) ||
            !parse_clip_rectangle_coord(state, tokens[3], 3, &result.y1)) {
            result.reason = "invalid rectangle clip coordinate";
            return result;
        }
        result.type = CLIP_RECTANGLE;
        return result;
    }

    if (count == 1) {
        if (!validate_clip_drawing_token(tokens[0])) {
            result.reason = "invalid vector clip drawing data";
            return result;
        }
        result.type = CLIP_VECTOR;
        result.scale = 1;
        result.drawing = tokens[0];
        return result;
    }

    if (count == 2) {
        if (!parse_clip_scale(state, tokens[0], &result.scale)) {
            result.reason = "invalid vector clip scale";
            return result;
        }
        if (!validate_clip_drawing_token(tokens[1])) {
            result.reason = "invalid vector clip drawing data";
            return result;
        }
        result.type = CLIP_VECTOR;
        result.drawing = tokens[1];
        return result;
    }

    result.reason = "unsupported clip argument count";
    return result;
}

static void apply_clip_tag(RenderContext *state, const char *tag_name, bool inverse,
                           char *name_end, char *q, char *end, double pwr)
{
    ClipParseResult parsed = parse_clip_tag(state, tag_name, name_end, q, end);

    if (parsed.type == CLIP_RECTANGLE) {
        state->clip_x0 = state->clip_x0 * (1 - pwr) + parsed.x0 * pwr;
        state->clip_x1 = state->clip_x1 * (1 - pwr) + parsed.x1 * pwr;
        state->clip_y0 = state->clip_y0 * (1 - pwr) + parsed.y0 * pwr;
        state->clip_y1 = state->clip_y1 * (1 - pwr) + parsed.y1 * pwr;
        state->clip_mode = inverse ? 1 : 0;
        return;
    }

    if (parsed.type == CLIP_VECTOR) {
        if (state->clip_drawing_text.str) {
            return;
        }

        state->clip_drawing_text.str = parsed.drawing.start;
        state->clip_drawing_text.len = parsed.drawing.end - parsed.drawing.start;
        state->clip_drawing_scale = parsed.scale;
        state->clip_drawing_mode = inverse ? 1 : 0;
        return;
    }

    ass_msg(state->renderer->library, MSGL_DBG2,
            "PARSE %s rejected: %s", tag_name, parsed.reason);
}

static int32_t parse_alpha_tag(char *str)
{
    int32_t alpha = 0;

    while (*str == '&' || *str == 'H')
        ++str;

    mystrtoi32(&str, 16, &alpha);
    return alpha;
}

static uint32_t parse_color_tag(char *str)
{
    int32_t color = 0;

    while (*str == '&' || *str == 'H')
        ++str;

    mystrtoi32(&str, 16, &color);
    return ass_bswap32((uint32_t) color);
}

static bool parse_hex_override_arg(struct arg arg, int32_t *value)
{
    char *p = arg.start;
    while (p < arg.end && (*p == '&' || *p == 'H' || *p == 'h'))
        p++;

    char *start = p;
    while (p < arg.end &&
           ((*p >= '0' && *p <= '9') ||
            (*p >= 'a' && *p <= 'f') ||
            (*p >= 'A' && *p <= 'F')))
        p++;
    if (p == start)
        return false;

    char *parse = start;
    if (!mystrtoi32(&parse, 16, value) || parse != p)
        return false;

    while (p < arg.end && *p == '&')
        p++;
    return p == arg.end;
}

static bool parse_decoration_color_arg(struct arg arg, uint32_t *color)
{
    int32_t value;
    if (!parse_hex_override_arg(arg, &value))
        return false;
    *color = ass_bswap32((uint32_t) value);
    return true;
}

static bool parse_decoration_alpha_arg(struct arg arg, uint32_t *alpha)
{
    int32_t value;
    if (!parse_hex_override_arg(arg, &value))
        return false;
    *alpha = value;
    return true;
}

typedef struct {
    bool has_color;
    bool alpha_fade;
    uint32_t color;
} FadColorArg;

static bool split_fad_raw_args(char *start, char *end,
                               struct arg *args, int *nargs)
{
    *nargs = 0;
    char *arg_start = start;
    while (*nargs < 4) {
        char *arg_end = arg_start;
        while (arg_end < end && *arg_end != ',')
            arg_end++;
        args[*nargs] = (struct arg) { arg_start, arg_end };
        trim_arg_inline(&args[*nargs]);
        (*nargs)++;
        if (arg_end >= end)
            return true;
        arg_start = arg_end + 1;
    }

    return false;
}

static FadColorArg parse_fad_color_arg(struct arg arg)
{
    FadColorArg out = { .alpha_fade = true };
    trim_arg_inline(&arg);
    if (arg.start >= arg.end)
        return out;

    out.alpha_fade = false;
    if (arg.end - arg.start >= 2 &&
            arg.end[-2] == '+' &&
            (arg.end[-1] == 'a' || arg.end[-1] == 'A')) {
        out.alpha_fade = true;
        arg.end -= 2;
        trim_arg_inline(&arg);
    }

    if (parse_decoration_color_arg(arg, &out.color))
        out.has_color = true;
    else
        out.alpha_fade = true;
    return out;
}

static bool parse_extended_fad(char *start, char *end, int *fade_in,
                               int *fade_out, FadColorArg *start_color,
                               FadColorArg *end_color)
{
    struct arg raw[4];
    int raw_nargs = 0;
    if (!split_fad_raw_args(start, end, raw, &raw_nargs))
        return false;
    if (raw_nargs != 2 && raw_nargs != 4)
        return false;

    int32_t in, out;
    if (!parse_int32_arg_strict(raw[0], &in) ||
            !parse_int32_arg_strict(raw[1], &out))
        return false;

    *fade_in = in;
    *fade_out = out;
    *start_color = (FadColorArg) { .alpha_fade = true };
    *end_color = (FadColorArg) { .alpha_fade = true };
    if (raw_nargs == 4) {
        *start_color = parse_fad_color_arg(raw[2]);
        *end_color = parse_fad_color_arg(raw[3]);
    }
    return true;
}

typedef enum {
    BORDER_TAG_NONE,
    BORDER_TAG_IGNORE,
    BORDER_TAG_SIZE,
    BORDER_TAG_SIZE_X,
    BORDER_TAG_SIZE_Y,
    BORDER_TAG_COLOR,
    BORDER_TAG_ALPHA,
    BORDER_TAG_COLOR_GRADIENT,
    BORDER_TAG_ALPHA_GRADIENT,
    BORDER_TAG_MANGETSU_GRADIENT,
} NumberedBorderTag;

static bool is_digit_char(char c)
{
    return c >= '0' && c <= '9';
}

static bool match_border_suffix(char *p, char *end, const char *suffix,
                                char **arg_start)
{
    char *q = p;
    while (*suffix && q < end && *q == *suffix) {
        q++;
        suffix++;
    }
    if (*suffix)
        return false;
    *arg_start = q;
    return true;
}

static NumberedBorderTag parse_numbered_border_tag(char *p, char *name_end,
                                                   int *layer,
                                                   struct arg *inline_arg)
{
    if (p >= name_end || !is_digit_char(*p))
        return BORDER_TAG_NONE;

    int raw_layer = 0;
    char *q = p;
    while (q < name_end && is_digit_char(*q)) {
        if (raw_layer <= 100)
            raw_layer = raw_layer * 10 + (*q - '0');
        q++;
    }

    if (q >= name_end || *q != 'b')
        return BORDER_TAG_NONE;

    NumberedBorderTag tag = BORDER_TAG_IGNORE;
    char *arg_start = q;
    if (match_border_suffix(q, name_end, "bsx", &arg_start))
        tag = BORDER_TAG_SIZE_X;
    else if (match_border_suffix(q, name_end, "bsy", &arg_start))
        tag = BORDER_TAG_SIZE_Y;
    else if (match_border_suffix(q, name_end, "bs", &arg_start))
        tag = BORDER_TAG_SIZE;
    else if (match_border_suffix(q, name_end, "bgrd", &arg_start))
        tag = BORDER_TAG_MANGETSU_GRADIENT;
    else if (match_border_suffix(q, name_end, "bvc", &arg_start))
        tag = BORDER_TAG_COLOR_GRADIENT;
    else if (match_border_suffix(q, name_end, "bva", &arg_start))
        tag = BORDER_TAG_ALPHA_GRADIENT;
    else if (match_border_suffix(q, name_end, "bc", &arg_start))
        tag = BORDER_TAG_COLOR;
    else if (match_border_suffix(q, name_end, "ba", &arg_start))
        tag = BORDER_TAG_ALPHA;

    if (tag == BORDER_TAG_IGNORE || raw_layer < 1 ||
            raw_layer > ASS_BORDER_LAYERS_MAX)
        return BORDER_TAG_IGNORE;

    *layer = raw_layer - 1;
    inline_arg->start = arg_start;
    inline_arg->end = name_end;
    rskip_spaces(&inline_arg->end, inline_arg->start);
    return tag;
}

static bool tag_name_matches(char *p, char *name_end, const char *name)
{
    size_t len = strlen(name);
    if (len > (size_t) (name_end - p) || strncmp(p, name, len))
        return false;
    if (p + len < name_end && isalpha((unsigned char) p[len]) &&
            strcmp(name, "fn"))
        return false;
    return true;
}

static bool colorcode_tag_allowed(char *p, char *name_end)
{
    static const char *const allowed[] = {
        "bord", "xbord", "ybord",
        "shad", "xshad", "yshad",
        "blur", "be",
        "alpha", "1a", "2a", "3a", "4a",
        "fn", "fs",
        "1c", "2c", "3c", "4c", "5c", "c",
        "1grd", "2grd", "3grd", "4grd", "5grd",
        "b", "i", "u", "s",
    };

    for (int i = 0; i < (int) (sizeof(allowed) / sizeof(allowed[0])); i++)
        if (tag_name_matches(p, name_end, allowed[i]))
            return true;
    return false;
}

static bool parse_double_arg_strict(struct arg arg, double *out)
{
    char *ptr = arg.start;
    if (!mystrtod(&ptr, out))
        return false;
    skip_spaces(&ptr);
    return ptr == arg.end && isfinite(*out);
}

static bool parse_int32_arg_strict(struct arg arg, int32_t *out)
{
    char *ptr = arg.start;
    if (!mystrtoi32(&ptr, 10, out))
        return false;
    skip_spaces(&ptr);
    return ptr == arg.end;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool parse_hex_arg_strict(struct arg arg, uint32_t *out)
{
    char *ptr = arg.start;
    while (ptr < arg.end && (*ptr == '&' || *ptr == 'H' || *ptr == 'h'))
        ptr++;

    uint32_t value = 0;
    bool have_digit = false;
    while (ptr < arg.end) {
        int v = hex_value(*ptr);
        if (v < 0)
            break;
        value = (value << 4) | (uint32_t) v;
        have_digit = true;
        ptr++;
    }
    if (!have_digit)
        return false;
    if (ptr < arg.end && *ptr == '&')
        ptr++;
    skip_spaces(&ptr);
    if (ptr != arg.end)
        return false;

    *out = value;
    return true;
}

static bool parse_ass_color_arg_strict(struct arg arg, uint32_t *out)
{
    uint32_t value;
    if (!parse_hex_arg_strict(arg, &value) || value > 0xFFFFFF)
        return false;
    *out = ass_bswap32(value);
    return true;
}

static bool parse_percentage_arg_strict(struct arg arg, double *out)
{
    if (arg.start >= arg.end || arg.end[-1] != '%')
        return false;

    struct arg value = { arg.start, arg.end - 1 };
    rskip_spaces(&value.end, value.start);
    double percent;
    if (!parse_double_arg_strict(value, &percent) ||
            percent < 0.0 || percent > 100.0)
        return false;

    *out = percent / 100.0;
    return true;
}

static bool parse_mangetsu_gradient_reset_arg(struct arg arg)
{
    int32_t value;
    return parse_int32_arg_strict(arg, &value) && value == 0;
}

static bool split_mangetsu_gradient_raw_args(char *start, char *end,
                                             struct arg *args, int *nargs)
{
    *nargs = 0;
    char *arg_start = start;
    while (1) {
        if (*nargs >= 2 * MANGETSU_GRADIENT_MAX_STOPS - 1)
            return false;

        char *arg_end = arg_start;
        while (arg_end < end && *arg_end != ',')
            arg_end++;

        struct arg arg = { arg_start, arg_end };
        trim_arg_inline(&arg);
        if (arg.start >= arg.end)
            return false;
        args[(*nargs)++] = arg;

        if (arg_end >= end)
            return true;
        arg_start = arg_end + 1;
    }
}

static bool parse_mangetsu_gradient_args(struct arg *args, int nargs,
                                         MangetsuGradientLayer *out)
{
    if (nargs < 3 || !(nargs & 1) ||
            nargs > 2 * MANGETSU_GRADIENT_MAX_STOPS - 1)
        return false;

    MangetsuGradientLayer parsed = {0};
    if (!parse_double_arg_strict(args[0], &parsed.angle) ||
            !parse_ass_color_arg_strict(args[1], &parsed.stops[0].color))
        return false;

    parsed.active = true;
    parsed.type = MANGETSU_GRADIENT_TYPE_LINEAR;
    parsed.n_stops = 1;
    parsed.stops[0].offset = 0.0;
    double last_offset = 0.0;
    for (int i = 2; i < nargs - 1; i += 2) {
        double offset;
        uint32_t color;
        if (!parse_percentage_arg_strict(args[i], &offset) ||
                offset < last_offset ||
                !parse_ass_color_arg_strict(args[i + 1], &color))
            return false;

        parsed.stops[parsed.n_stops++] = (MangetsuGradientStop) {
            .offset = offset,
            .color = color,
        };
        last_offset = offset;
    }

    uint32_t color;
    if (!parse_ass_color_arg_strict(args[nargs - 1], &color))
        return false;
    parsed.stops[parsed.n_stops++] = (MangetsuGradientStop) {
        .offset = 1.0,
        .color = color,
    };

    *out = parsed;
    return true;
}

static bool parse_mangetsu_gradient_raw_args(char *start, char *end,
                                             MangetsuGradientLayer *out)
{
    struct arg args[2 * MANGETSU_GRADIENT_MAX_STOPS - 1];
    int nargs = 0;
    return split_mangetsu_gradient_raw_args(start, end, args, &nargs) &&
           parse_mangetsu_gradient_args(args, nargs, out);
}

static void mark_rgba_needed(RenderContext *state);
static void default_extra_border_color(RenderContext *state, int layer);
static void sync_layer1_border(RenderContext *state);

static bool parse_mangetsu_fill_gradient_tag(char *p, char *name_end,
                                             int *layer,
                                             struct arg *inline_arg)
{
    if (name_end - p < 4 || !is_digit_char(p[0]) ||
            p[1] != 'g' || p[2] != 'r' || p[3] != 'd')
        return false;

    int raw_layer = p[0] - '0';
    if (raw_layer < 1 || raw_layer > MANGETSU_GRADIENT_LAYERS)
        return false;

    *layer = raw_layer - 1;
    inline_arg->start = p + 4;
    inline_arg->end = name_end;
    rskip_spaces(&inline_arg->end, inline_arg->start);
    return true;
}

static void disable_mangetsu_gradient_layer(RenderContext *state, int layer)
{
    if (layer < 0 || layer >= MANGETSU_GRADIENT_LAYERS)
        return;
    ass_mangetsu_gradient_layer_reset(&state->mangetsu_gradient.layer[layer]);
}

static void disable_mangetsu_border_gradient_layer(RenderContext *state,
                                                   int layer)
{
    if (layer < 0 || layer >= MANGETSU_GRADIENT_BORDER_LAYERS)
        return;
    ass_mangetsu_gradient_layer_reset(&state->mangetsu_gradient.border[layer]);
}

static void apply_mangetsu_gradient_layer(RenderContext *state, int layer,
                                          const MangetsuGradientLayer *gradient)
{
    if (layer < 0 || layer >= MANGETSU_GRADIENT_LAYERS)
        return;

    MangetsuGradientLayer dst = *gradient;
    dst.segment_id = ++state->mangetsu_gradient_next_id;
    dst.rect = (GradientRect) {0};
    state->mangetsu_gradient.layer[layer] = dst;
}

static void apply_mangetsu_border_gradient_layer(RenderContext *state,
                                                 int layer,
                                                 const MangetsuGradientLayer *gradient)
{
    if (layer < 0 || layer >= MANGETSU_GRADIENT_BORDER_LAYERS)
        return;

    MangetsuGradientLayer dst = *gradient;
    dst.segment_id = ++state->mangetsu_gradient_next_id;
    dst.rect = (GradientRect) {0};
    state->mangetsu_gradient.border[layer] = dst;
}

static uint32_t mix_mangetsu_tag_color(uint32_t old, uint32_t target,
                                       double pwr)
{
    uint32_t out = old;
    change_color(&out, target, pwr);
    return out;
}

static uint32_t sample_mangetsu_stop_color(const MangetsuGradientLayer *layer,
                                           double offset)
{
    if (!layer || !layer->active || layer->n_stops <= 0)
        return 0;
    if (offset <= layer->stops[0].offset)
        return layer->stops[0].color;

    for (int i = 1; i < layer->n_stops; i++) {
        const MangetsuGradientStop *prev = &layer->stops[i - 1];
        const MangetsuGradientStop *next = &layer->stops[i];
        if (offset > next->offset)
            continue;

        double span = next->offset - prev->offset;
        if (span <= 0.0)
            return next->color;
        return mix_mangetsu_tag_color(prev->color, next->color,
                                      (offset - prev->offset) / span);
    }

    return layer->stops[layer->n_stops - 1].color;
}

static bool same_mangetsu_stop_offset(double a, double b)
{
    return fabs(a - b) < 0.000001;
}

static void append_mangetsu_stop_offset(double *offsets, int *count,
                                        double offset)
{
    if (*count > 0 && same_mangetsu_stop_offset(offsets[*count - 1], offset))
        return;
    if (*count >= MANGETSU_GRADIENT_MAX_STOPS) {
        offsets[MANGETSU_GRADIENT_MAX_STOPS - 1] = 1.0;
        return;
    }
    offsets[(*count)++] = offset;
}

static int collect_mangetsu_transform_offsets(
    const MangetsuGradientLayer *source, const MangetsuGradientLayer *target,
    double *offsets)
{
    int count = 0;
    if (!source || !source->active) {
        for (int i = 0; i < target->n_stops; i++)
            append_mangetsu_stop_offset(offsets, &count,
                                        target->stops[i].offset);
    } else {
        int i = 0;
        int j = 0;
        while (i < source->n_stops || j < target->n_stops) {
            double offset;
            if (j >= target->n_stops ||
                    (i < source->n_stops &&
                     source->stops[i].offset < target->stops[j].offset)) {
                offset = source->stops[i++].offset;
            } else if (i >= source->n_stops ||
                       target->stops[j].offset < source->stops[i].offset) {
                offset = target->stops[j++].offset;
            } else {
                offset = source->stops[i].offset;
                i++;
                j++;
            }
            append_mangetsu_stop_offset(offsets, &count, offset);
        }
    }

    if (count == 0)
        append_mangetsu_stop_offset(offsets, &count, 0.0);
    if (!same_mangetsu_stop_offset(offsets[count - 1], 1.0)) {
        if (count >= MANGETSU_GRADIENT_MAX_STOPS)
            offsets[count - 1] = 1.0;
        else
            append_mangetsu_stop_offset(offsets, &count, 1.0);
    }
    return count;
}

static double interpolate_mangetsu_angle(double old, double target,
                                         double pwr)
{
    double delta = fmod(target - old, 360.0);
    if (delta > 180.0)
        delta -= 360.0;
    else if (delta < -180.0)
        delta += 360.0;

    double result = old + delta * pwr;
    result = fmod(result, 360.0);
    if (result < 0.0)
        result += 360.0;
    return result;
}

static void transform_mangetsu_gradient_layer(RenderContext *state,
                                              MangetsuGradientLayer *dst,
                                              const MangetsuGradientLayer *target,
                                              uint32_t solid_color,
                                              double pwr)
{
    MangetsuGradientLayer source = *dst;
    bool source_active = source.active && source.n_stops > 0;
    double offsets[MANGETSU_GRADIENT_MAX_STOPS];
    int count = collect_mangetsu_transform_offsets(
        source_active ? &source : NULL, target, offsets);

    MangetsuGradientLayer result = {0};
    result.active = true;
    result.type = MANGETSU_GRADIENT_TYPE_LINEAR;
    result.segment_id = source_active ?
        source.segment_id : ++state->mangetsu_gradient_next_id;
    result.angle = source_active ?
        interpolate_mangetsu_angle(source.angle, target->angle, pwr) :
        target->angle;
    result.n_stops = count;

    solid_color &= 0xFFFFFF00u;
    for (int i = 0; i < count; i++) {
        double offset = offsets[i];
        uint32_t source_color = source_active ?
            sample_mangetsu_stop_color(&source, offset) : solid_color;
        uint32_t target_color =
            sample_mangetsu_stop_color(target, offset);
        result.stops[i] = (MangetsuGradientStop) {
            .offset = offset,
            .color = mix_mangetsu_tag_color(source_color, target_color, pwr),
        };
    }

    *dst = result;
}

static void disable_mangetsu_color_source(RenderContext *state, int layer)
{
    if (layer == 2)
        disable_mangetsu_border_gradient_layer(state, 0);
    else
        disable_mangetsu_gradient_layer(state, layer);
}

static uint32_t mangetsu_solid_color_for_target(RenderContext *state,
                                                MangetsuGradientTarget target,
                                                int layer)
{
    if (target == MANGETSU_GRADIENT_TARGET_BORDER) {
        if (layer == 0)
            return state->c[2];
        default_extra_border_color(state, layer);
        return state->border_layers[layer].color;
    }

    if (layer == 2)
        return state->c[2];
    if (layer == 4) {
        if (state->decoration_color_set)
            return (state->decoration_color & 0xFFFFFF00u) |
                   _a(state->c[0]);
        return state->c[0];
    }
    return layer >= 0 && layer < 4 ? state->c[layer] : 0;
}

static bool apply_mangetsu_gradient_tag(RenderContext *state,
                                        MangetsuGradientTarget target,
                                        int layer, char *name_end, char *q,
                                        struct arg *args, int nargs,
                                        struct arg inline_arg, double pwr,
                                        bool nested)
{
    bool is_border = target == MANGETSU_GRADIENT_TARGET_BORDER;
    if ((is_border && (layer < 0 ||
                       layer >= MANGETSU_GRADIENT_BORDER_LAYERS)) ||
            (!is_border && (layer < 0 ||
                            layer >= MANGETSU_GRADIENT_LAYERS)))
        return true;

    if (*name_end == '(') {
        char *raw_start = name_end + 1;
        char *raw_end = q;
        if (raw_end > raw_start && raw_end[-1] == ')')
            raw_end--;
        if (raw_start == raw_end) {
            if (nested)
                return true;
            if (is_border)
                disable_mangetsu_border_gradient_layer(state, layer);
            else
                disable_mangetsu_color_source(state, layer);
            return true;
        }

        MangetsuGradientLayer gradient;
        if (!parse_mangetsu_gradient_raw_args(raw_start, raw_end, &gradient))
            return true;

        uint32_t solid_color =
            mangetsu_solid_color_for_target(state, target, layer);
        MangetsuGradientLayer *dst = NULL;
        if (is_border) {
            BorderLayerState *border = &state->border_layers[layer];
            default_extra_border_color(state, layer);
            ass_gradient_values_disable_color(&border->gradient,
                                              border->color, 1.0);
            border->has_color = true;
            dst = &state->mangetsu_gradient.border[layer];
            if (layer == 0) {
                ass_gradient_disable_color(&state->gradient, 2,
                                           state->c[2], 1.0);
                disable_image_fill_layer(state, 2);
                sync_layer1_border(state);
            }
        } else if (layer == 2) {
            ass_gradient_disable_color(&state->gradient, 2, state->c[2], 1.0);
            dst = &state->mangetsu_gradient.border[0];
            disable_image_fill_layer(state, 2);
            sync_layer1_border(state);
        } else {
            if (layer < 4) {
                ass_gradient_disable_color(&state->gradient, layer,
                                           state->c[layer], 1.0);
                if (layer == 0 || layer == 1) {
                    disable_image_fill_layer(state, 0);
                    disable_image_fill_layer(state, 1);
                } else {
                    disable_image_fill_layer(state, layer);
                }
            }
            dst = &state->mangetsu_gradient.layer[layer];
        }
        if (nested)
            transform_mangetsu_gradient_layer(state, dst, &gradient,
                                              solid_color, pwr);
        else if (is_border)
            apply_mangetsu_border_gradient_layer(state, layer, &gradient);
        else if (layer == 2)
            apply_mangetsu_border_gradient_layer(state, 0, &gradient);
        else
            apply_mangetsu_gradient_layer(state, layer, &gradient);
        mark_rgba_needed(state);
        return true;
    }

    struct arg arg = (inline_arg.start && inline_arg.start < inline_arg.end) ?
        inline_arg : (nargs ? args[0] : (struct arg) { NULL, NULL });
    if (arg.start && parse_mangetsu_gradient_reset_arg(arg)) {
        if (nested)
            return true;
        if (is_border)
            disable_mangetsu_border_gradient_layer(state, layer);
        else
            disable_mangetsu_color_source(state, layer);
    }
    return true;
}

static void fill_gradient_colors(GradientValues *values, uint32_t color)
{
    for (int i = 0; i < 4; i++)
        values->color[i] = color;
}

static void fill_gradient_alphas(GradientValues *values, uint8_t alpha)
{
    for (int i = 0; i < 4; i++)
        values->alpha[i] = alpha;
}

static void mark_rgba_needed(RenderContext *state)
{
    state->needs_rgba = true;
    state->renderer->track->has_rgba = 1;
}

static void default_extra_border_color(RenderContext *state, int layer)
{
    BorderLayerState *border = &state->border_layers[layer];
    uint32_t layer1 = state->border_layers[0].color;
    if (!border->has_color) {
        border->color = (layer1 & 0xFFFFFF00u) | _a(border->color);
        fill_gradient_colors(&border->gradient, border->color);
    }
    if (!border->has_alpha) {
        border->color = (border->color & 0xFFFFFF00u) | _a(layer1);
        fill_gradient_alphas(&border->gradient, _a(border->color));
    }
}

static void sync_layer1_border(RenderContext *state)
{
    state->border_layers[0].enabled = state->border_x > 0 || state->border_y > 0;
    state->border_layers[0].has_color = true;
    state->border_layers[0].has_alpha = true;
    state->border_layers[0].size_x = state->border_x;
    state->border_layers[0].size_y = state->border_y;
    state->border_layers[0].color = state->c[2];
    state->border_layers[0].gradient = state->gradient.layer[2];
}

static void apply_all_border_alpha(RenderContext *state, uint32_t alpha,
                                   double pwr)
{
    change_alpha(&state->c[2], alpha, pwr);
    ass_gradient_disable_alpha(&state->gradient, 2, _a(state->c[2]), pwr);
    sync_layer1_border(state);

    for (int layer = 1; layer < ASS_BORDER_LAYERS_MAX; layer++) {
        BorderLayerState *border = &state->border_layers[layer];
        default_extra_border_color(state, layer);
        change_alpha(&border->color, alpha, pwr);
        ass_gradient_values_disable_alpha(&border->gradient,
                                          _a(border->color), pwr);
        border->has_alpha = true;
    }
}

static void set_border_layer_size_pair(RenderContext *state, int layer,
                                       bool set_x, bool set_y,
                                       double val_x, double val_y,
                                       double pwr)
{
    BorderLayerState *border = &state->border_layers[layer];
    bool was_enabled = border->enabled;
    if (layer > 0)
        default_extra_border_color(state, layer);

    if (set_x) {
        double x = border->size_x * (1 - pwr) + val_x * pwr;
        border->size_x = x < 0 ? 0 : x;
    }
    if (set_y) {
        double y = border->size_y * (1 - pwr) + val_y * pwr;
        border->size_y = y < 0 ? 0 : y;
    }
    border->enabled = border->size_x > 0 || border->size_y > 0;
    if (layer > 0 && border->enabled && !was_enabled) {
        border->has_color = true;
        border->has_alpha = true;
    }

    if (layer == 0) {
        state->border_x = border->size_x;
        state->border_y = border->size_y;
    }
}

static void set_border_layer_size(RenderContext *state, int layer,
                                  bool set_x, bool set_y, double val,
                                  double pwr)
{
    set_border_layer_size_pair(state, layer, set_x, set_y, val, val, pwr);
}

static void copy_gradient_color(GradientValues *dst, const GradientValues *src)
{
    dst->color_enabled = src->color_enabled;
    for (int i = 0; i < 4; i++)
        dst->color[i] = src->color[i];
}

static void apply_numbered_border_tag(RenderContext *state,
                                      NumberedBorderTag tag, int layer,
                                      struct arg *args, int nargs,
                                      struct arg inline_arg,
                                      char *name_end, char *tag_end,
                                      double pwr, bool nested)
{
    struct arg arg = (inline_arg.start && inline_arg.start < inline_arg.end) ? inline_arg :
        (nargs ? args[0] : (struct arg) { NULL, NULL });

    switch (tag) {
    case BORDER_TAG_SIZE:
    case BORDER_TAG_SIZE_X:
    case BORDER_TAG_SIZE_Y: {
        double val;
        if (!arg.start) {
            const BorderLayerState *def =
                &state->default_style.border_layers[layer];
            set_border_layer_size_pair(state, layer,
                                       tag != BORDER_TAG_SIZE_Y,
                                       tag != BORDER_TAG_SIZE_X,
                                       def->size_x, def->size_y, pwr);
            break;
        } else if (!parse_double_arg_strict(arg, &val)) {
            return;
        }
        set_border_layer_size(state, layer,
                              tag != BORDER_TAG_SIZE_Y,
                              tag != BORDER_TAG_SIZE_X,
                              val, pwr);
        break;
    }
    case BORDER_TAG_COLOR: {
        uint32_t val;
        if (layer == 0) {
            if (arg.start) {
                if (!parse_hex_arg_strict(arg, &val))
                    return;
                change_color(&state->c[2], ass_bswap32(val), pwr);
                ass_gradient_disable_color(&state->gradient, 2,
                                           state->c[2], pwr);
            } else {
                change_color(&state->c[2],
                             state->default_style.c[2], 1);
                copy_gradient_color(&state->gradient.layer[2],
                                    &state->default_style.gradient.layer[2]);
            }
            if (pwr >= 1.0)
                disable_image_fill_layer(state, 2);
            if (!nested && pwr > 0.0)
                disable_mangetsu_border_gradient_layer(state, 0);
            sync_layer1_border(state);
        } else {
            BorderLayerState *border = &state->border_layers[layer];
            if (arg.start) {
                if (!parse_hex_arg_strict(arg, &val))
                    return;
                default_extra_border_color(state, layer);
                change_color(&border->color, ass_bswap32(val), pwr);
                ass_gradient_values_disable_color(&border->gradient,
                                                  border->color, pwr);
                border->has_color = true;
            } else {
                const BorderLayerState *def =
                    &state->default_style.border_layers[layer];
                border->color = (def->color & 0xFFFFFF00u) |
                                _a(border->color);
                copy_gradient_color(&border->gradient, &def->gradient);
                border->has_color = def->has_color;
            }
            if (!nested && pwr > 0.0)
                disable_mangetsu_border_gradient_layer(state, layer);
        }
        break;
    }
    case BORDER_TAG_ALPHA: {
        uint32_t val;
        if (layer == 0) {
            if (arg.start) {
                if (!parse_hex_arg_strict(arg, &val) || val > 0xFF)
                    return;
                change_alpha(&state->c[2], val, pwr);
            } else {
                change_alpha(&state->c[2],
                             _a(state->default_style.c[2]), 1);
            }
            ass_gradient_disable_alpha(&state->gradient, 2,
                                       _a(state->c[2]), pwr);
            sync_layer1_border(state);
        } else {
            BorderLayerState *border = &state->border_layers[layer];
            if (arg.start) {
                if (!parse_hex_arg_strict(arg, &val) || val > 0xFF)
                    return;
                default_extra_border_color(state, layer);
                change_alpha(&border->color, val, pwr);
                ass_gradient_values_disable_alpha(&border->gradient,
                                                  _a(border->color), pwr);
                border->has_alpha = true;
            } else {
                const BorderLayerState *def =
                    &state->default_style.border_layers[layer];
                border->color = (border->color & 0xFFFFFF00u) |
                                _a(def->color);
                ass_gradient_values_disable_alpha(&border->gradient,
                                                  _a(border->color), pwr);
                border->has_alpha = def->has_alpha;
            }
        }
        break;
    }
    case BORDER_TAG_MANGETSU_GRADIENT:
        apply_mangetsu_gradient_tag(state, MANGETSU_GRADIENT_TARGET_BORDER,
                                    layer, name_end, tag_end,
                                    args, nargs, inline_arg, pwr, nested);
        break;
    case BORDER_TAG_COLOR_GRADIENT:
        if (layer == 0) {
            if (nargs) {
                uint32_t vals[4];
                int cnt = FFMIN(nargs, 4);
                for (int i = 0; i < cnt; i++)
                    vals[i] = parse_color_tag(args[i].start);
                disable_mangetsu_border_gradient_layer(state, 0);
                ass_gradient_apply_color(&state->gradient, 2, vals, cnt, pwr);
                disable_image_fill_layer(state, 2);
                mark_rgba_needed(state);
            } else {
                if (pwr > 0.0)
                    disable_mangetsu_border_gradient_layer(state, 0);
                ass_gradient_disable_color(&state->gradient, 2,
                                           state->c[2], pwr);
            }
            sync_layer1_border(state);
        } else {
            BorderLayerState *border = &state->border_layers[layer];
            default_extra_border_color(state, layer);
            if (nargs) {
                uint32_t vals[4];
                int cnt = FFMIN(nargs, 4);
                for (int i = 0; i < cnt; i++)
                    vals[i] = parse_color_tag(args[i].start);
                disable_mangetsu_border_gradient_layer(state, layer);
                ass_gradient_values_apply_color(&border->gradient,
                                                vals, cnt, pwr);
                border->has_color = true;
                mark_rgba_needed(state);
            } else {
                if (pwr > 0.0)
                    disable_mangetsu_border_gradient_layer(state, layer);
                ass_gradient_values_disable_color(&border->gradient,
                                                  border->color, pwr);
            }
        }
        break;
    case BORDER_TAG_ALPHA_GRADIENT:
        if (layer == 0) {
            if (nargs) {
                uint8_t vals[4];
                int cnt = FFMIN(nargs, 4);
                for (int i = 0; i < cnt; i++)
                    vals[i] = (uint8_t) parse_alpha_tag(args[i].start);
                ass_gradient_apply_alpha(&state->gradient, 2, vals, cnt, pwr);
                disable_image_fill_layer(state, 2);
                mark_rgba_needed(state);
            } else {
                ass_gradient_disable_alpha(&state->gradient, 2,
                                           _a(state->c[2]), pwr);
            }
            sync_layer1_border(state);
        } else {
            BorderLayerState *border = &state->border_layers[layer];
            default_extra_border_color(state, layer);
            if (nargs) {
                uint8_t vals[4];
                int cnt = FFMIN(nargs, 4);
                for (int i = 0; i < cnt; i++)
                    vals[i] = (uint8_t) parse_alpha_tag(args[i].start);
                ass_gradient_values_apply_alpha(&border->gradient,
                                                vals, cnt, pwr);
                border->has_alpha = true;
                mark_rgba_needed(state);
            } else {
                ass_gradient_values_disable_alpha(&border->gradient,
                                                  _a(border->color), pwr);
            }
        }
        break;
    default:
        break;
    }
}

/**
 * \brief find style by name as in \r
 * \param track track
 * \param name style name
 * \param len style name length
 * \return style in track->styles
 * Returns NULL if no style has the given name.
 */
static ASS_Style *lookup_style_strict(ASS_Track *track, char *name, size_t len)
{
    int i;
    for (i = track->n_styles - 1; i >= 0; --i) {
        if (strncmp(track->styles[i].Name, name, len) == 0 &&
            track->styles[i].Name[len] == '\0')
            return track->styles + i;
    }
    ass_msg(track->library, MSGL_WARN,
            "[%p]: Warning: no style named '%.*s' found",
            track, (int) len, name);
    return NULL;
}

/**
 * \brief Parse style override tags.
 * \param p string to parse
 * \param end end of string to parse, which must be '}', ')', or the first
 *            of a number of spaces immediately preceding '}' or ')'
 * \param pwr multiplier for some tag effects (comes from \t tags)
 */
char *ass_parse_tags(RenderContext *state, char *p, char *end, double pwr,
                     bool nested)
{
    ASS_Renderer *render_priv = state->renderer;
    for (char *q; p < end; p = q) {
        while (*p != '\\' && p != end)
            ++p;
        if (*p != '\\')
            break;
        ++p;
        if (p != end)
            skip_spaces(&p);

        q = p;
        while (*q != '(' && *q != '\\' && q != end)
            ++q;
        if (q == p)
            continue;

        char *name_end = q;

        // Store one extra element to be able to detect excess arguments
        struct arg args[MAX_VALID_NARGS + 1];
        int nargs = 0;
        bool has_backslash_arg = false;
        for (int i = 0; i <= MAX_VALID_NARGS; ++i)
            args[i].start = args[i].end = "";

        size_t name_len = name_end - p;
        bool is_transition = name_len == 1 && p[0] == 't';

        // Split parenthesized arguments. Do this for all tags and before
        // any non-parenthesized argument because that's what VSFilter does.
        if (*q == '(') {
            ++q;
            if (is_transition) {
                int depth = 1;
                char *arg_start = q;
                while (q != end && depth > 0) {
                    if (*q == '\\')
                        has_backslash_arg = true;
                    if (*q == '(') {
                        depth++;
                    } else if (*q == ')') {
                        depth--;
                        if (depth == 0) {
                            push_arg(args, &nargs, arg_start, q);
                            ++q;
                            break;
                        }
                    } else if (*q == ',' && depth == 1) {
                        push_arg(args, &nargs, arg_start, q);
                        arg_start = q + 1;
                    }
                    ++q;
                }
                if (depth > 0 && arg_start < q)
                    push_arg(args, &nargs, arg_start, q);
            } else {
                while (1) {
                    if (q != end)
                        skip_spaces(&q);

                    // Split on commas. If there is a backslash, ignore any
                    // commas following it and lump everything starting from
                    // the last comma, through the backslash and all the way
                    // to the end of the argument string into a single argument.

                    char *r = q;
                    while (*r != ',' && *r != '\\' && *r != ')' && r != end)
                        ++r;

                    if (*r == ',') {
                        push_arg(args, &nargs, q, r);
                        q = r + 1;
                    } else {
                        // Swallow the rest of the parenthesized string. This could
                        // be either a backslash-argument or simply the last argument.
                        if (*r == '\\') {
                            has_backslash_arg = true;
                            char *paren = memchr(r, ')', end - r);
                            if (paren)
                                r = paren;
                            else
                                r = end;
                        }
                        push_arg(args, &nargs, q, r);
                        q = r;
                        // The closing parenthesis could be missing.
                        if (q != end)
                            ++q;
                        break;
                    }
                }
            }
        }

#define tag(name) (mystrcmp(&p, (name)) && (push_arg(args, &nargs, p, name_end), 1))
#define complex_tag(name) mystrcmp(&p, (name))
#define column_default(fields) do { \
            if (!nested && !state->colorcode_parse) \
                ass_column_update_default(state, (fields)); \
        } while (0)

        // New tags introduced in vsfilter 2.39
        int numbered_border_layer = -1;
        struct arg numbered_border_arg = { NULL, NULL };
        NumberedBorderTag numbered_border_tag =
            parse_numbered_border_tag(p, name_end, &numbered_border_layer,
                                      &numbered_border_arg);
        int mangetsu_fill_layer = -1;
        struct arg mangetsu_fill_arg = { NULL, NULL };
        bool mangetsu_fill_tag =
            parse_mangetsu_fill_gradient_tag(p, name_end,
                                             &mangetsu_fill_layer,
                                             &mangetsu_fill_arg);
        if (numbered_border_tag == BORDER_TAG_IGNORE) {
            continue;
        } else if (numbered_border_tag != BORDER_TAG_NONE) {
            apply_numbered_border_tag(state, numbered_border_tag,
                                      numbered_border_layer, args, nargs,
                                      numbered_border_arg, name_end, q, pwr,
                                      nested);
        } else if (state->colorcode_parse &&
                   !colorcode_tag_allowed(p, name_end)) {
            continue;
        } else if (tag("colsp")) {
            if (nargs) {
                double val;
                if (parse_double_arg_strict(*args, &val))
                    ass_column_set_spacing(state, val);
            }
        } else if (tag("colan")) {
            if (nargs) {
                int32_t val;
                if (parse_int32_arg_strict(*args, &val) && val >= 1 && val <= 9)
                    ass_column_set_align(state, val);
            }
        } else if (tag("col")) {
            if (nargs) {
                int32_t val;
                if (parse_int32_arg_strict(*args, &val) &&
                        (val == 0 || val == 1))
                    ass_column_set_mode(state, val == 1);
            }
        } else if (tag("xbord")) {
            double val;
            if (nargs) {
                val = argtod(*args);
                val = state->border_x * (1 - pwr) + val * pwr;
                val = (val < 0) ? 0 : val;
            } else
                val = state->default_style.border_x;
            state->border_x = val;
            sync_layer1_border(state);
            column_default(COLUMN_STYLE_BORDER_X);
        } else if (tag("ybord")) {
            double val;
            if (nargs) {
                val = argtod(*args);
                val = state->border_y * (1 - pwr) + val * pwr;
                val = (val < 0) ? 0 : val;
            } else
                val = state->default_style.border_y;
            state->border_y = val;
            sync_layer1_border(state);
            column_default(COLUMN_STYLE_BORDER_Y);
        } else if (tag("xshad")) {
            double val;
            if (nargs) {
                val = argtod(*args);
                val = state->shadow_x * (1 - pwr) + val * pwr;
            } else
                val = state->default_style.shadow_x;
            state->shadow_x = val;
            column_default(COLUMN_STYLE_SHADOW_X);
        } else if (tag("yshad")) {
            double val;
            if (nargs) {
                val = argtod(*args);
                val = state->shadow_y * (1 - pwr) + val * pwr;
            } else
                val = state->default_style.shadow_y;
            state->shadow_y = val;
            column_default(COLUMN_STYLE_SHADOW_Y);
        } else if (tag("fax")) {
            double val;
            if (nargs) {
                val = argtod(*args);
                state->fax =
                    val * pwr + state->fax * (1 - pwr);
            } else
                state->fax = 0.;
        } else if (tag("fay")) {
            double val;
            if (nargs) {
                val = argtod(*args);
                state->fay =
                    val * pwr + state->fay * (1 - pwr);
            } else
                state->fay = 0.;
        } else if (complex_tag("rndx")) {
            // Match axis-specific rnd* first so \rnd does not swallow them
            double val = 0.0;
            if (nargs) {
                val = fabs(argtod(*args));
            } else {
                char *tmp = p;
                if (rnd_numeric_start(*tmp))
                    mystrtod(&tmp, &val);
                val = fabs(val);
                p = tmp;
            }
            if (val > ASS_RND_MAX_PX)
                val = ASS_RND_MAX_PX;
            state->rnd_x = calc_anim(val, state->rnd_x, pwr);
        } else if (complex_tag("rndy")) {
            double val = 0.0;
            if (nargs) {
                val = fabs(argtod(*args));
            } else {
                char *tmp = p;
                if (rnd_numeric_start(*tmp))
                    mystrtod(&tmp, &val);
                val = fabs(val);
                p = tmp;
            }
            if (val > ASS_RND_MAX_PX)
                val = ASS_RND_MAX_PX;
            state->rnd_y = calc_anim(val, state->rnd_y, pwr);
        } else if (complex_tag("rndz")) {
            double val = 0.0;
            if (nargs) {
                val = fabs(argtod(*args));
            } else {
                char *tmp = p;
                if (rnd_numeric_start(*tmp))
                    mystrtod(&tmp, &val);
                val = fabs(val);
                p = tmp;
            }
            if (val > ASS_RND_MAX_PX)
                val = ASS_RND_MAX_PX;
            state->rnd_z = calc_anim(val, state->rnd_z, pwr);
        } else if (name_len >= 3 && !strncmp(p, "rnd", 3)) {
            char next = (name_len > 3) ? p[3] : '\0';
            if (!rnd_numeric_start(next))
                continue;
            if (!mystrcmp(&p, "rnd"))
                continue;

            push_arg(args, &nargs, p, name_end);
            double val = 0.0;
            if (nargs) {
                val = fabs(argtod(*args));
            } else {
                char *tmp = p;
                if (rnd_numeric_start(*tmp))
                    mystrtod(&tmp, &val);
                val = fabs(val);
                p = tmp;
            }
            if (val > ASS_RND_MAX_PX)
                val = ASS_RND_MAX_PX;
            state->rnd_x = calc_anim(val, state->rnd_x, pwr);
            state->rnd_y = calc_anim(val, state->rnd_y, pwr);
            state->rnd_z = calc_anim(val, state->rnd_z, pwr);
        } else if (complex_tag("distort")) {
            if (*name_end != '(' || has_backslash_arg)
                continue;

            double current[6] = {
                state->distort_u1, state->distort_v1,
                state->distort_u2, state->distort_v2,
                state->distort_u3, state->distort_v3,
            };
            double target[6];
            char *raw_start = name_end + 1;
            char *raw_end = q;
            if (raw_start >= raw_end)
                continue;
            if (raw_end[-1] == ')')
                raw_end--;

            char *ptr = raw_start;
            bool ok = true;
            for (int i = 0; i < 6; i++) {
                if (ptr < raw_end)
                    skip_spaces(&ptr);
                char *next = memchr(ptr, ',', raw_end - ptr);
                if (i < 5 && !next) {
                    ok = false;
                    break;
                }
                char *tok_end = next ? next : raw_end;
                rskip_spaces(&tok_end, ptr);
                if (tok_end < ptr)
                    tok_end = ptr;
                if (tok_end == ptr)
                    target[i] = current[i];
                else
                    target[i] = argtod((struct arg){ptr, tok_end});
                ptr = next ? next + 1 : raw_end;
            }
            skip_spaces(&ptr);
            if (ptr != raw_end)
                ok = false;

            if (!ok)
                continue;

            state->distort_enabled = true;
            state->distort_u1 = calc_anim(target[0], state->distort_u1, pwr);
            state->distort_v1 = calc_anim(target[1], state->distort_v1, pwr);
            state->distort_u2 = calc_anim(target[2], state->distort_u2, pwr);
            state->distort_v2 = calc_anim(target[3], state->distort_v2, pwr);
            state->distort_u3 = calc_anim(target[4], state->distort_u3, pwr);
            state->distort_v3 = calc_anim(target[5], state->distort_v3, pwr);
        } else if (complex_tag("iclip")) {
            apply_clip_tag(state, "iclip", true, name_end, q, end, pwr);
        } else if (tag("blur")) {
            double target = nargs ? argtod(*args) :
                            state->default_style.blur_x;
            double target_y = nargs ? target : state->default_style.blur_y;
            double val_x = state->blur_x * (1 - pwr) + target * pwr;
            double val_y = state->blur_y * (1 - pwr) + target_y * pwr;

            val_x = (val_x < 0) ? 0 : val_x;
            val_x = (val_x > BLUR_MAX_RADIUS) ? BLUR_MAX_RADIUS : val_x;
            val_y = (val_y < 0) ? 0 : val_y;
            val_y = (val_y > BLUR_MAX_RADIUS) ? BLUR_MAX_RADIUS : val_y;

            state->blur_x = val_x;
            state->blur_y = val_y;
            column_default(COLUMN_STYLE_BLUR);
        } else if (tag("xblur")) {
            double val;
            if (nargs) {
                val = argtod(*args);
                val = state->blur_x * (1 - pwr) + val * pwr;
                val = (val < 0) ? 0 : val;
                val = (val > BLUR_MAX_RADIUS) ? BLUR_MAX_RADIUS : val;
            } else
                val = 0.0;
            state->blur_x = val;
        } else if (tag("yblur")) {
            double val;
            if (nargs) {
                val = argtod(*args);
                val = state->blur_y * (1 - pwr) + val * pwr;
                val = (val < 0) ? 0 : val;
                val = (val > BLUR_MAX_RADIUS) ? BLUR_MAX_RADIUS : val;
            } else
                val = 0.0;
            state->blur_y = val;
            // ASS standard tags
        } else if (tag("fscx")) {
            double val;
            if (nargs) {
                val = argtod(*args) / 100;
                val = state->scale_x * (1 - pwr) + val * pwr;
                val = (val < 0) ? 0 : val;
            } else
                val = state->style->ScaleX;
            state->scale_x = val;
        } else if (tag("fscy")) {
            double val;
            if (nargs) {
                val = argtod(*args) / 100;
                val = state->scale_y * (1 - pwr) + val * pwr;
                val = (val < 0) ? 0 : val;
            } else
                val = state->style->ScaleY;
            state->scale_y = val;
        } else if (tag("fsc")) {
            if (nargs) {
                double val = argtod(*args) / 100;
                double x = state->scale_x * (1 - pwr) + val * pwr;
                double y = state->scale_y * (1 - pwr) + val * pwr;
                state->scale_x = x < 0 ? 0 : x;
                state->scale_y = y < 0 ? 0 : y;
            } else {
                state->scale_x = state->style->ScaleX;
                state->scale_y = state->style->ScaleY;
            }
        } else if (complex_tag("furipos")) {
            if (!nargs) {
                state->furi_offset_x = 0.0;
                state->furi_offset_y = 0.0;
            } else if (nargs == 2) {
                state->furi_offset_x =
                    state->furi_offset_x * (1 - pwr) + argtod(args[0]) * pwr;
                state->furi_offset_y =
                    state->furi_offset_y * (1 - pwr) + argtod(args[1]) * pwr;
            }
        } else if (tag("furifsp")) {
            double val = nargs ? argtod(*args) : 0.0;
            state->furi_hspacing =
                state->furi_hspacing * (1 - pwr) + val * pwr;
        } else if (tag("furistyle")) {
            int32_t val = nargs ? argtoi32(*args) : 0;
            if (val >= 0 && val <= 2)
                state->furi_style = val;
        } else if (tag("furisx")) {
            double val = nargs ? argtod(*args) : 50.0;
            val = state->furi_scale_x * (1 - pwr) + val * pwr;
            state->furi_scale_x = val < 0 ? 0 : val;
        } else if (tag("furisy")) {
            double val = nargs ? argtod(*args) : 50.0;
            val = state->furi_scale_y * (1 - pwr) + val * pwr;
            state->furi_scale_y = val < 0 ? 0 : val;
        } else if (tag("furis")) {
            double val = nargs ? argtod(*args) : 50.0;
            double x = state->furi_scale_x * (1 - pwr) + val * pwr;
            double y = state->furi_scale_y * (1 - pwr) + val * pwr;
            state->furi_scale_x = x < 0 ? 0 : x;
            state->furi_scale_y = y < 0 ? 0 : y;
        } else if (tag("furi")) {
            int32_t val = nargs ? argtoi32(*args) : 1;
            state->furi_enabled = val != 0;
        } else if (tag("fsp")) {
            double val;
            if (nargs) {
                val = argtod(*args);
                state->hspacing =
                    state->hspacing * (1 - pwr) + val * pwr;
            } else
                state->hspacing = state->style->Spacing;
        } else if (tag("fsvp")) {
            double val;
            if (nargs)
                val = state->fsvp * (1 - pwr) + argtod(*args) * pwr;
            else
                val = 0;
            state->fsvp = val;
        } else if (tag("fshp")) {
            double val;
            if (nargs)
                val = state->fshp * (1 - pwr) + argtod(*args) * pwr;
            else
                val = 0;
            state->fshp = val;
        } else if (tag("fs")) {
            double val = 0;
            if (nargs) {
                val = argtod(*args);
                if (*args->start == '+' || *args->start == '-')
                    val = state->font_size * (1 + pwr * val / 10);
                else
                    val = state->font_size * (1 - pwr) + val * pwr;
            }
            if (val <= 0)
                val = state->default_style.font_size;
            state->font_size = val;
            column_default(COLUMN_STYLE_FONT_SIZE);
        } else if (tag("bord")) {
            double val, xval, yval;
            if (nargs) {
                val = argtod(*args);
                xval = state->border_x * (1 - pwr) + val * pwr;
                yval = state->border_y * (1 - pwr) + val * pwr;
                xval = (xval < 0) ? 0 : xval;
                yval = (yval < 0) ? 0 : yval;
            } else {
                xval = state->default_style.border_x;
                yval = state->default_style.border_y;
            }
            state->border_x = xval;
            state->border_y = yval;
            sync_layer1_border(state);
            column_default(COLUMN_STYLE_BORDER_X | COLUMN_STYLE_BORDER_Y);
        } else if (complex_tag("movevc")) {
            if (nargs == 2 || nargs == 4 || nargs == 6) {
                MoveVCState mv = { .active = true };
                mv.x1 = argtod(args[0]);
                mv.y1 = argtod(args[1]);
                if (nargs >= 4) {
                    mv.x2 = argtod(args[2]);
                    mv.y2 = argtod(args[3]);
                    mv.animated = true;
                } else {
                    mv.x2 = mv.x1;
                    mv.y2 = mv.y1;
                    mv.animated = false;
                }
                if (nargs == 6) {
                    mv.t1 = argtoi32(args[4]);
                    mv.t2 = argtoi32(args[5]);
                    if (mv.t1 > mv.t2) {
                        int32_t tmp = mv.t2;
                        mv.t2 = mv.t1;
                        mv.t1 = tmp;
                    }
                    mv.has_timing = true;
                }
                state->movevc = mv;
            } else if (!nargs) {
                state->movevc = (MoveVCState) {0};
            }
        } else if (complex_tag("mover")) {
            MotionState mv = { .type = MOTION_MOVER };
            if (nargs == 4 || nargs == 6 || nargs == 8 || nargs == 10) {
                mv.x1 = argtod(args[0]);
                mv.y1 = argtod(args[1]);
                mv.x2 = argtod(args[2]);
                mv.y2 = argtod(args[3]);
                if (nargs >= 8) {
                    mv.angle1 = argtod(args[4]);
                    mv.angle2 = argtod(args[5]);
                    mv.radius1 = argtod(args[6]);
                    mv.radius2 = argtod(args[7]);
                }
                if (nargs == 6 || nargs == 10) {
                    mv.has_timing = true;
                    mv.t1 = argtoi32(args[nargs - 2]);
                    mv.t2 = argtoi32(args[nargs - 1]);
                    normalize_motion_timing(&mv);
                }
                apply_motion(state, mv, pwr, true);
            }
        } else if (complex_tag("moves3")) {
            MotionState mv = { .type = MOTION_MOVES3 };
            if (nargs == 6 || nargs == 8) {
                mv.x1 = argtod(args[0]);
                mv.y1 = argtod(args[1]);
                mv.x2 = argtod(args[2]);
                mv.y2 = argtod(args[3]);
                mv.x3 = argtod(args[4]);
                mv.y3 = argtod(args[5]);
                if (nargs == 8) {
                    mv.has_timing = true;
                    mv.t1 = argtoi32(args[6]);
                    mv.t2 = argtoi32(args[7]);
                    normalize_motion_timing(&mv);
                }
                apply_motion(state, mv, pwr, true);
            }
        } else if (complex_tag("moves4")) {
            MotionState mv = { .type = MOTION_MOVES4 };
            if (nargs == 8 || nargs == 10) {
                mv.x1 = argtod(args[0]);
                mv.y1 = argtod(args[1]);
                mv.x2 = argtod(args[2]);
                mv.y2 = argtod(args[3]);
                mv.x3 = argtod(args[4]);
                mv.y3 = argtod(args[5]);
                mv.x4 = argtod(args[6]);
                mv.y4 = argtod(args[7]);
                if (nargs == 10) {
                    mv.has_timing = true;
                    mv.t1 = argtoi32(args[8]);
                    mv.t2 = argtoi32(args[9]);
                    normalize_motion_timing(&mv);
                }
                apply_motion(state, mv, pwr, true);
            }
        } else if (complex_tag("move")) {
            MotionState mv = { .type = MOTION_MOVE };
            if (nargs == 4 || nargs == 6) {
                mv.x1 = argtod(args[0]);
                mv.y1 = argtod(args[1]);
                mv.x2 = argtod(args[2]);
                mv.y2 = argtod(args[3]);
                if (nargs == 6) {
                    mv.has_timing = true;
                    mv.t1 = argtoi32(args[4]);
                    mv.t2 = argtoi32(args[5]);
                    normalize_motion_timing(&mv);
                }
                apply_motion(state, mv, pwr, false);
            }
        } else if (tag("frx")) {
            double val;
            if (nargs) {
                val = argtod(*args);
                state->frx =
                    val * pwr + state->frx * (1 - pwr);
            } else
                state->frx = 0.;
        } else if (tag("fry")) {
            double val;
            if (nargs) {
                val = argtod(*args);
                state->fry =
                    val * pwr + state->fry * (1 - pwr);
            } else
                state->fry = 0.;
        } else if (tag("frs")) {
            double val;
            if (nargs) {
                val = argtod(*args);
                state->frs = val * pwr + state->frs * (1 - pwr);
            } else
                state->frs = 0.;
        } else if (tag("frz") || tag("fr")) {
            double val;
            if (nargs) {
                val = argtod(*args);
                state->frz =
                    val * pwr + state->frz * (1 - pwr);
            } else
                state->frz =
                    state->style->Angle;
        } else if (tag("z")) {
            double val;
            if (nargs) {
                val = argtod(*args);
                val = state->z * (1 - pwr) + val * pwr;
                if (!isfinite(val))
                    val = 0.0;
            } else
                val = 0.0;
            state->z = val;
        } else if (tag("ortho")) {
            int32_t val = argtoi32(*args);
            if (!nargs || !(val == 0 || val == 1))
                val = 0;
            state->ortho = !!val;
        } else if (tag("fn")) {
            char *start = args->start;
            if (nargs && strncmp(start, "0", args->end - start)) {
                skip_spaces(&start);
                state->family.str = start;
                state->family.len = args->end - start;
            } else {
                state->family = state->default_style.family;
            }
            ass_update_font(state);
            column_default(COLUMN_STYLE_FONT_NAME);
        } else if (tag("alpha")) {
            int i;
            if (nargs) {
                int32_t a = parse_alpha_tag(args->start);
                for (i = 0; i < 4; ++i)
                    change_alpha(&state->c[i], a, pwr);
            } else {
                change_alpha(&state->c[0],
                             _a(state->default_style.c[0]), 1);
                change_alpha(&state->c[1],
                             _a(state->default_style.c[1]), 1);
                change_alpha(&state->c[2],
                             _a(state->default_style.c[2]), 1);
                change_alpha(&state->c[3],
                             _a(state->default_style.c[3]), 1);
            }
            for (i = 0; i < 4; ++i)
                ass_gradient_disable_alpha(&state->gradient, i,
                                           _a(state->c[i]), pwr);
            sync_layer1_border(state);
            column_default(COLUMN_STYLE_ALL_ALPHAS);
            // FIXME: simplify
        } else if (tag("an")) {
            int32_t val = argtoi32(*args);
            if ((state->parsed_tags & PARSED_A) == 0) {
                if (val >= 1 && val <= 9)
                    state->alignment = numpad2align(val);
                else
                    state->alignment =
                        state->style->Alignment;
                state->parsed_tags |= PARSED_A;
            }
        } else if (tag("a")) {
            int32_t val = argtoi32(*args);
            if ((state->parsed_tags & PARSED_A) == 0) {
                if (val >= 1 && val <= 11)
                    // take care of a vsfilter quirk:
                    // handle illegal \a8 and \a4 like \a5
                    state->alignment = ((val & 3) == 0) ? 5 : val;
                else
                state->alignment =
                        state->style->Alignment;
                state->parsed_tags |= PARSED_A;
            }
        } else if (complex_tag("pos")) {
            double v1, v2;
            if (nargs == 2) {
                v1 = argtod(args[0]);
                v2 = argtod(args[1]);
            } else
                continue;
            if (state->evt_type & EVENT_POSITIONED) {
                ass_msg(render_priv->library, MSGL_V, "Subtitle has a new \\pos "
                       "after \\move or \\pos, ignoring");
            } else {
                MotionState mv = { .type = MOTION_POS, .x1 = v1, .y1 = v2 };
                apply_motion(state, mv, pwr, false);
            }
        } else if (tag("jitter0")) {
            state->jitter = ass_jitter_default_state();
        } else if (complex_tag("jitter")) {
            if (!nargs) {
                state->jitter = ass_jitter_default_state();
            } else if (nargs >= 4) {
                JitterState jit = ass_jitter_default_state();
                jit.enabled = true;
                jit.left = jitter_extent_from_arg(args[0]);
                jit.right = jitter_extent_from_arg(args[1]);
                jit.up = jitter_extent_from_arg(args[2]);
                jit.down = jitter_extent_from_arg(args[3]);
                if (nargs >= 5) {
                    double period_ms = fabs(argtod(args[4]));
                    jit.period = period_ms * 10000.0;
                    jit.has_period = true;
                    if (nargs >= 6) {
                        jit.seed = (uint32_t) argtoi32(args[5]);
                        jit.has_seed = true;
                    }
                }
                apply_jitter(state, jit, pwr);
            }
        } else if (complex_tag("fade")) {
            int32_t a1, a2, a3;
            int32_t t1, t2, t3, t4;
            if (nargs == 7) {
                // 7-argument version (\fade)
                a1 = argtoi32(args[0]);
                a2 = argtoi32(args[1]);
                a3 = argtoi32(args[2]);
                t1 = argtoi32(args[3]);
                t2 = argtoi32(args[4]);
                t3 = argtoi32(args[5]);
                t4 = argtoi32(args[6]);
            } else
                continue;
            if (t1 == -1 && t4 == -1) {
                t1 = 0;
                t4 = state->event->Duration;
                t3 = (uint32_t) t4 - t3;
            }
            if ((state->parsed_tags & PARSED_FADE) == 0) {
                state->fade =
                    interpolate_alpha(render_priv->time -
                            state->event->Start, t1, t2,
                            t3, t4, a1, a2, a3);
                state->parsed_tags |= PARSED_FADE;
            }
        } else if (complex_tag("fad")) {
            char *raw_start = name_end < q && *name_end == '(' ?
                              name_end + 1 : NULL;
            char *raw_end = q;
            if (raw_start && raw_end > raw_start && raw_end[-1] == ')')
                raw_end--;

            int32_t t1, t2, t3, t4;
            int fade_in, fade_out;
            FadColorArg start_color, end_color;
            if (!raw_start || !parse_extended_fad(raw_start, raw_end,
                                                  &fade_in, &fade_out,
                                                  &start_color, &end_color))
                continue;
            t1 = 0;
            t2 = fade_in;
            t4 = state->event->Duration;
            t3 = (uint32_t) t4 - fade_out;

            if ((state->parsed_tags & PARSED_FADE) == 0) {
                int a1 = start_color.alpha_fade ? 0xFF : 0;
                int a2 = 0;
                int a3 = end_color.alpha_fade ? 0xFF : 0;
                long long now = render_priv->time - state->event->Start;
                state->fade = interpolate_alpha(now, t1, t2, t3, t4,
                                                a1, a2, a3);

                bool fade_in_side = false;
                double amount = interpolate_fad_color_amount(now, t1, t2,
                                                             t3, t4,
                                                             &fade_in_side);
                FadColorArg active =
                    fade_in_side ? start_color : end_color;
                if (active.has_color && amount > 0) {
                    state->fade_color = (FadeColorState) {
                        .active = true,
                        .color = active.color,
                        .amount = amount,
                    };
                } else {
                    state->fade_color = (FadeColorState) {0};
                }
                state->parsed_tags |= PARSED_FADE;
            }
        } else if (complex_tag("org")) {
            double v1, v2;
            if (nargs == 2) {
                v1 = argtod(args[0]);
                v2 = argtod(args[1]);
            } else
                continue;
            if (!state->have_origin) {
                state->org_x = v1;
                state->org_y = v2;
                state->have_origin = 1;
                state->detect_collisions = 0;
            }
        } else if (complex_tag("t")) {
            double accel;
            int cnt = nargs - 1;
            int32_t t1, t2, t, delta_t;
            double k;
            // VSFilter compatibility (because we can): parse the
            // timestamps differently depending on argument count.
            if (cnt == 3) {
                t1 = argtoi32(args[0]);
                t2 = argtoi32(args[1]);
                accel = argtod(args[2]);
            } else if (cnt == 2) {
                t1 = dtoi32(argtod(args[0]));
                t2 = dtoi32(argtod(args[1]));
                accel = 1.;
            } else if (cnt == 1) {
                t1 = 0;
                t2 = 0;
                accel = argtod(args[0]);
            } else {
                t1 = 0;
                t2 = 0;
                accel = 1.;
            }
            state->detect_collisions = 0;
            if (t2 == 0)
                t2 = state->event->Duration;
            delta_t = (uint32_t) t2 - t1;
            t = render_priv->time - state->event->Start;        // FIXME: move to render_context
            if (t < t1)
                k = 0.;
            else if (t >= t2)
                k = 1.;
            else {
                assert(delta_t != 0.);
                k = pow((double) (int32_t) ((uint32_t) t - t1) / delta_t, accel);
            }
            if (nested)
                pwr = k;
            if (cnt < 0 || cnt > 3)
                continue;
            // If there's no backslash in the arguments, there are no
            // override tags, so it's pointless to try to parse them.
            if (!has_backslash_arg)
                continue;
            p = args[cnt].start;
            if (args[cnt].end < end) {
                assert(!nested);
                p = ass_parse_tags(state, p, args[cnt].end, k, true);
            } else {
                assert(q == end);
                // No other tags can possibly follow this \t tag,
                // so we don't need to restore pwr after parsing \t.
                // The recursive call is now essentially a tail call,
                // so optimize it away.
                pwr = k;
                nested = true;
                q = p;
            }
        } else if (complex_tag("clip")) {
            apply_clip_tag(state, "clip", false, name_end, q, end, pwr);
        } else if (tag("img") || tag("1img")) {
            apply_img_tag(state, 0, args, nargs, pwr);
        } else if (tag("2img")) {
            apply_img_tag(state, 1, args, nargs, pwr);
        } else if (tag("3img")) {
            apply_img_tag(state, 2, args, nargs, pwr);
        } else if (tag("4img")) {
            apply_img_tag(state, 3, args, nargs, pwr);
        } else if (mangetsu_fill_tag) {
            apply_mangetsu_gradient_tag(state, MANGETSU_GRADIENT_TARGET_COLOR,
                                        mangetsu_fill_layer, name_end, q,
                                        args, nargs, mangetsu_fill_arg, pwr,
                                        nested);
        } else if (tag("1vc")) {
            if (nargs) {
                uint32_t vals[4];
                int cnt = FFMIN(nargs, 4);
                for (int i = 0; i < cnt; i++)
                    vals[i] = parse_color_tag(args[i].start);
                ass_gradient_apply_color(&state->gradient, 0, vals, cnt, pwr);
                if (pwr > 0.0)
                    disable_mangetsu_gradient_layer(state, 0);
                disable_image_fill_layer(state, 0);
                disable_image_fill_layer(state, 1);
                state->needs_rgba = true;
                state->renderer->track->has_rgba = 1;
            } else {
                if (pwr > 0.0)
                    disable_mangetsu_gradient_layer(state, 0);
                ass_gradient_disable_color(&state->gradient, 0, state->c[0], pwr);
            }
        } else if (tag("2vc")) {
            if (nargs) {
                uint32_t vals[4];
                int cnt = FFMIN(nargs, 4);
                for (int i = 0; i < cnt; i++)
                    vals[i] = parse_color_tag(args[i].start);
                ass_gradient_apply_color(&state->gradient, 1, vals, cnt, pwr);
                if (pwr > 0.0)
                    disable_mangetsu_gradient_layer(state, 1);
                disable_image_fill_layer(state, 0);
                disable_image_fill_layer(state, 1);
                state->needs_rgba = true;
                state->renderer->track->has_rgba = 1;
            } else {
                if (pwr > 0.0)
                    disable_mangetsu_gradient_layer(state, 1);
                ass_gradient_disable_color(&state->gradient, 1, state->c[1], pwr);
            }
        } else if (tag("3vc")) {
            if (nargs) {
                uint32_t vals[4];
                int cnt = FFMIN(nargs, 4);
                for (int i = 0; i < cnt; i++)
                    vals[i] = parse_color_tag(args[i].start);
                ass_gradient_apply_color(&state->gradient, 2, vals, cnt, pwr);
                if (pwr > 0.0)
                    disable_mangetsu_border_gradient_layer(state, 0);
                disable_image_fill_layer(state, 2);
                state->needs_rgba = true;
                state->renderer->track->has_rgba = 1;
            } else {
                if (pwr > 0.0)
                    disable_mangetsu_border_gradient_layer(state, 0);
                ass_gradient_disable_color(&state->gradient, 2, state->c[2], pwr);
            }
        } else if (tag("4vc")) {
            if (nargs) {
                uint32_t vals[4];
                int cnt = FFMIN(nargs, 4);
                for (int i = 0; i < cnt; i++)
                    vals[i] = parse_color_tag(args[i].start);
                ass_gradient_apply_color(&state->gradient, 3, vals, cnt, pwr);
                if (pwr > 0.0)
                    disable_mangetsu_gradient_layer(state, 3);
                disable_image_fill_layer(state, 3);
                state->needs_rgba = true;
                state->renderer->track->has_rgba = 1;
            } else {
                if (pwr > 0.0)
                    disable_mangetsu_gradient_layer(state, 3);
                ass_gradient_disable_color(&state->gradient, 3, state->c[3], pwr);
            }
        } else if (tag("1va")) {
            if (nargs) {
                uint8_t vals[4];
                int cnt = FFMIN(nargs, 4);
                for (int i = 0; i < cnt; i++)
                    vals[i] = (uint8_t) parse_alpha_tag(args[i].start);
                ass_gradient_apply_alpha(&state->gradient, 0, vals, cnt, pwr);
                disable_image_fill_layer(state, 0);
                disable_image_fill_layer(state, 1);
                state->needs_rgba = true;
                state->renderer->track->has_rgba = 1;
            } else {
                ass_gradient_disable_alpha(&state->gradient, 0,
                                           _a(state->c[0]), pwr);
            }
        } else if (tag("2va")) {
            if (nargs) {
                uint8_t vals[4];
                int cnt = FFMIN(nargs, 4);
                for (int i = 0; i < cnt; i++)
                    vals[i] = (uint8_t) parse_alpha_tag(args[i].start);
                ass_gradient_apply_alpha(&state->gradient, 1, vals, cnt, pwr);
                disable_image_fill_layer(state, 0);
                disable_image_fill_layer(state, 1);
                state->needs_rgba = true;
                state->renderer->track->has_rgba = 1;
            } else {
                ass_gradient_disable_alpha(&state->gradient, 1,
                                           _a(state->c[1]), pwr);
            }
        } else if (tag("3va")) {
            if (nargs) {
                uint8_t vals[4];
                int cnt = FFMIN(nargs, 4);
                for (int i = 0; i < cnt; i++)
                    vals[i] = (uint8_t) parse_alpha_tag(args[i].start);
                ass_gradient_apply_alpha(&state->gradient, 2, vals, cnt, pwr);
                disable_image_fill_layer(state, 2);
                state->needs_rgba = true;
                state->renderer->track->has_rgba = 1;
            } else {
                ass_gradient_disable_alpha(&state->gradient, 2,
                                           _a(state->c[2]), pwr);
            }
        } else if (tag("4va")) {
            if (nargs) {
                uint8_t vals[4];
                int cnt = FFMIN(nargs, 4);
                for (int i = 0; i < cnt; i++)
                    vals[i] = (uint8_t) parse_alpha_tag(args[i].start);
                ass_gradient_apply_alpha(&state->gradient, 3, vals, cnt, pwr);
                disable_image_fill_layer(state, 3);
                state->needs_rgba = true;
                state->renderer->track->has_rgba = 1;
            } else {
                ass_gradient_disable_alpha(&state->gradient, 3,
                                           _a(state->c[3]), pwr);
            }
        } else if (tag("5c")) {
            if (nargs) {
                uint32_t val;
                if (parse_decoration_color_arg(*args, &val)) {
                    state->decoration_color = val;
                    state->decoration_color_set = true;
                    column_default(COLUMN_STYLE_DECORATION_COLOR);
                }
            } else {
                state->decoration_color_set = false;
                column_default(COLUMN_STYLE_DECORATION_COLOR);
            }
            if (!nested && pwr > 0.0)
                disable_mangetsu_gradient_layer(state, 4);
        } else if (tag("c") || tag("1c")) {
            if (nargs) {
                uint32_t val = parse_color_tag(args->start);
                change_color(&state->c[0], val, pwr);
            } else
                change_color(&state->c[0],
                             state->default_style.c[0], 1);
            ass_gradient_disable_color(&state->gradient, 0, state->c[0], pwr);
            if (!nested && pwr > 0.0)
                disable_mangetsu_gradient_layer(state, 0);
            if (pwr >= 1.0)
                disable_image_fill_layer(state, 0);
            column_default(COLUMN_STYLE_COLOR0);
        } else if (tag("2c")) {
            if (nargs) {
                uint32_t val = parse_color_tag(args->start);
                change_color(&state->c[1], val, pwr);
            } else
                change_color(&state->c[1],
                             state->default_style.c[1], 1);
            ass_gradient_disable_color(&state->gradient, 1, state->c[1], pwr);
            if (!nested && pwr > 0.0)
                disable_mangetsu_gradient_layer(state, 1);
            if (pwr >= 1.0)
                disable_image_fill_layer(state, 1);
            column_default(COLUMN_STYLE_COLOR1);
        } else if (tag("3c")) {
            if (nargs) {
                uint32_t val = parse_color_tag(args->start);
                change_color(&state->c[2], val, pwr);
            } else
                change_color(&state->c[2],
                             state->default_style.c[2], 1);
            ass_gradient_disable_color(&state->gradient, 2, state->c[2], pwr);
            if (!nested && pwr > 0.0)
                disable_mangetsu_border_gradient_layer(state, 0);
            if (pwr >= 1.0)
                disable_image_fill_layer(state, 2);
            sync_layer1_border(state);
            column_default(COLUMN_STYLE_COLOR2);
        } else if (tag("4c")) {
            if (nargs) {
                uint32_t val = parse_color_tag(args->start);
                change_color(&state->c[3], val, pwr);
            } else
                change_color(&state->c[3],
                             state->default_style.c[3], 1);
            ass_gradient_disable_color(&state->gradient, 3, state->c[3], pwr);
            if (!nested && pwr > 0.0)
                disable_mangetsu_gradient_layer(state, 3);
            if (pwr >= 1.0)
                disable_image_fill_layer(state, 3);
            column_default(COLUMN_STYLE_COLOR3);
        } else if (tag("1a")) {
            if (nargs) {
                uint32_t val = parse_alpha_tag(args->start);
                change_alpha(&state->c[0], val, pwr);
            } else
                change_alpha(&state->c[0],
                             _a(state->default_style.c[0]), 1);
            ass_gradient_disable_alpha(&state->gradient, 0,
                                       _a(state->c[0]), pwr);
            column_default(COLUMN_STYLE_ALPHA0);
        } else if (tag("2a")) {
            if (nargs) {
                uint32_t val = parse_alpha_tag(args->start);
                change_alpha(&state->c[1], val, pwr);
            } else
                change_alpha(&state->c[1],
                             _a(state->default_style.c[1]), 1);
            ass_gradient_disable_alpha(&state->gradient, 1,
                                       _a(state->c[1]), pwr);
            column_default(COLUMN_STYLE_ALPHA1);
        } else if (tag("3a")) {
            uint32_t val;
            if (nargs) {
                val = parse_alpha_tag(args->start);
                apply_all_border_alpha(state, val, pwr);
            } else
                apply_all_border_alpha(state,
                                       _a(state->default_style.c[2]), 1);
            column_default(COLUMN_STYLE_ALPHA2);
        } else if (tag("4a")) {
            if (nargs) {
                uint32_t val = parse_alpha_tag(args->start);
                change_alpha(&state->c[3], val, pwr);
            } else
                change_alpha(&state->c[3],
                             _a(state->default_style.c[3]), 1);
            ass_gradient_disable_alpha(&state->gradient, 3,
                                       _a(state->c[3]), pwr);
            column_default(COLUMN_STYLE_ALPHA3);
        } else if (tag("5a")) {
            if (nargs) {
                uint32_t val;
                if (parse_decoration_alpha_arg(*args, &val)) {
                    state->decoration_alpha = val;
                    state->decoration_alpha_set = true;
                    column_default(COLUMN_STYLE_DECORATION_ALPHA);
                }
            } else {
                state->decoration_alpha_set = false;
                column_default(COLUMN_STYLE_DECORATION_ALPHA);
            }
        } else if (tag("boxpx")) {
            if (nargs) {
                double val;
                if (parse_double_arg_strict(*args, &val))
                    state->box_extra_x = FFMAX(val, 0);
            }
        } else if (tag("boxpy")) {
            if (nargs) {
                double val;
                if (parse_double_arg_strict(*args, &val))
                    state->box_extra_y = FFMAX(val, 0);
            }
        } else if (tag("boxp")) {
            if (nargs) {
                double val;
                if (parse_double_arg_strict(*args, &val)) {
                    state->box_extra_x = FFMAX(val, 0);
                    state->box_extra_y = FFMAX(val, 0);
                }
            }
        } else if (tag("box")) {
            if (nargs) {
                int32_t val = argtoi32(*args);
                if (val == 0 || val == 1)
                    state->bs4_box_mode = val == 1;
            }
        } else if (tag("r")) {
            if (nargs) {
                int len = args->end - args->start;
                ass_reset_render_context(state,
                        lookup_style_strict(render_priv->track, args->start, len));
            } else
                ass_reset_render_context(state, NULL);
        } else if (tag("be")) {
            double dval;
            if (nargs) {
                int32_t val;
                dval = argtod(*args);
                // VSFilter always adds +0.5, even if the value is negative
                val = dtoi32(state->be * (1 - pwr) + dval * pwr + 0.5);
                // Clamp to a safe upper limit, since high values need excessive CPU
                val = (val < 0) ? 0 : val;
                val = (val > MAX_BE) ? MAX_BE : val;
                state->be = val;
            } else
                state->be = state->default_style.be;
            column_default(COLUMN_STYLE_BE);
        } else if (tag("b")) {
            int32_t val = argtoi32(*args);
            if (!nargs || !(val == 0 || val == 1 || val >= 100))
                val = state->default_style.bold;
            state->bold = val;
            ass_update_font(state);
            column_default(COLUMN_STYLE_BOLD);
        } else if (tag("i")) {
            int32_t val = argtoi32(*args);
            if (!nargs || !(val == 0 || val == 1))
                val = state->default_style.italic;
            state->italic = val;
            ass_update_font(state);
            column_default(COLUMN_STYLE_ITALIC);
        } else if (tag("kt")) {
            // v4++
            if (state->column_event && state->column_active)
                continue;
            double val = 0;
            if (nargs)
                val = argtod(*args) * 10;
            state->effect_skip_timing = dtoi32(val);
            state->effect_timing = 0;
            state->reset_effect = true;
        } else if (tag("kf") || tag("K")) {
            if (state->column_event && state->column_active)
                continue;
            double val = 100;
            if (nargs)
                val = argtod(*args);
            state->effect_type = EF_KARAOKE_KF;
            state->effect_skip_timing +=
                    (uint32_t) state->effect_timing;
            state->effect_timing = dtoi32(val * 10);
        } else if (tag("ko")) {
            if (state->column_event && state->column_active)
                continue;
            double val = 100;
            if (nargs)
                val = argtod(*args);
            state->effect_type = EF_KARAOKE_KO;
            state->effect_skip_timing +=
                    (uint32_t) state->effect_timing;
            state->effect_timing = dtoi32(val * 10);
        } else if (tag("k")) {
            if (state->column_event && state->column_active)
                continue;
            double val = 100;
            if (nargs)
                val = argtod(*args);
            state->effect_type = EF_KARAOKE;
            state->effect_skip_timing +=
                    (uint32_t) state->effect_timing;
            state->effect_timing = dtoi32(val * 10);
        } else if (tag("shad")) {
            double val, xval, yval;
            if (nargs) {
                val = argtod(*args);
                xval = state->shadow_x * (1 - pwr) + val * pwr;
                yval = state->shadow_y * (1 - pwr) + val * pwr;
                // VSFilter compatibility: clip for \shad but not for \[xy]shad
                xval = (xval < 0) ? 0 : xval;
                yval = (yval < 0) ? 0 : yval;
            } else {
                xval = state->default_style.shadow_x;
                yval = state->default_style.shadow_y;
            }
            state->shadow_x = xval;
            state->shadow_y = yval;
            column_default(COLUMN_STYLE_SHADOW_X | COLUMN_STYLE_SHADOW_Y);
        } else if (tag("s")) {
            int32_t val = argtoi32(*args);
            if (!nargs || !(val == 0 || val == 1))
                val = !!(state->default_style.flags & DECO_STRIKETHROUGH);
            if (val)
                state->flags |= DECO_STRIKETHROUGH;
            else
                state->flags &= ~DECO_STRIKETHROUGH;
            column_default(COLUMN_STYLE_STRIKEOUT);
        } else if (tag("u")) {
            int32_t val = argtoi32(*args);
            if (!nargs || !(val == 0 || val == 1))
                val = !!(state->default_style.flags & DECO_UNDERLINE);
            if (val)
                state->flags |= DECO_UNDERLINE;
            else
                state->flags &= ~DECO_UNDERLINE;
            column_default(COLUMN_STYLE_UNDERLINE);
        } else if (tag("pbo")) {
            double val = argtod(*args);
            state->pbo = val;
        } else if (tag("p")) {
            int32_t val = argtoi32(*args);
            val = (val < 0) ? 0 : val;
            state->drawing_scale = val;
        } else if (tag("q")) {
            int32_t val = argtoi32(*args);
            if (!nargs || !(val >= 0 && val <= 3))
                val = render_priv->track->WrapStyle;
            state->wrap_style = val;
        } else if (tag("fe")) {
            int32_t val;
            if (nargs)
                val = argtoi32(*args);
            else
                val = state->style->Encoding;
            state->font_encoding = val;
        }
    }

    return p;
}

void ass_apply_transition_effects(RenderContext *state)
{
    ASS_Renderer *render_priv = state->renderer;
    int v[4];
    int cnt;
    ASS_Event *event = state->event;
    char *p = event->Effect;

    if (!p || !*p)
        return;

    cnt = 0;
    while (cnt < 4 && (p = strchr(p, ';'))) {
        v[cnt++] = atoi(++p);
    }

    ASS_Vector layout_res = ass_layout_res(render_priv);
    if (strncmp(event->Effect, "Banner;", 7) == 0) {
        double delay;
        if (cnt < 1) {
            ass_msg(render_priv->library, MSGL_V,
                    "Error parsing effect: '%s'", event->Effect);
            return;
        }
        if (cnt >= 2 && v[1])   // left-to-right
            state->scroll_direction = SCROLL_LR;
        else                    // right-to-left
            state->scroll_direction = SCROLL_RL;

        delay = v[0];
        // VSF works in storage coordinates, but scales delay to PlayRes canvas
        // before applying max(scaled_ delay, 1). This means, if scaled_delay < 1
        // (esp. delay=0) we end up with 1 ms per _storage pixel_ without any
        // PlayRes scaling.
        // The way libass deals with delay, it is automatically relative to the
        // PlayRes canvas, so we only want to "unscale" the small delay values.
        //
        // VSF also casts the scaled delay to int, which if not emulated leads to
        // easily noticeable deviations from VSFilter as the effect goes on.
        // To achieve both we need to keep our Playres-relative delay with high precision,
        // but must temporarily convert to storage-relative and truncate and take the
        // maxuimum there, before converting back.
        double scale_x = ((double) layout_res.x) / render_priv->track->PlayResX;
        delay = ((int) FFMAX(delay / scale_x, 1)) * scale_x;
        state->scroll_shift =
            (render_priv->time - event->Start) / delay;
        state->evt_type |= EVENT_HSCROLL;
        state->detect_collisions = 0;
        state->wrap_style = 2;
        return;
    }

    if (strncmp(event->Effect, "Scroll up;", 10) == 0) {
        state->scroll_direction = SCROLL_BT;
    } else if (strncmp(event->Effect, "Scroll down;", 12) == 0) {
        state->scroll_direction = SCROLL_TB;
    } else {
        ass_msg(render_priv->library, MSGL_DBG2,
                "Unknown transition effect: '%s'", event->Effect);
        return;
    }
    // parse scroll up/down parameters
    {
        double delay;
        int y0, y1;
        if (cnt < 3) {
            ass_msg(render_priv->library, MSGL_V,
                    "Error parsing effect: '%s'", event->Effect);
            return;
        }
        delay = v[2];
        // See explanation for Banner
        double scale_y = ((double) layout_res.y) / render_priv->track->PlayResY;
        delay = ((int) FFMAX(delay / scale_y, 1)) * scale_y;
        state->scroll_shift =
            (render_priv->time - event->Start) / delay;
        if (v[0] < v[1]) {
            y0 = v[0];
            y1 = v[1];
        } else {
            y0 = v[1];
            y1 = v[0];
        }
        state->scroll_y0 = y0;
        state->scroll_y1 = y1;
        state->evt_type |= EVENT_VSCROLL;
        state->detect_collisions = 0;
    }

}

/**
 * \brief determine karaoke effects
 * Karaoke effects cannot be calculated during parse stage (ass_get_next_char()),
 * so they are done in a separate step.
 * Parse stage: when karaoke style override is found, its parameters are stored in the next glyph's
 * (the first glyph of the karaoke word)'s effect_type and effect_timing.
 * This function:
 * 1. sets effect_type for all glyphs in the word (_karaoke_ word)
 * 2. sets effect_timing for all glyphs to x coordinate of the border line between the left and right karaoke parts
 * (left part is filled with PrimaryColour, right one - with SecondaryColour).
 */
void ass_process_karaoke_effects(RenderContext *state)
{
    TextInfo *text_info = &state->text_info;
    long long tm_current = state->renderer->time - state->event->Start;

    int32_t timing = 0, skip_timing = 0;
    Effect effect_type = EF_NONE;
    GlyphInfo *last_boundary = NULL;
    bool has_reset = false;
    for (int i = 0; i <= text_info->length; i++) {
        if (i < text_info->length &&
            !text_info->glyphs[i].starts_new_run) {

            if (text_info->glyphs[i].reset_effect) {
                has_reset = true;
                skip_timing = 0;
            }

            // VSFilter compatibility: if we have \k12345\k0 without a run
            // break, subsequent text is still part of the same karaoke word,
            // the current word's starting and ending time stay unchanged,
            // but the starting time of the next karaoke word is advanced.
            skip_timing += (uint32_t) text_info->glyphs[i].effect_skip_timing;
            continue;
        }

        GlyphInfo *start = last_boundary;
        GlyphInfo *end = text_info->glyphs + i;
        last_boundary = end;
        if (!start)
            continue;

        if (start->effect_type != EF_NONE)
            effect_type = start->effect_type;
        if (effect_type == EF_NONE)
            continue;

        if (start->reset_effect)
            timing = 0;

        long long tm_start = timing + start->effect_skip_timing;
        long long tm_end = tm_start + start->effect_timing;
        timing = !has_reset * tm_end + skip_timing;
        skip_timing = 0;
        has_reset = false;

        if (effect_type != EF_KARAOKE_KF)
            tm_end = tm_start;

        int x;
        if (tm_current < tm_start)
            x = -100000000;
        else if (tm_current >= tm_end)
            x = 100000000;
        else {
            GlyphInfo *first_visible = start, *last_visible = end - 1;
            while (first_visible < last_visible && first_visible->skip)
                ++first_visible;
            while (first_visible < last_visible && last_visible->skip)
                --last_visible;

            int x_start = first_visible->pos.x;
            int x_end = last_visible->pos.x + last_visible->advance.x;
            double dt = (double) (tm_current - tm_start) / (tm_end - tm_start);
            double frz = fmod(start->frz, 360);
            if (frz > 90 && frz < 270) {
                // Fill from right to left
                dt = 1 - dt;
                for (GlyphInfo *info = start; info < end; info++) {
                    uint32_t tmp = info->c[0];
                    info->c[0] = info->c[1];
                    info->c[1] = tmp;
                }
            }
            x = x_start + ass_lrint((x_end - x_start) * dt);
        }

        for (GlyphInfo *info = start; info < end; info++) {
            info->effect_type = effect_type;
            info->effect_timing = x - info->pos.x;
        }
    }
}


/**
 * \brief Get next ucs4 char from string, parsing UTF-8 and escapes
 * \param str string pointer
 * \return ucs4 code of the next char
 * On return str points to the unparsed part of the string
 */
unsigned ass_get_next_char(RenderContext *state, char **str)
{
    char *p = *str;
    unsigned chr;
    if (*p == '\t') {
        ++p;
        *str = p;
        return ' ';
    }
    if (*p == '\\') {
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
        } else if (state->furi_enabled &&
                   (p[1] == '<' || p[1] == '>' ||
                    p[1] == '|' || p[1] == '\\')) {
            chr = (unsigned char) p[1];
            p += 2;
            *str = p;
            return chr;
        }
    }
    chr = ass_utf8_get_char((char **) &p);
    *str = p;
    return chr;
}

// Return 1 if the event contains tags that will apply overrides the selective
// style override code should not touch. Return 0 otherwise.
int ass_event_has_hard_overrides(char *str)
{
    // look for \pos and \move tags inside {...}
    // mirrors ass_get_next_char, but is faster and doesn't change any global state
    while (*str) {
        if (str[0] == '\\' && str[1] != '\0') {
            str += 2;
        } else if (str[0] == '{') {
            str++;
            while (*str && *str != '}') {
                if (*str == '\\') {
                    char *p = str + 1;
                    if (mystrcmp(&p, "pos") || mystrcmp(&p, "move") ||
                        mystrcmp(&p, "mover") || mystrcmp(&p, "moves3") ||
                        mystrcmp(&p, "moves4") || mystrcmp(&p, "jitter") ||
                        mystrcmp(&p, "movevc") ||
                        mystrcmp(&p, "clip") || mystrcmp(&p, "iclip") ||
                        mystrcmp(&p, "org") || mystrcmp(&p, "pbo") ||
                        mystrcmp(&p, "p"))
                        return 1;
                }
                str++;
            }
        } else {
            str++;
        }
    }
    return 0;
}
