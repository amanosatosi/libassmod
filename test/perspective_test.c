/* SPDX-License-Identifier: ISC */

#include "ass_perspective.h"
#include "ass_rasterizer.h"

static int failures;

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "perspective: line %d: %s\n", __LINE__, #condition); \
    failures++; } } while (0)

static ASS_DVector project(const double m[3][3], double x, double y)
{
    double w = m[2][0] * x + m[2][1] * y + m[2][2];
    return (ASS_DVector) {(m[0][0] * x + m[0][1] * y + m[0][2]) / w,
                         (m[1][0] * x + m[1][1] * y + m[1][2]) / w};
}

static bool near(ASS_DVector a, ASS_DVector b)
{
    return fabs(a.x - b.x) < 1e-8 && fabs(a.y - b.y) < 1e-8;
}

static const ASS_DVector relative_pins[] = {{0, 0}, {1, 0}, {0.8, 1}, {0.2, 1}};
static const ASS_DVector unit_corners[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

static ASS_DVector in_rect(ASS_DRect rect, ASS_DVector p)
{
    return (ASS_DVector) {rect.x_min + p.x * (rect.x_max - rect.x_min),
                         rect.y_min + p.y * (rect.y_max - rect.y_min)};
}

static void check_corners(const double m[3][3], ASS_DRect rect,
                          const ASS_DVector corners[4])
{
    for (int i = 0; i < 4; i++) {
        ASS_DVector source = in_rect(rect, unit_corners[i]);
        CHECK(near(project(m, source.x, source.y), corners[i]));
    }
}

static void test_solve(void)
{
    const ASS_DRect layouts[] = {{128, -640, 5248, 1920}, {-256, -832, 7424, 3008}};
    const ASS_DVector fixed[] = {{150, 100}, {620, 50}, {550, 310}, {210, 220}};
    for (int i = 0; i < 2; i++) {
        ASS_DRect rect = layouts[i];
        ASS_DVector corners[4];
        for (int j = 0; j < 4; j++)
            corners[j] = in_rect(rect, relative_pins[j]);

        double m[3][3] = {0};
        if (ass_perspective_solve(m, rect, corners)) {
            check_corners(m, rect, corners);
            ASS_DVector center = in_rect(rect, (ASS_DVector) {0.5, 0.5});
            // The image lies on both destination diagonals. A bilinear warp
            // instead puts this point at normalized (0.5, 0.5).
            ASS_DVector expected = in_rect(rect, (ASS_DVector) {0.5, 0.625});
            CHECK(near(project(m, center.x, center.y), expected));
        } else {
            CHECK(false);
        }

        if (ass_perspective_solve(m, rect, fixed)) {
            check_corners(m, rect, fixed);
            CHECK(m[2][0] != 0 && m[2][1] != 0);
        } else {
            CHECK(false);
        }
    }

    // Distinct invalid cases include an animation passing through zero area.
    const ASS_DVector invalid[][4] = {
        {{0, 0}, {1, 1}, {0, 1}, {1, 0}},
        {{0, 0}, {1, 0}, {0.4, 0.2}, {0, 1}},
        {{0, 0}, {1, 0}, {1, 0}, {0, 1}},
        {{0, 0}, {1, 0}, {1, 1e-12}, {0, 1e-12}},
        {{0, 0}, {1, 0}, {NAN, 1}, {0, 1}},
        {{0.5, 0}, {0.5, 0}, {0.5, 1}, {0.5, 1}},
    };
    double m[3][3];
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++)
        CHECK(!ass_perspective_solve(m, layouts[0], invalid[i]));
    CHECK(!ass_perspective_solve(m, (ASS_DRect) {0, 0, 0, 1}, fixed));
    CHECK(!ass_perspective_solve(m, (ASS_DRect) {0, 0, 1, INFINITY}, fixed));
}

