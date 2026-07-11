/*
 * Copyright (C) 2006 Evgeniy Stepanov <eugeni.stepanov@gmail.com>
 *
 * This file is part of libass.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"
#include "ass_compat.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>

#include "ass_library.h"
#include "ass.h"
#include "ass_utils.h"
#include "ass_string.h"
#include "ass_threading.h"

// Fallbacks
#ifndef HAVE_STRDUP
char *ass_strdup_fallback(const char *str)
{
    size_t len    = strlen(str) + 1;
    char *new_str = malloc(len);
    if (new_str)
        memcpy(new_str, str, len);
    return new_str;
}
#endif

#ifndef HAVE_STRNDUP
char *ass_strndup_fallback(const char *s, size_t n)
{
    char *end = memchr(s, 0, n);
    size_t len = end ? end - s : n;
    char *new = len < SIZE_MAX ? malloc(len + 1) : NULL;
    if (new) {
        memcpy(new, s, len);
        new[len] = 0;
    }
    return new;
}
#endif

void *ass_aligned_alloc(size_t alignment, size_t size, bool zero)
{
    assert(!(alignment & (alignment - 1))); // alignment must be power of 2
    if (size >= SIZE_MAX - alignment - sizeof(void *))
        return NULL;
    char *allocation = zero ? calloc(1, size + sizeof(void *) + alignment - 1)
                            : malloc(size + sizeof(void *) + alignment - 1);
    if (!allocation)
        return NULL;
    char *ptr = allocation + sizeof(void *);
    unsigned int misalign = (uintptr_t)ptr & (alignment - 1);
    if (misalign)
        ptr += alignment - misalign;
    *((void **)ptr - 1) = allocation;
    return ptr;
}

void ass_aligned_free(void *ptr)
{
    if (ptr)
        free(*((void **)ptr - 1));
}

/**
 * This works similar to realloc(ptr, nmemb * size), but checks for overflow.
 *
 * Unlike some implementations of realloc, this never acts as a call to free().
 * If the total size is 0, it is bumped up to 1. This means a NULL return always
 * means allocation failure, and the unportable realloc(0, 0) case is avoided.
 */
void *ass_realloc_array(void *ptr, size_t nmemb, size_t size)
{
    if (nmemb > (SIZE_MAX / size))
        return NULL;
    size *= nmemb;
    if (size < 1)
        size = 1;

    return realloc(ptr, size);
}

/**
 * Like ass_realloc_array(), but:
 * 1. on failure, return the original ptr value, instead of NULL
 * 2. set errno to indicate failure (errno!=0) or success (errno==0)
 */
void *ass_try_realloc_array(void *ptr, size_t nmemb, size_t size)
{
    void *new_ptr = ass_realloc_array(ptr, nmemb, size);
    if (new_ptr) {
        errno = 0;
        return new_ptr;
    } else {
        errno = ENOMEM;
        return ptr;
    }
}

void ass_msg(ASS_Library *priv, int lvl, const char *fmt, ...)
{
    pthread_mutex_lock(&priv->log_mutex);

    va_list va;
    va_start(va, fmt);
    priv->msg_callback(lvl, fmt, va, priv->msg_callback_data);
    va_end(va);

    pthread_mutex_unlock(&priv->log_mutex);
}

unsigned ass_utf8_get_char(char **str)
{
    uint8_t *strp = (uint8_t *) * str;
    unsigned c = *strp++;
    unsigned mask = 0x80;
    int len = -1;
    while (c & mask) {
        mask >>= 1;
        len++;
    }
    if (len <= 0 || len > 4)
        goto no_utf8;
    c &= mask - 1;
    while ((*strp & 0xc0) == 0x80) {
        if (len-- <= 0)
            goto no_utf8;
        c = (c << 6) | (*strp++ & 0x3f);
    }
    if (len)
        goto no_utf8;
    *str = (char *) strp;
    return c;

  no_utf8:
    strp = (uint8_t *) * str;
    c = *strp++;
    *str = (char *) strp;
    return c;
}

/**
 * Original version from http://www.cprogramming.com/tutorial/utf8.c
 * \brief Converts a single UTF-32 code point to UTF-8
 * \param dest Buffer to write to. Writes a NULL terminator.
 * \param ch 32-bit character code to convert
 * \return number of bytes written
 * converts a single character and ASSUMES YOU HAVE ENOUGH SPACE
 */
