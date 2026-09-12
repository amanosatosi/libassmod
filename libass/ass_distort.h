/*
 * Bilinear distortion helpers shared between renderer and tests.
 */

#ifndef LIBASS_DISTORT_H
#define LIBASS_DISTORT_H

#include "ass_outline.h"

typedef struct {
    double u0, v0;  // top-left
    double u1, v1;  // top-right
    double u2, v2;  // bottom-right
    double u3, v3;  // bottom-left
} ASS_DistortParams;

ASS_DVector ass_distort_map_point(const ASS_DistortParams *p,
                                  double min_x, double min_y,
                                  double max_x, double max_y,
                                  double x, double y);

#endif /* LIBASS_DISTORT_H */
