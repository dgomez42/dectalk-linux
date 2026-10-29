/* Tiny Win32 runtime: PE loader, heap, cooperative threads, and the KERNEL32/USER32/WINMM/
 * ADVAPI32/ole32/msvcrt functions a DECtalk DLL imports. All guest threads run on the one
 * host thread that calls winrt_call/winrt_pump, so no host locking is needed inside. */
#include "compat.h"
#include "winrt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
/* Guest memory: 4 GB reserved; pages are committed on first touch by a vectored exception handler. */
static LONG CALLBACK guest_fault(PEXCEPTION_POINTERS e);
#else
#include <sys/mman.h>
#endif
#include <stdatomic.h>

#define GUEST_SPACE   0x100000000ull
#define HEAP_BASE     0x20000000u
#define HEAP_END      0x60000000u
#define STACK_BASE    0x70000000u
#define STACK_SIZE    0x00100000u
#define TEB_BASE      0x7F000000u
#define MISC_BASE     0x7E000000u
#define MAX_THREADS   48
#define MAX_HANDLES   1024
#define MAX_FILES     256
#define QUANTUM       40000

#define MAGIC_RET     (THUNK_BASE + THUNK_SIZE - 16)   /* slot 0; slot 1 = MAGIC_RET - 64 */
#define MAGIC_RET1    (THUNK_BASE + THUNK_SIZE - 80)
#define THREAD_EXIT   (THUNK_BASE + THUNK_SIZE - 32)

enum { H_FREE, H_EVENT, H_THREAD, H_WINDOW, H_KEY };
enum { T_FREE, T_RUN, T_WAIT, T_DONE };
enum { W_OBJ, W_CS, W_SLEEP, W_FOREVER, W_MSG };
#define MAXQ 256
typedef struct { u32 hwnd, msg, wp, lp; } qmsg_t;

typedef struct { int type; int manual; int signaled; int thread; } hobj_t;

typedef struct {
    cpu_t cpu;
    int state;
    u32 id, handle, teb, exitcode;
    int wait_kind;
    int nwait; u32 waith[64]; int waitall;
    s64 deadline;          /* virtual ms, -1 = none */
    u32 wait_cs;
    u32 msgptr;
    qmsg_t q[MAXQ]; int qh, qt;
} thr_t;

typedef struct { int used; FILE *f; int sink; long pos; int header_skipped; u32 guest; int is_std; } gfile_t;

typedef struct { char *guest; char *host; } fmap_t;

struct winrt {
    u32 image_base, image_size, entry;
    struct { char name[64]; u32 addr; } *exports; int nexports;
    struct { char dll[32]; char name[64]; int id; } *imports; int nimports;
    hobj_t h[MAX_HANDLES];
    thr_t t[MAX_THREADS];
    int cur;
    u32 next_tid;
    s64 vclock_ms; u64 vinstr;
    gfile_t files[MAX_FILES];
    u32 iob;
    fmap_t *fmap; int nfmap;
    pcm_sink sink; void *sink_ctx; pcm_sink2 sink2;
    int (*post_hook)(void *, u32, u32, u32, u32); void *post_ctx; int (*poll_hook)(void *);
    int call_done[2]; u32 call_ret[2]; int call_thread[2]; u32 call_esp[2]; int call_busy[2];
    atomic_int interrupt;
    int trace;
    /* heap */
    u32 heap_top;
    u32 freelist[4097];    /* index = size/16 for size <= 65536; [4096] = large first-fit list */
    u32 last_error;
    u32 win_long[64][2]; int nwinlong;
    u32 next_hwnd, next_msg;
    struct { char name[64]; u32 proc; } cls[32]; int ncls;
    struct { u32 hwnd, proc; int thread; } win[64]; int nwin;
};

static winrt *W;  /* single instance (g_mem is global) */

/* ======================= heap ======================= */
/* block layout: [u32 size][u32 magic] payload...  size = payload size (multiple of 16) */
#define HMAGIC 0x48454150u
static u32 heap_alloc(u32 n) {
    if (n == 0) n = 1;
    u32 sz = (n + 15) & ~15u;
    if (sz <= 65536) {
        u32 *fl = &W->freelist[sz / 16 - 1];
        if (*fl) { u32 p = *fl; *fl = rd32(p); wr32(p - 8, sz); wr32(p - 4, HMAGIC); return p; }
    } else {
        u32 prev = 0, p = W->freelist[4096];
        while (p) {
            u32 bsz = rd32(p - 8);
            if (bsz >= sz) {
                u32 nx = rd32(p);
                if (prev) wr32(prev, nx); else W->freelist[4096] = nx;
                wr32(p - 4, HMAGIC); return p;
            }
            prev = p; p = rd32(p);
        }
    }
    u32 p = W->heap_top + 16;
    if ((u64)p + sz + 16 > HEAP_END) { fprintf(stderr, "winrt: guest heap exhausted\n"); abort(); }
    W->heap_top = p + sz;
    wr32(p - 8, sz); wr32(p - 4, HMAGIC);
    return p;
}
static void heap_free(u32 p) {
    if (!p) return;
    if (rd32(p - 4) != HMAGIC) { if (W->trace) fprintf(stderr, "winrt: free of bad pointer %08x\n", p); return; }
    u32 sz = rd32(p - 8);
    wr32(p - 4, 0);
    if (sz <= 65536) { u32 *fl = &W->freelist[sz / 16 - 1]; wr32(p, *fl); *fl = p; }
    else { wr32(p, W->freelist[4096]); W->freelist[4096] = p; }
}
static u32 heap_size(u32 p) { return p ? rd32(p - 8) : 0; }
static u32 heap_realloc(u32 p, u32 n) {
    if (!p) return heap_alloc(n);
    if (!n) { heap_free(p); return 0; }
    u32 old = heap_size(p);
    if (n <= old) return p;
    u32 q = heap_alloc(n);
    memcpy(GM(q), GM(p), old);
    heap_free(p);
    return q;
}

u32 winrt_alloc(winrt *w, u32 size) { (void)w; u32 p = heap_alloc(size); memset(GM(p), 0, size); return p; }
void winrt_free(winrt *w, u32 p) { (void)w; heap_free(p); }
u32 winrt_strdup(winrt *w, const char *s) { u32 n = (u32)strlen(s) + 1; u32 p = winrt_alloc(w, n); memcpy(GM(p), s, n); return p; }

/* ======================= strings ======================= */
static void gstr(u32 a, char *out, size_t max) {
    size_t i = 0;
    if (!a) { out[0] = 0; return; }
    for (; i + 1 < max; i++) { char ch = (char)rd8(a + i); out[i] = ch; if (!ch) return; }
    out[i] = 0;
}
static u32 gstrlen(u32 a) { u32 n = 0; while (rd8(a + n)) n++; return n; }

/* ======================= handles & threads ======================= */
static u32 new_handle(int type) {
    for (int i = 1; i < MAX_HANDLES; i++) if (W->h[i].type == H_FREE) { memset(&W->h[i], 0, sizeof W->h[i]); W->h[i].type = type; return 0x1000 + i * 4; }
    fprintf(stderr, "winrt: out of handles\n"); abort();
}
static hobj_t *hobj(u32 h) { if (h < 0x1000) return NULL; u32 i = (h - 0x1000) / 4; if (i >= MAX_HANDLES || W->h[i].type == H_FREE) return NULL; return &W->h[i]; }

static int new_thread(u32 start, u32 arg) {
    for (int i = 0; i < MAX_THREADS; i++) if (W->t[i].state == T_FREE) {
        thr_t *t = &W->t[i];
        memset(t, 0, sizeof *t);
        t->state = T_RUN; t->id = W->next_tid++; t->deadline = -1;
        t->teb = TEB_BASE + i * 0x2000;
        memset(GM(t->teb), 0, 0x2000);
        wr32(t->teb + 0x18, t->teb);           /* NT_TIB.Self */
        wr32(t->teb + 0x24, t->id);            /* ClientId.UniqueThread */
        wr32(t->teb + 0x04, STACK_BASE + (i + 1) * STACK_SIZE);
        wr32(t->teb + 0x08, STACK_BASE + i * STACK_SIZE);
        t->cpu.fsbase = t->teb;
        t->cpu.r[ESP] = STACK_BASE + (i + 1) * STACK_SIZE - 64;
        t->cpu.flags = 0x202; t->cpu.fcw = 0x27f; t->cpu.mxcsr = 0x1f80;
        if (start) { push32(&t->cpu, arg); push32(&t->cpu, THREAD_EXIT); t->cpu.eip = start; }
        t->handle = new_handle(H_THREAD);
        hobj(t->handle)->thread = i;
        return i;
    }
    fprintf(stderr, "winrt: too many threads\n"); abort();
}

