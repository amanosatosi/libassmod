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

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gradient.h"

#define CR(c)   ((uint8_t) ((c) >> 24))
#define CG(c)   ((uint8_t) ((c) >> 16))
#define CB(c)   ((uint8_t) ((c) >> 8))
#define CA(c)   ((uint8_t) (c))
#define MANGETSU_GRADIENT_PI 3.14159265358979323846

static inline double clamp01(double v)
{
    if (v < 0.0)
        return 0.0;
    if (v > 1.0)
        return 1.0;
    return v;
}

static inline uint8_t mix_byte(uint8_t oldv, uint8_t newv, double pwr)
{
    if (pwr <= 0.0)
        return oldv;
    if (pwr >= 1.0)
        return newv;
    return (uint8_t) lround((1.0 - pwr) * oldv + pwr * newv);
}

static uint32_t mix_color(uint32_t oldc, uint32_t newc, double pwr)
{
    if (pwr <= 0.0)
        return oldc;
    if (pwr >= 1.0)
        return newc;

    uint8_t r = mix_byte(CR(oldc), CR(newc), pwr);
    uint8_t g = mix_byte(CG(oldc), CG(newc), pwr);
    uint8_t b = mix_byte(CB(oldc), CB(newc), pwr);
    uint8_t a = mix_byte(CA(oldc), CA(newc), pwr);
    return ((uint32_t) r << 24) | ((uint32_t) g << 16) |
           ((uint32_t) b << 8) | a;
}

void ass_gradient_state_reset(GradientState *state, const uint32_t *base_colors)
{
    memset(state, 0, sizeof(*state));
    if (!base_colors)
        return;

    for (int i = 0; i < 4; i++)
        ass_gradient_values_reset(&state->layer[i], base_colors[i]);
}

void ass_gradient_values_reset(GradientValues *values, uint32_t base_color)
{
    if (!values)
        return;

    memset(values, 0, sizeof(*values));
    for (int i = 0; i < 4; i++) {
        values->color[i] = base_color;
        values->alpha[i] = CA(base_color);
    }
}

void ass_gradient_values_apply_color(GradientValues *dst, const uint32_t *values,
                                     int count, double pwr)
{
    if (!dst || count <= 0 || !values)
        return;

    for (int i = 0; i < 4; i++) {
        uint32_t v = values[i < count ? i : count - 1];
        dst->color[i] = mix_color(dst->color[i], v, pwr);
    }
    dst->color_enabled = true;
}

void ass_gradient_values_apply_alpha(GradientValues *dst, const uint8_t *values,
                                     int count, double pwr)
{
    if (!dst || count <= 0 || !values)
        return;

    uint8_t result[sizeof(dst->alpha)];
    for (int i = 0; i < (int) sizeof(result); i++)
        result[i] = mix_byte(dst->alpha[i], values[i < count ? i : count - 1], pwr);
    memcpy(dst->alpha, result, sizeof(result));
    dst->alpha_enabled = true;
}

void ass_gradient_values_disable_color(GradientValues *dst, uint32_t fallback,
                                       double pwr)
{
    if (!dst)
        return;

    for (int i = 0; i < 4; i++)
        dst->color[i] = mix_color(dst->color[i], fallback, pwr);

    dst->color_enabled = pwr < 1.0 ? dst->color_enabled : false;
}

void ass_gradient_values_disable_alpha(GradientValues *dst, uint8_t fallback,
                                       double pwr)
{
    if (!dst)
        return;

    for (int i = 0; i < 4; i++)
        dst->alpha[i] = mix_byte(dst->alpha[i], fallback, pwr);

    dst->alpha_enabled = pwr < 1.0 ? dst->alpha_enabled : false;
}

void ass_gradient_apply_color(GradientState *state, int layer, const uint32_t *values,
                              int count, double pwr)
{
    if (!state || layer < 0 || layer >= 4)
        return;
    ass_gradient_values_apply_color(&state->layer[layer], values, count, pwr);
}

