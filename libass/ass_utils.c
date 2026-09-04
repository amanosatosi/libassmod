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

#if CONFIG_ALIGNED_ALLOC_DEBUG
#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#endif
#endif

#include "ass_library.h"
#include "ass.h"
#include "ass_utils.h"
#include "ass_string.h"

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

#if CONFIG_ALIGNED_ALLOC_DEBUG

#define ALIGNED_DEBUG_BUCKETS 4096
#define ALIGNED_DEBUG_FREED_HISTORY 4096

typedef struct aligned_debug_allocation {
    void *ptr;
    void *raw;
    size_t size;
    size_t alignment;
    ASS_AlignedAllocCategory category;
    const void *owner;
    const char *file;
    int line;
    const char *last_operation;
    uint64_t id;
    struct aligned_debug_allocation *next;
} AlignedDebugAllocation;

typedef struct {
    void *ptr;
    size_t size;
    ASS_AlignedAllocCategory category;
    const void *owner;
    const char *allocation_file;
    int allocation_line;
    const char *free_file;
    int free_line;
    uint64_t id;
    uint64_t release_id;
} AlignedDebugFreed;

static AlignedDebugAllocation *aligned_debug_buckets[ALIGNED_DEBUG_BUCKETS];
static AlignedDebugFreed aligned_debug_freed[ALIGNED_DEBUG_FREED_HISTORY];
static size_t aligned_debug_freed_cursor;
static uint64_t aligned_debug_next_id = 1;
static uint64_t aligned_debug_next_release_id = 1;

#ifdef _WIN32
static volatile LONG aligned_debug_lock_state;

static void aligned_debug_lock(void)
{
    while (InterlockedCompareExchange(&aligned_debug_lock_state, 1, 0) != 0)
        Sleep(0);
}

static void aligned_debug_unlock(void)
{
    InterlockedExchange(&aligned_debug_lock_state, 0);
}
#else
static volatile int aligned_debug_lock_state;

static void aligned_debug_lock(void)
{
    while (__sync_lock_test_and_set(&aligned_debug_lock_state, 1)) {
        while (aligned_debug_lock_state)
            sched_yield();
    }
}

static void aligned_debug_unlock(void)
{
    __sync_lock_release(&aligned_debug_lock_state);
}
#endif

static const char *aligned_category_name(ASS_AlignedAllocCategory category)
{
    switch (category) {
    case ASS_ALIGNED_ALLOC_OTHER:            return "other";
    case ASS_ALIGNED_ALLOC_BITMAP:           return "bitmap";
    case ASS_ALIGNED_ALLOC_GLYPH_BITMAP:     return "glyph bitmap";
    case ASS_ALIGNED_ALLOC_LEGACY_IMAGE:     return "legacy image buffer";
    case ASS_ALIGNED_ALLOC_RGBA_IMAGE:       return "RGBA image buffer";
    case ASS_ALIGNED_ALLOC_CLIP_BUFFER:      return "clip buffer";
    case ASS_ALIGNED_ALLOC_COMPOSITE_BUFFER: return "composite buffer";
    case ASS_ALIGNED_ALLOC_BLUR_SCRATCH:     return "blur scratch";
    case ASS_ALIGNED_ALLOC_RASTERIZER_TILE:  return "rasterizer tile";
    case ASS_ALIGNED_ALLOC_BS4_MASK:         return "BS4 mask";
    }
    return "invalid";
}

static size_t aligned_debug_bucket(const void *ptr)
{
    uintptr_t value = (uintptr_t) ptr;
    value ^= value >> 17;
    value ^= value >> 9;
    return value & (ALIGNED_DEBUG_BUCKETS - 1);
}

static AlignedDebugAllocation **aligned_debug_find_link(void *ptr)
{
    AlignedDebugAllocation **link = &aligned_debug_buckets[aligned_debug_bucket(ptr)];
    while (*link && (*link)->ptr != ptr)
        link = &(*link)->next;
    return link;
}

static const AlignedDebugFreed *aligned_debug_find_freed(void *ptr)
{
    for (size_t i = 0; i < ALIGNED_DEBUG_FREED_HISTORY; i++)
        if (aligned_debug_freed[i].ptr == ptr)
            return &aligned_debug_freed[i];
    return NULL;
}

/* The pointer passed to free may be corrupted, while its Bitmap owner still
 * identifies the expected allocation. These run while the registry is locked. */
