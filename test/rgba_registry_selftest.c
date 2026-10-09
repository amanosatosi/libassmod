/* SPDX-License-Identifier: ISC
 * Concurrent debug-registry insertion, lookup, ownership, replacement and
 * removal on independent renderers. Rendezvous make live counts deterministic;
 * no sleeps or assumptions about which worker is scheduled first are used.
 */
#include "config.h"
#include "ass.h"
#include "ass_render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION gate_lock;
static CONDITION_VARIABLE gate_changed;
#else
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_changed = PTHREAD_COND_INITIALIZER;
#endif

enum { WORKERS = 4, ROUNDS = 64 };
static unsigned arrived, generation;

static void rendezvous(void)
{
#ifdef _WIN32
    EnterCriticalSection(&gate_lock);
#else
    if (pthread_mutex_lock(&gate_lock)) abort();
#endif
    unsigned previous = generation;
    if (++arrived == WORKERS) {
        arrived = 0;
        generation++;
#ifdef _WIN32
        WakeAllConditionVariable(&gate_changed);
#else
        if (pthread_cond_broadcast(&gate_changed)) abort();
#endif
    } else {
        while (generation == previous) {
#ifdef _WIN32
            if (!SleepConditionVariableCS(&gate_changed, &gate_lock, INFINITE)) abort();
#else
            if (pthread_cond_wait(&gate_changed, &gate_lock)) abort();
#endif
        }
    }
#ifdef _WIN32
    LeaveCriticalSection(&gate_lock);
#else
    if (pthread_mutex_unlock(&gate_lock)) abort();
#endif
}

typedef struct {
    ASS_Library *library;
    ASS_Renderer *renderer;
    int failed;
} Worker;

static void expect(Worker *worker, bool ok, const char *operation)
{
    if (!ok) {
        fprintf(stderr, "RGBA registry concurrency: %s\n", operation);
        worker->failed = 1;
    }
}

static void exercise_registry(Worker *worker)
{
    for (int round = 0; round < ROUNDS; round++) {
        ASS_ImageRGBA *img = ass_rgba_image_alloc(worker->renderer, 16, 12, 0, 0,
            IMAGE_TYPE_CHARACTER, ASS_RGBA_OWNER_EVENT, "concurrent registry insertion");
        expect(worker, img != NULL, "image allocation failed");
        rendezvous();
        size_t live;
        uint64_t scans, steps;
        ass_rgba_debug_allocation_stats(&live, &scans, &steps);
#ifndef NDEBUG
        expect(worker, live == WORKERS, "insertion lost or duplicated a registry entry");
#endif
        rendezvous();
        if (img) {
            ASS_ImageRGBAPriv *priv = ass_rgba_image_private(img, "concurrent lookup");
            memset(priv->buffer, 255, priv->alloc_size);
            expect(worker, ass_rgba_image_view_valid(img, "concurrent view lookup"), "invalid allocation view");
            ass_rgba_images_set_owner(img, ASS_RGBA_OWNER_FRAME_RESULT, "concurrent ownership transfer");
#ifndef NDEBUG
            expect(worker, priv->owner == ASS_RGBA_OWNER_FRAME_RESULT, "ownership transfer was lost");
#endif
            int stride;
            size_t size;
            uint8_t *replacement = ass_rgba_alloc_buffer(worker->renderer, 16, 12,
                priv->alloc_size, &stride, &size, "concurrent replacement insertion");
            expect(worker, replacement != NULL, "replacement allocation failed");
            if (replacement) {
                memset(replacement, 255, size);
                ass_rgba_image_replace_buffer(img, replacement, size, 16, 12, stride);
                expect(worker, ass_rgba_image_private(img, "replacement lookup")->buffer == replacement,
                       "replacement lookup returned old allocation");
            }
        }
        rendezvous();
#ifndef NDEBUG
        expect(worker, ass_rgba_debug_live_allocation_count() == WORKERS,
               "replacement leaked or removed a live entry");
#endif
        rendezvous();
        ass_rgba_images_set_owner(img, ASS_RGBA_OWNER_CALLER, "concurrent caller ownership");
        ass_rgba_image_free(worker->renderer, img);
        rendezvous();
        expect(worker, ass_rgba_debug_live_allocation_count() == 0, "removal leaked a registry entry");
        expect(worker, worker->renderer->rgba_output_size == 0, "allocation accounting leaked");
        rendezvous();
    }
}

