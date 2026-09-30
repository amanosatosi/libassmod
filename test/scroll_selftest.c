#include <math.h>
#include <float.h>
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

static void directions(void)
{
    const double rows[] = {10, 20, 40, 80, 160, 320};
    const char *names[] = {"ue", "shita", "sita"};
    char source[256];
    for (size_t i = 0; i < 3; i++) {
        double sign = i ? -1 : 1;
        snprintf(source, sizeof(source), "%s,3000,6", names[i]);
        ASS_ScrollDefinition *d = parse(source, 300);
        CHECK(d && d->direction == (i ? ASS_SCROLL_DOWN : ASS_SCROLL_UP));
        CHECK(!strcmp(d->source, source) && ass_scroll_map(d, rows, 6));
        near(d, 2999, 0); near(d, 3000, 0); near(d, 3150, sign * 315);
        near(d, 3300, sign * 630); near(d, 3301, sign * 630);
        ass_scroll_free(d);

        snprintf(source, sizeof(source), "%s,1000,+3,2000|200,-1,3000,+2", names[i]);
        d = parse(source, 500);
        CHECK(d && d->count == 3 && ass_scroll_map(d, rows, 6));
        CHECK(d->cues[0].lines == 3 && d->cues[1].lines == -1);
        CHECK(d->cues[0].duration == 500 && d->cues[1].duration == 200 &&
              d->cues[2].duration == 500);
        CHECK(d->cues[0].distance == sign * 70 &&
              d->cues[1].distance == sign * -40 &&
              d->cues[2].distance == sign * 120);
        near(d, 1500, sign * 70); near(d, 2100, sign * 50);
        near(d, 2200, sign * 30); near(d, 3250, sign * 90);
        near(d, 3500, sign * 150);
        ass_scroll_free(d);

        snprintf(source, sizeof(source), "%s,3000|1000,+4,3500|500,-1", names[i]);
        d = parse(source, 300);
        CHECK(d && ass_scroll_map(d, rows, 6));
        near(d, 3500, sign * 75);
        near(d, 3750, sign * (150 * .75 - 80 * .5));
        near(d, 4000, sign * 70);
        ass_scroll_free(d);

        snprintf(source, sizeof(source), "%s,1000,-2147483648,2000,+2147483647,"
                 "3000,-2147483648,4000,+0,5000,-0,6000,+2", names[i]);
        d = parse(source, 0);
        CHECK(d && ass_scroll_map(d, rows, 6));
        near(d, 1000, 0); near(d, 2000, sign * 630);
        near(d, 3000, 0); near(d, 5000, 0); near(d, 6000, sign * 30);
        double *cached = d->advances;
        CHECK(ass_scroll_map(d, rows, 6) && cached == d->advances);
        const double bad[] = {DBL_MAX, DBL_MAX};
        CHECK(!ass_scroll_map(d, bad, 2) && d->advances == cached);
        near(d, 6000, sign * 30); // failed mapping leaves valid state intact
        CHECK(ass_scroll_map(d, NULL, 0)); near(d, 6000, 0);
        ass_scroll_free(d);
    }
    ASS_ScrollDefinition *d = parse(" shita , 1000 | 0 , +2 , 2000 , -1 ", 300);
    CHECK(d && ass_scroll_map(d, rows, 6));
    near(d, 1000, -30); near(d, 2150, -20);
    ass_scroll_free(d);
    d = parse("ue,2000,+3,1000|0,-1,5000,+2", 300);
    CHECK(d && ass_scroll_map(d, rows, 6));
    near(d, 1000, -40); near(d, 2300, 30); near(d, 5300, 150);
    ass_scroll_free(d);
}

int main(void)
{
    directions();
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
        "3000,abc", "3000|-5,6", "3000|abc,6",
        "3000|1|2,3", "3000,", ",3", "3000,2,", "2147483648,1",
        "1,2147483648", "1|2147483648,1", "1.5,1", "1,1.5",
        "ue", "shita", "sita", "ue,", "nope,3000,1", "UE,3000,1",
        "ue,3000", "ue,3000,1,5000", "ue,abc,1", "ue,3000,abc",
        "ue,3000|abc,1", "ue,3000|-5,1", "shita,-1,2", "sita,1,+",
        "ue,1,-", "ue,1,--1", "ue,1,+-1", "ue,1,-2147483649",
        "shita,1,+2147483648", "ue,1,+ 1", "ue,1,-1.5"};
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
    /* Signed oscillation must stay O(rows + cues) when mapping long lists. */
    used = snprintf(long_list, 20000, "sita,");
    for (int i = 0; i < 1024; i++)
        used += snprintf(long_list + used, 20000 - used, "%s%d|500,%s",
                         i ? "," : "", i, i % 2 ? "-1" : "+1");
    def = parse(long_list, 300);
    CHECK(def && def->count == 1024 && ass_scroll_map(def, rows, 64));
    near(def, 2000, 0);
    CHECK(def->cues[1022].distance == -10 && def->cues[1023].distance == 10);
    ass_scroll_free(def);
    free(long_list);
    return 0;
}
