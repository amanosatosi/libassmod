/* True projective homography geometry regressions. */

#undef NDEBUG
#include <assert.h>
#include <math.h>

#include "../libass/ass_perspective.h"

static ASS_DVector map(const ASS_Homography *h, double x, double y)
{
    ASS_DVector result;
    assert(ass_perspective_map_point(h, x, y, &result));
    return result;
}

static void near_point(ASS_DVector actual, ASS_DVector expected)
{
    assert(fabs(actual.x - expected.x) < 1e-9);
    assert(fabs(actual.y - expected.y) < 1e-9);
}

static void test_identity(void)
{
    ASS_PerspectiveParams p = {.corner = {
        {10, 20}, {110, 20}, {110, 220}, {10, 220},
    }};
    ASS_Homography h;
    assert(ass_perspective_solve(&p, 10, 20, 110, 220, &h));
    near_point(map(&h, 37.5, 81.25), (ASS_DVector) {37.5, 81.25});
}

static void test_corners_and_straight_lines(void)
{
    ASS_PerspectiveParams p = {.corner = {
        {20, 10}, {80, 20}, {120, 110}, {-30, 90},
    }};
    ASS_Homography h;
    assert(ass_perspective_solve(&p, 0, 0, 100, 100, &h));
    const ASS_DVector source[] = {{0, 0}, {100, 0}, {100, 100}, {0, 100}};
    for (int i = 0; i < 4; i++)
        near_point(map(&h, source[i].x, source[i].y), p.corner[i]);

    ASS_DVector a = map(&h, 0, 37);
    ASS_DVector b = map(&h, 43, 37);
    ASS_DVector c = map(&h, 100, 37);
    double cross = (b.x - a.x) * (c.y - a.y) -
                   (b.y - a.y) * (c.x - a.x);
    assert(fabs(cross) < 1e-8);
}

static void test_projective_foreshortening(void)
{
    ASS_PerspectiveParams p = {.corner = {
        {35, 0}, {65, 0}, {100, 100}, {0, 100},
    }};
    ASS_Homography h;
    assert(ass_perspective_solve(&p, 0, 0, 100, 100, &h));

    ASS_DVector top_left = map(&h, 25, 25);
    ASS_DVector top_right = map(&h, 75, 25);
    ASS_DVector bottom_left = map(&h, 25, 75);
    ASS_DVector bottom_right = map(&h, 75, 75);
    double upper_width = hypot(top_right.x - top_left.x,
                               top_right.y - top_left.y);
    double lower_width = hypot(bottom_right.x - bottom_left.x,
                               bottom_right.y - bottom_left.y);
    assert(lower_width > upper_width);

    /* Both projected source edges are exact lines and converge at one
     * vanishing point rather than following independent bilinear curves. */
    ASS_DVector l0 = map(&h, 0, 20), l1 = map(&h, 0, 80);
    ASS_DVector r0 = map(&h, 100, 20), r1 = map(&h, 100, 80);
    double ldx = l1.x - l0.x, ldy = l1.y - l0.y;
    double rdx = r1.x - r0.x, rdy = r1.y - r0.y;
    double denominator = ldx * rdy - ldy * rdx;
    assert(fabs(denominator) > 1e-9);
    double t = ((r0.x - l0.x) * rdy - (r0.y - l0.y) * rdx) /
               denominator;
    ASS_DVector vanishing = {l0.x + t * ldx, l0.y + t * ldy};
    double right_cross = (vanishing.x - r0.x) * rdy -
                         (vanishing.y - r0.y) * rdx;
    assert(fabs(right_cross) < 1e-8);
}

static void test_extreme_and_invalid(void)
{
    ASS_Homography h;
    ASS_PerspectiveParams extreme = {.corner = {
        {49.999, 0}, {50.001, 0}, {1000, 100}, {-900, 100},
    }};
    assert(ass_perspective_solve(&extreme, 0, 0, 100, 100, &h));
    ASS_DVector center = map(&h, 50, 50);
    assert(isfinite(center.x) && isfinite(center.y));

    ASS_PerspectiveParams collinear = {.corner = {
        {0, 0}, {10, 0}, {20, 0}, {30, 0},
    }};
    assert(!ass_perspective_solve(&collinear, 0, 0, 100, 100, &h));
    ASS_PerspectiveParams flat_parallelogram = {.corner = {
        {0, 0}, {10, 0}, {20, 0}, {10, 0},
    }};
    assert(!ass_perspective_solve(&flat_parallelogram, 0, 0, 100, 100, &h));
    ASS_PerspectiveParams bow_tie = {.corner = {
        {0, 0}, {100, 100}, {100, 0}, {0, 100},
    }};
    assert(!ass_perspective_solve(&bow_tie, 0, 0, 100, 100, &h));
    assert(!ass_perspective_solve(&extreme, 0, 0, 0, 100, &h));
    extreme.corner[2].x = NAN;
    assert(!ass_perspective_solve(&extreme, 0, 0, 100, 100, &h));
}

int main(void)
{
    test_identity();
    test_corners_and_straight_lines();
    test_projective_foreshortening();
    test_extreme_and_invalid();
    return 0;
}
