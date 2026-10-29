/* DECtalk text-to-speech API for Linux (same functions and arguments as the DECtalk 4.99 Linux
 * library), backed by the restored engine running in the built-in x86 interpreter. */
#include "compat.h"
#define _GNU_SOURCE
#include "dtcore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define API __attribute__((visibility("default")))
typedef unsigned int DWORD;
typedef unsigned int UINT;
typedef long LONG;
typedef unsigned int MMRESULT;
#define MMSYSERR_NOERROR 0
#define MMSYSERR_ERROR 1
#define MMSYSERR_INVALHANDLE 5
#define MMSYSERR_NOTSUPPORTED 8
#define MMSYSERR_INVALPARAM 11
#define DO_NOT_USE_AUDIO_DEVICE 0x80000000u

#define GET(h, p) dtc_handle *h = dtc_check(p); if (!h) return MMSYSERR_INVALHANDLE

static MMRESULT startup(void **ph, UINT dev, DWORD opts, void (*cb)(LONG, LONG, DWORD, UINT), LONG cbp)
{
    (void)dev;
    if (!ph) return MMSYSERR_INVALPARAM;
    unsigned st;
    dtc_handle *h = dtc_open_cb(!(opts & DO_NOT_USE_AUDIO_DEVICE), (dtc_user_cb)cb, (unsigned int)cbp, &st);
    *ph = h;
    return h ? MMSYSERR_NOERROR : st;
}

API MMRESULT TextToSpeechStartup(void **ph, UINT dev, DWORD opts, void (*cb)(LONG, LONG, DWORD, UINT), LONG cbp)
{ return startup(ph, dev, opts, cb, cbp); }
API MMRESULT TextToSpeechStartupEx(void **ph, UINT dev, DWORD opts, void (*cb)(LONG, LONG, DWORD, UINT), LONG cbp)
{ return startup(ph, dev, opts, cb, cbp); }
API MMRESULT TextToSpeechStartupExFonix(void **ph, UINT dev, DWORD opts, void (*cb)(LONG, LONG, DWORD, UINT), LONG cbp, char *dict)
{ (void)dict; return startup(ph, dev, opts, cb, cbp); }

API MMRESULT TextToSpeechShutdown(void *p) { GET(h, p); dtc_close(h); return 0; }
API MMRESULT TextToSpeechSpeak(void *p, char *text, DWORD flags) { GET(h, p); return text ? dtc_speak(h, text, flags) : MMSYSERR_INVALPARAM; }
API MMRESULT TextToSpeechSync(void *p) { GET(h, p); return dtc_sync(h); }
API MMRESULT TextToSpeechReset(void *p, int flush) { GET(h, p); return dtc_reset(h, flush); }

API MMRESULT TextToSpeechPause(void *p)
{
    GET(h, p);
    pthread_mutex_lock(&dtc_lock); u32 y[1] = { h->gh }; u32 r = dtc_call("TextToSpeechPause", 1, y); pthread_mutex_unlock(&dtc_lock);
    if (h->device) audio_pause(1);
    return r;
}
API MMRESULT TextToSpeechResume(void *p)
{
    GET(h, p);
    pthread_mutex_lock(&dtc_lock); u32 y[1] = { h->gh }; u32 r = dtc_call("TextToSpeechResume", 1, y); pthread_mutex_unlock(&dtc_lock);
    if (h->device) audio_pause(0);
    return r;
}

