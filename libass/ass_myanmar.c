#include "ass_myanmar.h"

static bool consonant(uint32_t c)
{
    return (c >= 0x1000 && c <= 0x1021) ||
           (c >= 0x1023 && c <= 0x1027) || c == 0x1029 || c == 0x102a ||
           c == 0x103f || c == 0x104e;
}

static bool mark(uint32_t c)
{
    return (c >= 0x102b && c <= 0x103e) ||
           (c >= 0x1056 && c <= 0x1059) ||
           (c >= 0x1060 && c <= 0x109d) ||
           (c >= 0xaa7b && c <= 0xaa7d);
}

bool ass_myanmar_layout_break(const uint32_t *text, int length, int index)
{
    if (!text || index <= 0 || index >= length)
        return false;
    uint32_t c = text[index], prev = text[index - 1];
    if (!consonant(c))
        return !mark(c) && c != 0x200c && c != 0x200d;
    /* A subjoined consonant and the consonant in a kinzi prefix stay with
     * their base. Final consonant + asat is the coda of the same syllable. */
    if (prev == 0x1039 || prev == 0x200d)
        return false;
    if (index + 1 < length && text[index + 1] == 0x103a)
        return false;
    return true;
}
