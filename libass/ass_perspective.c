/* SPDX-License-Identifier: ISC */

#include "config.h"
#include "ass_compat.h"

#include <float.h>

#include "ass_perspective.h"

// Reject poorly conditioned quads and points too close to the horizon.
static const double conditioning_epsilon = 1e-8;
enum { MAX_SUBDIVISION_DEPTH = 16 };

static double cross(ASS_DVector a, ASS_DVector b)
{
    return a.x * b.y - a.y * b.x;
}

bool ass_perspective_solve(double matrix[3][3], ASS_DRect source,
                           const ASS_DVector corners[4])
{
    double width = source.x_max - source.x_min;
    double height = source.y_max - source.y_min;
    if (!isfinite(width) || !isfinite(height) || width <= 0 || height <= 0)
        return false;

    // Solve near the origin at unit scale to keep the cross products stable.
    ASS_DVector q[4];
    double scale = 0;
    for (int i = 0; i < 4; i++) {
        q[i] = (ASS_DVector) {corners[i].x - corners[0].x,
                             corners[i].y - corners[0].y};
        if (!isfinite(q[i].x) || !isfinite(q[i].y))
            return false;
        scale = FFMAX(scale, FFMAX(fabs(q[i].x), fabs(q[i].y)));
    }
    if (scale == 0)
        return false;

    for (int i = 0; i < 4; i++) {
        q[i].x /= scale;
        q[i].y /= scale;
    }
    ASS_DVector edges[4];
    for (int i = 0; i < 4; i++) {
        int next = (i + 1) % 4;
        edges[i] = (ASS_DVector) {q[next].x - q[i].x, q[next].y - q[i].y};
    }
    double turn = cross(edges[0], edges[1]);
    if (fabs(turn) <= conditioning_epsilon)
        return false;
    // Strict convexity with either winding. A crossed or collapsed quad fails.
    for (int i = 1; i < 4; i++) {
        double next_turn = cross(edges[i], edges[(i + 1) % 4]);
        if (fabs(next_turn) <= conditioning_epsilon || (next_turn > 0) != (turn > 0))
            return false;
    }

    // Solve the denominator 1 + g*u + h*v on the unit source square.
    ASS_DVector delta = {edges[1].x + edges[3].x, edges[1].y + edges[3].y};
    double determinant = cross(edges[1], edges[2]);
    double g = cross(edges[2], delta) / determinant;
    double h = cross(edges[1], delta) / determinant;
    double w1 = 1 + g, w3 = 1 + h, w2 = 1 + g + h;
    double max_w = FFMAX(1, FFMAX(w1, FFMAX(w2, w3)));
    if (FFMIN(1, FFMIN(w1, FFMIN(w2, w3))) <= conditioning_epsilon * max_w)
        return false;

    double m[3][3] = {
        {w1 * (corners[1].x - corners[0].x), w3 * (corners[3].x - corners[0].x), 0},
        {w1 * (corners[1].y - corners[0].y), w3 * (corners[3].y - corners[0].y), 0},
        {g, h, 1},
    };
    for (int i = 0; i < 2; i++) {
        double origin = i == 0 ? corners[0].x : corners[0].y;
        for (int j = 0; j < 3; j++)
            m[i][j] += origin * m[2][j];
    }
    for (int i = 0; i < 3; i++) {
        m[i][0] /= width;
        m[i][1] /= height;
        m[i][2] -= m[i][0] * source.x_min + m[i][1] * source.y_min;
        if (!isfinite(m[i][0]) || !isfinite(m[i][1]) || !isfinite(m[i][2]))
            return false;
    }
    memcpy(matrix, m, sizeof(m));
    return true;
}

typedef struct {
    double x, y, w;
} HomogeneousPoint;

static bool transform_control(HomogeneousPoint *out, ASS_Vector p,
                              const double m[3][3])
{
    double wx = m[2][0] * p.x, wy = m[2][1] * p.y;
    double w = wx + wy + m[2][2];
    double depth_scale = fabs(wx) + fabs(wy) + fabs(m[2][2]);
    if (!isfinite(w) || w < DBL_MIN || !(w > conditioning_epsilon * depth_scale))
        return false;

    double x = m[0][0] * p.x + m[0][1] * p.y + m[0][2];
    double y = m[1][0] * p.x + m[1][1] * p.y + m[1][2];
    if (!(fabs(x / w) <= OUTLINE_MAX && fabs(y / w) <= OUTLINE_MAX))
        return false;
    *out = (HomogeneousPoint) {x, y, w};
    return true;
}

static bool curve_is_flat(const ASS_DVector p[4], int order, double error_sq)
{
    double dx = p[order].x - p[0].x, dy = p[order].y - p[0].y;
    double len_sq = dx * dx + dy * dy;
    for (int i = 1; i < order; i++) {
        double x = p[i].x - p[0].x, y = p[i].y - p[0].y;
        double t = len_sq > 0 ? (x * dx + y * dy) / len_sq : 0;
        t = FFMINMAX(t, 0, 1);
        x -= t * dx;
        y -= t * dy;
        if (x * x + y * y > error_sq)
            return false;
    }
    return true;
}