static int obj_signaled(u32 h) {
    hobj_t *o = hobj(h);
    if (!o) return 1;
    if (o->type == H_EVENT) return o->signaled;
    if (o->type == H_THREAD) return W->t[o->thread].state == T_DONE;
    return 1;
}
static void obj_consume(u32 h) { hobj_t *o = hobj(h); if (o && o->type == H_EVENT && !o->manual) o->signaled = 0; }

/* CRITICAL_SECTION fields we use: +8 RecursionCount, +12 OwningThread */
static int cs_try(u32 cs, thr_t *t) {
    u32 owner = rd32(cs + 12), rc = rd32(cs + 8);
    if (rc == 0 || owner == t->id) { wr32(cs + 12, t->id); wr32(cs + 8, rc + 1); return 1; }
    return 0;
}

/* Check whether a waiting thread can wake; if so complete its wait. */
static int try_wake(thr_t *t) {
    if (t->state != T_WAIT) return 0;
    switch (t->wait_kind) {
    case W_CS:
        if (cs_try(t->wait_cs, t)) { t->state = T_RUN; return 1; }
        return 0;
    case W_SLEEP:
        if (W->vclock_ms >= t->deadline) { t->state = T_RUN; return 1; }
        return 0;
    case W_FOREVER:
        return 0;
    case W_MSG:
        if (t->qh != t->qt) {
            qmsg_t m = t->q[t->qh]; t->qh = (t->qh + 1) % MAXQ;
            u32 p = t->msgptr;
            wr32(p, m.hwnd); wr32(p + 4, m.msg); wr32(p + 8, m.wp); wr32(p + 12, m.lp); wr32(p + 16, (u32)W->vclock_ms); wr32(p + 20, 0); wr32(p + 24, 0);
            t->cpu.r[EAX] = m.msg == 0x12 ? 0 : 1; t->state = T_RUN; return 1;
        }
        return 0;
    case W_OBJ:
        if (t->waitall) {
            int all = 1;
            for (int i = 0; i < t->nwait; i++) if (!obj_signaled(t->waith[i])) all = 0;
            if (all) { for (int i = 0; i < t->nwait; i++) obj_consume(t->waith[i]); t->cpu.r[EAX] = 0; t->state = T_RUN; return 1; }
        } else {
            for (int i = 0; i < t->nwait; i++) if (obj_signaled(t->waith[i])) { obj_consume(t->waith[i]); t->cpu.r[EAX] = i; t->state = T_RUN; return 1; }
        }
        if (t->deadline >= 0 && W->vclock_ms >= t->deadline) { t->cpu.r[EAX] = 0x102; t->state = T_RUN; return 1; }
        return 0;
    }
    return 0;
}

/* ======================= files ======================= */
static gfile_t *gfile(u32 fp) {
    for (int i = 0; i < MAX_FILES; i++) if (W->files[i].used && W->files[i].guest == fp) return &W->files[i];
    return NULL;
}
static const char *basename_of(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '\\' || *q == '/' || *q == ':') b = q + 1;
    return b;
}
static u32 gfopen(const char *name, const char *mode) {
    int slot = -1;
    for (int i = 3; i < MAX_FILES; i++) if (!W->files[i].used) { slot = i; break; }
    if (slot < 0) return 0;
    gfile_t *g = &W->files[slot];
    memset(g, 0, sizeof *g);
    const char *bn = basename_of(name);
    if (bn[0] == '@') {
        int id = atoi(bn + 1);
        for (int i = 3; i < MAX_FILES; i++) if (i != slot && W->files[i].used && W->files[i].sink == id + 1) { heap_free(W->files[i].guest); W->files[i].used = 0; }
        g->sink = id + 1;
    } else {
        const char *host = NULL;
        for (int i = 0; i < W->nfmap; i++) if (!strcasecmp(W->fmap[i].guest, bn)) { host = W->fmap[i].host; break; }
        if (!host) {
            if (W->trace) fprintf(stderr, "winrt: fopen(%s,%s) refused\n", name, mode);
            return 0;
        }
#ifdef _WIN32
        char bm[16]; snprintf(bm, sizeof bm, "%s%s", mode, strchr(mode, 'b') ? "" : "b");   /* binary, like the guest's own C library */
        g->f = fopen(host, bm);
#else
        g->f = fopen(host, mode);
#endif
        if (!g->f) { if (W->trace) fprintf(stderr, "winrt: fopen(%s -> %s) failed\n", name, host); return 0; }
    }
    g->used = 1;
    g->guest = heap_alloc(32);
    memset(GM(g->guest), 0, 32);
    return g->guest;
}

/* ======================= printf ======================= */
static size_t gformat(char *out, size_t max, u32 fmt, u32 ap) {
    size_t o = 0;
    char f[32], tmp[512];
#define PUT(ch) do { if (o + 1 < max) out[o] = (ch); o++; } while (0)
    for (u32 p = fmt;; p++) {
        char ch = (char)rd8(p);
        if (!ch) break;
        if (ch != '%') { PUT(ch); continue; }
        int k = 0; f[k++] = '%';
        p++;
        while (strchr("-+ #0", rd8(p)) && k < 20) f[k++] = (char)rd8(p++);
        if (rd8(p) == '*') { k += snprintf(f + k, 12, "%d", (s32)rd32(ap)); ap += 4; p++; }
        else while (isdigit(rd8(p)) && k < 24) f[k++] = (char)rd8(p++);
        if (rd8(p) == '.') { f[k++] = '.'; p++; if (rd8(p) == '*') { k += snprintf(f + k, 12, "%d", (s32)rd32(ap)); ap += 4; p++; } else while (isdigit(rd8(p)) && k < 28) f[k++] = (char)rd8(p++); }
        int lng = 0, hh = 0;
        while (rd8(p) == 'l' || rd8(p) == 'h' || rd8(p) == 'L' || rd8(p) == 'I' || rd8(p) == 'w') {
            if (rd8(p) == 'l') lng++;
            if (rd8(p) == 'h') hh++;
            if (rd8(p) == 'I' && rd8(p + 1) == '6' && rd8(p + 2) == '4') { lng = 2; p += 2; }
            p++;
        }
        char cv = (char)rd8(p);
        switch (cv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': {
            if (lng == 2) {
                u64 v = rd64(ap); ap += 8;
                f[k++] = 'l'; f[k++] = 'l'; f[k++] = cv; f[k] = 0;
                snprintf(tmp, sizeof tmp, f, v);
            } else {
                u32 v = rd32(ap); ap += 4;
                if (hh == 1 && cv != 'c') v = (cv == 'd' || cv == 'i') ? (u32)(s32)(s16)v : (u16)v;
                f[k++] = cv; f[k] = 0;
                if (cv == 'd' || cv == 'i') snprintf(tmp, sizeof tmp, f, (int)v); else snprintf(tmp, sizeof tmp, f, (unsigned)v);
            }
            for (char *q = tmp; *q; q++) PUT(*q);
            break; }
        case 'p': { u32 v = rd32(ap); ap += 4; snprintf(tmp, sizeof tmp, "%08X", v); for (char *q = tmp; *q; q++) PUT(*q); break; }
        case 'f': case 'g': case 'e': case 'G': case 'E': {
            double v; u64 q = rd64(ap); memcpy(&v, &q, 8); ap += 8;
            f[k++] = cv; f[k] = 0; snprintf(tmp, sizeof tmp, f, v); for (char *r = tmp; *r; r++) PUT(*r); break; }
        case 's': {
            u32 s = rd32(ap); ap += 4;
            char buf[4096]; if (s) gstr(s, buf, sizeof buf); else strcpy(buf, "(null)");
            f[k++] = 's'; f[k] = 0;
            char *big = malloc(strlen(buf) + 600); sprintf(big, f, buf);
            for (char *r = big; *r; r++) PUT(*r);
            free(big); break; }
        case '%': PUT('%'); break;
        case 'n': ap += 4; break;
        default: PUT('%'); PUT(cv); break;
        }
    }
    if (max) out[o < max ? o : max - 1] = 0;
