/*
 * Copyright (C) 2026 libass contributors
 * SPDX-License-Identifier: ISC
 */

#include "config.h"
#include "ass_compat.h"

#include <math.h>
#include <string.h>

#include "ass_expression.h"
#include "ass_utils.h"

enum {
    CONST, VARIABLE, PERCENT, FROM_END, NEGATE, ADD, SUB, MUL, DIV,
};

static const char *const variables[] = {
    "start", "mid", "end", "dur", "width", "height", "left", "right",
    "center", "top", "bottom", "middle",
};

static bool ascii_digit(char c) { return c >= '0' && c <= '9'; }
static bool name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           ascii_digit(c) || c == '_';
}

static void spaces(const char **p, const char *end)
{
    while (*p < end && (**p == ' ' || **p == '\t'))
        ++*p;
}

static int variable_id(const char *p, const char *end)
{
    for (int i = 0; i < EXPR_VARIABLES; i++)
        if (strlen(variables[i]) == (size_t) (end - p) &&
                !memcmp(p, variables[i], end - p))
            return i;
    return -1;
}

bool ass_expr_uses_geometry(const char *start, const char *end)
{
    for (const char *p = start; p < end; p++) {
        if (*p != '$')
            continue;
        const char *name = p + 1, *next = name;
        while (next < end && name_char(*next))
            next++;
        if (variable_id(name, next) >= EXPR_WIDTH)
            return true;
        p = next - 1;
    }
    return false;
}

typedef struct {
    const char *p, *end;
    ASS_Expression *out;
    bool percent;
} Parser;

static bool emit(Parser *p, uint8_t op, uint8_t var, double value)
{
    if (p->out->count == ASS_EXPR_MAX_OPS)
        return false;
    p->out->code[p->out->count++] = (ASS_ExprOp) {value, op, var};
    return true;
}

static bool number(Parser *p, double *value)
{
    const char *start = p->p;
    const char *end = p->end;
    const char *s = start;
    while (s < end && ascii_digit(*s))
        s++;
    bool digits = s != start;
    if (s < end && *s == '.') {
        const char *fraction = ++s;
        while (s < end && ascii_digit(*s))
            s++;
        digits |= s != fraction;
    }
    if (!digits)
        return false;
    if (s < end && (*s == 'e' || *s == 'E')) {
        s++;
        if (s < end && (*s == '-' || *s == '+'))
            s++;
        const char *exponent = s;
        while (s < end && ascii_digit(*s))
            s++;
        if (s == exponent)
            return false;
    }
    char text[ASS_EXPR_MAX_LENGTH + 1], *tail;
    memcpy(text, start, s - start);
    text[s - start] = 0;
    *value = ass_strtod(text, &tail);
    if (tail != text + (s - start) || !isfinite(*value))
        return false;
    p->p = s;
    return true;
}

static bool negative_percentage_literal(const char *start, const char *end)
{
    spaces(&start, end);
    if (start == end || *start++ != '-')
        return false;
    spaces(&start, end);
    Parser p = {.p = start, .end = end};
    double value;
    if (!number(&p, &value))
        return false;
    spaces(&p.p, end);
    if (p.p == end || *p.p++ != '%')
        return false;
    spaces(&p.p, end);
    return p.p == end;
}

static bool sum(Parser *p, int depth);

static bool primary(Parser *p, int depth)
{
    spaces(&p->p, p->end);
    if (p->p == p->end || depth == ASS_EXPR_MAX_DEPTH)
        return false;
    char c = *p->p;
    if (c == '-' || c == '+') {
        p->p++;
        return primary(p, depth + 1) && (c == '+' || emit(p, NEGATE, 0, 0));
    }
    if (c == '(') {
        p->p++;
        if (!sum(p, depth + 1) || p->p == p->end || *p->p != ')')
            return false;
        p->p++;
        return true;
    }
    if (c == '$') {
        const char *name = ++p->p;
        while (p->p < p->end && name_char(*p->p))
            p->p++;
        int var = variable_id(name, p->p);
        if (var < 0)
            return false;
        p->out->geometry |= var >= EXPR_WIDTH;
        return emit(p, VARIABLE, var, 0);
    }
    double value;
    if (!number(p, &value))
        return false;
    spaces(&p->p, p->end);
    if (p->percent && p->p < p->end && *p->p == '%') {
        p->p++;
        return emit(p, PERCENT, 0, value / 100);
    }
    return emit(p, CONST, 0, value);
}

