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

/* Sidecars use the same cluster measurement/orientation without wrapping or
 * event column alignment. These views never take ownership of glyph arrays. */
void ass_vertical_prepare_text(RenderContext *state, TextInfo *text);
void ass_vertical_mark_text_syllables(TextInfo *text);
bool ass_vertical_layout_reading(RenderContext *state, FuriGroup *group);
