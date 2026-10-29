/* Minimal 32-bit x86 interpreter (flat memory, user mode) for running DECtalk DLLs. */
#ifndef CPU_H
#define CPU_H
#include <stdint.h>
#include <string.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;

enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

#define F_CF 0x0001
#define F_PF 0x0004
#define F_AF 0x0010
#define F_ZF 0x0040
#define F_SF 0x0080
#define F_DF 0x0400
#define F_OF 0x0800

typedef union { u8 b[16]; u16 w[8]; u32 d[4]; u64 q[2]; s8 sb[16]; s16 sw[8]; s32 sd[4]; s64 sq[2]; float f[4]; double g[2]; } xmm_t;

typedef struct cpu {
    u32 r[8];
    u32 eip;
    u32 flags;
    u32 fsbase;
    xmm_t x[8];
    long double st[8];
    int top;
    u16 fcw, fsw;
    u32 mxcsr;
    u64 icount;
    int halted;          /* set by the host to stop the run loop */
} cpu_t;

extern u8 *g_mem;        /* guest address 0 .. 4 GB maps to g_mem + addr */
#define GM(a) (g_mem + (u32)(a))

static inline u8  rd8 (u32 a) { return g_mem[a]; }
static inline u16 rd16(u32 a) { u16 v; memcpy(&v, GM(a), 2); return v; }
static inline u32 rd32(u32 a) { u32 v; memcpy(&v, GM(a), 4); return v; }
static inline u64 rd64(u32 a) { u64 v; memcpy(&v, GM(a), 8); return v; }
static inline void wr8 (u32 a, u8 v)  { g_mem[a] = v; }
static inline void wr16(u32 a, u16 v) { memcpy(GM(a), &v, 2); }
static inline void wr32(u32 a, u32 v) { memcpy(GM(a), &v, 4); }
static inline void wr64(u32 a, u64 v) { memcpy(GM(a), &v, 8); }

/* Called when EIP lands in the thunk area [THUNK_BASE, THUNK_BASE+THUNK_SIZE). */
#define THUNK_BASE 0xFFF00000u
#define THUNK_SIZE 0x00010000u
typedef void (*thunk_fn)(cpu_t *c, u32 eip);
extern thunk_fn g_thunk_handler;

/* Run until c->halted is set or `budget` instructions have executed. Returns instructions run. */
u64 cpu_run(cpu_t *c, u64 budget);
void cpu_fatal(cpu_t *c, const char *fmt, ...);

static inline void push32(cpu_t *c, u32 v) { c->r[ESP] -= 4; wr32(c->r[ESP], v); }
static inline u32 pop32(cpu_t *c) { u32 v = rd32(c->r[ESP]); c->r[ESP] += 4; return v; }
#endif
