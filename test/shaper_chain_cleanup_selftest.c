#include <stdio.h>
#include <stdlib.h>

#include "ass_render.h"
#include "ass_shaper.h"

int main(void)
{
    GlyphInfo root = {0};
    root.next = calloc(1, sizeof(*root.next));
    if (!root.next) {
        fprintf(stderr, "could not allocate shaped glyph chain\n");
        return 1;
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
