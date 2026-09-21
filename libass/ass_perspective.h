/*
 * True projective corner-pin helpers shared between renderer and tests.
 */

#ifndef LIBASS_PERSPECTIVE_H
#define LIBASS_PERSPECTIVE_H

#include <stdbool.h>

#include "ass_outline.h"

typedef struct {
    ASS_DVector corner[4];  // P0 top-left, P1 top-right, P2 bottom-right, P3 bottom-left
} ASS_PerspectiveParams;

typedef struct {
    double m[3][3];
} ASS_Homography;

bool ass_perspective_solve(const ASS_PerspectiveParams *p,
                           double min_x, double min_y,
                           double max_x, double max_y,
                           ASS_Homography *homography);

bool ass_perspective_map_point(const ASS_Homography *homography,
                               double x, double y, ASS_DVector *mapped);

#endif /* LIBASS_PERSPECTIVE_H */
