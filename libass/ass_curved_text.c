/*
 * Arc-length path support for Mangetsu curved text.
 */

#include "config.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ass_curved_text.h"

#define CURVED_PATH_MAX_POINTS 65536
#define CURVED_PATH_MAX_DEPTH 16
#define CURVED_PATH_TOLERANCE 0.25

typedef struct {
    ASS_DVector *points;
    size_t n_points;
    size_t max_points;
} PathBuilder;

static bool add_point(PathBuilder *builder, ASS_DVector point)
{
    if (!isfinite(point.x) || !isfinite(point.y))
        return false;

    if (builder->n_points) {
        ASS_DVector last = builder->points[builder->n_points - 1];
        if (point.x == last.x && point.y == last.y)
            return true;
    }

    if (builder->n_points >= CURVED_PATH_MAX_POINTS)
        return false;
    if (builder->n_points >= builder->max_points) {
        size_t next = builder->max_points ? builder->max_points * 2 : 64;
        if (next > CURVED_PATH_MAX_POINTS)
            next = CURVED_PATH_MAX_POINTS;
        if (next <= builder->n_points || next > SIZE_MAX / sizeof(*builder->points))
            return false;
        ASS_DVector *points = realloc(builder->points,
                                      next * sizeof(*builder->points));
        if (!points)
            return false;
        builder->points = points;
        builder->max_points = next;
    }

    builder->points[builder->n_points++] = point;
    return true;
}

static double point_line_distance(ASS_DVector point,
                                  ASS_DVector start, ASS_DVector end)
{
    double dx = end.x - start.x;
    double dy = end.y - start.y;
    double length = hypot(dx, dy);
    if (length == 0)
        return hypot(point.x - start.x, point.y - start.y);
    return fabs(dx * (start.y - point.y) -
                (start.x - point.x) * dy) / length;
}

static bool flatten_cubic(PathBuilder *builder,
                          ASS_DVector p0, ASS_DVector p1,
                          ASS_DVector p2, ASS_DVector p3,
                          int depth)
{
    double flatness = fmax(point_line_distance(p1, p0, p3),
                           point_line_distance(p2, p0, p3));
    if (flatness <= CURVED_PATH_TOLERANCE || depth >= CURVED_PATH_MAX_DEPTH)
        return add_point(builder, p3);

    ASS_DVector p01 = {(p0.x + p1.x) * 0.5, (p0.y + p1.y) * 0.5};
    ASS_DVector p12 = {(p1.x + p2.x) * 0.5, (p1.y + p2.y) * 0.5};
    ASS_DVector p23 = {(p2.x + p3.x) * 0.5, (p2.y + p3.y) * 0.5};
    ASS_DVector p012 = {(p01.x + p12.x) * 0.5, (p01.y + p12.y) * 0.5};
    ASS_DVector p123 = {(p12.x + p23.x) * 0.5, (p12.y + p23.y) * 0.5};
    ASS_DVector mid = {(p012.x + p123.x) * 0.5,
                       (p012.y + p123.y) * 0.5};

    return flatten_cubic(builder, p0, p01, p012, mid, depth + 1) &&
           flatten_cubic(builder, mid, p123, p23, p3, depth + 1);
}

bool ass_curved_outline_usable(const ASS_Outline *outline)
{
    if (!outline || !outline->points || !outline->segments ||
            outline->n_segments < 2 || !outline->n_points)
        return false;

    size_t points = 0;
    int contours = 0;
    for (size_t i = 0; i < outline->n_segments; i++) {
        int order = outline->segments[i] & OUTLINE_COUNT_MASK;
        if (order != OUTLINE_LINE_SEGMENT &&
                order != OUTLINE_CUBIC_SPLINE)
            return false;
        if (outline->segments[i] & OUTLINE_CONTOUR_END) {
            contours++;
            if (i + 1 != outline->n_segments)
                return false;
        }
        if (points > SIZE_MAX - (size_t) order)
            return false;
        points += order;
    }

    /* ass_drawing_parse() appends one implicit closing line.  It is omitted
     * when this outline is interpreted as an open baseline. */
    if (contours != 1 || points != outline->n_points ||
            (outline->segments[outline->n_segments - 1] &
             OUTLINE_COUNT_MASK) != OUTLINE_LINE_SEGMENT)
        return false;

    ASS_Vector first = outline->points[0];
    for (size_t i = 1; i < outline->n_points; i++)
        if (outline->points[i].x != first.x ||
                outline->points[i].y != first.y)
            return true;
    return false;
}