#undef PUT
    return o;
}

/* ======================= API implementations ======================= */
typedef struct { const char *name; int stdcall_args; u32 (*fn)(thr_t *t, u32 a); } api_t;
#define ARG(i) rd32(a + 4 * (i))
static int g_block;   /* set by an API to make the calling thread wait */

static void block_on(thr_t *t, int kind) { t->state = T_WAIT; t->wait_kind = kind; g_block = 1; }

/* ---- KERNEL32 ---- */
static u32 k_CloseHandle(thr_t *t, u32 a) { (void)t; hobj_t *o = hobj(ARG(0)); if (o && o->type == H_EVENT) o->type = H_FREE; return 1; }
static u32 k_CreateEventA(thr_t *t, u32 a) { (void)t; u32 h = new_handle(H_EVENT); hobj(h)->manual = ARG(1) != 0; hobj(h)->signaled = ARG(2) != 0; return h; }
static u32 k_InitializeCriticalSection(thr_t *t, u32 a) { (void)t; memset(GM(ARG(0)), 0, 24); return 0; }
static u32 k_DeleteCriticalSection(thr_t *t, u32 a) { (void)t; (void)a; return 0; }
static u32 k_EnterCriticalSection(thr_t *t, u32 a) {
    u32 cs = ARG(0);
    if (cs_try(cs, t)) return 0;
    t->wait_cs = cs; block_on(t, W_CS); return 0;
}
static u32 k_LeaveCriticalSection(thr_t *t, u32 a) {
    u32 cs = ARG(0); u32 rc = rd32(cs + 8);
    (void)t;
    if (rc) { rc--; wr32(cs + 8, rc); if (!rc) wr32(cs + 12, 0); }
    return 0;
}
static u32 k_GetCurrentThread(thr_t *t, u32 a) { (void)a; return t->handle; }
static u32 k_GetCurrentThreadId(thr_t *t, u32 a) { (void)a; return t->id; }
static u32 k_GetExitCodeThread(thr_t *t, u32 a) {
    (void)t; hobj_t *o = hobj(ARG(0));
    if (!o || o->type != H_THREAD) return 0;
    thr_t *x = &W->t[o->thread];
    wr32(ARG(1), x->state == T_DONE ? x->exitcode : 259);
    return 1;
}
static u32 k_GetLastError(thr_t *t, u32 a) { (void)t; (void)a; return W->last_error; }
static u32 k_GetThreadPriority(thr_t *t, u32 a) { (void)t; (void)a; return 0; }
static u32 k_SetThreadPriority(thr_t *t, u32 a) { (void)t; (void)a; return 1; }
static u32 k_GetTickCount(thr_t *t, u32 a) { (void)t; (void)a; return (u32)W->vclock_ms; }
static u32 k_GlobalAlloc(thr_t *t, u32 a) { (void)t; u32 p = heap_alloc(ARG(1) + 16); if (ARG(0) & 0x40) memset(GM(p), 0, ARG(1)); return p; }
static u32 k_GlobalFree(thr_t *t, u32 a) { (void)t; heap_free(ARG(0)); return 0; }
static u32 k_GlobalHandle(thr_t *t, u32 a) { (void)t; return ARG(0); }
static u32 k_GlobalLock(thr_t *t, u32 a) { (void)t; return ARG(0); }
static u32 k_GlobalUnlock(thr_t *t, u32 a) { (void)t; (void)a; return 1; }
static u32 k_GlobalReAlloc(thr_t *t, u32 a) {
    (void)t; u32 old = ARG(0), n = ARG(1);
    u32 osz = heap_size(old);
    u32 p = heap_realloc(old, n + 16);
    if ((ARG(2) & 0x40) && n > osz) memset(GM(p + osz), 0, n - osz);
    return p;
}
static u32 k_IsBadPtr(thr_t *t, u32 a) { (void)t; (void)a; return 0; }
static u32 k_ResetEvent(thr_t *t, u32 a) { (void)t; hobj_t *o = hobj(ARG(0)); if (o) o->signaled = 0; return 1; }
static u32 k_SetEvent(thr_t *t, u32 a) { (void)t; hobj_t *o = hobj(ARG(0)); if (o) o->signaled = 1; return 1; }
static u32 k_Sleep(thr_t *t, u32 a) {
    u32 ms = ARG(0);
    t->deadline = W->vclock_ms + (ms ? ms : 1);
    block_on(t, W_SLEEP);
    return 0;
}
static u32 k_TlsGetValue(thr_t *t, u32 a) { (void)t; (void)a; return 0; }
static u32 k_VirtualProtect(thr_t *t, u32 a) { (void)t; if (ARG(3)) wr32(ARG(3), 0x40); return 1; }
static u32 k_VirtualQuery(thr_t *t, u32 a) {
    (void)t; u32 addr = ARG(0), buf = ARG(1);
    memset(GM(buf), 0, 28);
    wr32(buf + 0, addr & ~0xfffu); wr32(buf + 4, W->image_base); wr32(buf + 8, 0x40);
    wr32(buf + 12, 0x1000); wr32(buf + 16, 0x1000); wr32(buf + 20, 0x40); wr32(buf + 24, 0x1000000);
    return 28;
}
static u32 wait_common(thr_t *t, int n, u32 harr, int waitall, u32 timeout) {
    t->nwait = n; t->waitall = waitall;
    for (int i = 0; i < n; i++) t->waith[i] = rd32(harr + 4 * i);
    t->deadline = timeout == 0xffffffffu ? -1 : W->vclock_ms + timeout;
    t->wait_kind = W_OBJ; t->state = T_WAIT;
    if (try_wake(t)) return t->cpu.r[EAX];
    if (timeout == 0) { t->state = T_RUN; return 0x102; }
    g_block = 1;
    return 0;
}
static u32 k_WaitForSingleObject(thr_t *t, u32 a) {
    u32 tmp = MISC_BASE + 0x100 + (u32)(t - W->t) * 4;
    wr32(tmp, ARG(0));
    return wait_common(t, 1, tmp, 0, ARG(1));
}
static u32 k_WaitForMultipleObjects(thr_t *t, u32 a) { return wait_common(t, (int)ARG(0), ARG(1), ARG(2) != 0, ARG(3)); }

