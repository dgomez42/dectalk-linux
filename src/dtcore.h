/* Shared core for the Linux DECtalk builds: runs the Windows DECtalk engine (embedded) in the
 * x86 interpreter and routes its audio to files, memory or the sound card. */
#ifndef DTCORE_H
#define DTCORE_H
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include "winrt.h"

#define DTC_MAXH 32

/* Wave-file encodings (DECtalk WAVE_FORMAT_* values) */
#define DTC_1M08 0x00000001u   /* 11025 Hz, 8-bit PCM  */
#define DTC_1M16 0x00000004u   /* 11025 Hz, 16-bit PCM */
#define DTC_08M08 0x00000007u  /* 8000 Hz, mu-law      */
#define DTC_08M16 0x00002000u  /* 8000 Hz, 16-bit PCM  */

typedef void (*dtc_pcm_cb)(void *ctx, const int16_t *s, size_t n);
/* program callback (StartupEx): (lParam1, lParam2, user parameter, message) */
typedef void (*dtc_user_cb)(long, long, unsigned int, unsigned int);
#define DTC_MAXBUF 64

/* host view of TTS_BUFFER_T (Linux layout) */
typedef struct {
    char *lpData; void *lpPhonemeArray; void *lpIndexArray;
    unsigned int dwMaximumBufferLength, dwMaximumNumberOfPhonemeChanges, dwMaximumNumberOfIndexMarks;
    unsigned int dwBufferLength, dwNumberOfPhonemeChanges, dwNumberOfIndexMarks, dwReserved;
} DTC_BUFFER;
typedef struct { u32 guest; DTC_BUFFER *host; } dtc_bufmap;

typedef struct dtc_handle {
    int used, id;
    u32 gh;                 /* guest TTS handle */
    int device;             /* play through the sound card when no file is open */
    /* wave file */
    int wav_open; char *wav_path; unsigned wav_fmt;
    int16_t *buf; size_t nbuf, cap;
    /* user callback for raw samples (memory mode, Epson API) */
    dtc_pcm_cb cb; void *cb_ctx;
    /* program callback, messages from the engine, in-memory output */
    u32 token;              /* fake window handle the engine posts its messages to */
    dtc_user_cb ucb; unsigned int ucb_param;
    int memory;
    dtc_bufmap bufs[DTC_MAXBUF]; int nbufs;
} dtc_handle;

extern winrt *dtc_w;
extern pthread_mutex_t dtc_lock;

int dtc_init(char *err, size_t errlen);              /* idempotent */
const char *dtc_dict_path(void);
u32 dtc_call(const char *name, int n, const u32 *args);
u32 dtc_strdup(const char *s);
void dtc_free(u32 p);

dtc_handle *dtc_open(int device, unsigned *status);  /* Startup */
dtc_handle *dtc_open_cb(int device, dtc_user_cb cb, unsigned int param, unsigned *status);
void dtc_reopen_out(dtc_handle *h);
void dtc_close(dtc_handle *h);                      /* Shutdown */
dtc_handle *dtc_check(void *p);
unsigned dtc_speak(dtc_handle *h, const char *text, unsigned flags);
unsigned dtc_sync(dtc_handle *h);
unsigned dtc_reset(dtc_handle *h, int flush);
int dtc_write_wav(const char *path, unsigned fmt, const int16_t *pcm, size_t n);

/* sound card (audio.c) */
int audio_open(void);
void audio_write(const int16_t *s, size_t n);
void audio_drain(void);       /* wait until everything queued has been played */
void audio_flush(void);       /* drop queued audio */
void audio_pause(int on);
int audio_busy(void);

/* embedded data */
extern const unsigned char engine_dll[];
extern const unsigned long engine_dll_len;
#endif