static ASS_DVector sample_curve(const ASS_Vector *points, int order, double t)
{
    // Independent Bernstein evaluation in the source plane.
    double s = 1 - t;
    double weights[4];
    if (order == 2) {
        weights[0] = s * s;
        weights[1] = 2 * s * t;
        weights[2] = t * t;
    } else {
        weights[0] = s * s * s;
        weights[1] = 3 * s * s * t;
        weights[2] = 3 * s * t * t;
        weights[3] = t * t * t;
    }
    ASS_DVector point = {0};
    for (int i = 0; i <= order; i++) {
        point.x += points[i].x * weights[i];
        point.y += points[i].y * weights[i];
    }
    return point;
}

static double distance_to_edge(ASS_DVector p, ASS_Vector a, ASS_Vector b)
{
    double dx = b.x - a.x, dy = b.y - a.y;
    double length = hypot(dx, dy);
    if (length == 0)
        return hypot(p.x - a.x, p.y - a.y);
    double along = ((p.x - a.x) * dx + (p.y - a.y) * dy) / length;
    if (along <= 0)
        return hypot(p.x - a.x, p.y - a.y);
    if (along >= length)
        return hypot(p.x - b.x, p.y - b.y);
    return fabs((p.x - a.x) * dy - (p.y - a.y) * dx) / length;
}

static void test_curve(const double m[3][3], int order, ASS_Vector points[4])
{
    char segments[] = {order, OUTLINE_LINE_SEGMENT | OUTLINE_CONTOUR_END};
    ASS_Outline source = {.points = points, .segments = segments,
                          .n_points = order + 1, .n_segments = 2};
    ASS_Outline projected;
    const double tolerance = 4;
    if (!ass_perspective_outline(&projected, &source, m, tolerance, 1024, 4, 65536)) {
        CHECK(false);
        return;
    }
    CHECK(projected.n_points > source.n_points);
    CHECK(projected.n_points == projected.n_segments);
    for (size_t i = 0; i + 1 < projected.n_segments; i++)
        CHECK(projected.segments[i] == OUTLINE_LINE_SEGMENT);
    CHECK(projected.segments[projected.n_segments - 1] ==
          (OUTLINE_LINE_SEGMENT | OUTLINE_CONTOUR_END));

    for (int i = 0; i <= 512; i++) {
        ASS_DVector p = sample_curve(points, order, i / 512.0);
        p = project(m, p.x, p.y);
        double error = INFINITY;
        // Exclude the closing edge so it cannot hide a missing curved edge.
        for (size_t j = 0; j + 1 < projected.n_points; j++)
            error = fmin(error, distance_to_edge(p, projected.points[j],
                                                 projected.points[j + 1]));
        CHECK(error <= tolerance + 0.71); // includes 1/64 pixel point rounding
    }
    ass_outline_free(&projected);

    CHECK(!ass_perspective_outline(&projected, &source, m, tolerance, 2, 4, 65536));
    CHECK(!projected.n_points && !projected.points && !projected.segments);
    ass_outline_free(&projected);
}

