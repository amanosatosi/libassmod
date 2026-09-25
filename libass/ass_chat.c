/*
 * Renderer-native chat syntax. This pass only identifies event structure;
 * ordinary ASS formatting stays in each message's text for the main parser.
 */
#include "ass_chat.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    size_t name_len;
    const char *arg;
    size_t arg_len;
    bool parenthesized;
} ChatTag;

static char *copy_span(const char *start, size_t length)
{
    if (length == SIZE_MAX)
        return NULL;
    char *copy = malloc(length + 1);
    if (copy) {
        memcpy(copy, start, length);
        copy[length] = 0;
    }
    return copy;
}

static bool tag_is(ChatTag tag, const char *name)
{
    size_t length = strlen(name);
    return tag.name_len == length && !memcmp(tag.name, name, length);
}

static bool parse_integer(const char *start, size_t length, int64_t *value)
{
    while (length && isspace((unsigned char) *start)) {
        start++;
        length--;
    }
    while (length && isspace((unsigned char) start[length - 1]))
        length--;
    if (!length || length >= 64)
        return false;
    char buffer[64];
    memcpy(buffer, start, length);
    buffer[length] = 0;
    errno = 0;
    char *end;
    long long number = strtoll(buffer, &end, 10);
    if (errno || *end)
        return false;
    *value = number;
    return true;
}

static bool ascii_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' ||
           c == '\n' || c == '\f' || c == '\v';
}

/* Match Unicode White_Space without applying a byte-oriented locale to UTF-8
 * continuation bytes. Only names and their optional leading padding use it. */
static size_t space_prefix(const char *text, size_t length)
{
    if (!length)
        return 0;
    const unsigned char *p = (const unsigned char *) text;
    if (ascii_space(p[0]))
        return 1;
    if (length >= 2 && p[0] == 0xC2 &&
        (p[1] == 0x85 || p[1] == 0xA0))
        return 2;
    if (length >= 3 &&
        ((p[0] == 0xE1 && p[1] == 0x9A && p[2] == 0x80) ||
         (p[0] == 0xE2 && p[1] == 0x80 &&
          ((p[2] >= 0x80 && p[2] <= 0x8A) ||
           p[2] == 0xA8 || p[2] == 0xA9 || p[2] == 0xAF)) ||
         (p[0] == 0xE2 && p[1] == 0x81 && p[2] == 0x9F) ||
         (p[0] == 0xE3 && p[1] == 0x80 && p[2] == 0x80)))
        return 3;
    return 0;
}

static char *trimmed_span(const char *start, size_t length)
{
    size_t width;
    while ((width = space_prefix(start, length))) {
        start += width;
        length -= width;
    }
    for (size_t width = 1; width <= 3 && width <= length;) {
        size_t matched = space_prefix(start + length - width, width);
        if (matched == width) {
            length -= width;
            width = 1;
        } else {
            width++;
        }
    }
    return copy_span(start, length);
}

/* Skip nested \t arguments instead of treating their tags as event structure. */
static const char *next_tag(const char *p, const char *end, ChatTag *tag)
{
    memset(tag, 0, sizeof(*tag));
    while (p < end && *p != '\\')
        p++;
    if (p == end)
        return end;
    p++;
    while (p < end && isspace((unsigned char) *p))
        p++;
    tag->name = p;
    while (p < end && *p != '\\' && *p != '(')
        p++;
    tag->name_len = p - tag->name;
    while (tag->name_len && isspace((unsigned char)
           tag->name[tag->name_len - 1]))
        tag->name_len--;
    tag->parenthesized = p < end && *p == '(';
    tag->arg = NULL;
    tag->arg_len = 0;
    if (tag->parenthesized) {
        int depth = 1;
        tag->arg = ++p;
        while (p < end && depth) {
            if (*p == '(')
                depth++;
            else if (*p == ')')
                depth--;
            if (depth)
                p++;
        }
        tag->arg_len = p - tag->arg;
        if (depth)
            tag->parenthesized = false;
        if (p < end)
            p++;
    }
    return p;
}

