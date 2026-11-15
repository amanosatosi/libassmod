/* SPDX-License-Identifier: ISC */

#ifndef LIBASS_PERSPECTIVE_H
#define LIBASS_PERSPECTIVE_H

#include "ass_outline.h"

/* Solve a rectangle-to-quad homography. Destination corners are already
 * resolved into the output plane, ordered TL, TR, BR, BL. */
bool ass_perspective_solve(double matrix[3][3], ASS_DRect source,
                           const ASS_DVector corners[4]);

bool ass_perspective_raster_fits(const ASS_Rect *bounds, unsigned tile_order,
                                 size_t max_pixels);

/* Source and destination coordinates use the outline rasterizer's 1/64 pixel
 * units. Tolerance excludes final point rounding. Limits apply to one outline
 * and its tile-padded raster area (tile order 4 or 5), before bitmap allocation.
 * Result must be distinct from source and uninitialized or empty. It is empty
 * on failure. */
bool ass_perspective_outline(ASS_Outline *result, const ASS_Outline *source,
                             const double matrix[3][3], double tolerance,
                             size_t max_segments, unsigned tile_order,
                             size_t max_pixels);

#endif /* LIBASS_PERSPECTIVE_H */