static bool product(Parser *p, int depth)
{
    if (!primary(p, depth))
        return false;
    for (;;) {
        spaces(&p->p, p->end);
        if (p->p == p->end || (*p->p != '*' && *p->p != '/'))
            return true;
        char op = *p->p++;
        if (!primary(p, depth) || !emit(p, op == '*' ? MUL : DIV, 0, 0))
            return false;
    }
}

static bool sum(Parser *p, int depth)
{
    if (!product(p, depth))
        return false;
    while (p->p < p->end && (*p->p == '+' || *p->p == '-')) {
        char op = *p->p++;
        if (!product(p, depth) || !emit(p, op == '+' ? ADD : SUB, 0, 0))
            return false;
    }
    return true;
}

bool ass_expr_compile(const char *start, const char *end, bool percent,
                      ASS_Expression *out)
{
    out->count = 0;
    out->geometry = false;
    if (start >= end || end - start > ASS_EXPR_MAX_LENGTH)
        return false;
    Parser p = {start, end, out, percent};
    if (!sum(&p, 0) || p.p != end) {
        out->count = 0;
        return false;
    }
    // Grouped or repeated unary minus is arithmetic, even around a literal.
    if (percent && out->count == 2 &&
            out->code[0].op == PERCENT && out->code[1].op == NEGATE &&
            negative_percentage_literal(start, end)) {
        out->code[0].op = FROM_END;
        out->count = 1;
    }
    return true;
}

static double variable_value(int id, double duration, const ASS_ExprBounds *b)
{
    switch (id) {
    case EXPR_START: return 0;
    case EXPR_MID: return duration * 0.5;
    case EXPR_END: case EXPR_DURATION: return duration;
    case EXPR_WIDTH: return b->right - b->left;
    case EXPR_HEIGHT: return b->bottom - b->top;
    case EXPR_LEFT: return b->left;
    case EXPR_RIGHT: return b->right;
    case EXPR_CENTER: return (b->right + b->left) * 0.5;
    case EXPR_TOP: return b->top;
    case EXPR_BOTTOM: return b->bottom;
    case EXPR_MIDDLE: return (b->top + b->bottom) * 0.5;
    default: return 0;
    }
}

bool ass_expr_eval(const ASS_Expression *expr, double duration,
                   const ASS_ExprBounds *bounds, double *out)
{
    if (expr->geometry && !bounds->valid)
        return false;
    double stack[ASS_EXPR_MAX_OPS];
    int n = 0;
    for (int i = 0; i < expr->count; i++) {
        ASS_ExprOp op = expr->code[i];
        double value = op.number;
        switch (op.op) {
        case VARIABLE: value = variable_value(op.variable, duration, bounds); break;
        case PERCENT: value *= duration; break;
        case FROM_END: value = duration - value * duration; break;
        case NEGATE: value = -stack[--n]; break;
        case ADD: n--; value = stack[n - 1] + stack[n]; n--; break;
        case SUB: n--; value = stack[n - 1] - stack[n]; n--; break;
        case MUL: n--; value = stack[n - 1] * stack[n]; n--; break;
        case DIV:
            n--;
            if (stack[n] == 0)
                return false;
            value = stack[n - 1] / stack[n];
            n--;
            break;
        }
        if (!isfinite(value))
            return false;
        stack[n++] = value;
    }
    if (n != 1)
        return false;
    *out = stack[0];
    return true;
}