static bool each_block_tag(const char *start, const char *end,
                           bool (*callback)(ChatTag, void *), void *data)
{
    for (const char *p = start; p < end;) {
        ChatTag tag;
        const char *next = next_tag(p, end, &tag);
        if (!tag.name)
            break;
        if (next <= p)
            break;
        if (tag.name_len && !callback(tag, data))
            return false;
        p = next;
    }
    return true;
}

typedef struct {
    ASS_ChatScene *scene;
    bool title_seen;
    int64_t *times;
    int time_count;
    int time_capacity;
} ConfigScan;

static bool append_time(ConfigScan *scan, int64_t time)
{
    if (scan->time_count >= scan->time_capacity) {
        if (scan->time_capacity > INT_MAX / 2)
            return false;
        int capacity = scan->time_capacity ? scan->time_capacity * 2 : 8;
        if (capacity < scan->time_capacity ||
            (size_t) capacity > SIZE_MAX / sizeof(*scan->times))
            return false;
        int64_t *times = realloc(scan->times, (size_t) capacity * sizeof(*times));
        if (!times)
            return false;
        scan->times = times;
        scan->time_capacity = capacity;
    }
    scan->times[scan->time_count++] = time;
    return true;
}

static bool config_tag(ChatTag tag, void *opaque)
{
    ConfigScan *scan = opaque;
    ASS_ChatScene *scene = scan->scene;
    if (!scene->mode && !tag.parenthesized && tag_is(tag, "chatmode1"))
        scene->mode = ASS_CHAT_MODE_EXPLICIT;
    else if (!scene->mode && !tag.parenthesized && tag_is(tag, "chatmode2"))
        scene->mode = ASS_CHAT_MODE_NAMED_LAZY;
    else if (!scene->mode && !tag.parenthesized && tag_is(tag, "chatmode3"))
        scene->mode = ASS_CHAT_MODE_ALIGNMENT_SHORTHAND;
    else if (tag_is(tag, "msgtitle") && tag.parenthesized &&
             !scan->title_seen) {
        /* First successfully parsed title wins, including an empty title. */
        scene->title = trimmed_span(tag.arg, tag.arg_len);
        if (!scene->title)
            return false;
        scan->title_seen = true;
    } else if (tag_is(tag, "msgm") && tag.parenthesized) {
        char *speaker = trimmed_span(tag.arg, tag.arg_len);
        if (!speaker)
            return false;
        free(scene->main_speaker);
        scene->main_speaker = speaker;
    } else if (!tag.parenthesized && tag_is(tag, "msgshowname1")) {
        scene->show_names = true;
    } else if (!tag.parenthesized && tag_is(tag, "msgshowname0")) {
        scene->show_names = false;
    } else if (tag_is(tag, "msgstartcount") && tag.parenthesized) {
        int64_t value;
        if (parse_integer(tag.arg, tag.arg_len, &value))
            scene->start_count = value < 0 ? 0 :
                value > INT_MAX ? INT_MAX : (int) value;
    } else if (tag_is(tag, "msganim") && tag.parenthesized) {
        int64_t value;
        if (parse_integer(tag.arg, tag.arg_len, &value))
            scene->animation_ms = value < 0 ? 0 :
                value > INT_MAX ? INT_MAX : (int) value;
    } else if (tag_is(tag, "msgtime") && tag.parenthesized) {
        scene->has_time = true;
        scan->time_count = 0;
        const char *p = tag.arg, *end = p + tag.arg_len;
        while (p <= end) {
            const char *q = p;
            while (q < end && *q != ',')
                q++;
            int64_t value;
            if (parse_integer(p, q - p, &value) &&
                !append_time(scan, value < 0 ? 0 : value))
                return false;
            if (q == end)
                break;
            p = q + 1;
        }
    }
    return true;
}

static bool scan_config(const char *source, ConfigScan *scan)
{
    for (const char *p = source; *p;) {
        if (*p == '\\' && p[1]) {
            p += 2;
        } else if (*p == '{') {
            const char *end = strchr(p + 1, '}');
            if (!end)
                break;
            if (!each_block_tag(p + 1, end, config_tag, scan))
                return false;
            p = end + 1;
        } else {
            p++;
        }
    }
    return true;
}

