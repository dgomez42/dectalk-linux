/* Shared core for the Linux DECtalk builds (see dtcore.h). */
#include "compat.h"
#define _GNU_SOURCE
#include "dtcore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <limits.h>
#include <dlfcn.h>
#include <math.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>

winrt *dtc_w;
pthread_mutex_t dtc_lock;
static pthread_once_t lock_once = PTHREAD_ONCE_INIT;
static dtc_handle H[DTC_MAXH];
static char g_dict[PATH_MAX];
static pthread_t g_pump;
static int g_pump_started;
static int g_depth;                 /* nesting of engine calls (0 = none in progress) */
static int g_in_dispatch;

/* optional embedded dictionary (Epson build); weak so other builds need not define it */
extern const unsigned char embedded_dic[] __attribute__((weak));
extern const unsigned long embedded_dic_len __attribute__((weak));

static void init_lock(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&dtc_lock, &a);
}

static int readable(const char *p) { return p && *p && access(p, R_OK) == 0; }

static int try_dir(const char *dir)
{
    static const char *rel[] = { "dtalk_us.dic", "dic/dtalk_us.dic", "../dtalk_us.dic", "../dic/dtalk_us.dic" };
    char p[PATH_MAX];
    for (unsigned i = 0; i < sizeof rel / sizeof *rel; i++)
    {
        snprintf(p, sizeof p, "%s/%s", dir, rel[i]);
        if (readable(p)) { if (!realpath(p, g_dict)) snprintf(g_dict, sizeof g_dict, "%s", p); return 1; }
    }
    return 0;
}

/* DECtalk.conf: "US_dict:<path>" (relative paths are relative to the conf file's folder) */
static int try_conf(const char *dir)
{
    char p[PATH_MAX], line[1024];
    snprintf(p, sizeof p, "%s/DECtalk.conf", dir);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int ok = 0;
    while (fgets(line, sizeof line, f))
    {
        if (strncasecmp(line, "US_dict:", 8)) continue;
        char *v = line + 8; size_t n = strlen(v);
        while (n && (v[n - 1] == '\n' || v[n - 1] == '\r' || v[n - 1] == ' ')) v[--n] = 0;
        char q[PATH_MAX];
        if (v[0] == '/') snprintf(q, sizeof q, "%s", v); else snprintf(q, sizeof q, "%s/%s", dir, v);
        if (readable(q)) { if (!realpath(q, g_dict)) snprintf(g_dict, sizeof g_dict, "%s", q); ok = 1; }
        break;
    }
    fclose(f);
    return ok;
}

static int find_dict(void)
{
    const char *e = getenv("DECTALK_DICT");
    if (readable(e)) { snprintf(g_dict, sizeof g_dict, "%s", e); return 1; }
    char cwd[PATH_MAX], self[PATH_MAX], exe[PATH_MAX];
    if (getcwd(cwd, sizeof cwd) && (try_conf(cwd) || try_dir(cwd))) return 1;
    Dl_info di;
    if (dladdr((void *)find_dict, &di) && di.dli_fname && realpath(di.dli_fname, self))
    {
        char *s = strrchr(self, '/'); if (s) *s = 0;
        if (try_conf(self) || try_dir(self)) return 1;
    }
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k > 0)
    {
        exe[k] = 0; char *s = strrchr(exe, '/'); if (s) *s = 0;
        if (try_conf(exe) || try_dir(exe)) return 1;
    }
    if (try_conf("/etc") || try_dir("/usr/lib/dectalk") || try_dir("/usr/local/lib/dectalk") || try_dir("/usr/share/dectalk")) return 1;
    if (&embedded_dic_len && embedded_dic_len)
    {
        int fd = (int)syscall(319 /* memfd_create */, "dtalk_us.dic", 0);
        if (fd >= 0 && write(fd, embedded_dic, embedded_dic_len) == (ssize_t)embedded_dic_len)
        {
            snprintf(g_dict, sizeof g_dict, "/proc/self/fd/%d", fd);
            return 1;
        }
    }
    return 0;
}

const char *dtc_dict_path(void) { return g_dict; }

