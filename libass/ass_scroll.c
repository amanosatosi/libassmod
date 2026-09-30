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

static bool signed_lines(const char *start, const char *end, int32_t *value)
{
    while (start < end && space(*start)) start++;
    while (end > start && space(end[-1])) end--;
    bool negative = start < end && *start == '-';
    if (start < end && (*start == '+' || *start == '-')) start++;
    if (start == end)
        return false;
    uint32_t n = 0, limit = (uint32_t) INT32_MAX + negative;
    for (; start < end; start++) {
        unsigned digit = (unsigned char) *start - '0';
        if (digit > 9 || n > (limit - digit) / 10)
            return false;
        n = n * 10 + digit;
    }
    *value = negative ? (int32_t) -(int64_t) n : (int32_t) n;
    return true;
}

ASS_ScrollDefinition *ass_scroll_parse(const char *start, const char *end,
                                       int32_t duration)
{
    if (duration < 0 || start == end)
        return NULL;
    const char *source = start;
    ASS_ScrollDirection direction = ASS_SCROLL_UP;
    const char *first = memchr(start, ',', end - start);
    const char *name = start, *name_end = first ? first : end;
    while (name < name_end && space(*name)) name++;
    while (name_end > name && space(name_end[-1])) name_end--;
    size_t name_len = name_end - name;
    if (name_len == 2 && !memcmp(name, "ue", 2)) {
        if (!first) return NULL;
        start = first + 1;
    } else if ((name_len == 5 && !memcmp(name, "shita", 5)) ||
               (name_len == 4 && !memcmp(name, "sita", 4))) {
        if (!first) return NULL;
        direction = ASS_SCROLL_DOWN;
        start = first + 1;
    }
    /* Directionless lists remain implicit UP. Unknown names fail through the
     * same strict time parser as malformed legacy lists. */
    size_t fields = 1;
    for (const char *p = start; p < end; p++)
        fields += *p == ',';
    if (fields % 2 || fields / 2 > SIZE_MAX / sizeof(ASS_ScrollCue))
        return NULL;
    ASS_ScrollDefinition *def = calloc(1, sizeof(*def));
    if (!def)
        return NULL;
    def->count = fields / 2;
    def->direction = direction;
    def->cues = calloc(def->count, sizeof(*def->cues));
    def->source_len = end - source;
    def->source = malloc(def->source_len + 1);
    def->default_duration = duration;
    if (!def->cues || !def->source)
        goto invalid;
    memcpy(def->source, source, def->source_len);
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
        if (!signed_lines(start, comma ? comma : end, &cue->lines))
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
    if (rows >= SIZE_MAX / sizeof(*advances))
        return false;
    if (rows == def->rows && (!rows ||
            !memcmp(advances, def->advances, rows * sizeof(*advances))))
        return true;
    double *copy = rows ? malloc(rows * sizeof(*copy)) : NULL;
    double *prefix = malloc((rows + 1) * sizeof(*prefix));
    if ((rows && !copy) || !prefix) {
        free(copy);
        free(prefix);
        return false;
    }
    prefix[0] = 0;
    for (size_t i = 0; i < rows; i++) {
        prefix[i + 1] = prefix[i] + advances[i];
        if (!isfinite(advances[i]) || advances[i] < 0 ||
                !isfinite(prefix[i + 1])) {
            free(copy);
            free(prefix);
            return false;
        }
        copy[i] = advances[i];
    }
    /* Authored logical traversal is clamped before animation. Prefix sums let
     * reversals retrace actual rows in O(rows + cues), even with oscillation.
     * Evaluate each signed transition independently when animations overlap. */
    size_t row = 0;
    for (size_t i = 0; i < def->count; i++) {
        int64_t lines = def->cues[i].lines;
        size_t amount = (size_t) (lines < 0 ? -lines : lines);
        size_t next;
        if (lines < 0) {
            if (amount > row) amount = row;
            next = row - amount;
        } else {
            if (amount > rows - row) amount = rows - row;
            next = row + amount;
        }
        def->cues[i].distance = def->direction * (prefix[next] - prefix[row]);
        row = next;
    }
    free(prefix);
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
