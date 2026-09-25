/* Native chat scene syntax shared by the renderer and parser tests. */
#ifndef LIBASS_ASS_CHAT_H
#define LIBASS_ASS_CHAT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ASS_CHAT_MODE_EXPLICIT = 1,
    ASS_CHAT_MODE_NAMED_LAZY = 2,
    ASS_CHAT_MODE_ALIGNMENT_SHORTHAND = 3,
} ASS_ChatMode;

typedef struct {
    char *text;                 /* ASS text, including ordinary inline tags */
    char *speaker;
    int side;                   /* 0 = left, 1 = right */
    int64_t reveal_ms;          /* INT64_MAX means no assigned reveal */
} ASS_ChatMessage;

typedef struct ass_chat_scene {
    ASS_ChatMode mode;
    char *title;
    char *main_speaker;
    bool show_names;
    bool has_time;
    int start_count;
    int animation_ms;
    ASS_ChatMessage *messages;
    int count;
    int capacity;
    char *prefix;              /* formatting before the first message */
} ASS_ChatScene;

ASS_ChatScene *ass_chat_parse(const char *source);
void ass_chat_free(ASS_ChatScene *scene);

/* The previous visible count and progress describe one grouped transition. */
int ass_chat_visible(const ASS_ChatScene *scene, int64_t event_ms,
                     int *previous, double *progress);

#endif