void ass_gradient_apply_alpha(GradientState *state, int layer, const uint8_t *values,
                              int count, double pwr)
{
    if (!state || layer < 0 || layer >= 4)
        return;
    ass_gradient_values_apply_alpha(&state->layer[layer], values, count, pwr);
}

void ass_gradient_disable_color(GradientState *state, int layer, uint32_t fallback,
                                double pwr)
{
    if (!state || layer < 0 || layer >= 4)
        return;
    ass_gradient_values_disable_color(&state->layer[layer], fallback, pwr);
}

void ass_gradient_disable_alpha(GradientState *state, int layer, uint8_t fallback,
                                double pwr)
{
    if (!state || layer < 0 || layer >= 4)
        return;
    ass_gradient_values_disable_alpha(&state->layer[layer], fallback, pwr);
}

bool ass_gradient_equal(const GradientState *a, const GradientState *b)
{
    return !memcmp(a, b, sizeof(*a));
}

static inline uint32_t sample_channel(uint8_t c0, uint8_t c1,
                                      uint8_t c2, uint8_t c3,
                                      int32_t uf, int32_t vf)
{
    // uf, vf in 0..65536
    uint32_t w0 = (uint32_t) (65536 - uf);
    uint32_t w1 = (uint32_t) uf;
    uint32_t h0 = (uint32_t) (65536 - vf);
    uint32_t h1 = (uint32_t) vf;

    uint64_t w00 = (uint64_t) w0 * h0;
    uint64_t w10 = (uint64_t) w1 * h0;
    uint64_t w01 = (uint64_t) w0 * h1;
    uint64_t w11 = (uint64_t) w1 * h1;

    uint64_t acc = w00 * c0 + w10 * c1 + w01 * c2 + w11 * c3;
    // divide by 65536*65536 with truncation
    uint32_t res = (uint32_t) (acc >> 32);
    return res > 255 ? 255 : res;
}

uint32_t ass_gradient_sample_color_fixed(const GradientValues *val, int32_t uf, int32_t vf)
{
    if (uf < 0) uf = 0; else if (uf > 65536) uf = 65536;
    if (vf < 0) vf = 0; else if (vf > 65536) vf = 65536;

    uint8_t r = (uint8_t) sample_channel(CR(val->color[0]), CR(val->color[1]),
                                         CR(val->color[2]), CR(val->color[3]),
                                         uf, vf);
    uint8_t g = (uint8_t) sample_channel(CG(val->color[0]), CG(val->color[1]),
                                         CG(val->color[2]), CG(val->color[3]),
                                         uf, vf);
    uint8_t b = (uint8_t) sample_channel(CB(val->color[0]), CB(val->color[1]),
                                         CB(val->color[2]), CB(val->color[3]),
                                         uf, vf);
    uint8_t a = CA(val->color[0]);
    return ((uint32_t) r << 24) | ((uint32_t) g << 16) |
           ((uint32_t) b << 8) | a;
}

uint8_t ass_gradient_sample_alpha_fixed(const GradientValues *val, int32_t uf, int32_t vf)
{
    if (uf < 0) uf = 0; else if (uf > 65536) uf = 65536;
    if (vf < 0) vf = 0; else if (vf > 65536) vf = 65536;

    uint8_t a = (uint8_t) sample_channel(val->alpha[0], val->alpha[1],
                                         val->alpha[2], val->alpha[3],
                                         uf, vf);
    return a;
}

uint32_t ass_gradient_sample_color(const GradientValues *val, double u, double v)
{
    u = clamp01(u);
    v = clamp01(v);
    int32_t uf = (int32_t) (u * 65536.0);
    int32_t vf = (int32_t) (v * 65536.0);
    return ass_gradient_sample_color_fixed(val, uf, vf);
}

uint8_t ass_gradient_sample_alpha(const GradientValues *val, double u, double v)
{
    u = clamp01(u);
    v = clamp01(v);
    int32_t uf = (int32_t) (u * 65536.0);
    int32_t vf = (int32_t) (v * 65536.0);
    return ass_gradient_sample_alpha_fixed(val, uf, vf);
}

