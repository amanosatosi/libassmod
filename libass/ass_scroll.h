#ifndef LIBASS_SCROLL_H
#define LIBASS_SCROLL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t start, duration, lines;
    double distance;
} ASS_ScrollCue;

/* Renderer-owned, shared by active contexts. Geometry is invalidated by an
 * exact comparison of the measured row advances, including animated metrics. */
typedef struct ass_scroll_definition {
    struct ass_scroll_definition *next;
    char *source;
    size_t source_len, count, users;
    int32_t default_duration;
    ASS_ScrollCue *cues;
    double *advances;
    size_t rows;
} ASS_ScrollDefinition;

typedef struct {
    ASS_ScrollDefinition *definition;
    double *advances;
    size_t rows, capacity;
    int last_line, show_lines;
    double top, bottom, displacement;
} ASS_ScrollContext;

bool ass_scroll_integer(const char *start, const char *end, int32_t *value);
ASS_ScrollDefinition *ass_scroll_parse(const char *start, const char *end,
                                       int32_t duration);
void ass_scroll_free(ASS_ScrollDefinition *definition);
bool ass_scroll_map(ASS_ScrollDefinition *definition,
                    const double *advances, size_t rows);
double ass_scroll_evaluate(const ASS_ScrollDefinition *definition, int64_t now);

#endif
