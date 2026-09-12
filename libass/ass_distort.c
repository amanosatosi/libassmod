/*
 * Bilinear distortion helper.
 */

#include "ass_distort.h"
#include "ass_utils.h"

ASS_DVector ass_distort_map_point(const ASS_DistortParams *p,
                                  double min_x, double min_y,
                                  double max_x, double max_y,
                                  double x, double y)
{
    double w = FFMAX(max_x - min_x, 0);
    double h = FFMAX(max_y - min_y, 0);
    if (w == 0.0 || h == 0.0 || !p)
        return (ASS_DVector){x, y};

    double u = (x - min_x) / w;
    double v = (y - min_y) / h;

    double dx = u * p->u1 + v * p->u3 + u * v * (p->u2 - p->u1 - p->u3);
    double dy = u * p->v1 + v * p->v3 + u * v * (p->v2 - p->v1 - p->v3);

    // Keep the legacy arithmetic unchanged when P0 is (0,0), including
    // rounding and non-finite inputs. P0 supplies the fourth bilinear weight.
    if (p->u0 != 0.0 || p->v0 != 0.0) {
        double p0_weight = (1.0 - u) * (1.0 - v);
        dx += p0_weight * p->u0;
        dy += p0_weight * p->v0;
    }

    return (ASS_DVector){
        .x = min_x + dx * w,
        .y = min_y + dy * h,
    };
}