static MMRESULT set1(dtc_handle *h, const char *fn, u32 v)
{
    pthread_mutex_lock(&dtc_lock); u32 a[2] = { h->gh, v }; u32 r = dtc_call(fn, 2, a); pthread_mutex_unlock(&dtc_lock);
    return r;
}
static MMRESULT get1(dtc_handle *h, const char *fn, DWORD *out)
{
    if (!out) return MMSYSERR_INVALPARAM;
    pthread_mutex_lock(&dtc_lock);
    u32 m = winrt_alloc(dtc_w, 4); wr32(m, 0);
    u32 a[2] = { h->gh, m }; u32 r = dtc_call(fn, 2, a);
    *out = rd32(m); winrt_free(dtc_w, m);
    pthread_mutex_unlock(&dtc_lock);
    return r;
}
API MMRESULT TextToSpeechSetRate(void *p, DWORD v) { GET(h, p); return set1(h, "TextToSpeechSetRate", v); }
API MMRESULT TextToSpeechGetRate(void *p, DWORD *v) { GET(h, p); return get1(h, "TextToSpeechGetRate", v); }
API MMRESULT TextToSpeechSetSpeaker(void *p, DWORD v) { GET(h, p); return set1(h, "TextToSpeechSetSpeaker", v); }
API MMRESULT TextToSpeechGetSpeaker(void *p, DWORD *v) { GET(h, p); return get1(h, "TextToSpeechGetSpeaker", v); }
API MMRESULT TextToSpeechSetLanguage(void *p, DWORD v) { GET(h, p); return set1(h, "TextToSpeechSetLanguage", v); }
API MMRESULT TextToSpeechGetLanguage(void *p, DWORD *v) { GET(h, p); return get1(h, "TextToSpeechGetLanguage", v); }

API MMRESULT TextToSpeechSetVolume(void *p, int type, int vol)
{
    GET(h, p);
    pthread_mutex_lock(&dtc_lock); u32 a[3] = { h->gh, (u32)type, (u32)vol }; u32 r = dtc_call("TextToSpeechSetVolume", 3, a); pthread_mutex_unlock(&dtc_lock);
    return r;
}
API MMRESULT TextToSpeechGetVolume(void *p, int type, int *vol)
{
    GET(h, p);
    if (!vol) return MMSYSERR_INVALPARAM;
    pthread_mutex_lock(&dtc_lock);
    u32 m = winrt_alloc(dtc_w, 4); wr32(m, 0);
    u32 a[3] = { h->gh, (u32)type, m }; u32 r = dtc_call("TextToSpeechGetVolume", 3, a);
    *vol = (int)rd32(m); winrt_free(dtc_w, m);
    pthread_mutex_unlock(&dtc_lock);
    return r;
}

API MMRESULT TextToSpeechGetStatus(void *p, DWORD *ids, DWORD *status, DWORD n)
{
    GET(h, p);
    if (!ids || !status || !n || n > 64) return MMSYSERR_INVALPARAM;
    pthread_mutex_lock(&dtc_lock);
    u32 gi = winrt_alloc(dtc_w, 4 * n), gs = winrt_alloc(dtc_w, 4 * n);
    for (DWORD i = 0; i < n; i++) { wr32(gi + 4 * i, ids[i]); wr32(gs + 4 * i, 0); }
    u32 a[4] = { h->gh, gi, gs, n }; u32 r = dtc_call("TextToSpeechGetStatus", 4, a);
    for (DWORD i = 0; i < n; i++) status[i] = rd32(gs + 4 * i);
    /* STATUS_SPEAKING (1): also report the sound card still playing */
    for (DWORD i = 0; i < n; i++) if (ids[i] == 1 && h->device && audio_busy()) status[i] = 1;
    winrt_free(dtc_w, gi); winrt_free(dtc_w, gs);
    pthread_mutex_unlock(&dtc_lock);
    return r;
}

