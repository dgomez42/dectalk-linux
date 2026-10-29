/* Event helpers exported by the DECtalk 4.99 Linux libtts_us.so (used by tools such as tunecheck_us). */
#include "compat.h"
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#define API __attribute__((visibility("default")))
typedef struct { pthread_mutex_t m; pthread_cond_t c; int manual, sig; } op_event;

API op_event *OP_CreateEvent(unsigned int manual_reset, unsigned int initial)
{
    op_event *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    pthread_mutex_init(&e->m, NULL); pthread_cond_init(&e->c, NULL);
    e->manual = manual_reset != 0; e->sig = initial != 0;
    return e;
}
API void OP_DestroyEvent(op_event *e)
{
    if (!e) return;
    pthread_cond_destroy(&e->c); pthread_mutex_destroy(&e->m); free(e);
}
API void OP_SetEvent(op_event *e)
{
    if (!e) return;
    pthread_mutex_lock(&e->m); e->sig = 1; pthread_cond_broadcast(&e->c); pthread_mutex_unlock(&e->m);
}
API void OP_ResetEvent(op_event *e)
{
    if (!e) return;
    pthread_mutex_lock(&e->m); e->sig = 0; pthread_mutex_unlock(&e->m);
}
/* returns 0 when signalled, 1 on timeout */
API unsigned int OP_WaitForEvent(op_event *e, unsigned int ms)
{
    if (!e) return 1;
    unsigned int r = 0;
    pthread_mutex_lock(&e->m);
    if (ms == 0xFFFFFFFFu)
        while (!e->sig) pthread_cond_wait(&e->c, &e->m);
    else
    {
        struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
        t.tv_sec += ms / 1000; t.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
        while (!e->sig) if (pthread_cond_timedwait(&e->c, &e->m, &t) == ETIMEDOUT) break;
        if (!e->sig) r = 1;
    }
    if (!r && !e->manual) e->sig = 0;
    pthread_mutex_unlock(&e->m);
    return r;
}