static void test_affine_and_horizon(void)
{
    ASS_Vector points[] = {{64, -128}, {128, 192}, {256, 64}};
    char segments[] = {OUTLINE_QUADRATIC_SPLINE, OUTLINE_LINE_SEGMENT | OUTLINE_CONTOUR_END};
    ASS_Outline source = {.points = points, .segments = segments, .n_points = 3, .n_segments = 2};
    const ASS_DRect rect = {0, 0, 512, 512};
    const ASS_DVector identity[] = {{0, 0}, {512, 0}, {512, 512}, {0, 512}};
    const ASS_DVector mirror[] = {{512, 0}, {0, 0}, {0, 512}, {512, 512}};
    const ASS_DVector *quads[] = {identity, mirror};
    for (int i = 0; i < 2; i++) {
        double m[3][3];
        if (!ass_perspective_solve(m, rect, quads[i])) {
            CHECK(false);
            continue;
        }
        CHECK(m[2][0] == 0 && m[2][1] == 0);
        ASS_Outline result;
        if (!ass_perspective_outline(&result, &source, m, 4, 2, 4, 1024)) {
            CHECK(false);
            continue;
        }
        CHECK(result.n_points == source.n_points && result.n_segments == source.n_segments);
        CHECK(!memcmp(result.segments, segments, sizeof(segments)));
        for (size_t j = 0; j < result.n_points; j++) {
            CHECK(result.points[j].x == (i ? 512 - points[j].x : points[j].x));
            CHECK(result.points[j].y == points[j].y);
        }
        ass_outline_free(&result);
    }

    // Both endpoints are in front of the horizon but their control is behind it.
    const double horizon[3][3] = {{1, 0, 0}, {0, 1, 0}, {-1.0 / 100, 0, 2}};
    points[1].x = 224;
    points[2].x = 128;
    const double overflow[3][3] = {{OUTLINE_MAX, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const double (*rejected[])[3] = {horizon, overflow};
    for (int i = 0; i < 2; i++) {
        ASS_Outline result;
        CHECK(!ass_perspective_outline(&result, &source, rejected[i], 4, 64, 4, 4096));
        CHECK(!result.points && !result.n_segments);
        ass_outline_free(&result);
    }
}

static void test_raster(const double m[3][3])
{
    ASS_Vector points[] = {{0, 0}, {4096, 0}, {4096, 4096}, {0, 4096},
                          {1536, 1536}, {1536, 2560}, {2560, 2560}, {2560, 1536}};
    char segments[] = {1, 1, 1, 1 | OUTLINE_CONTOUR_END, 1, 1, 1, 1 | OUTLINE_CONTOUR_END};
    ASS_Outline source = {.points = points, .segments = segments, .n_points = 8, .n_segments = 8};
    ASS_Outline result;
    CHECK(!ass_perspective_outline(&result, &source, m, 4, 8, 4, 80 * 80 - 1));
    CHECK(!result.points && !result.n_segments);
    ass_outline_free(&result);
    if (!ass_perspective_outline(&result, &source, m, 4, 8, 4, 80 * 80)) {
        CHECK(false);
        return;
    }

    CHECK(result.n_points == 8 && result.n_segments == 8);
    CHECK(!memcmp(result.segments, segments, sizeof(segments)));
    BitmapEngine engine = ass_bitmap_engine_init(0);
    RasterizerData rasterizer;
    if (ass_rasterizer_init(&engine, &rasterizer, 4)) {
        Bitmap bitmap;
        if (ass_alloc_bitmap(&engine, &bitmap, 64, 64, true)) {
            if (ass_rasterizer_set_outline(&rasterizer, &result, false) &&
                    ass_rasterizer_fill(&engine, &rasterizer, bitmap.buffer,
                                        0, 0, 64, 64, bitmap.stride)) {
                CHECK(bitmap.buffer[12 * bitmap.stride + 32] == 255);
                CHECK(bitmap.buffer[40 * bitmap.stride + 32] == 0); // hole
                CHECK(bitmap.buffer[55 * bitmap.stride + 2] == 0);  // outside
            } else {
                CHECK(false);
            }
            ass_free_bitmap(&bitmap);
        } else {
            CHECK(false);
        }
        ass_rasterizer_done(&rasterizer);
    } else {
        CHECK(false);
    }
    ass_outline_free(&result);
}

int main(void)
{
    test_solve();
    test_affine_and_horizon();
    ASS_DRect rect = {0, 0, 4096, 4096};
    ASS_DVector corners[4];
    for (int i = 0; i < 4; i++)
        corners[i] = in_rect(rect, relative_pins[i]);
    double m[3][3];
    if (!ass_perspective_solve(m, rect, corners))
        return 1;

    ASS_Vector quadratic[] = {{256, 256}, {1600, 3600}, {3500, 300}, {0, 0}};
    ASS_Vector cubic[] = {{256, 256}, {3500, 4000}, {500, 4000}, {3500, 300}};
    ASS_Vector overshoot[] = {{256, 256}, {3800, 256}, {1900, 256}, {0, 0}};
    test_curve(m, 2, quadratic);
    test_curve(m, 3, cubic);
    test_curve(m, 2, overshoot);
    test_raster(m);
    return failures != 0;
}