static bool flatten_curve(ASS_Outline *result, const HomogeneousPoint p[4],
                          int order, double error_sq, size_t max_segments,
                          int depth)
{
    if (result->n_segments >= max_segments)
        return false;

    ASS_DVector projected[4];
    for (int i = 0; i <= order; i++)
        projected[i] = (ASS_DVector) {p[i].x / p[i].w, p[i].y / p[i].w};

    // Positive weights keep the rational curve in the projected control hull.
    // Distance to the finite chord also detects collinear overshoots.
    if (curve_is_flat(projected, order, error_sq)) {
        ASS_Vector start = {ass_lrint(projected[0].x), ass_lrint(projected[0].y)};
        return ass_outline_add_point(result, start, OUTLINE_LINE_SEGMENT);
    }
    if (depth == MAX_SUBDIVISION_DEPTH)
        return false;

    HomogeneousPoint left[4], right[4], work[4];
    memcpy(work, p, (order + 1) * sizeof(*work));
    left[0] = p[0];
    right[order] = p[order];
    for (int i = 1; i <= order; i++) {
        for (int j = 0; j <= order - i; j++) {
            work[j].x = 0.5 * work[j].x + 0.5 * work[j + 1].x;
            work[j].y = 0.5 * work[j].y + 0.5 * work[j + 1].y;
            work[j].w = 0.5 * work[j].w + 0.5 * work[j + 1].w;
        }
        left[i] = work[0];
        right[order - i] = work[order - i];
    }
    return flatten_curve(result, left, order, error_sq, max_segments, depth + 1) &&
           flatten_curve(result, right, order, error_sq, max_segments, depth + 1);
}

static bool flatten_outline(ASS_Outline *result, const ASS_Outline *source,
                            const double matrix[3][3], double error_sq,
                            size_t max_segments)
{
    if (!ass_outline_alloc(result, source->n_segments, source->n_segments))
        return false;

    size_t start = 0, point = 0;
    for (size_t i = 0; i < source->n_segments; i++) {
        unsigned flags = (unsigned char) source->segments[i];
        unsigned order = flags & OUTLINE_COUNT_MASK;
        bool close = flags & OUTLINE_CONTOUR_END;
        if (!order || (flags & ~(OUTLINE_COUNT_MASK | OUTLINE_CONTOUR_END)) ||
                order > source->n_points - point)
            return false;

        size_t next = point + order;
        size_t end = close ? start : next;
        if (end >= source->n_points)
            return false;

        HomogeneousPoint p[4];
        for (unsigned j = 0; j < order; j++)
            if (!transform_control(&p[j], source->points[point + j], matrix))
                return false;
        if (!transform_control(&p[order], source->points[end], matrix) ||
                !flatten_curve(result, p, order, error_sq, max_segments, 0))
            return false;

        point = next;
        if (close) {
            ass_outline_close_contour(result);
            start = point;
        }
    }
    return point == source->n_points && start == point;
}

static bool fits_raster(const ASS_Outline *outline, unsigned tile_order,
                        size_t max_pixels)
{
    ASS_Rect box;
    rectangle_reset(&box);
    ass_outline_update_cbox(outline, &box);
    // Match the padding in ass_outline_to_bitmap. Control bounds also cover
    // the affine fast path, where the rasterizer subdivides curves itself.
    int32_t width = ((box.x_max + 127) >> 6) - ((box.x_min - 1) >> 6);
    int32_t height = ((box.y_max + 127) >> 6) - ((box.y_min - 1) >> 6);
    unsigned mask = (1U << tile_order) - 1;
    size_t tile_width = (width + mask) & ~mask;
    size_t tile_height = (height + mask) & ~mask;
    return tile_width <= max_pixels / tile_height;
}

bool ass_perspective_outline(ASS_Outline *result, const ASS_Outline *source,
                             const double matrix[3][3], double tolerance,
                             size_t max_segments, unsigned tile_order,
                             size_t max_pixels)
{
    ass_outline_clear(result);
    if (!source->n_segments && !source->n_points)
        return true;
    if (!source->n_segments || !source->n_points ||
            source->n_segments > max_segments || !isfinite(tolerance) ||
            tolerance <= 0 || tile_order < 4 || tile_order > 5 || !max_pixels)
        return false;

    bool ok;
    if (matrix[2][0] == 0 && matrix[2][1] == 0) {
        double w = matrix[2][2];
        if (!isfinite(w) || w <= 0)
            return false;
        double affine[2][3];
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 3; j++)
                affine[i][j] = matrix[i][j] / w;
        ok = ass_outline_transform_2d(result, source, affine);
    } else {
        ok = flatten_outline(result, source, matrix, tolerance * tolerance,
                             max_segments);
    }
    if (ok && fits_raster(result, tile_order, max_pixels))
        return true;

    ass_outline_free(result);
    return false;
}
