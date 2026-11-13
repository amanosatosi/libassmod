/*
 * Copyright (C) 2026 libass contributors
 * SPDX-License-Identifier: ISC
 */
#ifndef LIBASS_EXPRESSION_H
#define LIBASS_EXPRESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ASS_EXPR_MAX_LENGTH 256
#define ASS_EXPR_MAX_OPS 64
#define ASS_EXPR_MAX_DEPTH 16

typedef enum {
    EXPR_START, EXPR_MID, EXPR_END, EXPR_DURATION,
    EXPR_WIDTH, EXPR_HEIGHT, EXPR_LEFT, EXPR_RIGHT, EXPR_CENTER,
    EXPR_TOP, EXPR_BOTTOM, EXPR_MIDDLE, EXPR_VARIABLES,
} ASS_ExprVariable;

typedef struct {
    double number;
    uint8_t op, variable;
} ASS_ExprOp;

typedef struct {
    uint8_t count;
    bool geometry;
    ASS_ExprOp code[ASS_EXPR_MAX_OPS];
} ASS_Expression;

typedef struct {
    double left, right, top, bottom;
    bool valid;
} ASS_ExprBounds;

typedef struct {
    bool valid, percent;
    uint16_t length;
    char text[ASS_EXPR_MAX_LENGTH];
    ASS_Expression expr;
} ASS_ExprCacheEntry;

/* Keep typical literal operands on a short local scan. */
static inline bool ass_expr_candidate(const char *start, const char *end, bool percent)
{
    if (!start)
        return false;
    while (start < end && (*start == ' ' || *start == '\t'))
        start++;
    for (const char *p = start; p < end; p++) {
        switch (*p) {
        case '$': case '*': case '/': case '(': case ')':
            return true;
        case '%':
            if (percent)
                return true;
            break;
        case '+': case '-':
            if (p > start && !(p == start + 1 && *start == '~') &&
                    !((p[-1] == 'e' || p[-1] == 'E') && p > start + 1 &&
                      ((p[-2] >= '0' && p[-2] <= '9') || p[-2] == '.')))
                return true;
            break;
        }
    }
    return false;
}

bool ass_expr_uses_geometry(const char *start, const char *end);
bool ass_expr_compile(const char *start, const char *end, bool percent,
                      ASS_Expression *out);
bool ass_expr_eval(const ASS_Expression *expr, double duration,
                   const ASS_ExprBounds *bounds, double *out);

#endif