/* ---- USER32 ---- */
static int win_find(u32 hwnd) { for (int i = 0; i < W->nwin; i++) if (W->win[i].hwnd == hwnd) return i; return -1; }
static u32 u_RegisterClassA(thr_t *t, u32 a) {
    (void)t; u32 wc = ARG(0);
    if (W->ncls < 32) { gstr(rd32(wc + 36), W->cls[W->ncls].name, 64); W->cls[W->ncls].proc = rd32(wc + 4); W->ncls++; }
    return 0xC000 + W->ncls;
}
static u32 u_CreateWindowExA(thr_t *t, u32 a) {
    char cn[64]; u32 cls = ARG(1); u32 proc = 0;
    if (cls < 0x10000) { int i = (int)(cls - 0xC001); if (i >= 0 && i < W->ncls) proc = W->cls[i].proc; }
    else { gstr(cls, cn, sizeof cn); for (int i = 0; i < W->ncls; i++) if (!strcasecmp(W->cls[i].name, cn)) proc = W->cls[i].proc; }
    u32 h = W->next_hwnd++;
    if (W->nwin < 64) { W->win[W->nwin].hwnd = h; W->win[W->nwin].proc = proc; W->win[W->nwin].thread = (int)(t - W->t); W->nwin++; }
    return h;
}
static u32 u_zero(thr_t *t, u32 a) { (void)t; (void)a; return 0; }
static u32 u_one(thr_t *t, u32 a) { (void)t; (void)a; return 1; }
static void post_to_thread(int ti, u32 hwnd, u32 msg, u32 wp, u32 lp) {
    thr_t *x = &W->t[ti];
    int nt = (x->qt + 1) % MAXQ;
    if (nt == x->qh) { fprintf(stderr, "winrt: message queue full\n"); return; }
    x->q[x->qt].hwnd = hwnd; x->q[x->qt].msg = msg; x->q[x->qt].wp = wp; x->q[x->qt].lp = lp; x->qt = nt;
}
static u32 u_PostMessageA(thr_t *t, u32 a) {
    int i = win_find(ARG(0));
    if (i < 0 && W->post_hook && W->post_hook(W->post_ctx, ARG(0), ARG(1), ARG(2), ARG(3))) return 1;
    post_to_thread(i >= 0 ? W->win[i].thread : (int)(t - W->t), ARG(0), ARG(1), ARG(2), ARG(3));
    return 1;
}
static u32 u_PostQuitMessage(thr_t *t, u32 a) { post_to_thread((int)(t - W->t), 0, 0x12, ARG(0), 0); return 0; }
static u32 u_GetMessageA(thr_t *t, u32 a) {
    t->msgptr = ARG(0);
    t->state = T_WAIT; t->wait_kind = W_MSG;
    if (try_wake(t)) return t->cpu.r[EAX];
    g_block = 1;
    return 0;
}
static char g_msgname[64][48]; static u32 g_msgid[64]; static int g_nmsg;
static u32 u_RegisterWindowMessageA(thr_t *t, u32 a) {
    (void)t; char n[48]; gstr(ARG(0), n, sizeof n);
    for (int i = 0; i < g_nmsg; i++) if (!strcmp(g_msgname[i], n)) return g_msgid[i];
    u32 id = W->next_msg++;
    if (g_nmsg < 64) { strcpy(g_msgname[g_nmsg], n); g_msgid[g_nmsg++] = id; }
    return id;
}
u32 winrt_msg_id(const char *name) { for (int i = 0; i < g_nmsg; i++) if (!strcmp(g_msgname[i], name)) return g_msgid[i]; return 0; }
static u32 u_GetWindowLongA(thr_t *t, u32 a) {
    (void)t; for (int i = 0; i < W->nwinlong; i++) if (W->win_long[i][0] == (ARG(0) ^ (ARG(1) << 20))) return W->win_long[i][1];
    return 0;
}
static u32 u_SetWindowLongA(thr_t *t, u32 a) {
    (void)t; u32 key = ARG(0) ^ (ARG(1) << 20);
    for (int i = 0; i < W->nwinlong; i++) if (W->win_long[i][0] == key) { u32 o = W->win_long[i][1]; W->win_long[i][1] = ARG(2); return o; }
    if (W->nwinlong < 64) { W->win_long[W->nwinlong][0] = key; W->win_long[W->nwinlong][1] = ARG(2); W->nwinlong++; }
    return 0;
}
/* DispatchMessageA / SendMessageA are handled in thunk() because they call guest code. */
static u32 u_dummy(thr_t *t, u32 a) { (void)t; (void)a; return 0; }

/* ---- ADVAPI32 ---- */
static u32 a_RegOpenKeyExA(thr_t *t, u32 a) { (void)t; (void)a; return 2; }
static u32 a_RegQueryValueExA(thr_t *t, u32 a) { (void)t; (void)a; return 2; }

/* ---- WINMM ---- */
static u32 w_err(thr_t *t, u32 a) { (void)t; (void)a; return 6; /* MMSYSERR_NODRIVER */ }
static u32 w_GetNumDevs(thr_t *t, u32 a) { (void)t; (void)a; return 0; }
static u32 w_timeGetTime(thr_t *t, u32 a) { (void)t; (void)a; return (u32)W->vclock_ms; }
static u32 w_mmioOpenA(thr_t *t, u32 a) { (void)t; (void)a; return 0; }

/* ---- ole32 ---- */
static u32 o_CoTaskMemAlloc(thr_t *t, u32 a) { (void)t; return heap_alloc(ARG(0)); }
static u32 o_CoTaskMemFree(thr_t *t, u32 a) { (void)t; heap_free(ARG(0)); return 0; }