#if !defined(_WIN32) && !defined(NDEBUG)
static int rejected_invalid_ownership(void)
{
    // Isolate aborting diagnostics before starting any worker threads.
    for (int misuse = 0; misuse < 2; misuse++) {
        int output[2];
        if (pipe(output)) return 1;
        pid_t child = fork();
        if (child < 0) return 1;
        if (!child) {
            close(output[0]);
            if (dup2(output[1], STDERR_FILENO) < 0) _exit(2);
            close(output[1]);
            ASS_Library *library = ass_library_init();
            ASS_Renderer *renderer = library ? ass_renderer_init(library) : NULL;
            if (!renderer) _exit(2);
            ASS_ImageRGBA *img = ass_rgba_image_alloc(renderer, 8, 8, 0, 0,
                IMAGE_TYPE_CHARACTER, ASS_RGBA_OWNER_EVENT, "invalid ownership regression");
            if (!img) _exit(2);
            if (!misuse) {
                ass_free_images_rgba(img);
                ass_free_images_rgba(img);
            } else {
                ASS_ImageRGBAPriv *priv = ass_rgba_image_private(img, "owned buffer regression");
                // An already-owned buffer cannot be claimed as a replacement.
                ass_rgba_image_replace_buffer(img, priv->buffer, priv->alloc_size,
                    img->w, img->h, img->stride);
            }
            _exit(0);
        }
        close(output[1]);
        char diagnostic[8192] = {0}, discard[1024];
        size_t used = 0;
        ssize_t bytes;
        while ((bytes = read(output[0], used < sizeof(diagnostic) - 1 ?
                diagnostic + used : discard, used < sizeof(diagnostic) - 1 ?
                sizeof(diagnostic) - 1 - used : sizeof(discard))) > 0)
            if (used < sizeof(diagnostic) - 1) used += bytes;
        close(output[0]);
        int status;
        if (waitpid(child, &status, 0) != child || !WIFSIGNALED(status) ||
            WTERMSIG(status) != SIGABRT ||
            !strstr(diagnostic, "Invalid RGBA aligned free/ownership operation") ||
            !strstr(diagnostic, misuse ? "operation=replace buffer" : "operation=frame result cleanup")) {
            fprintf(stderr, "RGBA registry failed to reject misuse %d: %s\n", misuse, diagnostic);
            return 1;
        }
    }
    return 0;
}
#endif

#ifdef _WIN32
static DWORD WINAPI worker_main(void *arg)
{
    exercise_registry(arg);
    return 0;
}
#else
static void *worker_main(void *arg)
{
    exercise_registry(arg);
    return NULL;
}
#endif

int main(void)
{
#if !defined(_WIN32) && !defined(NDEBUG)
    if (rejected_invalid_ownership()) return 1;
#endif
    Worker workers[WORKERS] = {{0}};
#ifdef _WIN32
    HANDLE threads[WORKERS];
    InitializeCriticalSection(&gate_lock);
    InitializeConditionVariable(&gate_changed);
#else
    pthread_t threads[WORKERS];
#endif
    uint64_t scans_before, steps_before;
    ass_rgba_debug_allocation_stats(NULL, &scans_before, &steps_before);
    for (int i = 0; i < WORKERS; i++) {
        workers[i].library = ass_library_init();
        if (!workers[i].library) return 1;
        workers[i].renderer = ass_renderer_init(workers[i].library);
        if (!workers[i].renderer) return 1;
#ifdef _WIN32
        threads[i] = CreateThread(NULL, 0, worker_main, &workers[i], 0, NULL);
        if (!threads[i]) return 1;
#else
        if (pthread_create(&threads[i], NULL, worker_main, &workers[i])) return 1;
#endif
    }
    int failed = 0;
    for (int i = 0; i < WORKERS; i++) {
#ifdef _WIN32
        if (WaitForSingleObject(threads[i], INFINITE) != WAIT_OBJECT_0) return 1;
        CloseHandle(threads[i]);
#else
        if (pthread_join(threads[i], NULL)) return 1;
#endif
        failed |= workers[i].failed;
        ass_renderer_done(workers[i].renderer);
        ass_library_done(workers[i].library);
    }
    uint64_t scans_after, steps_after;
    ass_rgba_debug_allocation_stats(NULL, &scans_after, &steps_after);
#ifndef NDEBUG
    if (scans_after <= scans_before || steps_after <= steps_before) {
        fprintf(stderr, "RGBA registry concurrency: lookup counters did not advance\n");
        failed = 1;
    }
#endif
#ifdef _WIN32
    DeleteCriticalSection(&gate_lock);
#else
    if (pthread_cond_destroy(&gate_changed) || pthread_mutex_destroy(&gate_lock)) return 1;
#endif
    return failed;
}