static void reopen(dtc_handle *h)
{
    char name[32]; snprintf(name, sizeof name, "@%d.wav", h->id);
    u32 y[1] = { h->gh };
    dtc_call("TextToSpeechCloseWaveOutFile", 1, y);
    u32 fn = dtc_strdup(name); u32 o[3] = { h->gh, fn, DTC_1M16 };
    dtc_call("TextToSpeechOpenWaveOutFile", 3, o); dtc_free(fn);
}
/* ---- wave files: the engine always writes 11 kHz 16-bit; we convert when the file is closed ---- */
API MMRESULT TextToSpeechOpenWaveOutFile(void *p, char *path, DWORD fmt)
{
    GET(h, p);
    if (!path) return MMSYSERR_INVALPARAM;
    if (fmt != DTC_1M08 && fmt != DTC_1M16 && fmt != DTC_08M08 && fmt != DTC_08M16) return MMSYSERR_INVALPARAM;
    FILE *t = strncmp(path, "stdout:", 7) ? fopen(path, "ab") : NULL;   /* check the path is writable now */
    if (strncmp(path, "stdout:", 7) && !t) return MMSYSERR_ERROR;
    if (t) fclose(t);
    pthread_mutex_lock(&dtc_lock);
    u32 y[1] = { h->gh }; dtc_call("TextToSpeechSync", 1, y);
    if (h->memory) { pthread_mutex_unlock(&dtc_lock); return MMSYSERR_ERROR; }
    reopen(h);   /* like a real wave file: the engine starts the file from a fresh output state */
    free(h->wav_path); h->wav_path = strdup(path); h->wav_fmt = fmt;
    h->nbuf = 0; h->wav_open = 1;
    pthread_mutex_unlock(&dtc_lock);
    return 0;
}
API MMRESULT TextToSpeechCloseWaveOutFile(void *p)
{
    GET(h, p);
    pthread_mutex_lock(&dtc_lock);
    if (!h->wav_open) { pthread_mutex_unlock(&dtc_lock); return MMSYSERR_ERROR; }
    u32 y[1] = { h->gh }; dtc_call("TextToSpeechSync", 1, y);
    reopen(h);
    int r = dtc_write_wav(h->wav_path, h->wav_fmt, h->buf, h->nbuf);
    h->wav_open = 0; h->nbuf = 0;
    pthread_mutex_unlock(&dtc_lock);
    return r ? MMSYSERR_ERROR : 0;
}

static void reopen_out(dtc_handle *h)
{
    char name[32]; snprintf(name, sizeof name, "@%d.wav", h->id);
    u32 fn = dtc_strdup(name); u32 o[3] = { h->gh, fn, DTC_1M16 };
    dtc_call("TextToSpeechOpenWaveOutFile", 3, o); dtc_free(fn);
}
/* ---- log files and user dictionaries: the engine opens a mapped name ---- */
API MMRESULT TextToSpeechOpenLogFile(void *p, char *path, DWORD flags)
{
    GET(h, p);
    if (!path) return MMSYSERR_INVALPARAM;
    char g[32]; snprintf(g, sizeof g, "dtlog%d.txt", h->id);
    pthread_mutex_lock(&dtc_lock);
    winrt_remap_file(dtc_w, g, path);
    /* the engine only opens a log file while it is not writing a wave file */
    u32 y[1] = { h->gh };
    dtc_call("TextToSpeechSync", 1, y);
    if (!h->memory) dtc_call("TextToSpeechCloseWaveOutFile", 1, y);
    u32 gp = dtc_strdup(g); u32 a[3] = { h->gh, gp, flags }; u32 r = dtc_call("TextToSpeechOpenLogFile", 3, a); dtc_free(gp);
    if (r && !h->memory) reopen_out(h);
    pthread_mutex_unlock(&dtc_lock);
    return r;
}
API MMRESULT TextToSpeechCloseLogFile(void *p)
{
    GET(h, p);
    pthread_mutex_lock(&dtc_lock); u32 y[1] = { h->gh }; dtc_call("TextToSpeechSync", 1, y); u32 r = dtc_call("TextToSpeechCloseLogFile", 1, y);
    if (!h->memory) reopen_out(h);
    pthread_mutex_unlock(&dtc_lock);
    return r;
}
API MMRESULT TextToSpeechLoadUserDictionary(void *p, char *path)
{
    GET(h, p);
    if (!path) return MMSYSERR_INVALPARAM;
    char g[32]; snprintf(g, sizeof g, "udict%d.dic", h->id);
    pthread_mutex_lock(&dtc_lock);
    winrt_remap_file(dtc_w, g, path);
    u32 gp = dtc_strdup(g); u32 a[2] = { h->gh, gp }; u32 r = dtc_call("TextToSpeechLoadUserDictionary", 2, a); dtc_free(gp);
    pthread_mutex_unlock(&dtc_lock);
    return r;
}
API MMRESULT TextToSpeechUnloadUserDictionary(void *p)
{
    GET(h, p);
    pthread_mutex_lock(&dtc_lock); u32 y[1] = { h->gh }; u32 r = dtc_call("TextToSpeechUnloadUserDictionary", 1, y); pthread_mutex_unlock(&dtc_lock);
    return r;
}