static const AlignedDebugAllocation *aligned_debug_find_owner(
    const void *owner, ASS_AlignedAllocCategory category)
{
    const AlignedDebugAllocation *match = NULL;
    if (!owner)
        return NULL;
    for (size_t i = 0; i < ALIGNED_DEBUG_BUCKETS; i++) {
        for (AlignedDebugAllocation *entry = aligned_debug_buckets[i]; entry;
             entry = entry->next) {
            if (entry->owner == owner && entry->category == category &&
                    (!match || entry->id > match->id))
                match = entry;
        }
    }
    return match;
}

static const AlignedDebugFreed *aligned_debug_find_freed_owner(
    const void *owner, ASS_AlignedAllocCategory category)
{
    const AlignedDebugFreed *match = NULL;
    if (!owner)
        return NULL;
    for (size_t i = 0; i < ALIGNED_DEBUG_FREED_HISTORY; i++) {
        const AlignedDebugFreed *entry = &aligned_debug_freed[i];
        if (entry->owner == owner && entry->category == category &&
                (!match || entry->release_id > match->release_id))
            match = entry;
    }
    return match;
}

static void aligned_debug_fail(const char *reason, void *ptr,
                               ASS_AlignedAllocCategory free_category,
                               const void *free_owner, const char *free_file,
                               int free_line,
                               const AlignedDebugAllocation *live,
                               const AlignedDebugFreed *freed,
                               const AlignedDebugAllocation *owner_live,
                               const AlignedDebugFreed *owner_freed)
{
    const char *allocation_file = live && live->file ? live->file :
        freed && freed->allocation_file ? freed->allocation_file : "unknown";
    const char *last_operation = live && live->last_operation ?
        live->last_operation : "unknown";
    const char *previous_free_file = freed && freed->free_file ?
        freed->free_file : "none";
    char diagnostic[4096];
    int length = snprintf(
        diagnostic, sizeof(diagnostic),
        "Invalid aligned free: reason=%s pointer=%p "
        "free_category=%s free_owner=%p free_site=%s:%d "
        "allocation_id=%" PRIu64 " allocation_size=%zu "
        "allocation_category=%s allocation_owner=%p "
        "allocation_site=%s:%d last_operation=%s "
        "previous_free_site=%s:%d "
        "owner_live_pointer=%p owner_live_id=%" PRIu64 " "
        "owner_live_size=%zu owner_live_site=%s:%d "
        "owner_live_operation=%s "
        "owner_previous_pointer=%p owner_previous_id=%" PRIu64 " "
        "owner_previous_size=%zu owner_previous_site=%s:%d "
        "owner_previous_free_site=%s:%d\n",
        reason ? reason : "unknown", ptr,
        aligned_category_name(free_category), free_owner,
        free_file ? free_file : "unknown", free_line,
        live ? live->id : freed ? freed->id : 0,
        live ? live->size : freed ? freed->size : 0,
        aligned_category_name(live ? live->category :
                              freed ? freed->category : ASS_ALIGNED_ALLOC_OTHER),
        live ? live->owner : freed ? freed->owner : NULL,
        allocation_file, live ? live->line : freed ? freed->allocation_line : 0,
        last_operation, previous_free_file, freed ? freed->free_line : 0,
        owner_live ? owner_live->ptr : NULL,
        owner_live ? owner_live->id : 0,
        owner_live ? owner_live->size : 0,
        owner_live && owner_live->file ? owner_live->file : "none",
        owner_live ? owner_live->line : 0,
        owner_live && owner_live->last_operation ? owner_live->last_operation :
            "none",
        owner_freed ? owner_freed->ptr : NULL,
        owner_freed ? owner_freed->id : 0,
        owner_freed ? owner_freed->size : 0,
        owner_freed && owner_freed->allocation_file ?
            owner_freed->allocation_file : "none",
        owner_freed ? owner_freed->allocation_line : 0,
        owner_freed && owner_freed->free_file ? owner_freed->free_file : "none",
        owner_freed ? owner_freed->free_line : 0);
    if (length < 0)
        fputs("Invalid aligned free: diagnostic formatting failed\n", stderr);
    else {
        fputs(diagnostic, stderr);
        fflush(stderr);
#ifdef _WIN32
        /* A GUI host often has no inherited stderr. Keep the complete
         * allocation provenance visible to a debugger or DebugView. */
        OutputDebugStringA(diagnostic);
#endif
    }
    abort();
}

