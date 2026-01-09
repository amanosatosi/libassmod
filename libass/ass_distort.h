/*
 * Bilinear distortion helpers shared between renderer and tests.
 */

#ifndef LIBASS_DISTORT_H
#define LIBASS_DISTORT_H

#include "ass_outline.h"

typedef struct {
    double u1, v1;
    double u2, v2;
    double u3, v3;
} ASS_DistortParams;

ASS_DVector ass_distort_map_point(const ASS_DistortParams *p,
                                  double min_x, double min_y,
                                  double max_x, double max_y,
                                  double x, double y);

#endif /* LIBASS_DISTORT_H */