void ass_mangetsu_gradient_layer_reset(MangetsuGradientLayer *layer)
{
    if (!layer)
        return;
    memset(layer, 0, sizeof(*layer));
}

void ass_mangetsu_gradient_state_reset(MangetsuGradientState *state)
{
    if (!state)
        return;
    memset(state, 0, sizeof(*state));
}

bool ass_mangetsu_gradient_layer_equal(const MangetsuGradientLayer *a,
                                       const MangetsuGradientLayer *b)
{
    if (a->active != b->active)
        return false;
    if (!a->active)
        return true;
    if (a->slide != b->slide)
        return false;
    if (a->type != b->type || a->coordinate_mode != b->coordinate_mode ||
            a->segment_id != b->segment_id || a->angle != b->angle ||
            a->n_stops != b->n_stops)
        return false;
    if (a->coordinate_mode == MANGETSU_GRADIENT_POSITIONED_RECT &&
            (a->script_x1 != b->script_x1 || a->script_y1 != b->script_y1 ||
             a->script_x2 != b->script_x2 || a->script_y2 != b->script_y2))
        return false;
    for (int i = 0; i < a->n_stops; i++)
        if (a->stops[i].offset != b->stops[i].offset ||
                a->stops[i].color != b->stops[i].color)
            return false;
    return true;
}

bool ass_mangetsu_gradient_state_equal(const MangetsuGradientState *a,
                                       const MangetsuGradientState *b)
{
    for (int i = 0; i < MANGETSU_GRADIENT_LAYERS; i++)
        if (!ass_mangetsu_gradient_layer_equal(&a->layer[i], &b->layer[i]))
            return false;
    for (int i = 0; i < MANGETSU_GRADIENT_BORDER_LAYERS; i++)
        if (!ass_mangetsu_gradient_layer_equal(&a->border[i], &b->border[i]))
            return false;
    for (int i = 0; i < MANGETSU_GRADIENT_LAYERS; i++)
        if (!ass_mangetsu_gradient_layer_equal(&a->alpha[i], &b->alpha[i]))
            return false;
    for (int i = 0; i < MANGETSU_GRADIENT_BORDER_LAYERS; i++)
        if (!ass_mangetsu_gradient_layer_equal(&a->border_alpha[i],
                                               &b->border_alpha[i]))
            return false;
    return true;
}

static uint32_t mix_mangetsu_color(uint32_t c0, uint32_t c1, double t)
{
    t = clamp01(t);
    uint8_t r = mix_byte(CR(c0), CR(c1), t);
    uint8_t g = mix_byte(CG(c0), CG(c1), t);
    uint8_t b = mix_byte(CB(c0), CB(c1), t);
    uint8_t a = mix_byte(CA(c0), CA(c1), t);
    return ((uint32_t) r << 24) | ((uint32_t) g << 16) |
           ((uint32_t) b << 8) | a;
}

static double mangetsu_project(double x, double y, double dx, double dy)
{
    return x * dx + y * dy;
}

static double mangetsu_gradient_position(const MangetsuGradientLayer *layer,
                                         double x, double y)
{
    if (layer->coordinate_mode != MANGETSU_GRADIENT_ATTACHED ||
            !layer->rect.valid || layer->rect.x1 <= layer->rect.x0 ||
            layer->rect.y1 <= layer->rect.y0)
        return 0.0;

    double radians = layer->angle * MANGETSU_GRADIENT_PI / 180.0;
    double dx = cos(radians);
    double dy = sin(radians);

    double p00 = mangetsu_project(layer->rect.x0, layer->rect.y0, dx, dy);
    double p10 = mangetsu_project(layer->rect.x1, layer->rect.y0, dx, dy);
    double p01 = mangetsu_project(layer->rect.x0, layer->rect.y1, dx, dy);
    double p11 = mangetsu_project(layer->rect.x1, layer->rect.y1, dx, dy);
    double p_min = fmin(fmin(p00, p10), fmin(p01, p11));
    double p_max = fmax(fmax(p00, p10), fmax(p01, p11));
    double span = p_max - p_min;
    double t = span > 0.0 ?
        (mangetsu_project(x, y, dx, dy) - p_min) / span : 0.0;
    return clamp01(t);
}