static void aligned_debug_track(void *ptr, void *raw, size_t size,
                                size_t alignment,
                                ASS_AlignedAllocCategory category,
                                const void *owner, const char *file, int line)
{
    AlignedDebugAllocation *entry = malloc(sizeof(*entry));
    if (!entry) {
        free(raw);
        fprintf(stderr, "Could not allocate aligned-allocation registry entry\n");
        abort();
    }

    aligned_debug_lock();
    AlignedDebugAllocation **link = aligned_debug_find_link(ptr);
    if (*link)
        aligned_debug_fail("live pointer allocated twice", ptr, category, owner,
                           file, line, *link, NULL, NULL, NULL);
    uint64_t id = aligned_debug_next_id++;
    if (!id)
        id = aligned_debug_next_id++;
    *entry = (AlignedDebugAllocation) {
        .ptr = ptr,
        .raw = raw,
        .size = size,
        .alignment = alignment,
        .category = category,
        .owner = owner,
        .file = file,
        .line = line,
        .last_operation = "allocated",
        .id = id,
        .next = NULL,
    };
    *link = entry;
    aligned_debug_unlock();
}

#endif

int ass_aligned_alloc_debug_enabled(void)
{
#if CONFIG_ALIGNED_ALLOC_DEBUG
    return 1;
#else
    return 0;
#endif
}

void *ass_aligned_alloc_impl(size_t alignment, size_t size, bool zero,
                             ASS_AlignedAllocCategory category,
                             const void *owner, const char *file, int line)
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
#if CONFIG_ALIGNED_ALLOC_DEBUG
    aligned_debug_track(ptr, allocation, size, alignment, category, owner,
                        file, line);
#else
    (void) category;
    (void) owner;
    (void) file;
    (void) line;
#endif
    return ptr;
}

void ass_aligned_free_impl(void *ptr, ASS_AlignedAllocCategory category,
                           const void *owner, const char *file, int line)
{
    if (!ptr)
        return;
#if CONFIG_ALIGNED_ALLOC_DEBUG
    aligned_debug_lock();
    AlignedDebugAllocation **link = aligned_debug_find_link(ptr);
    AlignedDebugAllocation *entry = *link;
    if (!entry) {
        const AlignedDebugFreed *freed = aligned_debug_find_freed(ptr);
        const AlignedDebugAllocation *owner_live =
            aligned_debug_find_owner(owner, category);
        const AlignedDebugFreed *owner_freed =
            aligned_debug_find_freed_owner(owner, category);
        aligned_debug_fail(freed ? "double free" : "untracked pointer",
                           ptr, category, owner, file, line, NULL, freed,
                           owner_live, owner_freed);
    }
    void *metadata_raw = *((void **) ptr - 1);
    if (metadata_raw != entry->raw)
        aligned_debug_fail("aligned header corrupted", ptr, category, owner,
                           file, line, entry, NULL, NULL, NULL);
    if (entry->owner && owner && entry->owner != owner)
        aligned_debug_fail("owner mismatch", ptr, category, owner,
                           file, line, entry, NULL, NULL, NULL);

    *link = entry->next;
    AlignedDebugFreed *freed =
        &aligned_debug_freed[aligned_debug_freed_cursor++ % ALIGNED_DEBUG_FREED_HISTORY];
    *freed = (AlignedDebugFreed) {
        .ptr = entry->ptr,
        .size = entry->size,
        .category = entry->category,
        .owner = entry->owner,
        .allocation_file = entry->file,
        .allocation_line = entry->line,
        .free_file = file,
        .free_line = line,
        .id = entry->id,
        .release_id = aligned_debug_next_release_id++,
    };
    if (!freed->release_id)
        freed->release_id = aligned_debug_next_release_id++;
    void *raw = entry->raw;
    aligned_debug_unlock();
    free(entry);
    free(raw);
#else
    (void) category;
    (void) owner;
    (void) file;
    (void) line;
    free(*((void **)ptr - 1));
#endif
}