/* ---- msvcrt ---- */
static u32 c_malloc(thr_t *t, u32 a) { (void)t; return heap_alloc(ARG(0)); }
static u32 c_calloc(thr_t *t, u32 a) { (void)t; u32 n = ARG(0) * ARG(1); u32 p = heap_alloc(n); memset(GM(p), 0, n); return p; }
static u32 c_realloc(thr_t *t, u32 a) { (void)t; return heap_realloc(ARG(0), ARG(1)); }
static u32 c_free(thr_t *t, u32 a) { (void)t; heap_free(ARG(0)); return 0; }
static u32 c_memcpy(thr_t *t, u32 a) { (void)t; memmove(GM(ARG(0)), GM(ARG(1)), ARG(2)); return ARG(0); }
static u32 c_memset(thr_t *t, u32 a) { (void)t; memset(GM(ARG(0)), (int)ARG(1), ARG(2)); return ARG(0); }
static u32 c_memcmp(thr_t *t, u32 a) { (void)t; int r = memcmp(GM(ARG(0)), GM(ARG(1)), ARG(2)); return (u32)(r < 0 ? -1 : r > 0 ? 1 : 0); }
static u32 c_strlen(thr_t *t, u32 a) { (void)t; return gstrlen(ARG(0)); }
static u32 c_strcpy(thr_t *t, u32 a) { (void)t; u32 n = gstrlen(ARG(1)) + 1; memmove(GM(ARG(0)), GM(ARG(1)), n); return ARG(0); }
static u32 c_strcat(thr_t *t, u32 a) { (void)t; u32 d = ARG(0) + gstrlen(ARG(0)); u32 n = gstrlen(ARG(1)) + 1; memmove(GM(d), GM(ARG(1)), n); return ARG(0); }
static u32 c_strcmp(thr_t *t, u32 a) { (void)t; int r = strcmp((char *)GM(ARG(0)), (char *)GM(ARG(1))); return (u32)(r < 0 ? -1 : r > 0 ? 1 : 0); }
static u32 c_strncmp(thr_t *t, u32 a) { (void)t; int r = strncmp((char *)GM(ARG(0)), (char *)GM(ARG(1)), ARG(2)); return (u32)(r < 0 ? -1 : r > 0 ? 1 : 0); }
static u32 c_stricmp(thr_t *t, u32 a) { (void)t; int r = strcasecmp((char *)GM(ARG(0)), (char *)GM(ARG(1))); return (u32)(r < 0 ? -1 : r > 0 ? 1 : 0); }
static u32 c_atoi(thr_t *t, u32 a) { (void)t; return (u32)atoi((char *)GM(ARG(0))); }
static u32 c_sprintf(thr_t *t, u32 a) { (void)t; char buf[8192]; size_t n = gformat(buf, sizeof buf, ARG(1), a + 8); memcpy(GM(ARG(0)), buf, strlen(buf) + 1); return (u32)n; }
static void out_text(u32 fp, const char *s) {
    if (fp == W->iob + 32 || fp == W->iob + 64) { if (W->trace) fputs(s, stderr); return; }
    gfile_t *g = gfile(fp);
    if (g && g->f) fputs(s, g->f);
}
static u32 c_printf(thr_t *t, u32 a) { (void)t; char buf[8192]; size_t n = gformat(buf, sizeof buf, ARG(0), a + 4); if (W->trace) fputs(buf, stderr); return (u32)n; }
static u32 c_fprintf(thr_t *t, u32 a) { (void)t; char buf[8192]; size_t n = gformat(buf, sizeof buf, ARG(1), a + 8); out_text(ARG(0), buf); return (u32)n; }
static u32 c_vfprintf(thr_t *t, u32 a) { (void)t; char buf[8192]; size_t n = gformat(buf, sizeof buf, ARG(1), ARG(2)); out_text(ARG(0), buf); return (u32)n; }
static u32 c_puts(thr_t *t, u32 a) { (void)t; if (W->trace) { char b[4096]; gstr(ARG(0), b, sizeof b); fprintf(stderr, "%s\n", b); } return 0; }
static u32 c_putchar(thr_t *t, u32 a) { (void)t; if (W->trace) fputc((int)ARG(0), stderr); return ARG(0); }
static u32 c_fputc(thr_t *t, u32 a) { (void)t; char s[2] = {(char)ARG(0), 0}; out_text(ARG(1), s); return ARG(0); }
static u32 c_fputs(thr_t *t, u32 a) { (void)t; char b[8192]; gstr(ARG(0), b, sizeof b); out_text(ARG(1), b); return 0; }
static u32 c_fopen(thr_t *t, u32 a) { (void)t; char n[512], m[16]; gstr(ARG(0), n, sizeof n); gstr(ARG(1), m, sizeof m); return gfopen(n, m); }
static u32 c_fclose(thr_t *t, u32 a) {
    (void)t; gfile_t *g = gfile(ARG(0));
    if (!g) return (u32)-1;
    if (g->f) fclose(g->f);
    heap_free(g->guest);
    g->used = 0;
    return 0;
}
static void guest_commit(u32 a, u32 n) {
#ifdef _WIN32
    /* the OS cannot fault in reserved pages during a read() into them, so commit the range first */
    if (n) VirtualAlloc(g_mem + (a & ~0xfffu), (SIZE_T)((a & 0xfffu) + n + 0xfff) & ~(SIZE_T)0xfff, MEM_COMMIT, PAGE_READWRITE);
#else
    (void)a; (void)n;
#endif
}
static u32 c_fread(thr_t *t, u32 a) {
    (void)t; gfile_t *g = gfile(ARG(3));
    if (!g || !g->f || !ARG(1)) return 0;
    guest_commit(ARG(0), ARG(1) * ARG(2));
    return (u32)fread(GM(ARG(0)), ARG(1), ARG(2), g->f);
}
u32 winrt_wav_rate;
static u32 c_fwrite(thr_t *t, u32 a) {
    (void)t; u32 fp = ARG(3), sz = ARG(1), cnt = ARG(2);
    if (fp == W->iob + 32 || fp == W->iob + 64) return cnt;
    gfile_t *g = gfile(fp);
    if (!g) return 0;
    if (g->f) return (u32)fwrite(GM(ARG(0)), sz, cnt, g->f);
    /* PCM sink: the first write is the wave header; later writes at position 0 are header rewrites */
    u32 bytes = sz * cnt;
    if (!g->header_skipped) { g->header_skipped = 1; if (bytes >= 28) memcpy(&winrt_wav_rate, GM(ARG(0)) + 24, 4); g->pos += bytes; return cnt; }
    if (g->pos == 0) { g->pos += bytes; return cnt; }
    if (W->sink2 && bytes >= 2) W->sink2(W->sink_ctx, g->sink - 1, (const int16_t *)GM(ARG(0)), bytes / 2);
    else if (W->sink && bytes >= 2) W->sink(W->sink_ctx, (const int16_t *)GM(ARG(0)), bytes / 2);
    g->pos += bytes;
    return cnt;
}
static u32 c_fseek(thr_t *t, u32 a) {
    (void)t; gfile_t *g = gfile(ARG(0));
    if (!g) return (u32)-1;
    if (g->f) return (u32)fseek(g->f, (long)(s32)ARG(1), (int)ARG(2));
    if (ARG(2) == 0) g->pos = (s32)ARG(1);
    return 0;
}
static u32 c_feof(thr_t *t, u32 a) { (void)t; gfile_t *g = gfile(ARG(0)); return g && g->f ? (u32)feof(g->f) : 1; }
static u32 c_fflush(thr_t *t, u32 a) { (void)t; gfile_t *g = gfile(ARG(0)); if (g && g->f) fflush(g->f); return 0; }
static u32 c_time(thr_t *t, u32 a) { (void)t; u32 v = (u32)time(NULL); if (ARG(0)) wr32(ARG(0), v); return v; }
static u32 c_strdate(thr_t *t, u32 a) { (void)t; time_t now = time(NULL); strftime((char *)GM(ARG(0)), 9, "%m/%d/%y", localtime(&now)); return ARG(0); }
static u32 c_strtime(thr_t *t, u32 a) { (void)t; time_t now = time(NULL); strftime((char *)GM(ARG(0)), 9, "%H:%M:%S", localtime(&now)); return ARG(0); }
static u32 c_getcwd(thr_t *t, u32 a) { (void)t; if (ARG(0)) { strcpy((char *)GM(ARG(0)), "C:\\"); return ARG(0); } return 0; }
static void wstr_put(u32 dst, u32 cap, const char *s) { u32 i; for (i = 0; s[i] && i + 1 < cap; i++) { ((u8 *)GM(dst))[2*i] = (u8)s[i]; ((u8 *)GM(dst))[2*i+1] = 0; } ((u8 *)GM(dst))[2*i] = 0; ((u8 *)GM(dst))[2*i+1] = 0; }
static u32 k_GetModuleHandleExW(thr_t *t, u32 a) { (void)t; if (ARG(2)) wr32(ARG(2), W->image_base); return 1; }
static u32 k_GetModuleFileNameW(thr_t *t, u32 a) { (void)t; const char *p = "C:\\DECtalk.dll"; if (ARG(2)) wstr_put(ARG(1), ARG(2), p); return (u32)strlen(p); }
static u32 k_GetCurrentDirectoryW(thr_t *t, u32 a) { (void)t; if (ARG(0) >= 4 && ARG(1)) wstr_put(ARG(1), ARG(0), "C:\\"); return 3; }
static u32 k_SetCurrentDirectoryW(thr_t *t, u32 a) { (void)t; (void)a; return 1; }
static u32 c_access(thr_t *t, u32 a) {
    (void)t; char n[512]; gstr(ARG(0), n, sizeof n);
    const char *bn = basename_of(n);
    for (int i = 0; i < W->nfmap; i++) if (!strcasecmp(W->fmap[i].guest, bn)) return 0;
    return (u32)-1;
}
static u32 c_searchenv(thr_t *t, u32 a) {
    (void)t; char n[512]; gstr(ARG(0), n, sizeof n);
    for (int i = 0; i < W->nfmap; i++) if (!strcasecmp(W->fmap[i].guest, basename_of(n))) { sprintf((char *)GM(ARG(2)), "C:\\%s", n); return 0; }
    wr8(ARG(2), 0); return 0;
}
static u32 c_abort(thr_t *t, u32 a) { (void)a; cpu_fatal(&t->cpu, "guest abort()"); return 0; }
static u32 c_nop(thr_t *t, u32 a) { (void)t; (void)a; return 0; }
static u32 c_p_iob(thr_t *t, u32 a) { (void)t; (void)a; return W->iob; }
static u32 c_beginthreadex(thr_t *t, u32 a) {
    (void)t;
    int i = new_thread(ARG(2), ARG(3));
    if (ARG(5)) wr32(ARG(5), W->t[i].id);
    if (ARG(4) & 4) { cpu_fatal(&t->cpu, "CREATE_SUSPENDED not supported"); }
    return W->t[i].handle;
}
static u32 c_endthreadex(thr_t *t, u32 a) { t->exitcode = ARG(0); t->state = T_DONE; g_block = 1; return 0; }

static u32 g_cmdA, g_cmdW;
static u32 k_GetCommandLineA(thr_t *t, u32 a) { (void)t; (void)a; if (!g_cmdA) { g_cmdA = heap_alloc(16); memcpy(GM(g_cmdA), "dectalk.exe", 12); } return g_cmdA; }
static u32 k_GetCommandLineW(thr_t *t, u32 a) { (void)t; (void)a; if (!g_cmdW) { g_cmdW = heap_alloc(32); const char *s = "dectalk.exe"; for (int i = 0; i < 12; i++) { ((u16*)GM(g_cmdW))[i] = (u16)s[i]; } } return g_cmdW; }
static u32 k_CommandLineToArgvW(thr_t *t, u32 a) { (void)t; u32 w = k_GetCommandLineW(t, a); u32 arr = heap_alloc(8); wr32(arr, w); wr32(arr + 4, 0); if (ARG(1)) wr32(ARG(1), 1); return arr; }
static u32 c_wcstombs(thr_t *t, u32 a) { (void)t; u32 d = ARG(0), sp = ARG(1), n = ARG(2), i = 0;
    for (;; i++) { u16 c = rd16(sp + 2 * i); if (d) { if (i >= n) break; wr8(d + i, c < 256 ? (u8)c : '?'); } if (!c) break; } return i; }
static u32 c_mbstowcs(thr_t *t, u32 a) { (void)t; u32 d = ARG(0), sp = ARG(1), n = ARG(2), i = 0;
    for (;; i++) { u8 c = rd8(sp + i); if (d) { if (i >= n) break; wr16(d + 2 * i, c); } if (!c) break; } return i; }