static uint32_t mangetsu_gradient_sample_color_at(
    const MangetsuGradientLayer *layer, double t)
{
    if (t <= layer->stops[0].offset)
        return layer->stops[0].color;

    for (int i = 1; i < layer->n_stops; i++) {
        const MangetsuGradientStop *prev = &layer->stops[i - 1];
        const MangetsuGradientStop *next = &layer->stops[i];
        if (t > next->offset)
            continue;
        double stop_span = next->offset - prev->offset;
        if (stop_span <= 0.0)
            return next->color;
        return mix_mangetsu_color(prev->color, next->color,
                                  (t - prev->offset) / stop_span);
    }

    return layer->stops[layer->n_stops - 1].color;
}

static MangetsuGradientPositionedRect prepare_gradient_rect(
    double angle, double x1, double y1, double x2, double y2)
{
    if (!isfinite(x1) || !isfinite(y1) || !isfinite(x2) || !isfinite(y2) ||
            !isfinite(angle))
        return (MangetsuGradientPositionedRect) {0};

    double left = fmin(x1, x2);
    double right = fmax(x1, x2);
    double top = fmin(y1, y2);
    double bottom = fmax(y1, y2);
    const double epsilon = 0.000000001;
    if (right - left <= epsilon || bottom - top <= epsilon)
        return (MangetsuGradientPositionedRect) {0};

    double radians = angle * MANGETSU_GRADIENT_PI / 180.0;
    double dx = cos(radians);
    double dy = sin(radians);
    if (!isfinite(dx) || !isfinite(dy))
        return (MangetsuGradientPositionedRect) {0};

    double p00 = mangetsu_project(left, top, dx, dy);
    double p10 = mangetsu_project(right, top, dx, dy);
    double p01 = mangetsu_project(left, bottom, dx, dy);
    double p11 = mangetsu_project(right, bottom, dx, dy);
    double p_min = fmin(fmin(p00, p10), fmin(p01, p11));
    double p_max = fmax(fmax(p00, p10), fmax(p01, p11));
    double span = p_max - p_min;
    if (!isfinite(p_min) || !isfinite(p_max) || span <= epsilon)
        return (MangetsuGradientPositionedRect) {0};

    return (MangetsuGradientPositionedRect) {
        .valid = true,
        .left = left,
        .right = right,
        .top = top,
        .bottom = bottom,
        .dx = dx,
        .dy = dy,
        .projection_min = p_min,
        .inverse_projection_span = 1.0 / span,
    };
}

void ass_mangetsu_gradient_prepare_positioned(MangetsuGradientLayer *layer,
                                              double x1, double y1,
                                              double x2, double y2)
{
    if (layer && layer->coordinate_mode == MANGETSU_GRADIENT_POSITIONED_RECT)
        layer->positioned_rect = layer->active && layer->n_stops > 0 ?
            prepare_gradient_rect(layer->angle, x1, y1, x2, y2) :
            (MangetsuGradientPositionedRect) {0};
}

static bool mangetsu_positioned_gradient_position(
    const MangetsuGradientLayer *layer, double x, double y, double *position)
{
    if (!layer || !position || !layer->active || layer->n_stops <= 0 ||
            layer->coordinate_mode != MANGETSU_GRADIENT_POSITIONED_RECT ||
            !layer->positioned_rect.valid)
        return false;

    const MangetsuGradientPositionedRect *rect = &layer->positioned_rect;
    if (x < rect->left || x > rect->right || y < rect->top || y > rect->bottom)
        return false;

    double t = (mangetsu_project(x, y, rect->dx, rect->dy) -
                rect->projection_min) * rect->inverse_projection_span;
    if (!isfinite(t))
        return false;
    *position = clamp01(t);
    return true;
}