static void sink(void *ctx, int id, const int16_t *s, size_t n)
{
    (void)ctx;
    if (id < 0 || id >= DTC_MAXH || !H[id].used) return;
    dtc_handle *h = &H[id];
    if (h->cb) { h->cb(h->cb_ctx, s, n); return; }
    if (h->wav_open)
    {
        if (h->nbuf + n > h->cap) { h->cap = (h->nbuf + n) * 2 + 65536; h->buf = realloc(h->buf, h->cap * 2); }
        memcpy(h->buf + h->nbuf, s, n * 2); h->nbuf += n;
        return;
    }
    if (h->device) audio_write(s, n);
}

/* ---------------- engine messages -> program callback ---------------- */
#define TTS_MSG_INDEX_MARK 1
#define TTS_MSG_STATUS 2
#define TTS_MSG_BUFFER 9
typedef struct { int h; u32 msg, wp, lp; } ev_t;
static ev_t q[1024]; static int qh, qt;

static dtc_handle *by_token(u32 tok)
{
    for (int i = 0; i < DTC_MAXH; i++) if (H[i].used && H[i].token == tok) return &H[i];
    return NULL;
}

static int post_hook(void *ctx, u32 hwnd, u32 msg, u32 wp, u32 lp)
{
    (void)ctx;
    dtc_handle *h = by_token(hwnd);
    if (!h) return 0;
    if ((qt + 1) % 1024 != qh) { q[qt].h = h->id; q[qt].msg = msg; q[qt].wp = wp; q[qt].lp = lp; qt = (qt + 1) % 1024; }
    return 1;
}

/* copy a finished engine buffer into the program's buffer and release the engine's copy */
static DTC_BUFFER *take_buffer(dtc_handle *h, u32 g)
{
    for (int i = 0; i < h->nbufs; i++) if (h->bufs[i].guest == g)
    {
        DTC_BUFFER *b = h->bufs[i].host;
        u32 gd = rd32(g), gp = rd32(g + 4), gi = rd32(g + 8);
        unsigned len = rd32(g + 24), np = rd32(g + 28), ni = rd32(g + 32);
        if (len > b->dwMaximumBufferLength) len = b->dwMaximumBufferLength;
        if (np > b->dwMaximumNumberOfPhonemeChanges) np = b->dwMaximumNumberOfPhonemeChanges;
        if (ni > b->dwMaximumNumberOfIndexMarks) ni = b->dwMaximumNumberOfIndexMarks;
        if (b->lpData && len) memcpy(b->lpData, GM(gd), len);
        if (b->lpPhonemeArray && np) memcpy(b->lpPhonemeArray, GM(gp), np * 16);
        if (b->lpIndexArray && ni) memcpy(b->lpIndexArray, GM(gi), ni * 12);
        b->dwBufferLength = len; b->dwNumberOfPhonemeChanges = b->lpPhonemeArray ? np : 0; b->dwNumberOfIndexMarks = b->lpIndexArray ? ni : 0;
        winrt_free(dtc_w, gd); if (gp) winrt_free(dtc_w, gp); if (gi) winrt_free(dtc_w, gi); winrt_free(dtc_w, g);
        h->bufs[i] = h->bufs[--h->nbufs];
        return b;
    }
    return NULL;
}

static int poll_hook(void *ctx)
{
    (void)ctx;
    if (g_in_dispatch || g_depth > 1 || qh == qt) return 0;
    g_in_dispatch = 1;
    int did = 0;
    while (qh != qt)
    {
        ev_t e = q[qh]; qh = (qh + 1) % 1024;
        dtc_handle *h = &H[e.h];
        if (!h->used) continue;
        unsigned msg; long l1 = (e.wp && e.wp == h->gh) ? 0 : (long)e.wp, l2 = (long)e.lp;
        if (e.msg == winrt_msg_id("DECtalkBufferMessage"))
        {
            DTC_BUFFER *b = take_buffer(h, e.lp);
            if (!b) continue;
            msg = TTS_MSG_BUFFER; l1 = 0; l2 = (long)b;
        }
        else if (e.msg == winrt_msg_id("DECtalkIndexMessage")) msg = TTS_MSG_INDEX_MARK;
        else if (e.msg == winrt_msg_id("DECtalkErrorMessage")) msg = TTS_MSG_STATUS;
        else continue;
        did = 1;
        if (h->ucb) h->ucb(l1, l2, h->ucb_param, msg);
    }
    g_in_dispatch = 0;
    return did;
}