static u32 k_LocalFree(thr_t *t, u32 a) { (void)t; (void)a; return 0; }
static const api_t APIS[] = {
    {"LocalFree", 1, k_LocalFree},
    {"GetModuleHandleExW", 3, k_GetModuleHandleExW}, {"GetModuleFileNameW", 3, k_GetModuleFileNameW},
    {"GetCurrentDirectoryW", 2, k_GetCurrentDirectoryW}, {"SetCurrentDirectoryW", 1, k_SetCurrentDirectoryW},
    {"wcstombs", 3, c_wcstombs}, {"mbstowcs", 3, c_mbstowcs},
    {"CommandLineToArgvW", 2, k_CommandLineToArgvW},
    {"GetCommandLineA", 0, k_GetCommandLineA}, {"GetCommandLineW", 0, k_GetCommandLineW},
    {"CloseHandle", 1, k_CloseHandle}, {"CreateEventA", 4, k_CreateEventA},
    {"DeleteCriticalSection", 1, k_DeleteCriticalSection}, {"EnterCriticalSection", 1, k_EnterCriticalSection},
    {"InitializeCriticalSection", 1, k_InitializeCriticalSection}, {"LeaveCriticalSection", 1, k_LeaveCriticalSection},
    {"GetCurrentThread", 0, k_GetCurrentThread}, {"GetCurrentThreadId", 0, k_GetCurrentThreadId},
    {"GetExitCodeThread", 2, k_GetExitCodeThread}, {"GetLastError", 0, k_GetLastError},
    {"GetThreadPriority", 1, k_GetThreadPriority}, {"SetThreadPriority", 2, k_SetThreadPriority},
    {"GetTickCount", 0, k_GetTickCount},
    {"GlobalAlloc", 2, k_GlobalAlloc}, {"GlobalFree", 1, k_GlobalFree}, {"GlobalHandle", 1, k_GlobalHandle},
    {"GlobalLock", 1, k_GlobalLock}, {"GlobalReAlloc", 3, k_GlobalReAlloc}, {"GlobalUnlock", 1, k_GlobalUnlock},
    {"IsBadReadPtr", 2, k_IsBadPtr}, {"IsBadWritePtr", 2, k_IsBadPtr},
    {"ResetEvent", 1, k_ResetEvent}, {"SetEvent", 1, k_SetEvent}, {"Sleep", 1, k_Sleep},
    {"TlsGetValue", 1, k_TlsGetValue}, {"VirtualProtect", 4, k_VirtualProtect}, {"VirtualQuery", 3, k_VirtualQuery},
    {"WaitForMultipleObjects", 4, k_WaitForMultipleObjects}, {"WaitForSingleObject", 2, k_WaitForSingleObject},
    {"CreateWindowExA", 12, u_CreateWindowExA}, {"DefWindowProcA", 4, u_zero}, {"DestroyWindow", 1, u_one},
    {"DispatchMessageA", 1, u_dummy}, {"GetMessageA", 4, u_GetMessageA}, {"GetWindowLongA", 2, u_GetWindowLongA},
    {"LoadCursorA", 2, u_one}, {"LoadIconA", 2, u_one}, {"PostMessageA", 4, u_PostMessageA}, {"PostQuitMessage", 1, u_PostQuitMessage},
    {"RegisterClassA", 1, u_RegisterClassA}, {"RegisterWindowMessageA", 1, u_RegisterWindowMessageA}, {"SendMessageA", 4, u_dummy},
    {"SetWindowLongA", 3, u_SetWindowLongA}, {"ShowWindow", 2, u_one}, {"UnregisterClassA", 2, u_one},
    {"RegCloseKey", 1, u_zero}, {"RegOpenKeyExA", 5, a_RegOpenKeyExA}, {"RegQueryValueExA", 6, a_RegQueryValueExA},
    {"mmioAscend", 3, w_err}, {"mmioClose", 2, w_err}, {"mmioDescend", 4, w_err}, {"mmioOpenA", 3, w_mmioOpenA},
    {"mmioRead", 3, w_err}, {"timeGetTime", 0, w_timeGetTime},
    {"waveOutClose", 1, w_err}, {"waveOutGetDevCapsA", 3, w_err}, {"waveOutGetID", 2, w_err},
    {"waveOutGetNumDevs", 0, w_GetNumDevs}, {"waveOutGetPosition", 3, w_err}, {"waveOutGetVolume", 2, w_err},
    {"waveOutOpen", 6, w_err}, {"waveOutPause", 1, w_err}, {"waveOutPrepareHeader", 3, w_err},
    {"waveOutReset", 1, w_err}, {"waveOutRestart", 1, w_err}, {"waveOutSetVolume", 2, w_err},
    {"waveOutUnprepareHeader", 3, w_err}, {"waveOutWrite", 3, w_err},
    {"CoTaskMemAlloc", 1, o_CoTaskMemAlloc}, {"CoTaskMemFree", 1, o_CoTaskMemFree},
    /* msvcrt (cdecl: stdcall_args = -1) */
    {"malloc", -1, c_malloc}, {"calloc", -1, c_calloc}, {"realloc", -1, c_realloc}, {"free", -1, c_free},
    {"memcpy", -1, c_memcpy}, {"memmove", -1, c_memcpy}, {"memset", -1, c_memset}, {"memcmp", -1, c_memcmp},
    {"strlen", -1, c_strlen}, {"strcpy", -1, c_strcpy}, {"strcat", -1, c_strcat}, {"strcmp", -1, c_strcmp},
    {"strncmp", -1, c_strncmp}, {"_stricmp", -1, c_stricmp}, {"atoi", -1, c_atoi},
    {"sprintf", -1, c_sprintf}, {"printf", -1, c_printf}, {"fprintf", -1, c_fprintf}, {"vfprintf", -1, c_vfprintf},
    {"puts", -1, c_puts}, {"putchar", -1, c_putchar}, {"fputc", -1, c_fputc}, {"fputs", -1, c_fputs},
    {"fopen", -1, c_fopen}, {"fclose", -1, c_fclose}, {"fread", -1, c_fread}, {"fwrite", -1, c_fwrite},
    {"fseek", -1, c_fseek}, {"feof", -1, c_feof}, {"fflush", -1, c_fflush},
    {"time", -1, c_time}, {"_strdate", -1, c_strdate}, {"_strtime", -1, c_strtime}, {"_getcwd", -1, c_getcwd},
    {"_access", -1, c_access}, {"_searchenv", -1, c_searchenv}, {"abort", -1, c_abort},
    {"_amsg_exit", -1, c_abort}, {"_lock", -1, c_nop}, {"_unlock", -1, c_nop}, {"__p__iob", -1, c_p_iob},
    {"_beginthreadex", -1, c_beginthreadex}, {"_endthreadex", -1, c_endthreadex},
    {NULL, 0, NULL}
};

/* x86 implementation of _initterm(begin, end): calls each non-null function pointer. */
static const u8 INITTERM_CODE[] = {
    0x56, 0x57,                         /* push esi; push edi */
    0x8b, 0x74, 0x24, 0x0c,             /* mov esi,[esp+12] */
    0x8b, 0x7c, 0x24, 0x10,             /* mov edi,[esp+16] */
    0x39, 0xfe,                         /* L: cmp esi,edi */
    0x73, 0x0d,                         /* jae done */
    0x8b, 0x06,                         /* mov eax,[esi] */
    0x85, 0xc0,                         /* test eax,eax */
    0x74, 0x02,                         /* je skip */
    0xff, 0xd0,                         /* call eax */
    0x83, 0xc6, 0x04,                   /* skip: add esi,4 */
    0xeb, 0xef,                         /* jmp L */
    0x5f, 0x5e, 0xc3                    /* done: pop edi; pop esi; ret */
};

/* ======================= thunk dispatch ======================= */
static const api_t **g_thunk_api;   /* by thunk id */