bool ass_mangetsu_positioned_gradient_sample_color(
    const MangetsuGradientLayer *layer, double x, double y, uint32_t *color)
{
    double t;
    if (!color || !mangetsu_positioned_gradient_position(layer, x, y, &t))
        return false;
    *color = mangetsu_gradient_sample_color_at(layer, t);
    return true;
}

uint32_t ass_mangetsu_gradient_sample_color(const MangetsuGradientLayer *layer,
                                            double x, double y)
{
    if (!layer || !layer->active || layer->n_stops <= 0)
        return 0;

    if (layer->coordinate_mode == MANGETSU_GRADIENT_POSITIONED_RECT) {
        uint32_t color = 0;
        return ass_mangetsu_positioned_gradient_sample_color(layer, x, y,
                                                              &color) ? color : 0;
    }

    double t = mangetsu_gradient_position(layer, x, y);
    return mangetsu_gradient_sample_color_at(layer, t);
}

static uint8_t mangetsu_gradient_sample_alpha_at(
    const MangetsuGradientLayer *layer, double t)
{
    if (t <= layer->stops[0].offset)
        return (uint8_t) layer->stops[0].color;

    for (int i = 1; i < layer->n_stops; i++) {
        const MangetsuGradientStop *prev = &layer->stops[i - 1];
        const MangetsuGradientStop *next = &layer->stops[i];
        if (t > next->offset)
            continue;
        double stop_span = next->offset - prev->offset;
        if (stop_span <= 0.0)
            return (uint8_t) next->color;
        return mix_byte((uint8_t) prev->color, (uint8_t) next->color,
                        (t - prev->offset) / stop_span);
    }

    return (uint8_t) layer->stops[layer->n_stops - 1].color;
}

uint8_t ass_mangetsu_gradient_sample_alpha(const MangetsuGradientLayer *layer,
                                           double x, double y)
{
    if (!layer || !layer->active || layer->n_stops <= 0)
        return 0;
    return mangetsu_gradient_sample_alpha_at(layer,
                                            mangetsu_gradient_position(layer, x, y));
}

bool ass_mangetsu_positioned_gradient_sample_alpha(
    const MangetsuGradientLayer *layer, double x, double y, uint8_t *alpha)
{
    double t;
    if (!alpha || !mangetsu_positioned_gradient_position(layer, x, y, &t))
        return false;
    *alpha = mangetsu_gradient_sample_alpha_at(layer, t);
    return true;
}

void ass_gradient_slide_unref(MangetsuGradientSlide *slide)
{
    while (slide && !--slide->references) {
        MangetsuGradientSlide *source = slide->source.slide;
        free(slide);
        slide = source;
    }
}

void ass_gradient_slides_clear(MangetsuGradientSlide **list)
{
    while (*list) {
        MangetsuGradientSlide *slide = *list;
        *list = slide->next;
        slide->next = NULL;
        ass_gradient_slide_unref(slide);
    }
}

static MangetsuGradientSlideEndpoint prepare_slide_endpoint(
    const MangetsuGradientLayer *paint, GradientRect bounds,
    GradientScriptMapping mapping)
{
    MangetsuGradientSlideEndpoint endpoint = {.paint = paint};
    if (!paint->active || paint->coordinate_mode == MANGETSU_GRADIENT_VECTOR)
        return endpoint;
    if (paint->coordinate_mode == MANGETSU_GRADIENT_POSITIONED_RECT) {
        bounds = (GradientRect) {
            paint->script_x1 * mapping.scale_x + mapping.origin_x,
            paint->script_y1 * mapping.scale_y + mapping.origin_y,
            paint->script_x2 * mapping.scale_x + mapping.origin_x,
            paint->script_y2 * mapping.scale_y + mapping.origin_y, true,
        };
    }
    if (bounds.valid)
        endpoint.field = prepare_gradient_rect(paint->angle,
            bounds.x0, bounds.y0, bounds.x1, bounds.y1);
    return endpoint;
}

