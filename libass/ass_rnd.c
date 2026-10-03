/*
 * Copyright (C) 2026 libass contributors
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

/* Behavioral implementation of VSFilterMod's Microsoft-CRT/SSE2 path RNG.
 * The reference is src/subtitles/RTS.cpp in sorayuki/VSFilterMod. */
#include "ass_rnd.h"

#include <math.h>
#include <limits.h>

int32_t ass_rnd_truncate(double value)
{
    if (!isfinite(value))
        return 0;
    if (value >= INT32_MAX)
        return INT32_MAX;
    if (value <= INT32_MIN)
        return INT32_MIN;
    return (int32_t) value;
}

uint32_t ass_rnd_next(uint32_t *state)
{
    *state = *state * UINT32_C(214013) + UINT32_C(2531011);
    return (*state >> 16) & UINT32_C(32767);
}

void ass_rnd_group(uint32_t *state, const int32_t amplitude[3], float out[4][3])
{
    for (int lane = 0; lane < 4; lane++) {
        for (int axis = 0; axis < 3; axis++) {
            float offset = 0;
            if (amplitude[axis] > 0) {
                /* Wider arithmetic avoids the reference's signed overflow,
                 * without changing its 15-bit modulo bias at large values. */
                int64_t amp = (int64_t) amplitude[axis] * 100;
                int64_t delta = amp - ass_rnd_next(state) % (amp * 2 + 1);
                /* Match the SSE2 float conversion and 0.01f multiply. */
                offset = (float) delta * 0.01f;
            }
            out[3 - lane][axis] = offset;
        }
    }
}

bool ass_rnd_project(const double matrix[3][4], double x, double y, double z,
                     double minimum_depth, double out[2])
{
    double v[3];
    for (int i = 0; i < 3; i++)
        v[i] = matrix[i][0] * x + matrix[i][1] * y + matrix[i][2] + matrix[i][3] * z;
    double w = 1 / fmax(v[2], minimum_depth);
    out[0] = v[0] * w;
    out[1] = v[1] * w;
    return isfinite(out[0]) && isfinite(out[1]);
}
