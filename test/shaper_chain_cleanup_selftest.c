#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "ass_curved_text.h"
#include "ass_drawing.h"
#include "ass_render.h"
#include "ass_shaper.h"

int main(void)
{
    ASS_Outline outline = {0};
    ASS_Rect cbox;
    if (!ass_drawing_parse(&outline, &cbox,
            "m 0 0 b 25 -80 75 -80 100 0", NULL)) {
        fprintf(stderr, "could not parse curved-text path with drawing parser\n");
        return 1;
    }
    ASS_CurvedPath path;
    if (!ass_curved_path_flatten(&path, &outline, 1.0, 1.0) ||
            path.n_points < 3 || path.length <= 100.0) {
        fprintf(stderr, "cubic arc-length path flattening failed\n");
        ass_outline_free(&outline);
        return 1;
    }
    ASS_DVector point, tangent;
    if (!ass_curved_path_sample(&path, path.length * 0.5,
                                &point, &tangent) ||
            fabs(point.x - 50.0) > 0.5 || point.y >= -50.0 ||
            fabs(hypot(tangent.x, tangent.y) - 1.0) > 1e-9) {
        fprintf(stderr, "arc-length midpoint/tangent lookup failed\n");
        ass_curved_path_free(&path);
        ass_outline_free(&outline);
        return 1;
    }
    ass_curved_path_free(&path);
    ass_outline_free(&outline);

    GlyphInfo horizontal = {
        .pos = {640, 1280},
        .offset = {64, 128},
        .advance = {192, 64},
    };
    if (!ass_curved_text_transform_cluster(&horizontal,
            (ASS_DVector) {100, 200}, (ASS_DVector) {1, 0}) ||
            horizontal.pos.x != 6464 || horizontal.pos.y != 12928 ||
            horizontal.advance.x != 192 || horizontal.advance.y != 64 ||
            fabs(horizontal.curved_angle) > 1e-9) {
        fprintf(stderr, "horizontal curved path changed glyph orientation\n");
        return 1;
    }

    GlyphInfo root = {0};
    root.next = calloc(1, sizeof(*root.next));
    if (!root.next) {
        fprintf(stderr, "could not allocate shaped glyph chain\n");
        return 1;
    }
    root.next->next = calloc(1, sizeof(*root.next->next));
    if (!root.next->next) {
        free(root.next);
        return 1;
    }
    root.next->next->next = calloc(1, sizeof(*root.next->next->next));
    if (!root.next->next->next) {
        free(root.next->next);
        free(root.next);
        return 1;
    }

    /* Synthetic four-glyph Myanmar shaping cluster.  The exact glyphs are
     * font-dependent; the invariant is not: base, medial, vowel and tone
     * positions must all pass through one rigid cluster frame. */
    root.pos = (ASS_Vector) {640, 1280};          // (10, 20)
    root.next->pos = (ASS_Vector) {832, 1024};    // (13, 16)
    root.next->next->pos = (ASS_Vector) {512, 960}; // (8, 15)
    root.next->next->next->pos = (ASS_Vector) {704, 896}; // (11, 14)
    if (!ass_curved_text_transform_cluster(&root,
            (ASS_DVector) {100, 200}, (ASS_DVector) {0, 1})) {
        fprintf(stderr, "curved cluster transform rejected finite positions\n");
        return 1;
    }

    const ASS_Vector expected[] = {
        {6400, 12800}, {6656, 12992}, {6720, 12672}, {6784, 12864},
    };
    GlyphInfo *glyph = &root;
    for (int i = 0; i < 4; i++, glyph = glyph->next) {
        if (!glyph || glyph->pos.x != expected[i].x ||
                glyph->pos.y != expected[i].y ||
                fabs(glyph->curved_angle + 90.0) > 1e-9) {
            fprintf(stderr, "curved layout detached a shaped cluster member\n");
            return 1;
        }
    }

    TextInfo text_info = {
        .glyphs = &root,
        .length = 1,
    };
    ass_shaper_cleanup(NULL, &text_info);
    if (root.next) {
        fprintf(stderr, "shaped glyph chain was not detached before cleanup\n");
        return 1;
    }
    return 0;
}
