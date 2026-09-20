/*
 * Curved-text path helpers.
 *
 * The ASS drawing parser remains authoritative for path syntax.  These
 * helpers only turn its single-contour outline into an open, arc-length
 * parameterised polyline suitable for baseline layout.
 */

#ifndef LIBASS_CURVED_TEXT_H
#define LIBASS_CURVED_TEXT_H

#include <stdbool.h>
#include <stddef.h>

#include "ass_outline.h"

typedef struct {
    ASS_DVector *points;
    double *distance;
    size_t n_points;
    double length;
} ASS_CurvedPath;

bool ass_curved_outline_usable(const ASS_Outline *outline);
bool ass_curved_path_flatten(ASS_CurvedPath *path,
                             const ASS_Outline *outline,
                             double scale_x, double scale_y);
void ass_curved_path_free(ASS_CurvedPath *path);

/* Samples by physical polyline distance.  Distances outside the path are
 * extrapolated along the first/last tangent, avoiding collapsed clusters on
 * paths shorter than the shaped line. */
bool ass_curved_path_sample(const ASS_CurvedPath *path, double distance,
                            ASS_DVector *point, ASS_DVector *tangent);

#endif /* LIBASS_CURVED_TEXT_H */
