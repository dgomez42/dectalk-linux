/* Tiny Win32 runtime for running a DECtalk DLL inside the x86 interpreter. */
#ifndef WINRT_H
#define WINRT_H
#include "cpu.h"
#include <stddef.h>

typedef struct winrt winrt;

/* PCM sink: called with 16-bit samples as the DLL writes its wave output file. */
typedef void (*pcm_sink)(void *ctx, const int16_t *samples, size_t n);
/* Sink with stream id: wave files named "@<id>..." deliver to id. */
typedef void (*pcm_sink2)(void *ctx, int id, const int16_t *samples, size_t n);

/* Creates the runtime and loads the DLL. Returns NULL on failure (err filled). */
winrt *winrt_load(const char *dll_path, char *err, size_t errlen);
void winrt_destroy(winrt *w);

/* Map a file name the DLL may fopen (matched case-insensitively on its base name) to a host path. */
void winrt_map_file(winrt *w, const char *guest_name, const char *host_path);
/* Wave files the DLL opens whose name starts with '@' are routed to this sink. */
void winrt_set_pcm_sink(winrt *w, pcm_sink sink, void *ctx);
void winrt_set_pcm_sink2(winrt *w, pcm_sink2 sink, void *ctx);
void winrt_remap_file(winrt *w, const char *guest_name, const char *host_path);
winrt *winrt_load_mem(const unsigned char *src, long len, char *err, size_t errlen);

/* Address of an exported function (0 if missing). */
u32 winrt_export(winrt *w, const char *name);

/* Guest memory helpers for passing arguments. */
u32 winrt_alloc(winrt *w, u32 size);
void winrt_free(winrt *w, u32 ptr);
u32 winrt_strdup(winrt *w, const char *s);

/* Call a guest function (stdcall or cdecl both fine: the stack is restored afterwards) and
 * run all guest threads until it returns. Returns EAX. */
u32 winrt_call(winrt *w, u32 fn, int nargs, const u32 *args);

/* Asynchronous calls on one of two host "call threads" (slot 0 or 1), so one call (e.g. Reset)
 * can be made while another (e.g. Sync) is still waiting. */
int winrt_begin(winrt *w, int slot, u32 fn, int nargs, const u32 *args);
int winrt_poll(winrt *w, int slot, u32 *ret);           /* 1 when the call has returned */
int winrt_step(winrt *w, u64 instructions);              /* run guest threads; -1 if all are blocked */
u32 winrt_call_slot(winrt *w, int slot, u32 fn, int nargs, const u32 *args);

/* Run the other guest threads for a while without a call in progress (background work). */
void winrt_pump(winrt *w, u64 instructions);

/* Request that a running winrt_call returns early (used for Stop). Thread-safe flag. */
void winrt_interrupt(winrt *w);

/* Host hooks: post(ctx, hwnd, msg, wp, lp) sees PostMessage to windows the guest did not create
 * (return 1 if handled); poll(ctx) runs between scheduler slices (return 1 if it did work). */
void winrt_set_hooks(winrt *w, int (*post)(void *, u32, u32, u32, u32), int (*poll)(void *), void *ctx);
u32 winrt_msg_id(const char *name);   /* id the guest got from RegisterWindowMessageA(name), or 0 */
/* Diagnostics */
void winrt_set_trace(winrt *w, int level);
#endif
