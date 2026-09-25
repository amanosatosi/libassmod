#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../libass/ass_chat.h"

#undef assert
#define assert(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        abort(); \
    } \
} while (0)

static void expect_message(const ASS_ChatScene *scene, int index,
                           int side, const char *body)
{
    assert(index < scene->count);
    assert(scene->messages[index].side == side);
    assert(strstr(scene->messages[index].text, body));
}

static void expect_named(const ASS_ChatScene *scene, int index, int side,
                         const char *speaker, const char *body)
{
    assert(index < scene->count);
    assert(scene->messages[index].side == side);
    assert(scene->messages[index].speaker);
    assert(!strcmp(scene->messages[index].speaker, speaker));
    assert(!strcmp(scene->messages[index].text, body));
}

static void expect_visible(const ASS_ChatScene *scene, int64_t time,
                           int count, int previous)
{
    int prior = -1;
    double progress = -1.0;
    assert(ass_chat_visible(scene, time, &prior, &progress) == count);
    assert(prior == previous);
    assert(progress >= 0.0 && progress <= 1.0);
}

int main(void)
{
    assert(!ass_chat_parse("{\\ta7}ordinary\\Nsubtitle{|}"));

    ASS_ChatScene *scene = ass_chat_parse(
        "{\\chatmode1\\chatmode3\\msgm(Miku)\\msgtitle(Miku)"
        "\\msgtitle(Rin)}{\\msg(Miku)}A\\NB"
        "{\\msg(Yurf,right)}C{\\msg(Yurf)}D");
    assert(scene && scene->mode == ASS_CHAT_MODE_EXPLICIT && scene->count == 3);
    assert(!strcmp(scene->title, "Miku"));
    assert(scene->show_names);
    expect_message(scene, 0, 1, "A\\NB");
    expect_message(scene, 1, 1, "C");
    expect_message(scene, 2, 0, "D");
    assert(!strcmp(scene->messages[0].speaker, "Miku"));
    expect_visible(scene, 0, 3, 3); /* no msgtime: static */
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode3\\chatmode1}{\\ta7}A\\N{\\ta9}B");
    assert(scene && scene->mode == ASS_CHAT_MODE_ALIGNMENT_SHORTHAND &&
           scene->count == 2);
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode1(ignored)\\chatmode3}{\\ta7}A");
    assert(scene && scene->mode == ASS_CHAT_MODE_ALIGNMENT_SHORTHAND);
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode1\\msgtitle()\\msgtitle(Rin)}"
                           "{\\msg(A)}A{\\msg(B,right)}B");
    assert(scene && scene->title && !*scene->title);
    expect_message(scene, 0, 0, "A");
    expect_message(scene, 1, 1, "B");
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode1\\msgtitle(Miku}"
                           "{\\msgtitle(Rin)}{\\msg(A)}A");
    assert(scene && scene->title && !strcmp(scene->title, "Rin"));
    ass_chat_free(scene);

    scene = ass_chat_parse(
        "{\\chatmode3\\msgtitle(Miku)\\msgstartcount(1)"
        "\\msgtime(1200,2400,3000,3900)\\msganim(250)}"
        "{\\ta7}Hey\\N{\\ta9}What\\N{\\ta7}Look at this"
        "\\N{|}This shit crazy\\N{\\ta9}💀");
    assert(scene && scene->mode == ASS_CHAT_MODE_ALIGNMENT_SHORTHAND &&
           scene->count == 5);
    expect_message(scene, 0, 0, "Hey");
    expect_message(scene, 1, 1, "What");
    expect_message(scene, 2, 0, "Look at this");
    expect_message(scene, 3, 0, "This shit crazy");
    expect_message(scene, 4, 1, "💀");
    assert(!strstr(scene->messages[0].text, "\\N"));
    expect_visible(scene, 0, 1, 1);
    expect_visible(scene, 1199, 1, 1);
    expect_visible(scene, 1200, 2, 1);
    expect_visible(scene, 2400, 3, 2);
    expect_visible(scene, 3000, 4, 3);
    expect_visible(scene, 3900, 5, 4);
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode3}{\\ta7}A\\NB");
    assert(scene && scene->count == 1);
    assert(strstr(scene->messages[0].text, "A\\NB"));
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode3}{\\ta7}A\\N{|}B\\N{|}C"
                           "\\N{\\ta9}D\\N{|}E");
    assert(scene && scene->count == 5);
    expect_message(scene, 0, 0, "A");
    expect_message(scene, 1, 0, "B");
    expect_message(scene, 2, 0, "C");
    expect_message(scene, 3, 1, "D");
    expect_message(scene, 4, 1, "E");
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode3\\msgstartcount(0)"
                           "\\msgtime(-20,1000,1000,200)}"
                           "{\\ta7}A\\N{|}B\\N{|}C\\N{|}D\\N{|}E");
    assert(scene && scene->count == 5);
    assert(scene->messages[0].reveal_ms == 0);
    assert(scene->messages[1].reveal_ms == 1000);
    assert(scene->messages[2].reveal_ms == 1000);
    assert(scene->messages[3].reveal_ms == 1000);
    assert(scene->messages[4].reveal_ms == INT64_MAX);
    expect_visible(scene, -1, 0, 0);
    expect_visible(scene, 0, 1, 0);
    expect_visible(scene, 1000, 4, 1);
    expect_visible(scene, 9999, 4, 1);
    expect_visible(scene, INT64_MAX, 4, 1);
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode1\\msgshowname0\\msganim(0)"
                           "\\msgstartcount(99)\\msgtime(1000)}"
                           "{\\msg(A)}A{\\msg(B,left)}B");
    assert(scene && !scene->show_names && scene->start_count == 2);
    expect_visible(scene, 0, 2, 2);
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode1\\msgm(A)\\msgshowname0}"
                           "{\\msg(A)}A{\\msg(B)}B");
    assert(scene && !scene->show_names);
    expect_message(scene, 0, 1, "A");
    expect_message(scene, 1, 0, "B");
    assert(!strcmp(scene->messages[0].speaker, "A"));
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode3\\msgstartcount(1)"
                           "\\msgtime(1000,bad,2000,3000)\\msganim(0)}"
                           "{|}A\\N{|}B\\N{|}C");
    assert(scene && scene->count == 3);
    expect_message(scene, 0, 0, "A");
    expect_visible(scene, 1000, 2, 1);
    double progress = 0.0;
    ass_chat_visible(scene, 1000, NULL, &progress);
    assert(progress == 1.0);
    assert(scene->messages[2].reveal_ms == 2000);
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode2\\chatmode3\\msgm(Miku)}"
                           "|Miku:\\NHello|\\N\\N|Yurf:\\NYo|");
    assert(scene && scene->mode == ASS_CHAT_MODE_NAMED_LAZY &&
           scene->count == 2);
    expect_named(scene, 0, 1, "Miku", "Hello");
    expect_named(scene, 1, 0, "Yurf", "Yo");
    expect_visible(scene, 0, 2, 2);
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode3\\chatmode2}{\\ta7}A\\N{\\ta9}B");
    assert(scene && scene->mode == ASS_CHAT_MODE_ALIGNMENT_SHORTHAND &&
           scene->count == 2);
    ass_chat_free(scene);

    const char *separators[] = {"", "\\N", "\\N\\N", "  \\N  "};
    for (size_t i = 0; i < sizeof(separators) / sizeof(*separators); i++) {
        char source[160];
        snprintf(source, sizeof(source),
                 "{\\chatmode2\\msgm(A)}|A:\\None|%s|B:\\Ntwo|",
                 separators[i]);
        scene = ass_chat_parse(source);
        assert(scene && scene->count == 2);
        expect_named(scene, 0, 1, "A", "one");
        expect_named(scene, 1, 0, "B", "two");
        ass_chat_free(scene);
    }

    scene = ass_chat_parse("{\\chatmode2\\msgm(Miku)}"
                           "|Miku:\\NOne|\\N\\N|\\NTwo|\\N\\N|\\NThree|");
    assert(scene && scene->count == 3);
    expect_named(scene, 0, 1, "Miku", "One");
    expect_named(scene, 1, 1, "Miku", "Two");
    expect_named(scene, 2, 1, "Miku", "Three");
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode2\\msgm(A)}|A:\\N0|"
                           "|\\N1||\\N2||\\N3||\\N4||\\N5|"
                           "|\\N6||\\N7||\\N8||\\N9|");
    assert(scene && scene->count == 10);
    expect_named(scene, 9, 1, "A", "9");
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode2\\msgm(初音ミク)}"
                           "| 初音ミク　:\\Nこんにちは|\\N\\N|鏡音リン:\\Nやっほー|");
    assert(scene && scene->count == 2);
    expect_named(scene, 0, 1, "初音ミク", "こんにちは");
    expect_named(scene, 1, 0, "鏡音リン", "やっほー");
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode2\\msgm(Miku)\\msgshowname0}"
                           "|{\\c&HFFFFFF&\\2c&H39C5BB&\\3c&H303030&}"
                           "Miku:\\NHello|\\N|Yurf:\\N{\\fs60}BIG|");
    assert(scene && scene->count == 2 && !scene->show_names);
    expect_named(scene, 0, 1, "Miku",
                 "{\\c&HFFFFFF&\\2c&H39C5BB&\\3c&H303030&}Hello");
    expect_named(scene, 1, 0, "Yurf", "{\\fs60}BIG");
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode2\\msgshowname1}"
                           "|A:\\None||B:\\Ntwo|");
    assert(scene && scene->show_names && scene->count == 2);
    expect_named(scene, 0, 0, "A", "one");
    expect_named(scene, 1, 0, "B", "two");
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode2\\msgm(Miku)\\msgstartcount(1)"
                           "\\msgtime(1200,2400)\\msganim(250)}"
                           "|Miku:\\NA|\\N\\N|Yurf:\\NB|\\N\\N|Miku:\\NC|");
    assert(scene && scene->count == 3);
    expect_visible(scene, 0, 1, 1);
    expect_visible(scene, 1200, 2, 1);
    expect_visible(scene, 2400, 3, 2);
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode2\\msgm(Miku)}"
                           "|Miku:\\N<今日|きょう>と<明日|あした>はどう？|"
                           "\\N\\N|Yurf:\\Nいいよ|");
    assert(scene && scene->count == 2);
    expect_named(scene, 0, 1, "Miku", "<今日|きょう>と<明日|あした>はどう？");
    expect_named(scene, 1, 0, "Yurf", "いいよ");
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode2\\msgm(Miku)}"
                           "|{\\fnA|B}Miku:\\NHi \\| there|");
    assert(scene && scene->count == 1);
    expect_named(scene, 0, 1, "Miku", "{\\fnA|B}Hi \\| there");
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode2\\msgm(Miku)}"
                           "|Miku:\\N{\\ta9}A{|}B|");
    assert(scene && scene->count == 1);
    expect_named(scene, 0, 1, "Miku", "{\\ta9}A{|}B");
    ass_chat_free(scene);

    const char *malformed[] = {
        "|Miku Hello|", "|Miku:\\NHello", "Miku:\\NHello|", "||", "|"
    };
    for (size_t i = 0; i < sizeof(malformed) / sizeof(*malformed); i++) {
        char source[128];
        snprintf(source, sizeof(source), "{\\chatmode2}%s", malformed[i]);
        scene = ass_chat_parse(source);
        assert(scene && scene->count == 0);
        ass_chat_free(scene);
    }
    scene = ass_chat_parse("{\\chatmode2}|\\Nfallback|");
    assert(scene && scene->count == 1 && !scene->messages[0].speaker &&
           scene->messages[0].side == 0 &&
           !strcmp(scene->messages[0].text, "fallback"));
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode2}{\\ta7}A\\N{\\ta9}B");
    assert(scene && scene->mode == ASS_CHAT_MODE_NAMED_LAZY &&
           scene->count == 0);
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode3}|Miku:\\NHello|");
    assert(scene && scene->mode == ASS_CHAT_MODE_ALIGNMENT_SHORTHAND &&
           scene->count == 1 && !scene->messages[0].speaker);
    assert(strstr(scene->messages[0].text, "|Miku:"));
    ass_chat_free(scene);

    assert(!ass_chat_parse("|Miku:\\NHello|"));

    puts("chat syntax/timing tests passed");
    return 0;
}
