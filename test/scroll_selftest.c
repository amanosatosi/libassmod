#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ass_scroll.h"

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); \
} } while (0)

static ASS_ScrollDefinition *parse(const char *text, int duration)
{
    return ass_scroll_parse(text, text + strlen(text), duration);
}

static void near(ASS_ScrollDefinition *def, int64_t time, double expected)
{
    CHECK(fabs(ass_scroll_evaluate(def, time) - expected) < 1e-9);
}

int main(void)
{
    double rows[64];
    for (int i = 0; i < 64; i++) rows[i] = 10;
    ASS_ScrollDefinition *def = parse("3000,6", 300);
    CHECK(def && def->count == 1 && def->cues[0].duration == 300);
    CHECK(ass_scroll_map(def, rows, 64));
    near(def, 2999, 0); near(def, 3000, 0); near(def, 3150, 30);
    near(def, 3300, 60); near(def, 3301, 60);
    double *cached = def->advances;
    CHECK(ass_scroll_map(def, rows, 64) && cached == def->advances);
    ass_scroll_free(def);

    def = parse("3000,6,5000,6", 300);
    CHECK(def && ass_scroll_map(def, rows, 64));
    near(def, 2999, 0); near(def, 3000, 0); near(def, 3300, 60);
    near(def, 4999, 60); near(def, 5000, 60);
    near(def, 5150, 90); near(def, 5300, 120);
    ass_scroll_free(def);

    def = parse("3000|1000,4", 17);
    CHECK(def && def->cues[0].duration == 1000 && ass_scroll_map(def, rows, 64));
    near(def, 3500, 20); near(def, 4000, 40);
    ass_scroll_free(def);

    def = parse("1000,1,2000,2,3000|600,3,4500,1,6000|1000,4,8000,2", 300);
    CHECK(def && def->count == 6 && ass_scroll_map(def, rows, 64));
    near(def, 1300, 10); near(def, 2300, 30); near(def, 3300, 45);
    near(def, 3600, 60); near(def, 4800, 70); near(def, 6500, 90);
    near(def, 7000, 110); near(def, 8300, 130);
    CHECK(def->cues[3].duration == 300 && def->cues[5].duration == 300);
    ass_scroll_free(def);

    def = parse("1000|1000,2,1200|500,3", 300);
    const double mixed[] = {10, 20, 40, 80, 160};
    CHECK(def && ass_scroll_map(def, mixed, 5));
    near(def, 1450, 30 * .45 + 280 * .5);
    near(def, 2000, 310);
    CHECK(def->cues[0].distance == 30 && def->cues[1].distance == 280);
    const double changed[] = {20, 20, 40, 80, 160};
    CHECK(ass_scroll_map(def, changed, 5));
    near(def, 2000, 320);
    ass_scroll_free(def);

    def = parse("3000,2", 500);
    CHECK(def && ass_scroll_map(def, mixed, 5));
    near(def, 3250, 15); near(def, 3500, 30);
    ass_scroll_free(def);
    def = parse("3000,2", 0);
    CHECK(def && ass_scroll_map(def, mixed, 5));
    near(def, 2999, 0); near(def, 3000, 30);
    ass_scroll_free(def);
    def = parse("2000,100,1000|0,2", 300);
    CHECK(def && ass_scroll_map(def, mixed, 5));
    near(def, 1000, 0); near(def, 2300, 310);
    CHECK(ass_scroll_map(def, NULL, 0)); near(def, 2300, 0);
    ass_scroll_free(def);

    const char *invalid[] = {"", "3000", "3000,6,5000", "abc,6",
        "3000,abc", "3000|-5,6", "3000|abc,6", "3000,-3",
        "3000|1|2,3", "3000,", ",3", "3000,0", "3000,2,", "2147483648,1",
        "1,2147483648", "1|2147483648,1", "1.5,1", "1,1.5"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++)
        CHECK(!parse(invalid[i], 300));
    int32_t value;
    CHECK(!ass_scroll_integer("", "", &value));
    const char negative[] = "-100";
    CHECK(!ass_scroll_integer(negative, negative + 4, &value));

    /* Far beyond the general parser's 33-argument cap. */
    char *long_list = malloc(20000);
    CHECK(long_list);
    size_t used = 0;
    for (int i = 0; i < 1024; i++)
        used += snprintf(long_list + used, 20000 - used, "%s%d|500,1", i ? "," : "", i);
    def = parse(long_list, 300);
    CHECK(def && def->count == 1024 && ass_scroll_map(def, rows, 64));
    near(def, 2000, 640);
    ass_scroll_free(def);
    free(long_list);
    return 0;
}
