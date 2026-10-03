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

/* VSFilterMod random path compatibility. No process-global RNG state. */
#ifndef LIBASS_RND_H
#define LIBASS_RND_H

#include <stdbool.h>
#include <stdint.h>

int32_t ass_rnd_truncate(double value);
uint32_t ass_rnd_next(uint32_t *state);
/* Always consume four interleaved XYZ lanes, including a partial final group.
 * Output order is path order, the reverse of the reference SSE2 lanes. */
void ass_rnd_group(uint32_t *state, const int32_t amplitude[3], float out[4][3]);
/* Project a local XYZ point. Matrix columns are X, Y, translation, and Z. */
bool ass_rnd_project(const double matrix[3][4], double x, double y, double z,
                     double minimum_depth, double out[2]);

#endif