static bool append_message(ASS_ChatScene *scene, const char *start,
                           const char *end, const char *speaker,
                           size_t speaker_len, int side)
{
    if (scene->count == INT_MAX)
        return false;
    /* Copy first: an inherited speaker may point into messages, which realloc
     * can move when the next message is appended. */
    char *text = copy_span(start, end - start);
    char *name = speaker ? trimmed_span(speaker, speaker_len) : NULL;
    if (!text || (speaker && !name)) {
        free(text);
        free(name);
        return false;
    }
    if (scene->count == scene->capacity) {
        if (scene->capacity > INT_MAX / 2) {
            free(text);
            free(name);
            return false;
        }
        int capacity = scene->capacity ? scene->capacity * 2 : 8;
        if (capacity < scene->capacity ||
            (size_t) capacity > SIZE_MAX / sizeof(*scene->messages)) {
            free(text);
            free(name);
            return false;
        }
        ASS_ChatMessage *messages = realloc(scene->messages,
            (size_t) capacity * sizeof(*messages));
        if (!messages) {
            free(text);
            free(name);
            return false;
        }
        scene->messages = messages;
        scene->capacity = capacity;
    }
    ASS_ChatMessage *message = &scene->messages[scene->count];
    memset(message, 0, sizeof(*message));
    message->text = text;
    message->speaker = name;
    message->side = side;
    message->reveal_ms = INT64_MAX;
    scene->count++;
    return true;
}

typedef struct {
    bool boundary;
    int side;
    const char *speaker;
    size_t speaker_len;
    bool explicit_side;
} BoundaryScan;

static bool message_boundary_tag(ChatTag tag, void *opaque)
{
    BoundaryScan *scan = opaque;
    if (tag_is(tag, "msg") && tag.parenthesized) {
        scan->boundary = true;
        const char *comma = memchr(tag.arg, ',', tag.arg_len);
        scan->speaker = tag.arg;
        scan->speaker_len = comma ? (size_t) (comma - tag.arg) : tag.arg_len;
        if (comma) {
            const char *side = comma + 1;
            size_t length = tag.arg + tag.arg_len - side;
            while (length && isspace((unsigned char) *side)) {
                side++;
                length--;
            }
            while (length && isspace((unsigned char) side[length - 1]))
                length--;
            if (length == 4 && !memcmp(side, "left", 4)) {
                scan->side = 0;
                scan->explicit_side = true;
            } else if (length == 5 && !memcmp(side, "right", 5)) {
                scan->side = 1;
                scan->explicit_side = true;
            }
        }
    }
    return true;
}

static bool shorthand_boundary_tag(ChatTag tag, void *opaque)
{
    BoundaryScan *scan = opaque;
    if (tag.parenthesized || tag.name_len != 3 ||
        memcmp(tag.name, "ta", 2))
        return true;
    switch (tag.name[2]) {
    case '1': case '4': case '7':
        scan->boundary = true;
        scan->side = 0;
        break;
    case '3': case '6': case '9':
        scan->boundary = true;
        scan->side = 1;
        break;
    default: break; /* center is reserved */
    }
    return true;
}

/* Return the end of a balanced angle group. A pipe within <base|ruby> belongs
 * to furigana, including when its reading contains override blocks. */
static const char *skip_furi_group(const char *p, const char *end)
{
    for (const char *q = p + 1; q < end;) {
        if (*q == '\\' && q + 1 < end &&
            (q[1] == '<' || q[1] == '>' || q[1] == '|' ||
             q[1] == '{' || q[1] == '}' || q[1] == '\\')) {
            q += 2;
        } else if (*q == '{') {
            const char *close = memchr(q + 1, '}', end - (q + 1));
            if (!close)
                return NULL;
            q = close + 1;
        } else if (*q == '<' || *q == '}') {
            return NULL;
        } else if (*q == '>') {
            return q + 1;
        } else {
            q++;
        }
    }
    return NULL;
}

/* Only top-level pipes delimit named chat blocks. This also skips pipe bytes
 * in ASS overrides and existing escaped furigana punctuation. */
