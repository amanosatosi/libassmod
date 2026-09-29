#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "ass_myanmar.h"

static bool check(const uint32_t *text, int length,
                  const int *expected, int count, const char *name)
{
    int seen = 0;
    for (int i = 1; i < length; i++) {
        if (!ass_myanmar_layout_break(text, length, i))
            continue;
        if (seen >= count || expected[seen] != i) {
            fprintf(stderr, "%s: unexpected syllable break at %d\n", name, i);
            return false;
        }
        seen++;
    }
    if (seen != count)
        fprintf(stderr, "%s: got %d syllables, expected %d\n",
                name, seen + 1, count + 1);
    return seen == count;
}

int main(void)
{
    static const uint32_t myanmar[] =
        {0x1019, 0x103c, 0x1014, 0x103a, 0x1019, 0x102c};
    static const uint32_t country[] =
        {0x1019, 0x103c, 0x1014, 0x103a, 0x1019, 0x102c,
         0x1014, 0x102d, 0x102f, 0x1004, 0x103a, 0x1004, 0x1036};
    static const uint32_t school[] =
        {0x1000, 0x103b, 0x1031, 0x102c, 0x1004, 0x103a, 0x1038};
    static const uint32_t kinzi[] =
        {0x1021, 0x1004, 0x103a, 0x1039, 0x1002,
         0x101c, 0x102d, 0x1015, 0x103a};
    static const uint32_t stack[] =
        {0x1000, 0x1039, 0x1000, 0x102d, 0x1037, 0x1019, 0x102c};
    static const uint32_t malformed[] =
        {0x103b, 0x1037, 0x1000, 0x1039, 0x103a};
    static const int country_breaks[] = {4, 6, 11};
    static const int kinzi_breaks[] = {5};
    static const int stack_breaks[] = {5};
    static const int malformed_breaks[] = {2};
    bool ok = true;
    ok &= check(myanmar, 6, (const int[]){4}, 1, "မြန်မာ");
    ok &= check(country, 13, country_breaks, 3, "မြန်မာနိုင်ငံ");
    ok &= check(school, 7, NULL, 0, "medial/coda/tone");
    ok &= check(kinzi, 9, kinzi_breaks, 1, "kinzi");
    ok &= check(stack, 7, stack_breaks, 1, "subjoined consonant");
    ok &= check(malformed, 5, malformed_breaks, 1, "broken prefix");
    return ok ? 0 : 1;
}
