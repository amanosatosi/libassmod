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

#include <math.h>
#include <string.h>

#include "gradient.h"

#define CR(c)   ((uint8_t) ((c) >> 24))
#define CG(c)   ((uint8_t) ((c) >> 16))
#define CB(c)   ((uint8_t) ((c) >> 8))
#define CA(c)   ((uint8_t) (c))

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

    for (int i = 0; i < 4; i++) {
        uint32_t c = base_colors[i];
        for (int j = 0; j < 4; j++) {
            state->layer[i].color[j] = c;
            state->layer[i].alpha[j] = CA(c);
        }
    }
}

void ass_gradient_apply_color(GradientState *state, int layer, const uint32_t *values,
                              int count, double pwr)
{
    if (!state || layer < 0 || layer >= 4 || count <= 0 || !values)
        return;

    GradientValues *dst = &state->layer[layer];
    for (int i = 0; i < 4; i++) {
        uint32_t v = values[i < count ? i : count - 1];
        dst->color[i] = mix_color(dst->color[i], v, pwr);
    }
    dst->color_enabled = true;
}

void ass_gradient_apply_alpha(GradientState *state, int layer, const uint8_t *values,
                              int count, double pwr)
{
    if (!state || layer < 0 || layer >= 4 || count <= 0 || !values)
        return;

    GradientValues *dst = &state->layer[layer];
    for (int i = 0; i < 4; i++) {
        uint8_t v = values[i < count ? i : count - 1];
        dst->alpha[i] = mix_byte(dst->alpha[i], v, pwr);
    }
    dst->alpha_enabled = true;
}

void ass_gradient_disable_color(GradientState *state, int layer, uint32_t fallback,
                                double pwr)
{
    if (!state || layer < 0 || layer >= 4)
        return;

    GradientValues *dst = &state->layer[layer];
    for (int i = 0; i < 4; i++)
        dst->color[i] = mix_color(dst->color[i], fallback, pwr);

    dst->color_enabled = pwr < 1.0 ? dst->color_enabled : false;
}

void ass_gradient_disable_alpha(GradientState *state, int layer, uint8_t fallback,
                                double pwr)
{
    if (!state || layer < 0 || layer >= 4)
        return;

    GradientValues *dst = &state->layer[layer];
    for (int i = 0; i < 4; i++)
        dst->alpha[i] = mix_byte(dst->alpha[i], fallback, pwr);

    dst->alpha_enabled = pwr < 1.0 ? dst->alpha_enabled : false;
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

uint32_t ass_gradient_sample_color(const GradientValues *val, double u, double v)
{
    u = clamp01(u);
    v = clamp01(v);
    int32_t uf = (int32_t) (u * 65536.0);
    int32_t vf = (int32_t) (v * 65536.0);
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

uint8_t ass_gradient_sample_alpha(const GradientValues *val, double u, double v)
{
    u = clamp01(u);
    v = clamp01(v);
    int32_t uf = (int32_t) (u * 65536.0);
    int32_t vf = (int32_t) (v * 65536.0);
    if (uf < 0) uf = 0; else if (uf > 65536) uf = 65536;
    if (vf < 0) vf = 0; else if (vf > 65536) vf = 65536;

    uint8_t a = (uint8_t) sample_channel(val->alpha[0], val->alpha[1],
                                         val->alpha[2], val->alpha[3],
                                         uf, vf);
    return a;
}
