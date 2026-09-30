#include "ass_scroll.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool ass_scroll_integer(const char *start, const char *end, int32_t *value)
{
    while (start < end && space(*start)) start++;
    while (end > start && space(end[-1])) end--;
    if (start == end)
        return false;
    int32_t n = 0;
    for (; start < end; start++) {
        unsigned digit = (unsigned char) *start - '0';
        if (digit > 9 || n > (INT32_MAX - (int32_t) digit) / 10)
            return false;
        n = n * 10 + digit;
    }
    *value = n;
    return true;
}

ASS_ScrollDefinition *ass_scroll_parse(const char *start, const char *end,
                                       int32_t duration)
{
    if (duration < 0 || start == end)
        return NULL;
    size_t fields = 1;
    for (const char *p = start; p < end; p++)
        fields += *p == ',';
    if (fields % 2 || fields / 2 > SIZE_MAX / sizeof(ASS_ScrollCue))
        return NULL;
    ASS_ScrollDefinition *def = calloc(1, sizeof(*def));
    if (!def)
        return NULL;
    def->count = fields / 2;
    def->cues = calloc(def->count, sizeof(*def->cues));
    def->source_len = end - start;
    def->source = malloc(def->source_len + 1);
    def->default_duration = duration;
    if (!def->cues || !def->source)
        goto invalid;
    memcpy(def->source, start, def->source_len);
    def->source[def->source_len] = 0;
    for (size_t i = 0; i < def->count; i++) {
        ASS_ScrollCue *cue = &def->cues[i];
        const char *comma = memchr(start, ',', end - start);
        if (!comma)
            goto invalid;
        const char *pipe = memchr(start, '|', comma - start);
        cue->duration = duration;
        if (!ass_scroll_integer(start, pipe ? pipe : comma, &cue->start) ||
                (pipe && !ass_scroll_integer(pipe + 1, comma, &cue->duration)))
            goto invalid;
        start = comma + 1;
        comma = memchr(start, ',', end - start);
        if (!ass_scroll_integer(start, comma ? comma : end, &cue->lines) ||
                cue->lines <= 0)
            goto invalid;
        start = comma ? comma + 1 : end;
    }
    return def;
invalid:
    ass_scroll_free(def);
    return NULL;
}

void ass_scroll_free(ASS_ScrollDefinition *def)
{
    if (!def) return;
    free(def->source);
    free(def->cues);
    free(def->advances);
    free(def);
}

bool ass_scroll_map(ASS_ScrollDefinition *def,
                    const double *advances, size_t rows)
{
    if (rows == def->rows && (!rows ||
            !memcmp(advances, def->advances, rows * sizeof(*advances))))
        return true;
    if (rows > SIZE_MAX / sizeof(*advances))
        return false;
    double *copy = rows ? malloc(rows * sizeof(*copy)) : NULL;
    if (rows && !copy)
        return false;
    for (size_t i = 0; i < rows; i++) {
        if (!isfinite(advances[i]) || advances[i] < 0) {
            free(copy);
            return false;
        }
        copy[i] = advances[i];
    }
    /* Consume each measured row once, in authored cue order. No sorting and
     * no glyph-by-cue loop, even when counts greatly exceed available rows. */
    size_t row = 0;
    for (size_t i = 0; i < def->count; i++) {
        size_t amount = (size_t) def->cues[i].lines;
        if (amount > rows - row) amount = rows - row;
        double distance = 0;
        for (size_t j = 0; j < amount; j++)
            distance += copy[row++];
        def->cues[i].distance = distance;
    }
    free(def->advances);
    def->advances = copy;
    def->rows = rows;
    return true;
}

double ass_scroll_evaluate(const ASS_ScrollDefinition *def, int64_t now)
{
    double displacement = 0;
    for (size_t i = 0; i < def->count; i++) {
        const ASS_ScrollCue *cue = &def->cues[i];
        if (now < cue->start)
            continue;
        double elapsed = (double) now - cue->start;
        double progress = !cue->duration || elapsed >= cue->duration ?
                          1.0 : elapsed / cue->duration;
        displacement += cue->distance * progress;
    }
    return displacement;
}
