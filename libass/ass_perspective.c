/*
 * True projective corner-pin helper.
 */

#include "ass_perspective.h"

#include <float.h>
#include <math.h>

static bool finite_point(ASS_DVector p)
{
    return isfinite(p.x) && isfinite(p.y);
}

static bool same_finite_side(double a, double b, double c, double d)
{
    if (!isfinite(a) || !isfinite(b) || !isfinite(c) || !isfinite(d))
        return false;
    double scale = fmax(fmax(fabs(a), fabs(b)), fmax(fabs(c), fabs(d)));
    double eps = fmax(1.0, scale) * 64 * DBL_EPSILON;
    if (fabs(a) <= eps || fabs(b) <= eps || fabs(c) <= eps || fabs(d) <= eps)
        return false;
    bool positive = a > 0;
    return (b > 0) == positive && (c > 0) == positive && (d > 0) == positive;
}

bool ass_perspective_solve(const ASS_PerspectiveParams *p,
                           double min_x, double min_y,
                           double max_x, double max_y,
                           ASS_Homography *homography)
{
    if (!p || !homography || !isfinite(min_x) || !isfinite(min_y) ||
            !isfinite(max_x) || !isfinite(max_y))
        return false;
    double width = max_x - min_x;
    double height = max_y - min_y;
    if (!(width > 0) || !(height > 0) || !isfinite(width) || !isfinite(height))
        return false;
    for (int i = 0; i < 4; i++)
        if (!finite_point(p->corner[i]))
            return false;

    const ASS_DVector p0 = p->corner[0], p1 = p->corner[1];
    const ASS_DVector p2 = p->corner[2], p3 = p->corner[3];
    double dx1 = p1.x - p2.x, dy1 = p1.y - p2.y;
    double dx2 = p3.x - p2.x, dy2 = p3.y - p2.y;
    double dx3 = p0.x - p1.x + p2.x - p3.x;
    double dy3 = p0.y - p1.y + p2.y - p3.y;
    double coord_scale = 1.0;
    for (int i = 0; i < 4; i++) {
        coord_scale = fmax(coord_scale, fabs(p->corner[i].x));
        coord_scale = fmax(coord_scale, fabs(p->corner[i].y));
    }
    double eps = coord_scale * 256 * DBL_EPSILON;

    double g = 0.0, h = 0.0;
    if (fabs(dx3) > eps || fabs(dy3) > eps) {
        double det = dx1 * dy2 - dx2 * dy1;
        if (!isfinite(det) || fabs(det) <= eps * coord_scale)
            return false;
        g = (dx3 * dy2 - dx2 * dy3) / det;
        h = (dx1 * dy3 - dx3 * dy1) / det;
    }

    /* A finite rectangle cannot project to a bow-tie or cross the horizon.
     * Checking W at its four corners is sufficient because W is affine. */
    if (!same_finite_side(1.0, 1.0 + g, 1.0 + g + h, 1.0 + h))
        return false;

    double unit[3][3] = {
        {p1.x - p0.x + g * p1.x, p3.x - p0.x + h * p3.x, p0.x},
        {p1.y - p0.y + g * p1.y, p3.y - p0.y + h * p3.y, p0.y},
        {g, h, 1.0},
    };
    double det =
        unit[0][0] * (unit[1][1] * unit[2][2] - unit[1][2] * unit[2][1]) -
        unit[0][1] * (unit[1][0] * unit[2][2] - unit[1][2] * unit[2][0]) +
        unit[0][2] * (unit[1][0] * unit[2][1] - unit[1][1] * unit[2][0]);
    if (!isfinite(det) || fabs(det) <=
            fmax(1.0, coord_scale * coord_scale) * 256 * DBL_EPSILON)
        return false;
    double sx = 1.0 / width, sy = 1.0 / height;
    double source_to_unit[3][3] = {
        {sx, 0.0, -min_x * sx},
        {0.0, sy, -min_y * sy},
        {0.0, 0.0, 1.0},
    };
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            double value = 0.0;
            for (int k = 0; k < 3; k++)
                value += unit[row][k] * source_to_unit[k][col];
            if (!isfinite(value))
                return false;
            homography->m[row][col] = value;
        }
    }
    return true;
}

bool ass_perspective_map_point(const ASS_Homography *homography,
                               double x, double y, ASS_DVector *mapped)
{
    if (!homography || !mapped || !isfinite(x) || !isfinite(y))
        return false;
    const double (*m)[3] = homography->m;
    double X = m[0][0] * x + m[0][1] * y + m[0][2];
    double Y = m[1][0] * x + m[1][1] * y + m[1][2];
    double W = m[2][0] * x + m[2][1] * y + m[2][2];
    double scale = fabs(m[2][0] * x) + fabs(m[2][1] * y) + fabs(m[2][2]);
    if (!isfinite(X) || !isfinite(Y) || !isfinite(W) ||
            fabs(W) <= fmax(1.0, scale) * 64 * DBL_EPSILON)
        return false;
    mapped->x = X / W;
    mapped->y = Y / W;
    return finite_point(*mapped);
}

static void multiply(double out[3][3], const double a[3][3],
                     const double b[3][3])
{
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++) {
            out[row][col] = 0;
            for (int k = 0; k < 3; k++)
                out[row][col] += a[row][k] * b[k][col];
        }
}

bool ass_perspective_plane_matrix(const ASS_PerspectiveParams *p,
                                  double anchor_x, double anchor_y,
                                  double scale_x, double scale_y,
                                  ASS_Homography *homography)
{
    if (!p || !p->plane || !homography || !isfinite(anchor_x) ||
            !isfinite(anchor_y) || !isfinite(scale_x) || !isfinite(scale_y) ||
            !(scale_x > 0) || !(scale_y > 0))
        return false;
    double det =
        p->matrix[0][0] * (p->matrix[1][1] * p->matrix[2][2] -
                           p->matrix[1][2] * p->matrix[2][1]) -
        p->matrix[0][1] * (p->matrix[1][0] * p->matrix[2][2] -
                           p->matrix[1][2] * p->matrix[2][0]) +
        p->matrix[0][2] * (p->matrix[1][0] * p->matrix[2][1] -
                           p->matrix[1][1] * p->matrix[2][0]);
    if (!isfinite(det) || fabs(det) < 256 * DBL_EPSILON)
        return false;
    double to_local[3][3] = {
        {1 / scale_x, 0, -anchor_x / scale_x},
        {0, 1 / scale_y, -anchor_y / scale_y},
        {0, 0, 1},
    };
    double to_screen[3][3] = {
        {scale_x, 0, anchor_x}, {0, scale_y, anchor_y}, {0, 0, 1},
    };
    double intermediate[3][3];
    multiply(intermediate, p->matrix, to_local);
    multiply(homography->m, to_screen, intermediate);
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++)
            if (!isfinite(homography->m[row][col]))
                return false;
    return true;
}