static void thunk(cpu_t *c, u32 eip) {
    thr_t *t = &W->t[W->cur];
    if (eip == MAGIC_RET || eip == MAGIC_RET1) {
        int sl = eip == MAGIC_RET ? 0 : 1;
        W->call_ret[sl] = c->r[EAX]; W->call_done[sl] = 1;
        t->state = T_WAIT; t->wait_kind = W_FOREVER;   /* host thread parks until the next call */
        c->halted = 1; return;
    }
    if (eip == THREAD_EXIT) {
        t->exitcode = c->r[EAX]; t->state = T_DONE; c->halted = 1; return;
    }
    u32 id = (eip - THUNK_BASE) / 16;
    const api_t *api = g_thunk_api[id];
    if (!api) cpu_fatal(c, "call to unresolved import #%u (%s!%s)", id, W->imports[id].dll, W->imports[id].name);
    u32 a = c->r[ESP] + 4;
    if (api->fn == u_dummy) {
        /* call the window procedure: DispatchMessageA(MSG*) or SendMessageA(hwnd,msg,wp,lp) */
        u32 hwnd, msg, wp, lp; int nargs;
        if (!strcmp(api->name, "DispatchMessageA")) { u32 m = rd32(a); hwnd = rd32(m); msg = rd32(m + 4); wp = rd32(m + 8); lp = rd32(m + 12); nargs = 1; }
        else { hwnd = rd32(a); msg = rd32(a + 4); wp = rd32(a + 8); lp = rd32(a + 12); nargs = 4; }
        int wi = win_find(hwnd);
        u32 ret = pop32(c); c->r[ESP] += 4 * nargs;
        if (wi < 0 || !W->win[wi].proc) { c->r[EAX] = 0; c->eip = ret; return; }
        if (W->trace > 1) fprintf(stderr, "[t%d] wndproc(%x, msg %x, %x, %x)\n", W->cur, hwnd, msg, wp, lp);
        push32(c, lp); push32(c, wp); push32(c, msg); push32(c, hwnd); push32(c, ret);
        c->eip = W->win[wi].proc;
        return;
    }
    if (W->trace > 1) fprintf(stderr, "[t%d] %s(%08x,%08x,%08x)\n", W->cur, api->name, rd32(a), rd32(a + 4), rd32(a + 8));
    g_block = 0;
    u32 ret = api->fn(t, a);
    c->eip = pop32(c);
    if (api->stdcall_args > 0) c->r[ESP] += 4 * api->stdcall_args;
    if (!(g_block && t->state == T_WAIT && t->wait_kind == W_OBJ)) c->r[EAX] = ret;
    if (g_block) c->halted = 1;
}

/* ======================= scheduler ======================= */
static void advance_time(u64 instr) {
    W->vinstr += instr;
    while (W->vinstr >= 200000) { W->vinstr -= 200000; W->vclock_ms++; }
}

/* Run guest threads until `stop` returns nonzero. */
static int run_until(int (*stop)(void), u64 max_instr) {
    u64 total = 0;
    for (;;) {
        if (stop()) return 1;
        if (max_instr && total >= max_instr) return 0;
        /* wake anything that can wake */
        int runnable = 0;
        for (int i = 0; i < MAX_THREADS; i++) { if (W->t[i].state == T_WAIT) try_wake(&W->t[i]); if (W->t[i].state == T_RUN) runnable++; }
        if (!runnable && W->poll_hook && W->poll_hook(W->post_ctx)) continue;
        if (!runnable) {
            s64 next = -1;
            for (int i = 0; i < MAX_THREADS; i++) {
                thr_t *t = &W->t[i];
                if (t->state == T_WAIT && t->deadline >= 0 && (t->wait_kind == W_SLEEP || t->wait_kind == W_OBJ))
                    if (next < 0 || t->deadline < next) next = t->deadline;
            }
            if (next < 0) return -1;   /* everyone blocked forever */
            if (next > W->vclock_ms) W->vclock_ms = next;
            continue;
        }
        for (int k = 1; k <= MAX_THREADS; k++) {
            int i = (W->cur + k) % MAX_THREADS;
            if (W->t[i].state != T_RUN) continue;
            W->cur = i;
            u64 n = cpu_run(&W->t[i].cpu, QUANTUM);
            total += n; advance_time(n);
            break;
        }
        if (atomic_load(&W->interrupt)) return 2;
        if (W->poll_hook && (total & 0x3ffff) < QUANTUM) W->poll_hook(W->post_ctx);
        if (W->trace && (total % 50000000) < QUANTUM) {
            fprintf(stderr, "winrt: %llu instr, vclock %lld ms\n", (unsigned long long)total, (long long)W->vclock_ms);
            for (int i = 0; i < MAX_THREADS; i++) if (W->t[i].state != T_FREE)
                fprintf(stderr, "  t%d id=%x state=%d kind=%d eip=%08x esp=%08x\n", i, W->t[i].id, W->t[i].state, W->t[i].wait_kind, W->t[i].cpu.eip, W->t[i].cpu.r[ESP]);
        }
    }
}

static int g_stop_slot;
static int stop_call(void) { return W->call_done[g_stop_slot]; }
static int stop_never(void) { return 0; }

int winrt_begin(winrt *w, int slot, u32 fn, int nargs, const u32 *args) {
    if (w->call_busy[slot]) return 0;
    thr_t *t = &w->t[w->call_thread[slot]];
    w->call_esp[slot] = t->cpu.r[ESP];
    for (int i = nargs - 1; i >= 0; i--) push32(&t->cpu, args[i]);
    push32(&t->cpu, slot ? MAGIC_RET1 : MAGIC_RET);
    t->cpu.eip = fn;
    t->state = T_RUN;
    w->call_done[slot] = 0; w->call_busy[slot] = 1;
    return 1;
}
int winrt_poll(winrt *w, int slot, u32 *ret) {
    if (!w->call_busy[slot] || !w->call_done[slot]) return 0;
    thr_t *t = &w->t[w->call_thread[slot]];
    t->cpu.r[ESP] = w->call_esp[slot];
    t->state = T_WAIT; t->wait_kind = W_FOREVER;
    w->call_busy[slot] = 0;
    if (ret) *ret = w->call_ret[slot];
    return 1;
}
int winrt_step(winrt *w, u64 instructions) {
    (void)w;
    return run_until(stop_never, instructions);
}
u32 winrt_call_slot(winrt *w, int slot, u32 fn, int nargs, const u32 *args) {
    if (!winrt_begin(w, slot, fn, nargs, args)) { fprintf(stderr, "winrt: slot %d busy\n", slot); return 0xffffffffu; }
    int saved = g_stop_slot;
    g_stop_slot = slot;
    int r = run_until(stop_call, 0);
    g_stop_slot = saved;
    if (r == -1 && !w->call_done[slot]) fprintf(stderr, "winrt: deadlock during call to %08x\n", fn);
    u32 ret = 0; winrt_poll(w, slot, &ret);
    return ret;
}
u32 winrt_call(winrt *w, u32 fn, int nargs, const u32 *args) { return winrt_call_slot(w, 0, fn, nargs, args); }

void winrt_pump(winrt *w, u64 instructions) { (void)w; run_until(stop_never, instructions); }
void winrt_set_hooks(winrt *w, int (*post)(void *, u32, u32, u32, u32), int (*poll)(void *), void *ctx) { w->post_hook = post; w->poll_hook = poll; w->post_ctx = ctx; }
void winrt_interrupt(winrt *w) { atomic_store(&w->interrupt, 1); }
void winrt_set_trace(winrt *w, int level) { w->trace = level; }
void winrt_set_pcm_sink(winrt *w, pcm_sink sink, void *ctx) { w->sink = sink; w->sink_ctx = ctx; }
void winrt_set_pcm_sink2(winrt *w, pcm_sink2 sink, void *ctx) { w->sink2 = sink; w->sink_ctx = ctx; }
void winrt_remap_file(winrt *w, const char *guest_name, const char *host_path) {
    for (int i = 0; i < w->nfmap; i++) if (!strcasecmp(w->fmap[i].guest, guest_name)) { free(w->fmap[i].host); w->fmap[i].host = strdup(host_path); return; }
    winrt_map_file(w, guest_name, host_path);
}
void winrt_map_file(winrt *w, const char *guest_name, const char *host_path) {
    w->fmap = realloc(w->fmap, sizeof(fmap_t) * (w->nfmap + 1));
    w->fmap[w->nfmap].guest = strdup(guest_name);
    w->fmap[w->nfmap].host = strdup(host_path);
    w->nfmap++;
}
u32 winrt_export(winrt *w, const char *name) {
    for (int i = 0; i < w->nexports; i++) if (!strcmp(w->exports[i].name, name)) return w->exports[i].addr;
    return 0;
}

