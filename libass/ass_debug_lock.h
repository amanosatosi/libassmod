/* SPDX-License-Identifier: ISC
 * Private locks for debug allocation registries. Build systems link-probe
 * every builtin used here; compiler identification macros are insufficient.
 */
#ifndef LIBASS_DEBUG_LOCK_H
#define LIBASS_DEBUG_LOCK_H

#include "config.h"

#ifdef _WIN32
#include <windows.h>
typedef volatile LONG ASS_DebugLock;
#define ASS_DEBUG_LOCK_INITIALIZER 0

static inline void ass_debug_lock(ASS_DebugLock *lock)
{
    while (InterlockedCompareExchange(lock, 1, 0) != 0)
        Sleep(0);
}

static inline void ass_debug_unlock(ASS_DebugLock *lock)
{
    InterlockedExchange(lock, 0);
}
#elif HAVE_SYNC_BUILTINS
#include <sched.h>
typedef int ASS_DebugLock;
#define ASS_DEBUG_LOCK_INITIALIZER 0

static inline void ass_debug_lock(ASS_DebugLock *lock)
{
    while (__sync_lock_test_and_set(lock, 1)) {
        // Atomic polling avoids a data race with the releasing thread.
        while (__sync_val_compare_and_swap(lock, 0, 0))
            sched_yield();
    }
}

static inline void ass_debug_unlock(ASS_DebugLock *lock)
{
    __sync_lock_release(lock);
}
#elif HAVE_PTHREAD_MUTEX
#include <pthread.h>
#include <stdlib.h>
typedef pthread_mutex_t ASS_DebugLock;
#define ASS_DEBUG_LOCK_INITIALIZER PTHREAD_MUTEX_INITIALIZER

static inline void ass_debug_lock(ASS_DebugLock *lock)
{
    if (pthread_mutex_lock(lock))
        abort();
}

static inline void ass_debug_unlock(ASS_DebugLock *lock)
{
    if (pthread_mutex_unlock(lock))
        abort();
}
#else
#error No supported synchronization for debug allocation tracking
#endif

#endif