static const char *next_chat_pipe(const char *p, const char *end)
{
    while (p < end) {
        if (*p == '{') {
            const char *close = memchr(p + 1, '}', end - (p + 1));
            if (close) {
                p = close + 1;
                continue;
            }
        } else if (*p == '\\' && p + 1 < end &&
                   (p[1] == '|' || p[1] == '<' || p[1] == '{')) {
            p += 2;
            continue;
        } else if (*p == '<') {
            const char *after = skip_furi_group(p, end);
            if (after) {
                p = after;
                continue;
            }
        } else if (*p == '|') {
            return p;
        }
        p++;
    }
    return NULL;
}

/* Only override state before the first pipe is global. Source separators and
 * stray text outside blocks do not become extra glyphs or messages. */
static char *named_prefix(const char *source, const char *first_pipe)
{
    size_t capacity = first_pipe - source;
    char *prefix = malloc(capacity + 1);
    if (!prefix)
        return NULL;
    size_t length = 0;
    for (const char *p = source; p < first_pipe;) {
        if (*p == '{') {
            const char *close = memchr(p + 1, '}', first_pipe - (p + 1));
            if (close) {
                size_t block_length = close + 1 - p;
                memcpy(prefix + length, p, block_length);
                length += block_length;
                p = close + 1;
                continue;
            }
        }
        p++;
    }
    prefix[length] = 0;
    return prefix;
}

/* Decode only the mode-2 \:\ sequence. Override blocks remain intact so
 * their arguments still pass through the ordinary ASS tag parser. */
static size_t copy_decoded_colons(char *dst, const char *start,
                                  const char *end, bool skip_overrides)
{
    size_t length = 0;
    for (const char *p = start; p < end;) {
        if (skip_overrides && *p == '{') {
            const char *close = memchr(p + 1, '}', end - (p + 1));
            if (close) {
                size_t block_length = close + 1 - p;
                memcpy(dst + length, p, block_length);
                length += block_length;
                p = close + 1;
                continue;
            }
        }
        if (*p == '\\' && p + 1 < end && p[1] == '\\') {
            dst[length++] = *p++;
            dst[length++] = *p++;
        } else if (*p == '\\' && p + 2 < end &&
                   p[1] == ':' && p[2] == '\\') {
            dst[length++] = ':';
            p += 3;
        } else {
            dst[length++] = *p++;
        }
    }
    return length;
}

/* A block with an unescaped :\N sets its speaker. Otherwise nonempty body
 * text inherits the last speaker and side, with anonymous-left as fallback. */
static bool append_named_block(ASS_ChatScene *scene, const char *open,
                               const char *close)
{
    const char *p = open + 1;
    size_t capacity = close - p;
    char *text = malloc(capacity + 1);
    if (!text)
        return false;
    size_t length = 0;
    while (p < close) {
        size_t width;
        while ((width = space_prefix(p, close - p)))
            p += width;
        if (p == close || *p != '{')
            break;
        const char *end = memchr(p + 1, '}', close - (p + 1));
        if (!end)
            break;
        size_t block_length = end + 1 - p;
        memcpy(text + length, p, block_length);
        length += block_length;
        p = end + 1;
    }

    if (p == close) {
        free(text);
        return true;
    }
    const char *speaker = NULL;
    size_t speaker_len = 0;
    const char *body = p;
    char *decoded_name = NULL;
    int side = 0;
    if (scene->count) {
        const ASS_ChatMessage *last = &scene->messages[scene->count - 1];
        speaker = last->speaker;
        speaker_len = speaker ? strlen(speaker) : 0;
        side = last->side;
    }
    if (p + 1 < close && p[0] == '\\' && p[1] == 'N') {
        body = p + 2;
    } else {
        const char *delimiter = NULL;
        for (const char *q = p; q < close;) {
            if (*q == '{') {
                const char *end = memchr(q + 1, '}', close - (q + 1));
                if (end) {
                    q = end + 1;
                    continue;
                }
            } else if (*q == '<') {
                const char *after = skip_furi_group(q, close);
                if (after) {
                    q = after;
                    continue;
                }
            } else if (*q == '\\' && q + 1 < close && q[1] == '\\') {
                q += 2;
                continue;
            } else if (*q == '\\' && q + 2 < close &&
                       q[1] == ':' && q[2] == '\\') {
                q += 3;
                continue;
            } else if (q + 2 < close && q[0] == ':' &&
                       q[1] == '\\' && q[2] == 'N') {
                delimiter = q;
                break;
            }
            q++;
        }
        if (delimiter) {
            size_t raw_length = delimiter - p;
            char *raw_name = malloc(raw_length + 1);
            if (!raw_name) {
                free(text);
                return false;
            }
            size_t name_length = copy_decoded_colons(raw_name, p,
                                                      delimiter, false);
            decoded_name = trimmed_span(raw_name, name_length);
            free(raw_name);
            if (!decoded_name) {
                free(text);
                return false;
            }
            if (*decoded_name) {
                speaker = decoded_name;
                speaker_len = strlen(decoded_name);
                side = scene->main_speaker &&
                    !strcmp(decoded_name, scene->main_speaker);
                body = delimiter + 3;
            }
        }
    }
    length += copy_decoded_colons(text + length, body, close, true);
    text[length] = 0;
    bool ok = append_message(scene, text, text + length,
                             speaker, speaker_len, side);
    free(decoded_name);
    free(text);
    return ok;
}

