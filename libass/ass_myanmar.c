#include "ass_myanmar.h"

typedef enum {
    MY_OTHER, MY_BASE, MY_MARK, MY_ASAT, MY_VIRAMA, MY_JOINER
} MyanmarCategory;

static MyanmarCategory category(uint32_t c)
{
    if (c == 0x103a)
        return MY_ASAT;
    if (c == 0x1039)
        return MY_VIRAMA;
    if (c == 0x200c || c == 0x200d)
        return MY_JOINER;
    if ((c >= 0x1000 && c <= 0x1021) ||
            (c >= 0x1023 && c <= 0x1027) ||
            c == 0x1029 || c == 0x102a || c == 0x103f || c == 0x104e)
        return MY_BASE;
    if ((c >= 0x102b && c <= 0x103e) ||
            (c >= 0x1056 && c <= 0x1059) ||
            (c >= 0x1060 && c <= 0x109d) ||
            (c >= 0xaa7b && c <= 0xaa7d))
        return MY_MARK;
    return MY_OTHER;
}

/* A final consonant can carry tone/dependent marks before its asat, as in
 * မင့် (မ + င + ့ + ်).  Stop at the next base or unrelated symbol, so a
 * malformed mark run cannot consume the next syllable. */
static bool final_consonant(const uint32_t *text, int length, int index)
{
    for (int i = index + 1; i < length; i++) {
        MyanmarCategory next = category(text[i]);
        if (next == MY_ASAT)
            return true;
        if (next != MY_MARK && next != MY_JOINER)
            break;
    }
    return false;
}

bool ass_myanmar_layout_break(const uint32_t *text, int length, int index)
{
    if (!text || index <= 0 || index >= length)
        return false;
    MyanmarCategory cur = category(text[index]);
    MyanmarCategory prev = category(text[index - 1]);
    if (cur == MY_MARK || cur == MY_ASAT || cur == MY_VIRAMA ||
            cur == MY_JOINER)
        return false;
    if (cur != MY_BASE)
        return true;
    if (prev == MY_VIRAMA || prev == MY_JOINER)
        return false;
    if (prev == MY_OTHER)
        return true;
    return !final_consonant(text, length, index);
}