static void *pump(void *arg)
{
    (void)arg;
    struct timespec ts = { 0, 2000000 };
    for (;;)
    {
        int idle;
        pthread_mutex_lock(&dtc_lock);
        idle = winrt_step(dtc_w, 400000) == -1;
        if (idle) idle = !poll_hook(NULL);
        pthread_mutex_unlock(&dtc_lock);
        if (idle) nanosleep(&ts, NULL); else sched_yield();
    }
    return NULL;
}

int dtc_init(char *err, size_t errlen)
{
    pthread_once(&lock_once, init_lock);
    pthread_mutex_lock(&dtc_lock);
    if (dtc_w) { pthread_mutex_unlock(&dtc_lock); return 0; }
    if (!find_dict())
    {
        snprintf(err, errlen, "dtalk_us.dic not found (put it next to the program or library, in ./dic/, or set DECTALK_DICT)");
        pthread_mutex_unlock(&dtc_lock);
        return -1;
    }
    dtc_w = winrt_load_mem(engine_dll, (long)engine_dll_len, err, errlen);
    if (!dtc_w) { pthread_mutex_unlock(&dtc_lock); return -1; }
    winrt_map_file(dtc_w, "DTALK_US.DIC", g_dict);
    if (getenv("DECTALK_TRACE")) winrt_set_trace(dtc_w, atoi(getenv("DECTALK_TRACE")));
    winrt_set_pcm_sink2(dtc_w, sink, NULL);
    winrt_set_hooks(dtc_w, post_hook, poll_hook, NULL);
    pthread_mutex_unlock(&dtc_lock);
    return 0;
}

static void start_pump(void)
{
    if (g_pump_started) return;
    g_pump_started = 1;
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&g_pump, &a, pump, NULL);
}

u32 dtc_call(const char *name, int n, const u32 *args)
{
    u32 fn = winrt_export(dtc_w, name);
    if (!fn) return 1;   /* MMSYSERR_ERROR */
    if (g_depth > 1) return 1;
    int slot = g_depth++;
    u32 r = winrt_call_slot(dtc_w, slot, fn, n, args);
    g_depth--;
    if (g_depth == 0) poll_hook(NULL);
    return r;
}
u32 dtc_strdup(const char *s) { return winrt_strdup(dtc_w, s ? s : ""); }
void dtc_free(u32 p) { if (p) winrt_free(dtc_w, p); }

dtc_handle *dtc_check(void *p)
{
    dtc_handle *h = p;
    if (h < H || h >= H + DTC_MAXH || !h->used) return NULL;
    return h;
}

dtc_handle *dtc_open(int device, unsigned *status) { return dtc_open_cb(device, NULL, 0, status); }

void dtc_reopen_out(dtc_handle *h)
{
    char name[32]; snprintf(name, sizeof name, "@%d.wav", h->id);
    u32 fn = dtc_strdup(name); u32 o[3] = { h->gh, fn, DTC_1M16 };
    dtc_call("TextToSpeechOpenWaveOutFile", 3, o); dtc_free(fn);
}