static bool parse_named_messages(const char *source, ASS_ChatScene *scene)
{
    const char *end = source + strlen(source);
    const char *first = next_chat_pipe(source, end);
    scene->prefix = named_prefix(source, first ? first : end);
    if (!scene->prefix)
        return false;
    for (const char *p = first; p;) {
        const char *close = next_chat_pipe(p + 1, end);
        if (!close)
            break;
        if (!append_named_block(scene, p, close))
            return false;
        p = next_chat_pipe(close + 1, end);
    }
    return true;
}

static bool parse_shorthand_messages(const char *source, ASS_ChatScene *scene)
{
    /* In shorthand mode an ASS \N is pending until the next logical token. A
     * structural \ta or {|} consumes it as a message boundary; otherwise it
     * stays in the body as an ordinary multiline break. */
    const char *start = NULL, *pending_newline = NULL;
    const char *speaker = NULL;
    size_t speaker_len = 0;
    int side = 0;
    const char *p = source;
    while (*p) {
        if (*p == '{') {
            const char *end = strchr(p + 1, '}');
            if (!end)
                break;
            BoundaryScan scan = {0};
            if (scene->mode == ASS_CHAT_MODE_EXPLICIT)
                each_block_tag(p + 1, end, message_boundary_tag, &scan);
            else if (end == p + 2 && p[1] == '|') {
                scan.boundary = true;
                scan.side = side; /* {|} starts a message, inheriting side only. */
            } else
                each_block_tag(p + 1, end, shorthand_boundary_tag, &scan);
            bool new_message = scan.boundary &&
                (scene->mode == ASS_CHAT_MODE_EXPLICIT || !start || pending_newline);
            if (new_message) {
                const char *end_previous = pending_newline ? pending_newline : p;
                if (start && !append_message(scene, start, end_previous,
                                             speaker, speaker_len, side))
                    return false;
                else if (!start) {
                    scene->prefix = copy_span(source, p - source);
                    if (!scene->prefix)
                        return false;
                }
                start = pending_newline ? pending_newline + 2 : p;
                pending_newline = NULL;
                if (scene->mode == ASS_CHAT_MODE_EXPLICIT) {
                    speaker = scan.speaker;
                    speaker_len = scan.speaker_len;
                    if (scan.explicit_side)
                        side = scan.side;
                    else if (scene->main_speaker && speaker) {
                        char *name = trimmed_span(speaker, speaker_len);
                        if (!name)
                            return false;
                        side = !strcmp(name, scene->main_speaker);
                        free(name);
                    } else
                        side = 0;
                } else
                    side = scan.side;
            }
            p = end + 1;
        } else if (scene->mode == ASS_CHAT_MODE_ALIGNMENT_SHORTHAND &&
                   p[0] == '\\' && p[1] == 'N') {
            if (!start) {
                start = p;
                scene->prefix = copy_span(source, p - source);
                if (!scene->prefix)
                    return false;
            }
            pending_newline = p;
            p += 2;
        } else if (*p == '\\' && p[1]) {
            if (!start) {
                start = p;
                scene->prefix = copy_span(source, p - source);
                if (!scene->prefix)
                    return false;
            }
            pending_newline = NULL;
            p += 2;
        } else {
            if (!start && !isspace((unsigned char) *p)) {
                start = p;
                scene->prefix = copy_span(source, p - source);
                if (!scene->prefix)
                    return false;
            }
            if (!isspace((unsigned char) *p))
                pending_newline = NULL;
            p++;
        }
    }
    if (start && !append_message(scene, start, p, speaker, speaker_len, side))
        return false;
    if (!scene->prefix) {
        scene->prefix = copy_span(source, strlen(source));
        if (!scene->prefix)
            return false;
    }
    return true;
}