unsigned ass_utf8_put_char(char *dest, uint32_t ch)
{
    char *orig_dest = dest;

    if (ch < 0x80) {
        *dest++ = (char)ch;
    } else if (ch < 0x800) {
        *dest++ = (ch >> 6) | 0xC0;
        *dest++ = (ch & 0x3F) | 0x80;
    } else if (ch < 0x10000) {
        *dest++ = (ch >> 12) | 0xE0;
        *dest++ = ((ch >> 6) & 0x3F) | 0x80;
        *dest++ = (ch & 0x3F) | 0x80;
    } else if (ch < 0x110000) {
        *dest++ = (ch >> 18) | 0xF0;
        *dest++ = ((ch >> 12) & 0x3F) | 0x80;
        *dest++ = ((ch >> 6) & 0x3F) | 0x80;
        *dest++ = (ch & 0x3F) | 0x80;
    }

    *dest = '\0';
    return dest - orig_dest;
}

static int unicode_decimal_value(unsigned c)
{
    static const struct {
        unsigned first;
        unsigned last;
    } ranges[] = {
        { 0x0030, 0x0039 },
        { 0x0660, 0x0669 },
        { 0x06F0, 0x06F9 },
        { 0x0966, 0x096F },
        { 0x09E6, 0x09EF },
        { 0x0A66, 0x0A6F },
        { 0x0AE6, 0x0AEF },
        { 0x0B66, 0x0B6F },
        { 0x0BE6, 0x0BEF },
        { 0x0C66, 0x0C6F },
        { 0x0CE6, 0x0CEF },
        { 0x0D66, 0x0D6F },
        { 0x0E50, 0x0E59 },
        { 0x0ED0, 0x0ED9 },
        { 0x0F20, 0x0F29 },
        { 0x1040, 0x1049 },
        { 0x17E0, 0x17E9 },
        { 0x1810, 0x1819 },
        { 0xFF10, 0xFF19 },
    };

    for (int i = 0; i < (int) (sizeof(ranges) / sizeof(ranges[0])); i++)
        if (c >= ranges[i].first && c <= ranges[i].last)
            return c - ranges[i].first;
    return -1;
}

static bool decimal_syntax_char(char c)
{
    return c == '+' || c == '-' || c == '.' || c == 'e' || c == 'E' ||
           c == ' ' || c == '\t' || c == '\n' || c == '\v' ||
           c == '\f' || c == '\r';
}

static bool integer_syntax_char(char c)
{
    return c == '+' || c == '-' ||
           c == ' ' || c == '\t' || c == '\n' || c == '\v' ||
           c == '\f' || c == '\r';
}

typedef bool (*syntax_char_func)(char c);

static bool normalize_decimal_number(char *start, syntax_char_func syntax_char,
                                     char *stack_buf, char **stack_ends,
                                     size_t stack_cap, char **buf, char ***ends)
{
    size_t cap = stack_cap;
    char *tmp = stack_buf;
    char **map = stack_ends;

    size_t len = 0;
    char *p = start;
    while (*p) {
        char *next = p;
        unsigned c = ass_utf8_get_char(&next);
        int digit = unicode_decimal_value(c);
        char out;

        if (digit >= 0)
            out = '0' + digit;
        else if (c < 0x80 && syntax_char((char) c))
            out = (char) c;
        else
            break;

        if (len + 1 >= cap) {
            if (cap > SIZE_MAX / 2 || cap * 2 > SIZE_MAX / sizeof(*map))
                goto fail;
            size_t old_cap = cap;
            cap *= 2;
            char *new_tmp;
            if (tmp == stack_buf) {
                new_tmp = malloc(cap);
                if (new_tmp)
                    memcpy(new_tmp, tmp, old_cap);
            } else {
                new_tmp = realloc(tmp, cap);
            }
            if (!new_tmp)
                goto fail;
            tmp = new_tmp;
            char **new_map;
            if (map == stack_ends) {
                new_map = malloc(cap * sizeof(*map));
                if (new_map)
                    memcpy(new_map, map, old_cap * sizeof(*map));
            } else {
                new_map = realloc(map, cap * sizeof(*map));
            }
            if (!new_map)
                goto fail;
            map = new_map;
        }

        tmp[len] = out;
        map[len] = next;
        len++;
        p = next;
    }
    tmp[len] = '\0';

    *buf = tmp;
    *ends = map;
    return true;

fail:
    if (tmp != stack_buf)
        free(tmp);
    if (map != stack_ends)
        free(map);
    return false;
}

