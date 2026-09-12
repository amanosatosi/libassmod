/*
 * Distortion geometry and legacy arithmetic regression tests.
 */

#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <string.h>
#include "../libass/ass_distort.h"

static void test_identity(void)
{
    ASS_DistortParams p = {.u1 = 1, .u2 = 1, .v2 = 1, .v3 = 1};
    ASS_DVector out = ass_distort_map_point(&p, 0, 0, 10, 10, 10, 10);
    assert(fabs(out.x - 10.0) < 1e-6);
    assert(fabs(out.y - 10.0) < 1e-6);
}

static void test_stretch_right(void)
{
    ASS_DistortParams p = {.u1 = 1, .u2 = 2, .v2 = 1, .v3 = 1};
    ASS_DVector out = ass_distort_map_point(&p, 0, 0, 10, 10, 10, 10);
    assert(out.x > 10);
    assert(fabs(out.x - 20.0) < 1e-6);
    assert(fabs(out.y - 10.0) < 1e-6);
    ASS_DVector top = ass_distort_map_point(&p, 0, 0, 10, 10, 10, 0);
    assert(fabs(top.x - 10.0) < 1e-6);
    assert(fabs(top.y) < 1e-6);
}

/* Frozen pre-P0 formula. Compare bits, not an epsilon: rearranging the
 * original six-coordinate arithmetic can change outline rounding. */
static ASS_DVector legacy_map(const ASS_DistortParams *p,
                             double min_x, double min_y,
                             double max_x, double max_y, double x, double y)
{
    double w = FFMAX(max_x - min_x, 0);
    double h = FFMAX(max_y - min_y, 0);
    if (w == 0.0 || h == 0.0 || !p)
        return (ASS_DVector) {x, y};
    double u = (x - min_x) / w;
    double v = (y - min_y) / h;
    double dx = u * p->u1 + v * p->u3 + u * v * (p->u2 - p->u1 - p->u3);
    double dy = u * p->v1 + v * p->v3 + u * v * (p->v2 - p->v1 - p->v3);
    return (ASS_DVector) {min_x + dx * w, min_y + dy * h};
}

static void test_legacy_exact(void)
{
    const ASS_DistortParams pins[] = {
        {.u1 = 1, .u2 = 1, .v2 = 1, .v3 = 1},
        {.u1 = 1.6, .v1 = -0.2, .u2 = 1.6, .v2 = 1.2, .v3 = 1},
        {.u1 = -0.3, .u2 = 1, .v2 = 1.3, .v3 = 1},
        {.u1 = 0.123456789, .v1 = -2.7, .u2 = 3.1,
         .v2 = 0.999999999, .u3 = -1.2, .v3 = 0.4},
        {.u0 = -0.0, .v0 = -0.0, .u1 = 1, .u2 = 1, .v2 = 1, .v3 = 1},
    };
    const double boxes[][4] = {
        {0, 0, 10, 10}, {-123.25, 17.5, 387.125, 701.75},
        {0, 0, 0, 10}, {0, 0, 10, 0}, {10, 10, 0, 0},
    };
    const double positions[] = {-0.2, 0, 0.123456789, 0.5, 1, 1.3};
    for (size_t p = 0; p < sizeof(pins) / sizeof(pins[0]); p++)
        for (size_t b = 0; b < sizeof(boxes) / sizeof(boxes[0]); b++)
            for (size_t i = 0; i < sizeof(positions) / sizeof(positions[0]); i++)
                for (size_t j = 0; j < sizeof(positions) / sizeof(positions[0]); j++) {
                    const double *r = boxes[b];
                    double x = r[0] + positions[i] * (r[2] - r[0]);
                    double y = r[1] + positions[j] * (r[3] - r[1]);
                    ASS_DVector expected = legacy_map(&pins[p], r[0], r[1], r[2], r[3], x, y);
                    ASS_DVector actual = ass_distort_map_point(&pins[p], r[0], r[1], r[2], r[3], x, y);
                    assert(!memcmp(&actual.x, &expected.x, sizeof(double)));
                    assert(!memcmp(&actual.y, &expected.y, sizeof(double)));
                }
}

static void test_four_corners(void)
{
    const ASS_DVector p0[] = {{0.2, 0.1}, {0.25, 0}, {0, 0.25}, {-0.25, -0.125}};
    for (size_t i = 0; i < sizeof(p0) / sizeof(p0[0]); i++) {
        ASS_DistortParams p = {
            .u0 = p0[i].x, .v0 = p0[i].y,
            .u1 = 1, .u2 = 1, .v2 = 1, .v3 = 1,
        };
        const ASS_DVector corners[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (int c = 0; c < 4; c++) {
            ASS_DVector out = ass_distort_map_point(&p, -10, 20, 90, 220,
                -10 + corners[c].x * 100, 20 + corners[c].y * 200);
            ASS_DVector pin = c == 0 ? p0[i] : corners[c];
            assert(fabs(out.x - (-10 + pin.x * 100)) < 1e-10);
            assert(fabs(out.y - (20 + pin.y * 200)) < 1e-10);
        }
        // An interior point receives one quarter of each corner's weight.
        ASS_DVector out = ass_distort_map_point(&p, 0, 0, 100, 200, 50, 100);
        assert(fabs(out.x - (50 + 25 * p.u0)) < 1e-10);
        assert(fabs(out.y - (100 + 50 * p.v0)) < 1e-10);
    }
}

int main(void)
{
    test_identity();
    test_stretch_right();
    test_legacy_exact();
    test_four_corners();
    return 0;
}