ASS_ChatScene *ass_chat_parse(const char *source)
{
    if (!source)
        return NULL;
    ASS_ChatScene *scene = calloc(1, sizeof(*scene));
    if (!scene)
        return NULL;
    scene->show_names = true;
    scene->start_count = 1;
    scene->animation_ms = 250;
    ConfigScan scan = {.scene = scene};
    bool ok = scan_config(source, &scan);
    if (!ok || !scene->mode ||
        !(scene->mode == ASS_CHAT_MODE_NAMED_LAZY ?
          parse_named_messages(source, scene) :
          parse_shorthand_messages(source, scene))) {
        free(scan.times);
        ass_chat_free(scene);
        return NULL;
    }
    if (scene->start_count > scene->count)
        scene->start_count = scene->count;
    if (scene->has_time) {
        /* These are absolute offsets from event start, not durations. */
        int64_t last = 0;
        for (int i = scene->start_count; i < scene->count; i++) {
            int index = i - scene->start_count;
            if (index >= scan.time_count)
                break;
            int64_t time = scan.times[index];
            if (time < last)
                time = last;
            scene->messages[i].reveal_ms = last = time;
        }
    }
    free(scan.times);
    return scene;
}

void ass_chat_free(ASS_ChatScene *scene)
{
    if (!scene)
        return;
    for (int i = 0; i < scene->count; i++) {
        free(scene->messages[i].text);
        free(scene->messages[i].speaker);
    }
    free(scene->messages);
    free(scene->title);
    free(scene->main_speaker);
    free(scene->prefix);
    free(scene);
}

int ass_chat_visible(const ASS_ChatScene *scene, int64_t event_ms,
                     int *previous, double *progress)
{
    if (!scene) {
        if (previous) *previous = 0;
        if (progress) *progress = 1;
        return 0;
    }
    if (!scene->has_time) { /* No timing tag makes a static chat scene. */
        if (previous) *previous = scene->count;
        if (progress) *progress = 1;
        return scene->count;
    }
    int low = scene->start_count, high = scene->count;
    while (low < high) {
        int middle = low + (high - low) / 2;
        int64_t reveal = scene->messages[middle].reveal_ms;
        if (reveal != INT64_MAX && reveal <= event_ms)
            low = middle + 1;
        else
            high = middle;
    }
    int visible = low;
    int first = visible;
    if (visible > scene->start_count) {
        int64_t time = scene->messages[visible - 1].reveal_ms;
        low = scene->start_count;
        high = visible;
        while (low < high) {
            int middle = low + (high - low) / 2;
            if (scene->messages[middle].reveal_ms < time)
                low = middle + 1;
            else
                high = middle;
        }
        first = low;
        int64_t duration = scene->animation_ms;
        if (visible < scene->count &&
            scene->messages[visible].reveal_ms != INT64_MAX) {
            int64_t gap = scene->messages[visible].reveal_ms - time;
            if (gap < duration)
                duration = gap;
        }
        if (previous) *previous = first;
        if (progress) *progress = duration > 0 ?
            (double) (event_ms - time) / duration : 1.0;
        if (progress && *progress > 1.0) *progress = 1.0;
    } else {
        if (previous) *previous = visible;
        if (progress) *progress = 1.0;
    }
    return visible;
}