int ass_strtod_decimal(char **p, double *res)
{
    char *start = *p;
    char stack_buf[64];
    char *stack_ends[64];
    char *buf = stack_buf;
    char **ends = stack_ends;
    if (!normalize_decimal_number(start, decimal_syntax_char, stack_buf,
                                  stack_ends, 64, &buf, &ends)) {
        *res = ass_strtod(*p, p);
        return *p != start;
    }

    char *end = NULL;
    *res = ass_strtod(buf, &end);
    if (end != buf)
        *p = ends[end - buf - 1];
    else
        *p = start;

    if (buf != stack_buf)
        free(buf);
    if (ends != stack_ends)
        free(ends);
    return *p != start;
}

int ass_strtoi32_decimal(char **p, int32_t *res)
{
    char *start = *p;
    char stack_buf[64];
    char *stack_ends[64];
    char *buf = stack_buf;
    char **ends = stack_ends;
    if (!normalize_decimal_number(start, integer_syntax_char, stack_buf,
                                  stack_ends, 64, &buf, &ends)) {
        long long temp_res = strtoll(*p, p, 10);
        *res = FFMINMAX(temp_res, INT32_MIN, INT32_MAX);
        return *p != start;
    }

    char *end = NULL;
    long long temp_res = strtoll(buf, &end, 10);
    *res = FFMINMAX(temp_res, INT32_MIN, INT32_MAX);
    if (end != buf)
        *p = ends[end - buf - 1];
    else
        *p = start;

    if (buf != stack_buf)
        free(buf);
    if (ends != stack_ends)
        free(ends);
    return *p != start;
}

/**
 * \brief Parse UTF-16 and return the code point of the sequence starting at src.
 * \param src pointer to a pointer to the start of the UTF-16 data
 *            (will be set to the start of the next code point)
 * \return the code point
 */
static uint32_t ass_read_utf16be(uint8_t **src, size_t bytes)
{
    if (bytes < 2)
        goto too_short;

    uint32_t cp = ((*src)[0] << 8) | (*src)[1];
    *src += 2;
    bytes -= 2;

    if (cp >= 0xD800 && cp <= 0xDBFF) {
        if (bytes < 2)
            goto too_short;

        uint32_t cp2 = ((*src)[0] << 8) | (*src)[1];

        if (cp2 < 0xDC00 || cp2 > 0xDFFF)
            return 0xFFFD;

        *src += 2;

        cp = 0x10000 + ((cp - 0xD800) << 10) + (cp2 - 0xDC00);
    }

    if (cp >= 0xDC00 && cp <= 0xDFFF)
        return 0xFFFD;

    return cp;

too_short:
    *src += bytes;
    return 0xFFFD;
}

void ass_utf16be_to_utf8(char *dst, size_t dst_size, uint8_t *src, size_t src_size)
{
    uint8_t *end = src + src_size;

    if (!dst_size)
        return;

    while (src < end) {
        uint32_t cp = ass_read_utf16be(&src, end - src);
        if (dst_size < 5)
            break;
        unsigned s = ass_utf8_put_char(dst, cp);
        dst += s;
        dst_size -= s;
    }

    *dst = '\0';
}

/**
 * \brief find style by name the common way (\r matches differently)
 * \param track track
 * \param name style name
 * \return index in track->styles
 * Returns 0 if no styles found => expects at least 1 style.
 * Parsing code always adds "Default" style in the beginning.
 */
int ass_lookup_style(ASS_Track *track, char *name)
{
    int i;
    // '*' seem to mean literally nothing;
    // VSFilter removes them as soon as it can
    while (*name == '*')
        ++name;
    // VSFilter then normalizes the case of "Default"
    // (only in contexts where this function is called)
    if (ass_strcasecmp(name, "Default") == 0)
        name = "Default";
    for (i = track->n_styles - 1; i >= 0; --i) {
        if (strcmp(track->styles[i].Name, name) == 0)
            return i;
    }
    i = track->default_style;
    ass_msg(track->library, MSGL_WARN,
            "[%p]: Warning: no style named '%s' found, using '%s'",
            track, name, track->styles[i].Name);
    return i;
}
