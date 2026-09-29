#ifndef LIBASS_MYANMAR_H
#define LIBASS_MYANMAR_H

#include <stdbool.h>
#include <stdint.h>

/* True when a new linguistic/layout syllable may begin at index. */
bool ass_myanmar_layout_break(const uint32_t *text, int length, int index);

#endif