API void TextToSpeechTyping(void *p, unsigned char key)
{
    dtc_handle *h = dtc_check(p); if (!h) return;
    pthread_mutex_lock(&dtc_lock); u32 a[2] = { h->gh, key }; dtc_call("TextToSpeechTyping", 2, a); pthread_mutex_unlock(&dtc_lock);
}

/* ---- version ---- */
static char g_ver[128];
API DWORD TextToSpeechVersion(char **str)
{
    char err[256];
    if (dtc_init(err, sizeof err)) { if (str) *str = ""; return 0; }
    pthread_mutex_lock(&dtc_lock);
    u32 m = winrt_alloc(dtc_w, 4); wr32(m, 0);
    u32 a[1] = { m }; u32 v = dtc_call("TextToSpeechVersion", 1, a);
    u32 s = rd32(m); size_t i = 0;
    if (s) for (; i < sizeof g_ver - 1 && rd8(s + i); i++) g_ver[i] = (char)rd8(s + i);
    g_ver[i] = 0; winrt_free(dtc_w, m);
    pthread_mutex_unlock(&dtc_lock);
    if (str) *str = g_ver;
    return v;
}

typedef struct { DWORD StructSize, StructVersion; unsigned short DLLVersion, DTalkVersion; char *VerString, *Language; DWORD Features; } VERSION_INFO;
static VERSION_INFO g_vi;
API DWORD TextToSpeechVersionEx(VERSION_INFO **ver)
{
    char *s; DWORD v = TextToSpeechVersion(&s);
    g_vi.StructSize = sizeof g_vi; g_vi.StructVersion = 1;
    g_vi.DLLVersion = (unsigned short)(v >> 16); g_vi.DTalkVersion = (unsigned short)v;
    g_vi.VerString = s; g_vi.Language = "US: American English"; g_vi.Features = 0;
    if (ver) *ver = &g_vi;
    return 0;
}

typedef struct { char lang_code[3]; char lang_name[40]; } LANG_ENTRY;
typedef struct { DWORD Languages; int MultiLang; LANG_ENTRY *Entries; } LANG_ENUM;
static LANG_ENTRY g_le = { "us", "US English" };
static LANG_ENUM g_lenum = { 1, 0, &g_le };
API DWORD TextToSpeechEnumLangs(LANG_ENUM **e) { if (e) *e = &g_lenum; return 1; }
API unsigned TextToSpeechStartLang(char *l) { return (l && strncasecmp(l, "us", 2)) ? 0x7FFF : 1; }
API int TextToSpeechSelectLang(void *p, unsigned l) { (void)p; return l == 1; }
API int TextToSpeechCloseLang(char *l) { (void)l; return 1; }
API DWORD TextToSpeechGetFeatures(void) { return 0; }

typedef struct { DWORD dwNumberOfLanguages; void *lpLanguageParamsArray; DWORD dwSampleRate, dwMinimumSpeakingRate, dwMaximumSpeakingRate,
                 dwNumberOfPredefinedSpeakers, dwCharacterSet, version; } TTS_CAPS_T;
API MMRESULT TextToSpeechGetCaps(TTS_CAPS_T *c)
{
    if (!c) return MMSYSERR_INVALPARAM;
    memset(c, 0, sizeof *c);
    c->dwNumberOfLanguages = 1; c->dwSampleRate = 11025; c->dwMinimumSpeakingRate = 75; c->dwMaximumSpeakingRate = 600;
    c->dwNumberOfPredefinedSpeakers = 9; c->dwCharacterSet = 1; c->version = TextToSpeechVersion(NULL);
    return 0;
}