void ass_aligned_retag(void *ptr, ASS_AlignedAllocCategory category,
                       const void *owner, const char *operation)
{
#if CONFIG_ALIGNED_ALLOC_DEBUG
    if (!ptr)
        return;
    aligned_debug_lock();
    AlignedDebugAllocation *entry = *aligned_debug_find_link(ptr);
    if (!entry)
        aligned_debug_fail("retag of untracked pointer", ptr, category, owner,
                           operation, 0, NULL, aligned_debug_find_freed(ptr),
                           aligned_debug_find_owner(owner, category),
                           aligned_debug_find_freed_owner(owner, category));
    entry->category = category;
    entry->owner = owner;
    entry->last_operation = operation;
    aligned_debug_unlock();
#else
    (void) ptr;
    (void) category;
    (void) owner;
    (void) operation;
#endif
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
    va_list va;
    va_start(va, fmt);
    priv->msg_callback(lvl, fmt, va, priv->msg_callback_data);
    va_end(va);
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

int ass_unicode_decimal_value(unsigned c)
{
    // Unicode 17.0, General_Category=Decimal_Number. Decimal digit sets are
    // contiguous, ordered zero through nine; a few adjacent sets are folded
    // into a single range and therefore use modulo 10 below.
    static const struct {
        unsigned first;
        unsigned last;
    } ranges[] = {
        { 0x0030, 0x0039 },
        { 0x0660, 0x0669 },
        { 0x06F0, 0x06F9 },
        { 0x07C0, 0x07C9 },
        { 0x0966, 0x096F },
        { 0x09E6, 0x09EF },
        { 0x0A66, 0x0A6F },
        { 0x0AE6, 0x0AEF },
        { 0x0B66, 0x0B6F },
        { 0x0BE6, 0x0BEF },
        { 0x0C66, 0x0C6F },
        { 0x0CE6, 0x0CEF },
        { 0x0D66, 0x0D6F },
        { 0x0DE6, 0x0DEF },
        { 0x0E50, 0x0E59 },
        { 0x0ED0, 0x0ED9 },
        { 0x0F20, 0x0F29 },
        { 0x1040, 0x1049 },
        { 0x1090, 0x1099 },
        { 0x17E0, 0x17E9 },
        { 0x1810, 0x1819 },
        { 0x1946, 0x194F },
        { 0x19D0, 0x19D9 },
        { 0x1A80, 0x1A89 },
        { 0x1A90, 0x1A99 },
        { 0x1B50, 0x1B59 },
        { 0x1BB0, 0x1BB9 },
        { 0x1C40, 0x1C49 },
        { 0x1C50, 0x1C59 },
        { 0xA620, 0xA629 },
        { 0xA8D0, 0xA8D9 },
        { 0xA900, 0xA909 },
        { 0xA9D0, 0xA9D9 },
        { 0xA9F0, 0xA9F9 },
        { 0xAA50, 0xAA59 },
        { 0xABF0, 0xABF9 },
        { 0xFF10, 0xFF19 },
        { 0x104A0, 0x104A9 },
        { 0x10D30, 0x10D39 },
        { 0x10D40, 0x10D49 },
        { 0x11066, 0x1106F },
        { 0x110F0, 0x110F9 },
        { 0x11136, 0x1113F },
        { 0x111D0, 0x111D9 },
        { 0x112F0, 0x112F9 },
        { 0x11450, 0x11459 },
        { 0x114D0, 0x114D9 },
        { 0x11650, 0x11659 },
        { 0x116C0, 0x116C9 },
        { 0x116D0, 0x116E3 },
        { 0x11730, 0x11739 },
        { 0x118E0, 0x118E9 },
        { 0x11950, 0x11959 },
        { 0x11BF0, 0x11BF9 },
        { 0x11C50, 0x11C59 },
        { 0x11D50, 0x11D59 },
        { 0x11DA0, 0x11DA9 },
        { 0x11DE0, 0x11DE9 },
        { 0x11F50, 0x11F59 },
        { 0x16130, 0x16139 },
        { 0x16A60, 0x16A69 },
        { 0x16AC0, 0x16AC9 },
        { 0x16B50, 0x16B59 },
        { 0x16D70, 0x16D79 },
        { 0x1CCF0, 0x1CCF9 },
        { 0x1D7CE, 0x1D7FF },
        { 0x1E140, 0x1E149 },
        { 0x1E2F0, 0x1E2F9 },
        { 0x1E4F0, 0x1E4F9 },
        { 0x1E5F1, 0x1E5FA },
        { 0x1E950, 0x1E959 },
        { 0x1FBF0, 0x1FBF9 },
    };

    if (c >= '0' && c <= '9')
        return (int) (c - '0');

    int lo = 1;
    int hi = (int) (sizeof(ranges) / sizeof(ranges[0]));
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (c < ranges[mid].first)
            hi = mid;
        else if (c > ranges[mid].last)
            lo = mid + 1;
        else
            return (int) ((c - ranges[mid].first) % 10);
    }
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
        int digit = ass_unicode_decimal_value(c);
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

int ass_strtou32_modulo_decimal(char **p, uint32_t *res)
{
    char *start = *p;
    char *q = start;
    int sign = 1;
    uint32_t value = 0;
    bool have_digit = false;

    skip_spaces(&q);
    if (*q == '+')
        q++;
    else if (*q == '-')
        sign = -1, q++;

    while (*q) {
        char *next = q;
        int digit = ass_unicode_decimal_value(ass_utf8_get_char(&next));
        if (digit < 0)
            break;
        value = value * 10 + (unsigned) digit;
        q = next;
        have_digit = true;
    }

    if (!have_digit) {
        *p = start;
        *res = 0;
        return 0;
    }

    *p = q;
    *res = sign < 0 ? 0u - value : value;
    return 1;
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
