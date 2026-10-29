/* Sound-card output for the Linux DECtalk builds. Uses PulseAudio (libpulse-simple) or ALSA
 * (libasound), whichever is installed, loaded at run time so neither is a hard dependency;
 * falls back to piping into paplay/aplay. 11025 Hz, 16-bit mono. */
#include "compat.h"
#define _GNU_SOURCE
#include "dtcore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>

/* --- PulseAudio simple API --- */
typedef struct { int format; unsigned rate; unsigned char channels; } pa_ss;
static void *(*pa_new)(const char *, const char *, int, const char *, const char *, const pa_ss *, const void *, const void *, int *);
static int (*pa_write)(void *, const void *, size_t, int *);
static int (*pa_drain)(void *, int *);
static int (*pa_flush)(void *, int *);
static void *pa;

/* --- ALSA --- */
static int (*sa_open)(void **, const char *, int, int);
static int (*sa_set)(void *, int, int, unsigned, unsigned, int, unsigned);
static long (*sa_writei)(void *, const void *, unsigned long);
static int (*sa_recover)(void *, int, int);
static int (*sa_drain)(void *);
static int (*sa_drop)(void *);
static int (*sa_prepare)(void *);
static void *sa;

static FILE *pipe_out;

static pthread_mutex_t qm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qc = PTHREAD_COND_INITIALIZER, qd = PTHREAD_COND_INITIALIZER;
static int16_t *q; static size_t qhead, qtail, qcap;
static int playing, paused, opened, failed, flushgen;
static pthread_t th;

static int open_backend(void)
{
    /* DECTALK_AUDIO=pulse | alsa | pipe:<command> chooses the output explicitly */
    const char *force = getenv("DECTALK_AUDIO");
    if (force && !strncmp(force, "pipe:", 5)) return (pipe_out = popen(force + 5, "w")) ? 0 : -1;
    int want_pulse = !force || !strcmp(force, "pulse"), want_alsa = !force || !strcmp(force, "alsa");
    void *l = want_pulse ? dlopen("libpulse-simple.so.0", RTLD_NOW) : NULL;
    if (l)
    {
        pa_new = dlsym(l, "pa_simple_new"); pa_write = dlsym(l, "pa_simple_write");
        pa_drain = dlsym(l, "pa_simple_drain"); pa_flush = dlsym(l, "pa_simple_flush");
        if (pa_new && pa_write)
        {
            pa_ss ss = { 3 /* PA_SAMPLE_S16LE */, 11025, 1 }; int e;
            pa = pa_new(NULL, "DECtalk", 1 /* PA_STREAM_PLAYBACK */, NULL, "speech", &ss, NULL, NULL, &e);
            if (pa) return 0;
        }
    }
    l = want_alsa ? dlopen("libasound.so.2", RTLD_NOW) : NULL;
    if (l)
    {
        sa_open = dlsym(l, "snd_pcm_open"); sa_set = dlsym(l, "snd_pcm_set_params"); sa_writei = dlsym(l, "snd_pcm_writei");
        sa_recover = dlsym(l, "snd_pcm_recover"); sa_drain = dlsym(l, "snd_pcm_drain"); sa_drop = dlsym(l, "snd_pcm_drop");
        sa_prepare = dlsym(l, "snd_pcm_prepare");
        if (sa_open && sa_set && sa_writei && sa_open(&sa, "default", 0, 0) == 0)
        {
            /* SND_PCM_FORMAT_S16_LE = 2, SND_PCM_ACCESS_RW_INTERLEAVED = 3 */
            if (sa_set(sa, 2, 3, 1, 11025, 1, 150000) == 0) return 0;
            sa = NULL;
        }
    }
    const char *cmds[] = { "paplay --raw --format=s16le --rate=11025 --channels=1 2>/dev/null",
                           "aplay -q -t raw -f S16_LE -r 11025 -c 1 2>/dev/null", NULL };
    for (int i = 0; cmds[i]; i++)
    {
        char which[160]; snprintf(which, sizeof which, "command -v %.*s >/dev/null 2>&1", (int)strcspn(cmds[i], " "), cmds[i]);
        if (system(which) == 0 && (pipe_out = popen(cmds[i], "w"))) return 0;
    }
    return -1;
}