static void include_slide_projection(const MangetsuGradientPositionedRect *rect,
                                      double dx, double dy, double *low, double *high)
{
    if (!rect->valid)
        return;
    double a = dx * (dx >= 0 ? rect->left : rect->right) +
               dy * (dy >= 0 ? rect->top : rect->bottom);
    double b = dx * (dx >= 0 ? rect->right : rect->left) +
               dy * (dy >= 0 ? rect->bottom : rect->top);
    *low = fmin(*low, a);
    *high = fmax(*high, b);
}

void ass_gradient_slide_prepare(MangetsuGradientSlideSampler *sampler,
                               const MangetsuGradientLayer *layer,
                               GradientRect bounds, GradientScriptMapping mapping)
{
    sampler->count = layer->slide->depth;
    const MangetsuGradientSlide *slide = layer->slide;
    for (int i = sampler->count - 1; i >= 0; i--, slide = slide->source.slide) {
        MangetsuGradientSlideStep *step = &sampler->steps[i];
        step->target = prepare_slide_endpoint(&slide->target, bounds, mapping);
        if (!i)
            sampler->source = prepare_slide_endpoint(&slide->source, bounds, mapping);
        double radians = (slide->target.active &&
                          slide->target.coordinate_mode != MANGETSU_GRADIENT_VECTOR ?
                          slide->target.angle : slide->source.angle) *
                          MANGETSU_GRADIENT_PI / 180.0;
        step->dx = step->target.field.valid ? step->target.field.dx : cos(radians);
        step->dy = step->target.field.valid ? step->target.field.dy : sin(radians);
    }

    /* The wrapper of an in-progress slide is marked attached for transport.
     * The next wipe must use the actual source fields, including rectangles
     * from earlier slides that still contribute to the resolved paint. */
    const MangetsuGradientPositionedRect attached = {
        .valid = bounds.valid, .left = bounds.x0, .right = bounds.x1,
        .top = bounds.y0, .bottom = bounds.y1,
    };
    const MangetsuGradientPositionedRect *source =
        sampler->source.paint->coordinate_mode == MANGETSU_GRADIENT_VECTOR ?
        &attached : &sampler->source.field;
    double scale_x = fabs(mapping.scale_x), scale_y = fabs(mapping.scale_y);
    slide = layer->slide;
    for (int i = sampler->count - 1; i >= 0; i--, slide = slide->source.slide) {
        MangetsuGradientSlideStep *step = &sampler->steps[i];
        double low = INFINITY, high = -INFINITY;
        include_slide_projection(source, step->dx, step->dy, &low, &high);
        bool uses_attached = source == &attached;
        for (int j = 0; j <= i; j++) {
            const MangetsuGradientSlideEndpoint *target = &sampler->steps[j].target;
            if (target->paint->coordinate_mode == MANGETSU_GRADIENT_POSITIONED_RECT)
                include_slide_projection(&target->field, step->dx, step->dy, &low, &high);
            else if (target->paint->active)
                uses_attached = true;
        }
        if (uses_attached || !isfinite(low))
            include_slide_projection(&attached, step->dx, step->dy, &low, &high);
        double scale = scale_x == scale_y ? scale_x :
            scale_x > 0 && scale_y > 0 ?
            1 / hypot(step->dx / scale_x, step->dy / scale_y) : 0;
        if (!isfinite(low) || !isfinite(high) || !isfinite(scale)) {
            /* Degenerate surfaces still use the existing transform progress. */
            step->dx = step->dy = 0;
            step->width = 1;
            step->leading_edge = slide->progress;
            continue;
        }
        /* Keep a very wide band finite after conversion to screen pixels.
         * Interpolate the endpoints without subtracting opposite extremes. */
        step->width = scale > 1 && slide->width > DBL_MAX / scale ?
            DBL_MAX : slide->width * scale;
        double edge = low * (1 - slide->progress) + high * slide->progress;
        double band = step->width * slide->progress;
        step->leading_edge = edge > DBL_MAX - band ? DBL_MAX : edge + band;
    }
}

