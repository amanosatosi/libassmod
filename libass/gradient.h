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

#ifndef LIBASS_GRADIENT_H
#define LIBASS_GRADIENT_H

#include <stdbool.h>
#include <stdint.h>

#define gradient_state_reset ass_gradient_state_reset
#define gradient_apply_color ass_gradient_apply_color
#define gradient_apply_alpha ass_gradient_apply_alpha
#define gradient_disable_color ass_gradient_disable_color
#define gradient_disable_alpha ass_gradient_disable_alpha
#define gradient_equal ass_gradient_equal
#define gradient_sample_color ass_gradient_sample_color
#define gradient_sample_alpha ass_gradient_sample_alpha

#define MANGETSU_GRADIENT_MAX_STOPS 64
#define MANGETSU_GRADIENT_DEBUG_MAX_SEGMENTS 64
#define MANGETSU_GRADIENT_LAYERS 5
#define MANGETSU_GRADIENT_BORDER_LAYERS 10

typedef enum {
    MANGETSU_GRADIENT_TARGET_COLOR = 0,
    MANGETSU_GRADIENT_TARGET_BORDER = 1,
    MANGETSU_GRADIENT_TARGET_ALPHA = 2,
    MANGETSU_GRADIENT_TARGET_BORDER_ALPHA = 3,
} MangetsuGradientTarget;

typedef struct {
    bool color_enabled;
    bool alpha_enabled;
    uint32_t color[4];
    uint8_t alpha[4];
} GradientValues;

typedef struct {
    GradientValues layer[4];
} GradientState;

typedef struct {
    double x0, y0, x1, y1;
    bool valid;
} GradientRect;

typedef struct {
    double offset;
    uint32_t color;
} MangetsuGradientStop;

typedef enum {
    MANGETSU_GRADIENT_TYPE_LINEAR = 0,
} MangetsuGradientType;

typedef struct {
    bool active;
    MangetsuGradientType type;
    int segment_id;
    double angle;
    int n_stops;
    MangetsuGradientStop stops[MANGETSU_GRADIENT_MAX_STOPS];
    GradientRect rect;
} MangetsuGradientLayer;

typedef struct {
    MangetsuGradientLayer layer[MANGETSU_GRADIENT_LAYERS];
    MangetsuGradientLayer border[MANGETSU_GRADIENT_BORDER_LAYERS];
    MangetsuGradientLayer alpha[MANGETSU_GRADIENT_LAYERS];
    MangetsuGradientLayer border_alpha[MANGETSU_GRADIENT_BORDER_LAYERS];
} MangetsuGradientState;

typedef struct {
    bool active;
    MangetsuGradientTarget target;
    int layer;
    MangetsuGradientType type;
    int segment_id;
    double angle;
    int n_stops;
    int bitmap_count;
    bool rect_valid;
    MangetsuGradientStop stops[MANGETSU_GRADIENT_MAX_STOPS];
} MangetsuGradientDebugSegment;

typedef struct {
    int n_segments;
    MangetsuGradientDebugSegment segments[MANGETSU_GRADIENT_DEBUG_MAX_SEGMENTS];
} MangetsuGradientDebugState;

void ass_gradient_state_reset(GradientState *state, const uint32_t *base_colors);
void ass_gradient_apply_color(GradientState *state, int layer, const uint32_t *values,
                              int count, double pwr);
void ass_gradient_apply_alpha(GradientState *state, int layer, const uint8_t *values,
                              int count, double pwr);
void ass_gradient_disable_color(GradientState *state, int layer, uint32_t fallback,
                                double pwr);
void ass_gradient_disable_alpha(GradientState *state, int layer, uint8_t fallback,
                                double pwr);
void ass_gradient_values_reset(GradientValues *values, uint32_t base_color);
void ass_gradient_values_apply_color(GradientValues *dst, const uint32_t *values,
                                     int count, double pwr);
void ass_gradient_values_apply_alpha(GradientValues *dst, const uint8_t *values,
                                     int count, double pwr);
void ass_gradient_values_disable_color(GradientValues *dst, uint32_t fallback,
                                       double pwr);
void ass_gradient_values_disable_alpha(GradientValues *dst, uint8_t fallback,
                                       double pwr);
bool ass_gradient_equal(const GradientState *a, const GradientState *b);

uint32_t ass_gradient_sample_color(const GradientValues *val, double u, double v);
uint8_t ass_gradient_sample_alpha(const GradientValues *val, double u, double v);
uint32_t ass_gradient_sample_color_fixed(const GradientValues *val, int32_t uf, int32_t vf);
uint8_t ass_gradient_sample_alpha_fixed(const GradientValues *val, int32_t uf, int32_t vf);

void ass_mangetsu_gradient_state_reset(MangetsuGradientState *state);
void ass_mangetsu_gradient_layer_reset(MangetsuGradientLayer *layer);
bool ass_mangetsu_gradient_state_equal(const MangetsuGradientState *a,
                                       const MangetsuGradientState *b);
uint32_t ass_mangetsu_gradient_sample_color(const MangetsuGradientLayer *layer,
                                            double x, double y);
uint8_t ass_mangetsu_gradient_sample_alpha(const MangetsuGradientLayer *layer,
                                           double x, double y);

#endif /* LIBASS_GRADIENT_H */
