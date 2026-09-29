#ifndef LIBASS_VERTICAL_H
#define LIBASS_VERTICAL_H

#include <stdbool.h>
#include <stdint.h>
#include "ass_render.h"
#include "ass_myanmar.h"

/* A layout syllable boundary, independent of HarfBuzz's shaping syllables.
 * The caller still has to extend a boundary over a shaped glyph cluster. */
void ass_vertical_prepare(RenderContext *state);
void ass_vertical_mark_syllables(RenderContext *state);
bool ass_vertical_layout(RenderContext *state, double max_height);

#endif
