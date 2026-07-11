/*
 * Copyright (C) 2006 Evgeniy Stepanov <eugeni.stepanov@gmail.com>
 * Copyright (C) 2009 Grigori Goronzy <greg@geekmind.org>
 * Copyright (C) 2013 rcombs <rcombs@rcombs.me>
 *
 * This file is part of libass.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <limits.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif
#include "../libass/ass.h"

typedef struct image_s {
    int width, height, stride;
    unsigned char *buffer;      // RGB24
} image_t;

ASS_Library *ass_library;
ASS_Renderer *ass_renderer;

void msg_callback(int level, const char *fmt, va_list va, void *data)
{
    if (level > 6)
        return;
    printf("libass: ");
    vprintf(fmt, va);
    printf("\n");
}

static double wall_time(void)
{
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (!frequency.QuadPart)
        QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double) counter.QuadPart / frequency.QuadPart;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
#endif
}

static unsigned init(int frame_w, int frame_h, unsigned threads)
{
    ass_library = ass_library_init();
    if (!ass_library) {
        printf("ass_library_init failed!\n");
        exit(1);
    }

    ass_set_message_cb(ass_library, msg_callback, NULL);
    ass_set_extract_fonts(ass_library, 1);

    ass_renderer = ass_renderer_init(ass_library);
    if (!ass_renderer) {
        printf("ass_renderer_init failed!\n");
        exit(1);
    }

    unsigned effective_threads = threads == 1 ? 1 :
        ass_set_threads(ass_renderer, threads);
    if (!effective_threads)
        effective_threads = 1;

    ass_set_storage_size(ass_renderer, frame_w, frame_h);
    ass_set_frame_size(ass_renderer, frame_w, frame_h);
    ass_set_fonts(ass_renderer, NULL, "Sans", 1, NULL, 1);
    return effective_threads;
}

int main(int argc, char *argv[])
{
    const int frame_w = 1280;
    const int frame_h = 720;

    if (argc < 5 || argc > 7) {
        printf("usage: %s <subtitle file> <start time> <fps> <end time> "
               "[threads] [alpha|rgba]\n"
               "       threads=0 selects automatic concurrency\n",
               argv[0] ? argv[0] : "profile");
        exit(1);
    }
    char *subfile = argv[1];
    double tm = strtod(argv[2], 0);
    double fps = strtod(argv[3], 0);
    double end_time = strtod(argv[4], 0);
    unsigned threads = 1;
    if (argc >= 6) {
        char *end;
        unsigned long value = strtoul(argv[5], &end, 10);
        if (!*argv[5] || argv[5][0] == '-' || *end || value > UINT_MAX) {
            printf("invalid thread count: %s\n", argv[5]);
            exit(1);
        }
        threads = (unsigned) value;
    }
    bool rgba = argc == 7 && !strcmp(argv[6], "rgba");
    if (argc == 7 && !rgba && strcmp(argv[6], "alpha")) {
        printf("invalid output mode: %s\n", argv[6]);
        exit(1);
    }

    if (fps == 0) {
        printf("fps cannot equal 0\n");
        exit(1);
    }

    unsigned effective_threads = init(frame_w, frame_h, threads);
    ASS_Track *track = ass_read_file(ass_library, subfile, NULL);
    if (!track) {
        printf("track init failed!\n");
        exit(1);
    }

    double cold_time = 0;
    double warm_time = 0;
    size_t frames = 0;
    while (tm < end_time) {
        ASS_ImageRGBA *rgba_images = NULL;
        double start = wall_time();
        if (rgba)
            rgba_images = ass_render_frame_rgba(
                ass_renderer, track, (int) (tm * 1000), NULL);
        else
            ass_render_frame(ass_renderer, track, (int) (tm * 1000), NULL);
        double elapsed = wall_time() - start;
        ass_free_images_rgba(rgba_images);
        if (!frames)
            cold_time = elapsed;
        else
            warm_time += elapsed;
        frames++;
        tm += 1 / fps;
    }

    printf("mode: %s\nthreads: requested=%u effective=%u\n",
           rgba ? "rgba" : "alpha", threads, effective_threads);
    if (frames) {
        printf("cold: %.3f ms (1 frame)\n", cold_time * 1000);
        if (frames > 1)
            printf("warm: %.3f ms/frame (%zu frames, %.3f ms total)\n",
                   warm_time * 1000 / (frames - 1), frames - 1,
                   warm_time * 1000);
        else
            printf("warm: n/a (no remaining frames)\n");
    } else {
        printf("no frames rendered\n");
    }

    ass_free_track(track);
    ass_renderer_done(ass_renderer);
    ass_library_done(ass_library);

    return 0;
}