/* ======================= PE loader ======================= */
winrt *winrt_load_mem(const unsigned char *src, long len, char *err, size_t errlen);
winrt *winrt_load(const char *dll_path, char *err, size_t errlen) {
    FILE *f = fopen(dll_path, "rb");
    if (!f) { snprintf(err, errlen, "cannot open %s", dll_path); return NULL; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *b = malloc(len);
    if (fread(b, 1, len, f) != (size_t)len) { fclose(f); free(b); snprintf(err, errlen, "read error"); return NULL; }
    fclose(f);
    winrt *w = winrt_load_mem(b, len, err, errlen);
    free(b);
    return w;
}
winrt *winrt_load_mem(const unsigned char *src, long len, char *err, size_t errlen) {
    if (W) { snprintf(err, errlen, "only one engine can be loaded at a time"); return NULL; }
    u8 *b = malloc(len); memcpy(b, src, len);
    if (!g_mem) {
#ifdef _WIN32
        g_mem = VirtualAlloc(NULL, GUEST_SPACE, MEM_RESERVE, PAGE_NOACCESS);
        if (!g_mem) { free(b); snprintf(err, errlen, "cannot reserve guest memory"); return NULL; }
        AddVectoredExceptionHandler(1, guest_fault);
#else
        g_mem = mmap(NULL, GUEST_SPACE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (g_mem == MAP_FAILED) { g_mem = NULL; free(b); snprintf(err, errlen, "cannot reserve guest memory"); return NULL; }
#endif
    }
    winrt *w = calloc(1, sizeof *w);
    W = w;
    w->next_tid = 0x100; w->heap_top = HEAP_BASE; w->next_hwnd = 0x5000; w->next_msg = 0xC100;
    u32 pe; memcpy(&pe, b + 0x3c, 4);
    if (memcmp(b + pe, "PE\0\0", 4)) { snprintf(err, errlen, "not a PE file"); goto fail; }
    u16 nsec, optsz; memcpy(&nsec, b + pe + 6, 2); memcpy(&optsz, b + pe + 20, 2);
    u8 *opt = b + pe + 24;
    u32 imgbase, imgsize, entry, hdrsize; memcpy(&imgbase, opt + 28, 4); memcpy(&imgsize, opt + 56, 4); memcpy(&entry, opt + 16, 4); memcpy(&hdrsize, opt + 60, 4);
    u32 dirs[16][2]; memcpy(dirs, opt + 96, sizeof dirs);
    w->image_base = imgbase; w->image_size = imgsize; w->entry = entry ? imgbase + entry : 0;
    memcpy(GM(imgbase), b, hdrsize);
    for (int i = 0; i < nsec; i++) {
        u8 *s = opt + optsz + 40 * i;
        u32 vs, va, rs, ro; memcpy(&vs, s + 8, 4); memcpy(&va, s + 12, 4); memcpy(&rs, s + 16, 4); memcpy(&ro, s + 20, 4);
        u32 n = rs < vs || !vs ? rs : vs;
        if (ro && n) memcpy(GM(imgbase + va), b + ro, n);
    }
    /* exports */
    if (dirs[0][0]) {
        u32 e = imgbase + dirs[0][0];
        u32 nnames = rd32(e + 24), funcs = imgbase + rd32(e + 28), names = imgbase + rd32(e + 32), ords = imgbase + rd32(e + 36);
        w->exports = calloc(nnames, sizeof *w->exports); w->nexports = nnames;
        for (u32 i = 0; i < nnames; i++) {
            gstr(imgbase + rd32(names + 4 * i), w->exports[i].name, sizeof w->exports[i].name);
            u16 ord = rd16(ords + 2 * i);
            w->exports[i].addr = imgbase + rd32(funcs + 4 * ord);
        }
    }
    /* imports */
    g_thunk_api = calloc(THUNK_SIZE / 16, sizeof *g_thunk_api);
    w->imports = calloc(THUNK_SIZE / 16, sizeof *w->imports);
    u32 initterm_stub = 0;
    if (dirs[1][0]) {
        for (u32 d = imgbase + dirs[1][0]; rd32(d + 12); d += 20) {
            char dll[64]; gstr(imgbase + rd32(d + 12), dll, sizeof dll);
            u32 ilt = rd32(d) ? imgbase + rd32(d) : imgbase + rd32(d + 16);
            u32 iat = imgbase + rd32(d + 16);
            for (int k = 0; rd32(ilt + 4 * k); k++) {
                u32 ent = rd32(ilt + 4 * k);
                char name[64];
                if (ent & 0x80000000u) snprintf(name, sizeof name, "#%u", ent & 0xffff);
                else gstr(imgbase + ent + 2, name, sizeof name);
                int id = w->nimports++;
                snprintf(w->imports[id].dll, sizeof w->imports[id].dll, "%s", dll);
                snprintf(w->imports[id].name, sizeof w->imports[id].name, "%s", name);
                for (const api_t *a = APIS; a->name; a++) if (!strcmp(a->name, name)) { g_thunk_api[id] = a; break; }
                u32 target = THUNK_BASE + id * 16;
                if (!strcmp(name, "_initterm")) {
                    if (!initterm_stub) { initterm_stub = MISC_BASE + 0x1000; memcpy(GM(initterm_stub), INITTERM_CODE, sizeof INITTERM_CODE); }
                    target = initterm_stub;
                } else if (!g_thunk_api[id] && w->trace) fprintf(stderr, "winrt: unresolved import %s!%s\n", dll, name);
                wr32(iat + 4 * k, target);
            }
        }
    }
    /* stdio iob (3 * 32 bytes) */
    w->iob = MISC_BASE + 0x2000;
    memset(GM(w->iob), 0, 96);
    g_thunk_handler = thunk;
    /* host "call" thread */
    for (int sl = 0; sl < 2; sl++) {
        w->call_thread[sl] = new_thread(0, 0);
        w->t[w->call_thread[sl]].state = T_WAIT; w->t[w->call_thread[sl]].wait_kind = W_FOREVER;
    }
    free(b);
    /* DllMain(hinst, DLL_PROCESS_ATTACH, 0) */
    if (w->entry) {
        u32 args[3] = {imgbase, 1, 0};
        u32 r = winrt_call(w, w->entry, 3, args);
        if (!r) { snprintf(err, errlen, "DllMain failed"); goto fail2; }
    }
    return w;
fail:
    free(b);
fail2:
    W = NULL; free(w);
    return NULL;
}

void winrt_destroy(winrt *w) {
    if (!w) return;
    for (int i = 0; i < MAX_FILES; i++) if (w->files[i].used && w->files[i].f) fclose(w->files[i].f);
    for (int i = 0; i < w->nfmap; i++) { free(w->fmap[i].guest); free(w->fmap[i].host); }
    free(w->fmap); free(w->exports); free(w->imports);
    free(g_thunk_api); g_thunk_api = NULL;
    /* release guest memory pages (keeps the reservation) */
#ifdef _WIN32
    VirtualFree(g_mem, GUEST_SPACE, MEM_DECOMMIT);
#else
    madvise(g_mem, GUEST_SPACE, MADV_DONTNEED);
#endif
    memset(GM(0), 0, 0);  /* noop */
    W = NULL;
    free(w);
}
void winrt_dump(void) {
    if (!W) return;
    fprintf(stderr, "winrt dump: cur=t%d vclock=%lld\n", W->cur, (long long)W->vclock_ms);
    for (int i = 0; i < MAX_THREADS; i++) if (W->t[i].state != T_FREE) {
        cpu_t *c = &W->t[i].cpu;
        fprintf(stderr, "  t%d id=%x state=%d kind=%d eip=%08x esp=%08x eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x\n", i, W->t[i].id, W->t[i].state, W->t[i].wait_kind, c->eip, c->r[ESP], c->r[0], c->r[1], c->r[2], c->r[3], c->r[6], c->r[7]);
    }
}

#ifdef _WIN32
static LONG CALLBACK guest_fault(PEXCEPTION_POINTERS e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    u8 *a = (u8 *)e->ExceptionRecord->ExceptionInformation[1];
    if (!g_mem || a < g_mem || a >= g_mem + GUEST_SPACE) return EXCEPTION_CONTINUE_SEARCH;
    u8 *page = g_mem + (((a - g_mem)) & ~(u64)0xffff);       /* commit 64 KB at a time */
    if (!VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE)) return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_CONTINUE_EXECUTION;
}
#endif