dtc_handle *dtc_open_cb(int device, dtc_user_cb cb, unsigned int param, unsigned *status)
{
    char err[256];
    if (dtc_init(err, sizeof err)) { fprintf(stderr, "DECtalk: %s\n", err); *status = 1; return NULL; }
    pthread_mutex_lock(&dtc_lock);
    int i;
    for (i = 0; i < DTC_MAXH && H[i].used; i++) ;
    if (i == DTC_MAXH) { pthread_mutex_unlock(&dtc_lock); *status = 7; return NULL; }  /* MMSYSERR_NOMEM */
    dtc_handle *h = &H[i];
    memset(h, 0, sizeof *h);
    h->id = i; h->token = 0x7A000000u + (u32)i * 16; h->ucb = cb; h->ucb_param = param;
    h->used = 1;
    u32 hp = winrt_alloc(dtc_w, 4);
    u32 sa[4] = { h->token, hp, 0xffffffffu, 0x80000000u };
    u32 r = dtc_call("TextToSpeechStartup", 4, sa);
    if (r) { winrt_free(dtc_w, hp); h->used = 0; pthread_mutex_unlock(&dtc_lock); *status = r; return NULL; }
    h->gh = rd32(hp); winrt_free(dtc_w, hp);
    h->device = device && audio_open() == 0;
    if (device && !h->device)
    {
        static int warned;
        if (!warned++) fprintf(stderr, "DECtalk: no sound output found (PulseAudio, ALSA, paplay or aplay); speech is not played\n");
    }
    char name[32]; snprintf(name, sizeof name, "@%d.wav", i);
    u32 fn = dtc_strdup(name);
    u32 o[3] = { h->gh, fn, DTC_1M16 };
    dtc_call("TextToSpeechOpenWaveOutFile", 3, o);
    dtc_free(fn);
    start_pump();
    pthread_mutex_unlock(&dtc_lock);
    *status = 0;
    return h;
}

void dtc_close(dtc_handle *h)
{
    pthread_mutex_lock(&dtc_lock);
    u32 y[1] = { h->gh };
    dtc_call("TextToSpeechSync", 1, y);
    if (h->memory) dtc_call("TextToSpeechCloseInMemory", 1, y);
    else dtc_call("TextToSpeechCloseWaveOutFile", 1, y);
    dtc_call("TextToSpeechShutdown", 1, y);
    pthread_mutex_unlock(&dtc_lock);
    if (h->device) audio_drain();
    pthread_mutex_lock(&dtc_lock);
    free(h->buf); free(h->wav_path);
    memset(h, 0, sizeof *h);
    pthread_mutex_unlock(&dtc_lock);
}

unsigned dtc_speak(dtc_handle *h, const char *text, unsigned flags)
{
    pthread_mutex_lock(&dtc_lock);
    u32 tx = dtc_strdup(text);
    u32 s[3] = { h->gh, tx, flags };
    u32 r = dtc_call("TextToSpeechSpeak", 3, s);
    dtc_free(tx);
    pthread_mutex_unlock(&dtc_lock);
    return r;
}

unsigned dtc_sync(dtc_handle *h)
{
    pthread_mutex_lock(&dtc_lock);
    u32 y[1] = { h->gh };
    u32 r = dtc_call("TextToSpeechSync", 1, y);
    int dev = h->device && !h->wav_open && !h->cb && !h->memory;
    pthread_mutex_unlock(&dtc_lock);
    if (dev) audio_drain();
    return r;
}

unsigned dtc_reset(dtc_handle *h, int flush)
{
    pthread_mutex_lock(&dtc_lock);
    /* The engine can deadlock in a full reset while a wave file is open (on Windows too), so the
       engine's output file is closed around the reset and opened again afterwards. */
    u32 y[1] = { h->gh };
    if (!h->memory) dtc_call("TextToSpeechCloseWaveOutFile", 1, y);
    u32 a[2] = { h->gh, (u32)flush };
    u32 r = dtc_call("TextToSpeechReset", 2, a);
    if (!h->memory) dtc_reopen_out(h);
    pthread_mutex_unlock(&dtc_lock);
    if (h->device) audio_flush();
    return r;
}

/* ---------------- wave files ---------------- */
static void put32(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc(v >> 24, f); }
static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void putbe32(FILE *f, unsigned v) { fputc(v >> 24, f); fputc((v >> 16) & 255, f); fputc((v >> 8) & 255, f); fputc(v & 255, f); }