static uint32_t sample_slide_endpoint(const MangetsuGradientSlideEndpoint *endpoint,
                                      double x, double y, int32_t uf, int32_t vf,
                                      uint32_t fallback, bool alpha)
{
    const MangetsuGradientLayer *paint = endpoint->paint;
    if (!paint->active)
        return paint->n_stops ? paint->stops[0].color : fallback;
    if (paint->coordinate_mode == MANGETSU_GRADIENT_VECTOR) {
        GradientValues values;
        for (int i = 0; i < 4; i++) {
            if (alpha)
                values.alpha[i] = paint->stops[i].color;
            else
                values.color[i] = paint->stops[i].color;
        }
        return alpha ? ass_gradient_sample_alpha_fixed(&values, uf, vf) :
                       ass_gradient_sample_color_fixed(&values, uf, vf);
    }
    const MangetsuGradientPositionedRect *rect = &endpoint->field;
    if (paint->coordinate_mode == MANGETSU_GRADIENT_POSITIONED_RECT &&
            (!rect->valid || x < rect->left || x > rect->right ||
             y < rect->top || y > rect->bottom))
        return fallback;
    double t = rect->valid ?
        clamp01((mangetsu_project(x, y, rect->dx, rect->dy) -
                 rect->projection_min) * rect->inverse_projection_span) : 0.0;
    if (!isfinite(t))
        return paint->coordinate_mode == MANGETSU_GRADIENT_POSITIONED_RECT ?
            fallback : paint->stops[0].color;
    return alpha ? mangetsu_gradient_sample_alpha_at(paint, t) :
                   mangetsu_gradient_sample_color_at(paint, t);
}

uint32_t ass_gradient_slide_sample(const MangetsuGradientSlideSampler *sampler,
                                   double x, double y, int32_t uf, int32_t vf,
                                   uint32_t fallback, bool alpha)
{
    if (sampler->count == 1) {
        const MangetsuGradientSlideStep *step = &sampler->steps[0];
        double distance = step->leading_edge - mangetsu_project(x, y, step->dx, step->dy);
        if (distance <= 0)
            return sample_slide_endpoint(&sampler->source, x, y, uf, vf, fallback, alpha);
        uint32_t target = sample_slide_endpoint(&step->target, x, y, uf, vf, fallback, alpha);
        if (distance >= step->width)
            return target;
        uint32_t source = sample_slide_endpoint(&sampler->source, x, y, uf, vf, fallback, alpha);
        double weight = distance / step->width;
        return alpha ? mix_byte(source, target, weight) :
                       mix_mangetsu_color(source, target, weight);
    }
    /* Skip all older paints once the pixel is fully behind a boundary. */
    double weights[MANGETSU_GRADIENT_SLIDE_MAX_DEPTH];
    int first = sampler->count - 1;
    for (; first >= 0; first--) {
        const MangetsuGradientSlideStep *step = &sampler->steps[first];
        double distance = step->leading_edge - mangetsu_project(x, y, step->dx, step->dy);
        weights[first] = distance <= 0 ? 0 :
                         distance >= step->width ? 1 : distance / step->width;
        if (weights[first] == 1)
            break;
    }
    const MangetsuGradientSlideEndpoint *start = first < 0 ? &sampler->source :
        &sampler->steps[first].target;
    uint32_t value = sample_slide_endpoint(start, x, y, uf, vf, fallback, alpha);
    for (int i = first + 1; i < sampler->count; i++) {
        if (!weights[i])
            continue;
        uint32_t target = sample_slide_endpoint(
            &sampler->steps[i].target, x, y, uf, vf, fallback, alpha);
        value = alpha ? mix_byte(value, target, weights[i]) :
                        mix_mangetsu_color(value, target, weights[i]);
    }
    return value;
}