static void be_write(const int16_t *s, size_t n)
{
    int e;
    if (pa) pa_write(pa, s, n * 2, &e);
    else if (sa)
    {
        while (n)
        {
            long r = sa_writei(sa, s, n);
            if (r < 0) { if (sa_recover(sa, (int)r, 1) < 0) return; continue; }
            s += r; n -= (size_t)r;
        }
    }
    else if (pipe_out) { fwrite(s, 2, n, pipe_out); fflush(pipe_out); }
}
static void be_drain(void)
{
    int e;
    if (pa && pa_drain) pa_drain(pa, &e);
    else if (sa && sa_drain) { sa_drain(sa); if (sa_prepare) sa_prepare(sa); }
}
static void be_flush(void)
{
    int e;
    if (pa && pa_flush) pa_flush(pa, &e);
    else if (sa && sa_drop) { sa_drop(sa); if (sa_prepare) sa_prepare(sa); }
}

static void *player(void *arg)
{
    (void)arg;
    int16_t chunk[1024];
    pthread_mutex_lock(&qm);
    for (;;)
    {
        while (qhead == qtail || paused) { if (qhead == qtail && playing) { playing = 0; pthread_mutex_unlock(&qm); be_drain(); pthread_mutex_lock(&qm); if (qhead == qtail) pthread_cond_broadcast(&qd); continue; } pthread_cond_wait(&qc, &qm); }
        size_t n = qtail - qhead; if (n > 1024) n = 1024;
        memcpy(chunk, q + qhead, n * 2); qhead += n;
        if (qhead == qtail) qhead = qtail = 0;
        playing = 1;
        int gen = flushgen;
        pthread_mutex_unlock(&qm);
        be_write(chunk, n);
        pthread_mutex_lock(&qm);
        if (gen != flushgen) { pthread_mutex_unlock(&qm); be_flush(); pthread_mutex_lock(&qm); }
    }
    return NULL;
}

int audio_open(void)
{
    pthread_mutex_lock(&qm);
    if (!opened && !failed)
    {
        if (open_backend()) failed = 1;
        else { opened = 1; pthread_create(&th, NULL, player, NULL); pthread_detach(th); }
    }
    int r = opened ? 0 : -1;
    pthread_mutex_unlock(&qm);
    return r;
}

void audio_write(const int16_t *s, size_t n)
{
    pthread_mutex_lock(&qm);
    if (qhead && qtail + n > qcap) { memmove(q, q + qhead, (qtail - qhead) * 2); qtail -= qhead; qhead = 0; }
    if (qtail + n > qcap) { qcap = (qtail + n) * 2 + 65536; q = realloc(q, qcap * 2); }
    memcpy(q + qtail, s, n * 2); qtail += n;
    pthread_cond_signal(&qc);
    pthread_mutex_unlock(&qm);
}

void audio_drain(void)
{
    pthread_mutex_lock(&qm);
    while (opened && (qhead != qtail || playing) && !paused) pthread_cond_wait(&qd, &qm);
    pthread_mutex_unlock(&qm);
}

void audio_flush(void)
{
    pthread_mutex_lock(&qm);
    qhead = qtail = 0; flushgen++;
    pthread_cond_broadcast(&qd);
    pthread_mutex_unlock(&qm);
    if (!playing) be_flush();
}

void audio_pause(int on)
{
    pthread_mutex_lock(&qm);
    paused = on;
    pthread_cond_broadcast(&qc); pthread_cond_broadcast(&qd);
    pthread_mutex_unlock(&qm);
}

int audio_busy(void)
{
    pthread_mutex_lock(&qm);
    int b = qhead != qtail || playing;
    pthread_mutex_unlock(&qm);
    return b;
}
