#ifndef LIBASS_VERTICAL_H
#define LIBASS_VERTICAL_H

#include <stdbool.h>
#include <stdint.h>
#include "ass_render.h"
#include "ass_myanmar.h"

/* Prepare per-codepoint orientation and one event-wide column direction.
 * Layout groups shaped clusters into script-specific vertical units. */
void ass_vertical_prepare(RenderContext *state);
void ass_vertical_mark_syllables(RenderContext *state);
bool ass_vertical_layout(RenderContext *state, double max_height);

#endif
