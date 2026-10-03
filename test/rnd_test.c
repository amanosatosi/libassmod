/* Fixed Microsoft CRT and VSFilterMod SSE2-path compatibility vectors. */
#include "ass_rnd.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "rnd math: line %d: %s\n", __LINE__, #condition); \
    failures++; } } while (0)

int main(void)
{
    static const uint32_t zero[] = {
        38,7719,21238,2437,8855,11797,8365,32285,
        10450,30612,5853,28100,1142,281,20537,15921,
        8945,26285,2997,14680,20976,31891,21655,25906
    };
    static const uint32_t one[] = {41,18467,6334,26500,19169,15724,11478,29358};
    static const uint32_t wrap[] = {35,29739,3374,11141,31308,7870,5253,2445};
    static const uint32_t hex[] = {13289,23359,19469,24737,23446,14229,6193,18180};
    uint32_t s = 0;
    for (unsigned i = 0; i < sizeof(zero) / sizeof(zero[0]); i++)
        CHECK(ass_rnd_next(&s) == zero[i]);
    CHECK(s == UINT32_C(3845303128));
    const uint32_t seeds[] = {1, UINT32_MAX, UINT32_C(0x12345678)};
    const uint32_t *vectors[] = {one, wrap, hex};
    const uint32_t final[] = {1924036713,2307720487,3338983488};
    for (int k = 0; k < 3; k++) {
        s = seeds[k];
        for (int i = 0; i < 8; i++)
            CHECK(ass_rnd_next(&s) == vectors[k][i]);
        CHECK(s == final[k]);
    }

    float offsets[4][3];
    const int32_t xyz[3] = {8,8,8};
    const int expected[4][3] = {{607,-250,-83},{440,535,-44},{-36,-50,210},{762,-515,375}};
    s = 0;
    ass_rnd_group(&s, xyz, offsets);
    for (int p = 0; p < 4; p++)
        for (int a = 0; a < 3; a++)
            CHECK(fabs(offsets[p][a] - expected[p][a] / 100.0) < 0.00001);
    CHECK(ass_rnd_next(&s) == 1142);

    const int32_t x_only[3] = {8,-8,0};
    const int x_expected[] = {-36,375,-515,762};
    s = 0;
    ass_rnd_group(&s, x_only, offsets);
    for (int p = 0; p < 4; p++) {
        CHECK(fabs(offsets[p][0] - x_expected[p] / 100.0) < 0.00001);
        CHECK(offsets[p][1] == 0 && offsets[p][2] == 0);
    }
    /* A five-point path takes its final point from lane 3 of group two;
     * unused lanes still consume values. The next value is the ninth. */
    ass_rnd_group(&s, x_only, offsets);
    CHECK(fabs(offsets[0][0] - 5.35) < 0.00001);
    CHECK(ass_rnd_next(&s) == 10450);
    const int32_t disabled[3] = {-1,INT32_MIN,0};
    s = 123;
    ass_rnd_group(&s, disabled, offsets);
    CHECK(s == 123);

    const double amplitudes[] = {20,20.375,20.5,21,30,100};
    const int large[6][4] = {
        {13563,-5238,8281,15962}, {13863,-4938,8581,16262},
        {13963,-4838,8681,16362}, {14363,-4438,9081,16762},
        {21563,2762,16281,23962}, {77563,58762,72281,79962}
    };
    for (int k = 0; k < 6; k++) {
        int32_t amp[3] = {ass_rnd_truncate(amplitudes[k] * 8),0,0};
        s = 0;
        ass_rnd_group(&s, amp, offsets);
        for (int p = 0; p < 4; p++)
            CHECK(fabs(offsets[p][0] - large[k][p] / 100.0) < 0.0001);
        if (k >= 4)
            for (int p = 0; p < 4; p++) CHECK(offsets[p][0] > 0);
    }
    const int32_t huge[3] = {INT32_MAX,INT32_MAX,INT32_MAX};
    ass_rnd_group(&s, huge, offsets); // UBSan: wide amplitude arithmetic
    for (int p = 0; p < 4; p++)
        for (int a = 0; a < 3; a++) CHECK(isfinite(offsets[p][a]));
    CHECK(ass_rnd_truncate(NAN) == 0 && ass_rnd_truncate(INFINITY) == 0);
    CHECK(ass_rnd_truncate(1e100) == INT32_MAX);
    CHECK(ass_rnd_truncate(-1e100) == INT32_MIN);
    CHECK(ass_rnd_truncate(0.124 * 8) == 0);
    CHECK(ass_rnd_truncate(0.125 * 8) == 1);
    CHECK(ass_rnd_truncate(-0.126 * 8) == -1);

    /* XY noise follows font scaling, shear, then a 90 degree rotation. */
    const double affine[3][4] = {{0.75,3,100,0},{-2,-1,60,0},{0,0,1,0}};
    double base[2], deformed[2];
    CHECK(ass_rnd_project(affine, 12,40,0,1,base));
    CHECK(ass_rnd_project(affine, 12+7.62,40-5.15,0,1,deformed));
    CHECK(fabs((deformed[0]-base[0]) - (0.75*7.62-3*5.15)) < 1e-10);
    CHECK(fabs((deformed[1]-base[1]) - (-2*7.62+5.15)) < 1e-10);
    /* Local Z changes depth even without X/Y rotation. */
    const double depth[3][4] = {
        {20000,0,100*20000,100}, {0,20000,60*20000,60}, {0,0,20000,1}
    };
    CHECK(ass_rnd_project(depth,-1000,-500,1000,1000,deformed));
    CHECK(fabs(deformed[0] - (100-1000*20000.0/21000)) < 1e-10);
    CHECK(fabs(deformed[1] - (60-500*20000.0/21000)) < 1e-10);
    /* Orthographic rotated depth contributes to XY, not the denominator. */
    const double ortho[3][4] = {{1,0,100,-0.5},{0,1,60,0.75},{0,0,1,0}};
    CHECK(ass_rnd_project(ortho,12,40,1000,1,deformed));
    CHECK(deformed[0] == -388 && deformed[1] == 850);
    return failures ? 1 : 0;
}