/* ---- in-memory output: program buffers are shadowed by buffers in the engine's memory ---- */
#define LOCK() pthread_mutex_lock(&dtc_lock)
#define UNLOCK() pthread_mutex_unlock(&dtc_lock)
API MMRESULT TextToSpeechOpenInMemory(void *p, DWORD fmt)
{
    GET(h, p);
    LOCK();
    u32 y[1] = { h->gh }; dtc_call("TextToSpeechSync", 1, y);
    if (h->memory || h->wav_open) { UNLOCK(); return MMSYSERR_ERROR; }
    dtc_call("TextToSpeechCloseWaveOutFile", 1, y);
    u32 a[2] = { h->gh, fmt }; u32 r = dtc_call("TextToSpeechOpenInMemory", 2, a);
    if (r) dtc_reopen_out(h); else h->memory = 1;
    UNLOCK();
    return r;
}
API MMRESULT TextToSpeechCloseInMemory(void *p)
{
    GET(h, p);
    LOCK();
    u32 y[1] = { h->gh }; u32 r = dtc_call("TextToSpeechCloseInMemory", 1, y);
    if (!r) { h->memory = 0; dtc_reopen_out(h); }
    UNLOCK();
    return r;
}
API MMRESULT TextToSpeechAddBuffer(void *p, DTC_BUFFER *b)
{
    GET(h, p);
    if (!b) return MMSYSERR_INVALPARAM;
    LOCK();
    if (h->nbufs >= DTC_MAXBUF) { UNLOCK(); return MMSYSERR_ERROR; }
    u32 g = winrt_alloc(dtc_w, 44);
    u32 gd = winrt_alloc(dtc_w, b->dwMaximumBufferLength ? b->dwMaximumBufferLength : 1);
    u32 gp = b->lpPhonemeArray && b->dwMaximumNumberOfPhonemeChanges ? winrt_alloc(dtc_w, 16 * b->dwMaximumNumberOfPhonemeChanges) : 0;
    u32 gi = b->lpIndexArray && b->dwMaximumNumberOfIndexMarks ? winrt_alloc(dtc_w, 12 * b->dwMaximumNumberOfIndexMarks) : 0;
    wr32(g, gd); wr32(g + 4, gp); wr32(g + 8, gi);
    wr32(g + 12, b->dwMaximumBufferLength); wr32(g + 16, gp ? b->dwMaximumNumberOfPhonemeChanges : 0); wr32(g + 20, gi ? b->dwMaximumNumberOfIndexMarks : 0);
    wr32(g + 24, 0); wr32(g + 28, 0); wr32(g + 32, 0); wr32(g + 36, 0);
    h->bufs[h->nbufs].guest = g; h->bufs[h->nbufs].host = b; h->nbufs++;
    u32 a[2] = { h->gh, g }; u32 r = dtc_call("TextToSpeechAddBuffer", 2, a);
    UNLOCK();
    return r;
}
API MMRESULT TextToSpeechReturnBuffer(void *p, DTC_BUFFER **out)
{
    GET(h, p);
    if (!out) return MMSYSERR_INVALPARAM;
    LOCK();
    u32 m = winrt_alloc(dtc_w, 4); wr32(m, 0);
    u32 a[2] = { h->gh, m }; u32 r = dtc_call("TextToSpeechReturnBuffer", 2, a);
    u32 g = rd32(m); winrt_free(dtc_w, m);
    *out = NULL;
    if (g)
        for (int i = 0; i < h->nbufs; i++) if (h->bufs[i].guest == g)
        {
            DTC_BUFFER *b = h->bufs[i].host;
            DWORD len = rd32(g + 24), np = rd32(g + 28), ni = rd32(g + 32);
            if (len > b->dwMaximumBufferLength) len = b->dwMaximumBufferLength;
            if (b->lpData && len) memcpy(b->lpData, GM(rd32(g)), len);
            if (b->lpPhonemeArray && np) memcpy(b->lpPhonemeArray, GM(rd32(g + 4)), np * 16);
            if (b->lpIndexArray && ni) memcpy(b->lpIndexArray, GM(rd32(g + 8)), ni * 12);
            b->dwBufferLength = len; b->dwNumberOfPhonemeChanges = np; b->dwNumberOfIndexMarks = ni;
            winrt_free(dtc_w, rd32(g)); if (rd32(g + 4)) winrt_free(dtc_w, rd32(g + 4)); if (rd32(g + 8)) winrt_free(dtc_w, rd32(g + 8)); winrt_free(dtc_w, g);
            h->bufs[i] = h->bufs[--h->nbufs];
            *out = b;
            break;
        }
    UNLOCK();
    return r;
}

