/*
 * Minimal distortion unit test.
 */

#include <assert.h>
#include <math.h>
#include "../libass/ass_distort.h"

static void test_identity(void)
{
    ASS_DistortParams p = {1, 0, 1, 1, 0, 1};
    ASS_DVector out = ass_distort_map_point(&p, 0, 0, 10, 10, 10, 10);
    assert(fabs(out.x - 10.0) < 1e-6);
    assert(fabs(out.y - 10.0) < 1e-6);
}

static void test_stretch_right(void)
{
    ASS_DistortParams p = {1, 0, 2, 1, 0, 1};
    ASS_DVector out = ass_distort_map_point(&p, 0, 0, 10, 10, 10, 10);
    assert(out.x > 10);
    assert(fabs(out.x - 20.0) < 1e-6);
    assert(fabs(out.y - 10.0) < 1e-6);
    ASS_DVector top = ass_distort_map_point(&p, 0, 0, 10, 10, 10, 0);
    assert(fabs(top.x - 10.0) < 1e-6);
    assert(fabs(top.y) < 1e-6);
}

int main(void)
{
    test_identity();
    test_stretch_right();
    return 0;
}