static unsigned char mulaw(int s)
{
    const int BIAS = 0x84, CLIP = 32635;
    int sign = (s >> 8) & 0x80, exp, mant;
    if (sign) s = -s;
    if (s > CLIP) s = CLIP;
    s += BIAS;
    for (exp = 7; exp > 0 && !(s & (0x4000 >> (7 - exp))); exp--) ;
    mant = (s >> (exp + 3)) & 0x0F;
    return (unsigned char)~(sign | (exp << 4) | mant);
}

/* 11025 -> 8000 Hz with linear interpolation */
static int16_t *to8k(const int16_t *pcm, size_t n, size_t *out)
{
    size_t m = (size_t)((double)n * 8000.0 / 11025.0), i;
    int16_t *o = malloc((m + 1) * 2);
    for (i = 0; i < m; i++)
    {
        double p = (double)i * 11025.0 / 8000.0; size_t k = (size_t)p; double fr = p - k;
        int a = k < n ? pcm[k] : 0, b = k + 1 < n ? pcm[k + 1] : a;
        o[i] = (int16_t)lrint(a + (b - a) * fr);
    }
    *out = m;
    return o;
}

/* path may be "stdout:raw" or "stdout:au"; a ".au" extension writes a Sun .au file */
int dtc_write_wav(const char *path, unsigned fmt, const int16_t *pcm, size_t n)
{
    int raw = !strcmp(path, "stdout:raw"), au = !strcmp(path, "stdout:au");
    size_t pl = strlen(path);
    if (!au && pl > 3 && !strcasecmp(path + pl - 3, ".au")) au = 2;
    FILE *f = (raw || au == 1) ? stdout : fopen(path, "wb");
    if (!f) return -1;
    int rate = (fmt == DTC_08M08 || fmt == DTC_08M16) ? 8000 : 11025;
    int bits = (fmt == DTC_1M08 || fmt == DTC_08M08) ? 8 : 16;
    size_t m = n, i;
    int16_t *s8 = NULL; const int16_t *d = pcm;
    if (rate == 8000) { s8 = to8k(pcm, n, &m); d = s8; }
    size_t bytes = m * (bits / 8);
    if (au)
    {
        /* Sun .au: big-endian; encoding 1 = mu-law, 2 = 8-bit linear, 3 = 16-bit linear */
        fwrite(".snd", 1, 4, f); putbe32(f, 24); putbe32(f, au == 1 ? 0xffffffffu : (unsigned)bytes);
        putbe32(f, fmt == DTC_08M08 ? 1 : bits == 8 ? 2 : 3); putbe32(f, rate); putbe32(f, 1);
        for (i = 0; i < m; i++)
        {
            if (fmt == DTC_08M08) fputc(mulaw(d[i]), f);
            else if (bits == 8) fputc((unsigned char)(d[i] >> 8), f);
            else { fputc((d[i] >> 8) & 255, f); fputc(d[i] & 255, f); }
        }
    }
    else
    {
        if (!raw)
        {
            if (fmt == DTC_08M08)
            {
                fwrite("RIFF", 1, 4, f); put32(f, 38 + 12 + (unsigned)m); fwrite("WAVEfmt ", 1, 8, f);
                put32(f, 18); put16(f, 7); put16(f, 1); put32(f, 8000); put32(f, 8000); put16(f, 1); put16(f, 8); put16(f, 0);
                fwrite("fact", 1, 4, f); put32(f, 4); put32(f, (unsigned)m);
            }
            else
            {
                fwrite("RIFF", 1, 4, f); put32(f, 36 + (unsigned)bytes); fwrite("WAVEfmt ", 1, 8, f);
                put32(f, 16); put16(f, 1); put16(f, 1); put32(f, rate); put32(f, rate * bits / 8); put16(f, bits / 8); put16(f, bits);
            }
            fwrite("data", 1, 4, f); put32(f, (unsigned)bytes);
        }
        for (i = 0; i < m; i++)
        {
            if (fmt == DTC_08M08) fputc(mulaw(d[i]), f);
            else if (bits == 8) fputc((d[i] >> 8) + 128, f);
            else put16(f, (unsigned short)d[i]);
        }
    }
    free(s8);
    if (f == stdout) { fflush(f); return 0; }
    return fclose(f);
}