bool ass_curved_path_flatten(ASS_CurvedPath *path,
                             const ASS_Outline *outline,
                             double scale_x, double scale_y)
{
    if (!path)
        return false;
    memset(path, 0, sizeof(*path));
    if (!ass_curved_outline_usable(outline) ||
            !isfinite(scale_x) || !isfinite(scale_y) ||
            scale_x == 0 || scale_y == 0)
        return false;

    PathBuilder builder = {0};
    size_t point_index = 0;
    size_t useful_segments = outline->n_segments - 1;
    ASS_Vector first = outline->points[0];
    if (!add_point(&builder, (ASS_DVector) {
            first.x / 64.0 * scale_x, first.y / 64.0 * scale_y}))
        goto fail;

    for (size_t i = 0; i < useful_segments; i++) {
        int order = outline->segments[i] & OUTLINE_COUNT_MASK;
        if (point_index + (size_t) order >= outline->n_points)
            goto fail;

        ASS_DVector p[4];
        for (int j = 0; j <= order; j++) {
            ASS_Vector source = outline->points[point_index + j];
            p[j].x = source.x / 64.0 * scale_x;
            p[j].y = source.y / 64.0 * scale_y;
        }

        bool ok = order == OUTLINE_LINE_SEGMENT ?
            add_point(&builder, p[1]) :
            flatten_cubic(&builder, p[0], p[1], p[2], p[3], 0);
        if (!ok)
            goto fail;
        point_index += order;
    }

    if (builder.n_points < 2 ||
            builder.n_points > SIZE_MAX / sizeof(*path->distance))
        goto fail;
    path->distance = malloc(builder.n_points * sizeof(*path->distance));
    if (!path->distance)
        goto fail;

    path->distance[0] = 0;
    for (size_t i = 1; i < builder.n_points; i++) {
        double dx = builder.points[i].x - builder.points[i - 1].x;
        double dy = builder.points[i].y - builder.points[i - 1].y;
        double length = hypot(dx, dy);
        if (!isfinite(length))
            goto fail;
        path->distance[i] = path->distance[i - 1] + length;
    }
    if (!(path->distance[builder.n_points - 1] > 0) ||
            !isfinite(path->distance[builder.n_points - 1]))
        goto fail;

    path->points = builder.points;
    path->n_points = builder.n_points;
    path->length = path->distance[path->n_points - 1];
    return true;

fail:
    free(builder.points);
    ass_curved_path_free(path);
    return false;
}

void ass_curved_path_free(ASS_CurvedPath *path)
{
    if (!path)
        return;
    free(path->points);
    free(path->distance);
    memset(path, 0, sizeof(*path));
}

bool ass_curved_path_sample(const ASS_CurvedPath *path, double distance,
                            ASS_DVector *point, ASS_DVector *tangent)
{
    if (!path || path->n_points < 2 || !path->points || !path->distance ||
            !isfinite(distance) || !point || !tangent)
        return false;

    size_t hi;
    if (distance <= 0) {
        hi = 1;
    } else if (distance >= path->length) {
        hi = path->n_points - 1;
    } else {
        size_t lo = 1;
        hi = path->n_points - 1;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (path->distance[mid] < distance)
                lo = mid + 1;
            else
                hi = mid;
        }
    }

    size_t lo = hi - 1;
    double dx = path->points[hi].x - path->points[lo].x;
    double dy = path->points[hi].y - path->points[lo].y;
    double segment = path->distance[hi] - path->distance[lo];
    if (!(segment > 0) || !isfinite(segment))
        return false;

    tangent->x = dx / segment;
    tangent->y = dy / segment;
    double ratio = (distance - path->distance[lo]) / segment;
    point->x = path->points[lo].x + dx * ratio;
    point->y = path->points[lo].y + dy * ratio;
    return isfinite(point->x) && isfinite(point->y) &&
           isfinite(tangent->x) && isfinite(tangent->y);
}