/* ---- speaker parameters ---- */
#define SPDEFS_BYTES 78
static short g_sp[DTC_MAXH][4][40];
API MMRESULT TextToSpeechGetSpeakerParams(void *p, UINT idx, short **cur, short **lo, short **hi, short **def)
{
    GET(h, p);
    LOCK();
    u32 m = winrt_alloc(dtc_w, 16); for (int i = 0; i < 4; i++) wr32(m + 4 * i, 0);
    u32 a[6] = { h->gh, idx, m, m + 4, m + 8, m + 12 }; u32 r = dtc_call("TextToSpeechGetSpeakerParams", 6, a);
    short **o[4] = { cur, lo, hi, def };
    for (int i = 0; i < 4; i++)
    {
        u32 g = rd32(m + 4 * i);
        if (g) memcpy(g_sp[h->id][i], GM(g), SPDEFS_BYTES);
        if (o[i]) *o[i] = g ? g_sp[h->id][i] : NULL;
    }
    winrt_free(dtc_w, m);
    UNLOCK();
    return r;
}
API MMRESULT TextToSpeechSetSpeakerParams(void *p, short *sp)
{
    GET(h, p); if (!sp) return MMSYSERR_INVALPARAM;
    LOCK(); u32 g = winrt_alloc(dtc_w, 128); memset(GM(g), 0, 128); memcpy(GM(g), sp, SPDEFS_BYTES);
    u32 a[2] = { h->gh, g }; u32 r = dtc_call("TextToSpeechSetSpeakerParams", 2, a); winrt_free(dtc_w, g); UNLOCK();
    return r;
}
static short g_vdef[DTC_MAXH][64];
API short *TextToSpeechGetPhVdefParams(void *p, UINT idx)
{
    dtc_handle *h = dtc_check(p); if (!h) return NULL;
    LOCK(); u32 a[2] = { h->gh, idx }; u32 g = dtc_call("TextToSpeechGetPhVdefParams", 2, a);
    if (g) memcpy(g_vdef[h->id], GM(g), sizeof g_vdef[0]);
    UNLOCK();
    return g ? g_vdef[h->id] : NULL;
}
API unsigned long TextToSpeechGetLastError(void *p) { (void)p; return 0; }
API void TextToSpeechControlPanel(void *p) { (void)p; }

/* Rarely used functions that are not available in this build */
#define NS(name) API MMRESULT name() { return MMSYSERR_NOTSUPPORTED; }
NS(TextToSpeechAddUserEntry) NS(TextToSpeechChangeUserPhoneme) NS(TextToSpeechConvertToPhonemes) NS(TextToSpeechDeleteUserEntry)
NS(TextToSpeechDictionaryHit) NS(TextToSpeechDumpDictionary) NS(TextToSpeechDumpUserDictionary) NS(TextToSpeechUserDictionaryHit)
NS(TextToSpeechSaveUserDictionary) NS(TextToSpeechTuning) NS(TextToSpeechVisualMarks)
NS(TextToSpeechReserved1) NS(TextToSpeechReserved2) NS(TextToSpeechReserved3) NS(TextToSpeechReserved4) NS(TextToSpeechReserved5)
