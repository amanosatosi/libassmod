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
        "{\\chatmode1\\chatmode2\\msgm(Miku)\\msgtitle(Miku)"
        "\\msgtitle(Rin)}{\\msg(Miku)}A\\NB"
        "{\\msg(Yurf,right)}C{\\msg(Yurf)}D");
    assert(scene && scene->mode == 1 && scene->count == 3);
    assert(!strcmp(scene->title, "Miku"));
    assert(scene->show_names);
    expect_message(scene, 0, 1, "A\\NB");
    expect_message(scene, 1, 1, "C");
    expect_message(scene, 2, 0, "D");
    assert(!strcmp(scene->messages[0].speaker, "Miku"));
    expect_visible(scene, 0, 3, 3); /* no msgtime: static */
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode2\\chatmode1}{\\ta7}A\\N{\\ta9}B");
    assert(scene && scene->mode == 2 && scene->count == 2);
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode1(ignored)\\chatmode2}{\\ta7}A");
    assert(scene && scene->mode == 2);
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
        "{\\chatmode2\\msgtitle(Miku)\\msgstartcount(1)"
        "\\msgtime(1200,2400,3000,3900)\\msganim(250)}"
        "{\\ta7}Hey\\N{\\ta9}What\\N{\\ta7}Look at this"
        "\\N{|}This shit crazy\\N{\\ta9}💀");
    assert(scene && scene->mode == 2 && scene->count == 5);
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

    scene = ass_chat_parse("{\\chatmode2}{\\ta7}A\\NB");
    assert(scene && scene->count == 1);
    assert(strstr(scene->messages[0].text, "A\\NB"));
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode2}{\\ta7}A\\N{|}B\\N{|}C"
                           "\\N{\\ta9}D\\N{|}E");
    assert(scene && scene->count == 5);
    expect_message(scene, 0, 0, "A");
    expect_message(scene, 1, 0, "B");
    expect_message(scene, 2, 0, "C");
    expect_message(scene, 3, 1, "D");
    expect_message(scene, 4, 1, "E");
    ass_chat_free(scene);

    scene = ass_chat_parse("{\\chatmode2\\msgstartcount(0)"
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

    scene = ass_chat_parse("{\\chatmode2\\msgstartcount(1)"
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

    puts("chat syntax/timing tests passed");
    return 0;
}
