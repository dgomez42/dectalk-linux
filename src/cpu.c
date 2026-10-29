#include "compat.h"
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>

u8 *g_mem;
thunk_fn g_thunk_handler;

void cpu_fatal(cpu_t *c, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "x86: fatal at eip=%08x: ", c->eip);
    vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
    fprintf(stderr, "  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n",
            c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7]);
    fprintf(stderr, "  bytes:"); for (int i = 0; i < 12; i++) fprintf(stderr, " %02x", rd8(c->eip + i)); fprintf(stderr, "\n");
    va_end(ap);
    abort();
}

/* ---------------- flags ---------------- */
static const u32 MASK[5] = {0, 0xff, 0xffff, 0, 0xffffffffu};
static const u32 SIGN[5] = {0, 0x80, 0x8000, 0, 0x80000000u};

static inline int parity8(u32 v) { v &= 0xff; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return !(v & 1); }

static inline void set_szp(cpu_t *c, u32 res, int sz) {
    res &= MASK[sz];
    c->flags &= ~(F_ZF | F_SF | F_PF);
    if (!res) c->flags |= F_ZF;
    if (res & SIGN[sz]) c->flags |= F_SF;
    if (parity8(res)) c->flags |= F_PF;
}
static inline void setf(cpu_t *c, u32 f, int on) { if (on) c->flags |= f; else c->flags &= ~f; }

static inline u32 alu(cpu_t *c, int op, u32 a, u32 b, int sz) {
    u32 m = MASK[sz], s = SIGN[sz], res; u64 wide;
    a &= m; b &= m;
    switch (op) {
    case 0: /* add */
        wide = (u64)a + b; res = (u32)wide & m;
        setf(c, F_CF, wide > m); setf(c, F_OF, ((a ^ res) & (b ^ res) & s) != 0);
        setf(c, F_AF, ((a ^ b ^ res) & 0x10) != 0); set_szp(c, res, sz); return res;
    case 1: res = (a | b) & m; c->flags &= ~(F_CF | F_OF | F_AF); set_szp(c, res, sz); return res;
    case 2: { /* adc */
        u32 cf = c->flags & F_CF ? 1 : 0;
        wide = (u64)a + b + cf; res = (u32)wide & m;
        setf(c, F_CF, wide > m); setf(c, F_OF, ((a ^ res) & (b ^ res) & s) != 0);
        setf(c, F_AF, ((a ^ b ^ res) & 0x10) != 0); set_szp(c, res, sz); return res; }
    case 3: { /* sbb */
        u32 cf = c->flags & F_CF ? 1 : 0;
        res = (a - b - cf) & m;
        setf(c, F_CF, (u64)a < (u64)b + cf); setf(c, F_OF, ((a ^ b) & (a ^ res) & s) != 0);
        setf(c, F_AF, ((a ^ b ^ res) & 0x10) != 0); set_szp(c, res, sz); return res; }
    case 4: res = a & b; c->flags &= ~(F_CF | F_OF | F_AF); set_szp(c, res, sz); return res;
    case 5: case 7: /* sub, cmp */
        res = (a - b) & m;
        setf(c, F_CF, a < b); setf(c, F_OF, ((a ^ b) & (a ^ res) & s) != 0);
        setf(c, F_AF, ((a ^ b ^ res) & 0x10) != 0); set_szp(c, res, sz); return res;
    case 6: res = (a ^ b) & m; c->flags &= ~(F_CF | F_OF | F_AF); set_szp(c, res, sz); return res;
    }
    return 0;
}

static inline int cond(cpu_t *c, int cc) {
    u32 f = c->flags; int r;
    switch (cc >> 1) {
    case 0: r = (f & F_OF) != 0; break;
    case 1: r = (f & F_CF) != 0; break;
    case 2: r = (f & F_ZF) != 0; break;
    case 3: r = (f & (F_CF | F_ZF)) != 0; break;
    case 4: r = (f & F_SF) != 0; break;
    case 5: r = (f & F_PF) != 0; break;
    case 6: r = ((f & F_SF) != 0) != ((f & F_OF) != 0); break;
    default: r = (f & F_ZF) || (((f & F_SF) != 0) != ((f & F_OF) != 0)); break;
    }
    return (cc & 1) ? !r : r;
}

/* ---------------- decoding ---------------- */
typedef struct {
    int sz;          /* operand size 2 or 4 */
    int rep;         /* 0, 0xF2, 0xF3 */
    int seg_fs;
    int opsize66;
    u8 modrm; int mod, reg, rm;
    u32 addr;        /* effective address when mod != 3 */
} dec_t;

static inline u8 fetch8(cpu_t *c) { return rd8(c->eip++); }
static inline u16 fetch16(cpu_t *c) { u16 v = rd16(c->eip); c->eip += 2; return v; }
static inline u32 fetch32(cpu_t *c) { u32 v = rd32(c->eip); c->eip += 4; return v; }

static void decode_modrm(cpu_t *c, dec_t *d) {
    d->modrm = fetch8(c); d->mod = d->modrm >> 6; d->reg = (d->modrm >> 3) & 7; d->rm = d->modrm & 7;
    if (d->mod == 3) return;
    u32 a = 0;
    if (d->rm == 4) {
        u8 sib = fetch8(c); int sc = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
        if (base == 5 && d->mod == 0) a = fetch32(c); else a = c->r[base];
        if (idx != 4) a += c->r[idx] << sc;
    } else if (d->rm == 5 && d->mod == 0) {
        a = fetch32(c);
    } else a = c->r[d->rm];
    if (d->mod == 1) a += (u32)(s32)(s8)fetch8(c);
    else if (d->mod == 2) a += fetch32(c);
    if (d->seg_fs) a += c->fsbase;
    d->addr = a;
}

static inline u32 getreg(cpu_t *c, int r, int sz) {
    if (sz == 4) return c->r[r];
    if (sz == 2) return c->r[r] & 0xffff;
    return r < 4 ? (c->r[r] & 0xff) : ((c->r[r - 4] >> 8) & 0xff);
}
static inline void setreg(cpu_t *c, int r, int sz, u32 v) {
    if (sz == 4) c->r[r] = v;
    else if (sz == 2) c->r[r] = (c->r[r] & 0xffff0000u) | (v & 0xffff);
    else if (r < 4) c->r[r] = (c->r[r] & 0xffffff00u) | (v & 0xff);
    else c->r[r - 4] = (c->r[r - 4] & 0xffff00ffu) | ((v & 0xff) << 8);
}
static inline u32 rdm(u32 a, int sz) { return sz == 4 ? rd32(a) : sz == 2 ? rd16(a) : rd8(a); }
static inline void wrm(u32 a, int sz, u32 v) { if (sz == 4) wr32(a, v); else if (sz == 2) wr16(a, v); else wr8(a, v); }
static inline u32 get_rm(cpu_t *c, dec_t *d, int sz) { return d->mod == 3 ? getreg(c, d->rm, sz) : rdm(d->addr, sz); }
static inline void set_rm(cpu_t *c, dec_t *d, int sz, u32 v) { if (d->mod == 3) setreg(c, d->rm, sz, v); else wrm(d->addr, sz, v); }

/* ---------------- shifts ---------------- */
static u32 shift_op(cpu_t *c, int op, u32 v, int cnt, int sz) {
    u32 m = MASK[sz], s = SIGN[sz]; int bits = sz * 8;
    cnt &= 31;
    if (!cnt) return v & m;
    v &= m;
    u32 res = v;
    switch (op) {
    case 0: { /* rol */
        int n = cnt % bits; res = n ? ((v << n) | (v >> (bits - n))) & m : v;
        setf(c, F_CF, res & 1); if (cnt == 1) setf(c, F_OF, ((res & s) != 0) ^ (res & 1)); return res; }
    case 1: { /* ror */
        int n = cnt % bits; res = n ? ((v >> n) | (v << (bits - n))) & m : v;
        setf(c, F_CF, (res & s) != 0); if (cnt == 1) setf(c, F_OF, ((res ^ (res << 1)) & s) != 0); return res; }
    case 2: { /* rcl */
        u64 x = v; int cf = (c->flags & F_CF) != 0; int n = cnt % (bits + 1);
        for (int i = 0; i < n; i++) { int nc = (x & s) != 0; x = ((x << 1) | cf) & m; cf = nc; }
        setf(c, F_CF, cf); if (cnt == 1) setf(c, F_OF, ((x & s) != 0) ^ cf); return (u32)x; }
    case 3: { /* rcr */
        u64 x = v; int cf = (c->flags & F_CF) != 0; int n = cnt % (bits + 1);
        if (cnt == 1) setf(c, F_OF, ((x & s) != 0) ^ cf);
        for (int i = 0; i < n; i++) { int nc = x & 1; x = (x >> 1) | (cf ? s : 0); cf = nc; }
        setf(c, F_CF, cf); return (u32)x; }
    case 4: case 6: /* shl */
        res = cnt < bits ? (v << cnt) & m : 0;
        setf(c, F_CF, cnt <= bits ? (v >> (bits - cnt)) & 1 : 0);
        setf(c, F_OF, ((res & s) != 0) ^ ((c->flags & F_CF) != 0));
        set_szp(c, res, sz); return res;
    case 5: /* shr */
        res = cnt < bits ? v >> cnt : 0;
        setf(c, F_CF, cnt <= bits ? (v >> (cnt - 1)) & 1 : 0);
        setf(c, F_OF, (v & s) != 0);
        set_szp(c, res, sz); return res;
    case 7: { /* sar */
        s32 sv = sz == 4 ? (s32)v : sz == 2 ? (s16)v : (s8)v;
        int n = cnt < bits ? cnt : bits - 1;
        res = (u32)(sv >> n) & m;
        setf(c, F_CF, (sv >> (cnt < bits ? cnt - 1 : bits - 1)) & 1);
        setf(c, F_OF, 0);
        set_szp(c, res, sz); return res; }
    }
    return res;
}

/* ---------------- x87 (small subset) ---------------- */
#define ST(i) c->st[(c->top + (i)) & 7]
static void fpush(cpu_t *c, long double v) { c->top = (c->top - 1) & 7; c->st[c->top] = v; }
static long double fpop(cpu_t *c) { long double v = c->st[c->top]; c->top = (c->top + 1) & 7; return v; }
static void fcompare(cpu_t *c, long double a, long double b) {
    c->fsw &= ~0x4500;
    if (isnan(a) || isnan(b)) c->fsw |= 0x4500;
    else if (a < b) c->fsw |= 0x0100;
    else if (a == b) c->fsw |= 0x4000;
}
static void fcomi(cpu_t *c, long double a, long double b) {
    c->flags &= ~(F_ZF | F_PF | F_CF);
    if (isnan(a) || isnan(b)) c->flags |= F_ZF | F_PF | F_CF;
    else if (a < b) c->flags |= F_CF;
    else if (a == b) c->flags |= F_ZF;
}
static long double frnd(cpu_t *c, long double v) {
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyintl(v);
    case 1: return floorl(v);
    case 2: return ceill(v);
    default: return truncl(v);
    }
}
static void farith(cpu_t *c, int op, long double *dst, long double src) {
    switch (op) {
    case 0: *dst += src; break;
    case 1: *dst *= src; break;
    case 4: *dst -= src; break;
    case 5: *dst = src - *dst; break;
    case 6: *dst /= src; break;
    case 7: *dst = src / *dst; break;
    }
}
static void x87(cpu_t *c, dec_t *d, u8 op) {
    decode_modrm(c, d);
    int r = d->reg;
    if (d->mod != 3) {
        u32 a = d->addr;
        switch (op) {
        case 0xD8: { float f; memcpy(&f, GM(a), 4); if (r == 2) fcompare(c, ST(0), f); else if (r == 3) { fcompare(c, ST(0), f); fpop(c); } else farith(c, r, &ST(0), f); return; }
        case 0xDC: { double f; memcpy(&f, GM(a), 8); if (r == 2) fcompare(c, ST(0), f); else if (r == 3) { fcompare(c, ST(0), f); fpop(c); } else farith(c, r, &ST(0), f); return; }
        case 0xDA: { s32 i = (s32)rd32(a); if (r == 2) fcompare(c, ST(0), i); else if (r == 3) { fcompare(c, ST(0), i); fpop(c); } else farith(c, r, &ST(0), i); return; }
        case 0xDE: { s16 i = (s16)rd16(a); if (r == 2) fcompare(c, ST(0), i); else if (r == 3) { fcompare(c, ST(0), i); fpop(c); } else farith(c, r, &ST(0), i); return; }
        case 0xD9:
            switch (r) {
            case 0: { float f; memcpy(&f, GM(a), 4); fpush(c, f); return; }
            case 2: { float f = (float)ST(0); memcpy(GM(a), &f, 4); return; }
            case 3: { float f = (float)fpop(c); memcpy(GM(a), &f, 4); return; }
            case 5: c->fcw = rd16(a); return;
            case 7: wr16(a, c->fcw); return;
            case 4: case 6: return; /* fldenv/fnstenv: ignore */
            }
            break;
        case 0xDD:
            switch (r) {
            case 0: { double f; memcpy(&f, GM(a), 8); fpush(c, f); return; }
            case 1: { s64 v = (s64)truncl(fpop(c)); wr64(a, (u64)v); return; } /* fisttp m64 */
            case 2: { double f = (double)ST(0); memcpy(GM(a), &f, 8); return; }
            case 3: { double f = (double)fpop(c); memcpy(GM(a), &f, 8); return; }
            case 7: wr16(a, (c->fsw & ~0x3800) | ((c->top & 7) << 11)); return;
            }
            break;
        case 0xDB:
            switch (r) {
            case 0: fpush(c, (s32)rd32(a)); return;
            case 1: { s32 v = (s32)truncl(fpop(c)); wr32(a, (u32)v); return; }
            case 2: wr32(a, (u32)(s32)frnd(c, ST(0))); return;
            case 3: wr32(a, (u32)(s32)frnd(c, fpop(c))); return;
            case 5: { long double v; u8 buf[16] = {0}; memcpy(buf, GM(a), 10); memcpy(&v, buf, sizeof v < 16 ? sizeof v : 16); fpush(c, v); return; }
            case 7: { long double v = fpop(c); memcpy(GM(a), &v, 10); return; }
            }
            break;
        case 0xDF:
            switch (r) {
            case 0: fpush(c, (s16)rd16(a)); return;
            case 1: { s16 v = (s16)truncl(fpop(c)); wr16(a, (u16)v); return; }
            case 2: wr16(a, (u16)(s16)frnd(c, ST(0))); return;
            case 3: wr16(a, (u16)(s16)frnd(c, fpop(c))); return;
            case 5: fpush(c, (s64)rd64(a)); return;
            case 7: wr64(a, (u64)(s64)frnd(c, fpop(c))); return;
            }
            break;
        }
        cpu_fatal(c, "x87 mem op %02x /%d", op, r);
    }
    int i = d->rm;
    switch (op) {
    case 0xD8: if (r == 2) fcompare(c, ST(0), ST(i)); else if (r == 3) { fcompare(c, ST(0), ST(i)); fpop(c); } else farith(c, r, &ST(0), ST(i)); return;
    case 0xDC: { int rr = r; if (rr == 4) rr = 5; else if (rr == 5) rr = 4; else if (rr == 6) rr = 7; else if (rr == 7) rr = 6; farith(c, rr, &ST(i), ST(0)); return; }
    case 0xDE:
        if (r == 3 && i == 1) { fcompare(c, ST(0), ST(1)); fpop(c); fpop(c); return; }
        { int rr = r; if (rr == 4) rr = 5; else if (rr == 5) rr = 4; else if (rr == 6) rr = 7; else if (rr == 7) rr = 6; farith(c, rr, &ST(i), ST(0)); fpop(c); return; }
    case 0xD9:
        if (r == 0) { long double v = ST(i); fpush(c, v); return; }
        if (r == 1) { long double t = ST(0); ST(0) = ST(i); ST(i) = t; return; }
        if (d->modrm == 0xE0) { ST(0) = -ST(0); return; }
        if (d->modrm == 0xE1) { ST(0) = fabsl(ST(0)); return; }
        if (d->modrm == 0xE4) { fcompare(c, ST(0), 0.0L); return; }
        if (d->modrm == 0xE8) { fpush(c, 1.0L); return; }
        if (d->modrm == 0xEE) { fpush(c, 0.0L); return; }
        if (d->modrm == 0xEB) { fpush(c, 3.14159265358979323846264338327950288L); return; }
        if (d->modrm == 0xED) { fpush(c, 0.693147180559945309417232121458176568L); return; }
        if (d->modrm == 0xEA) { fpush(c, 1.44269504088896340735992468100189214L); return; }
        if (d->modrm == 0xFA) { ST(0) = sqrtl(ST(0)); return; }
        if (d->modrm == 0xFC) { ST(0) = frnd(c, ST(0)); return; }
        if (d->modrm == 0xFE) { ST(0) = sinl(ST(0)); return; }
        if (d->modrm == 0xFF) { ST(0) = cosl(ST(0)); return; }
        if (d->modrm == 0xC9) { long double t = ST(0); ST(0) = ST(1); ST(1) = t; return; }
        if (d->modrm == 0xF1) { long double x = fpop(c); ST(0) = ST(0) * log2l(x); return; } /* fyl2x */
        if (d->modrm == 0xF0) { ST(0) = exp2l(ST(0)) - 1; return; }
        if (d->modrm == 0xFD) { ST(0) = ST(0) * exp2l(truncl(ST(1))); return; }
        if (d->modrm == 0xF8) { ST(0) = fmodl(ST(0), ST(1)); return; }
        break;
    case 0xDD:
        if (r == 3) { ST(i) = ST(0); fpop(c); return; }
        if (r == 2) { ST(i) = ST(0); return; }
        if (r == 0) return; /* ffree */
        if (r == 4) { fcompare(c, ST(0), ST(i)); return; }
        if (r == 5) { fcompare(c, ST(0), ST(i)); fpop(c); return; }
        break;
    case 0xDA:
        if (d->modrm == 0xE9) { fcompare(c, ST(0), ST(1)); fpop(c); fpop(c); return; }
        if (r <= 3) { int cc[4] = {1 << 1, 2 << 1, 3 << 1, 5 << 1}; /* fcmovb/e/be/u */ if (cond(c, cc[r])) ST(0) = ST(i); return; }
        break;
    case 0xDB:
        if (d->modrm == 0xE2) { c->fsw &= 0x7f00; return; }
        if (d->modrm == 0xE3) { c->fcw = 0x37f; c->fsw = 0; c->top = 0; return; }
        if (r == 5) { fcomi(c, ST(0), ST(i)); return; }
        if (r == 6) { fcomi(c, ST(0), ST(i)); return; }
        if (r <= 3) { int cc[4] = {1 << 1 | 1, 2 << 1 | 1, 3 << 1 | 1, 5 << 1 | 1}; if (cond(c, cc[r])) ST(0) = ST(i); return; }
        break;
    case 0xDF:
        if (d->modrm == 0xE0) { setreg(c, EAX, 2, (c->fsw & ~0x3800) | ((c->top & 7) << 11)); return; }
        if (r == 5 || r == 6) { fcomi(c, ST(0), ST(i)); fpop(c); return; }
        break;
    }
    cpu_fatal(c, "x87 reg op %02x %02x", op, d->modrm);
}

/* ---------------- SSE helpers ---------------- */
static inline void xmm_load(cpu_t *c, dec_t *d, xmm_t *v) { if (d->mod == 3) *v = c->x[d->rm]; else memcpy(v, GM(d->addr), 16); }
static inline u64 xmm_load64(cpu_t *c, dec_t *d) { return d->mod == 3 ? c->x[d->rm].q[0] : rd64(d->addr); }
static inline u32 xmm_load32(cpu_t *c, dec_t *d) { return d->mod == 3 ? c->x[d->rm].d[0] : rd32(d->addr); }
static inline u8 sat_u8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (u8)v; }
static inline s16 sat_s16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (s16)v; }
static inline u16 sat_u16(int v) { return v < 0 ? 0 : v > 65535 ? 65535 : (u16)v; }
static inline s8 sat_s8(int v) { return v < -128 ? -128 : v > 127 ? 127 : (s8)v; }

static s32 cvt_d2i(cpu_t *c, double v, int trunc) {
    if (isnan(v) || v >= 2147483648.0 || v < -2147483648.0) return (s32)0x80000000;
    if (trunc) return (s32)v;
    switch ((c->mxcsr >> 13) & 3) {
    case 0: return (s32)nearbyint(v);
    case 1: return (s32)floor(v);
    case 2: return (s32)ceil(v);
    default: return (s32)v;
    }
}

static int cmp_pred(double a, double b, int p) {
    switch (p & 7) {
    case 0: return a == b;
    case 1: return a < b;
    case 2: return a <= b;
    case 3: return isnan(a) || isnan(b);
    case 4: return !(a == b);
    case 5: return !(a < b);
    case 6: return !(a <= b);
    default: return !(isnan(a) || isnan(b));
    }
}

static void sse_op(cpu_t *c, dec_t *d, u8 op) {
    int p66 = d->opsize66, pf3 = d->rep == 0xF3, pf2 = d->rep == 0xF2;
    decode_modrm(c, d);
    xmm_t *X = &c->x[d->reg], s;
    switch (op) {
    case 0x10:
        if (pf3) { u32 v = xmm_load32(c, d); if (d->mod == 3) X->d[0] = v; else { memset(X, 0, 16); X->d[0] = v; } }
        else if (pf2) { u64 v = xmm_load64(c, d); if (d->mod == 3) X->q[0] = v; else { X->q[0] = v; X->q[1] = 0; } }
        else xmm_load(c, d, X);
        return;
    case 0x11:
        if (pf3) { if (d->mod == 3) c->x[d->rm].d[0] = X->d[0]; else wr32(d->addr, X->d[0]); }
        else if (pf2) { if (d->mod == 3) c->x[d->rm].q[0] = X->q[0]; else wr64(d->addr, X->q[0]); }
        else { if (d->mod == 3) c->x[d->rm] = *X; else memcpy(GM(d->addr), X, 16); }
        return;
    case 0x12: if (d->mod == 3) X->q[0] = c->x[d->rm].q[1]; else X->q[0] = rd64(d->addr); return; /* movlps/movlpd/movhlps */
    case 0x13: wr64(d->addr, X->q[0]); return;
    case 0x14: xmm_load(c, d, &s);
        if (p66) { X->q[1] = s.q[0]; }
        else { xmm_t r; r.d[0] = X->d[0]; r.d[1] = s.d[0]; r.d[2] = X->d[1]; r.d[3] = s.d[1]; *X = r; }
        return;
    case 0x15: xmm_load(c, d, &s);
        if (p66) { X->q[0] = X->q[1]; X->q[1] = s.q[1]; }
        else { xmm_t r; r.d[0] = X->d[2]; r.d[1] = s.d[2]; r.d[2] = X->d[3]; r.d[3] = s.d[3]; *X = r; }
        return;
    case 0x16: if (d->mod == 3) X->q[1] = c->x[d->rm].q[0]; else X->q[1] = rd64(d->addr); return;
    case 0x17: wr64(d->addr, X->q[1]); return;
    case 0x28: xmm_load(c, d, X); return;
    case 0x29: if (d->mod == 3) c->x[d->rm] = *X; else memcpy(GM(d->addr), X, 16); return;
    case 0x2A: {
        s32 v = (s32)(d->mod == 3 ? c->r[d->rm] : rd32(d->addr));
        if (pf2) X->g[0] = v; else if (pf3) X->f[0] = (float)v; else cpu_fatal(c, "cvtpi2ps");
        return; }
    case 0x2C: case 0x2D: {
        double v;
        if (pf2) { u64 q = xmm_load64(c, d); memcpy(&v, &q, 8); }
        else if (pf3) { u32 q = xmm_load32(c, d); float f; memcpy(&f, &q, 4); v = f; }
        else { cpu_fatal(c, "cvtps2pi"); return; }
        c->r[d->reg] = (u32)cvt_d2i(c, v, op == 0x2C);
        return; }
    case 0x2E: case 0x2F: {
        double a, b;
        if (p66) { a = X->g[0]; u64 q = xmm_load64(c, d); memcpy(&b, &q, 8); }
        else { a = X->f[0]; u32 q = xmm_load32(c, d); float f; memcpy(&f, &q, 4); b = f; }
        c->flags &= ~(F_ZF | F_PF | F_CF | F_OF | F_SF | F_AF);
        if (isnan(a) || isnan(b)) c->flags |= F_ZF | F_PF | F_CF;
        else if (a < b) c->flags |= F_CF;
        else if (a == b) c->flags |= F_ZF;
        return; }
    case 0x51:
        if (pf2) { u64 q = xmm_load64(c, d); double v; memcpy(&v, &q, 8); X->g[0] = sqrt(v); }
        else if (pf3) { u32 q = xmm_load32(c, d); float v; memcpy(&v, &q, 4); X->f[0] = sqrtf(v); }
        else if (p66) { xmm_load(c, d, &s); X->g[0] = sqrt(s.g[0]); X->g[1] = sqrt(s.g[1]); }
        else { xmm_load(c, d, &s); for (int i = 0; i < 4; i++) X->f[i] = sqrtf(s.f[i]); }
        return;
    case 0x54: xmm_load(c, d, &s); X->q[0] &= s.q[0]; X->q[1] &= s.q[1]; return;
    case 0x55: xmm_load(c, d, &s); X->q[0] = ~X->q[0] & s.q[0]; X->q[1] = ~X->q[1] & s.q[1]; return;
    case 0x56: xmm_load(c, d, &s); X->q[0] |= s.q[0]; X->q[1] |= s.q[1]; return;
    case 0x57: xmm_load(c, d, &s); X->q[0] ^= s.q[0]; X->q[1] ^= s.q[1]; return;
    case 0x58: case 0x59: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        int n; int dbl;
        if (pf2) { n = 1; dbl = 1; u64 q = xmm_load64(c, d); s.q[0] = q; }
        else if (pf3) { n = 1; dbl = 0; s.d[0] = xmm_load32(c, d); }
        else if (p66) { n = 2; dbl = 1; xmm_load(c, d, &s); }
        else { n = 4; dbl = 0; xmm_load(c, d, &s); }
        for (int i = 0; i < n; i++) {
            if (dbl) {
                double a = X->g[i], b = s.g[i], r;
                switch (op) { case 0x58: r = a + b; break; case 0x59: r = a * b; break; case 0x5C: r = a - b; break;
                case 0x5E: r = a / b; break; case 0x5D: r = (a < b) ? a : b; break; default: r = (a > b) ? a : b; }
                X->g[i] = r;
            } else {
                float a = X->f[i], b = s.f[i], r;
                switch (op) { case 0x58: r = a + b; break; case 0x59: r = a * b; break; case 0x5C: r = a - b; break;
                case 0x5E: r = a / b; break; case 0x5D: r = (a < b) ? a : b; break; default: r = (a > b) ? a : b; }
                X->f[i] = r;
            }
        }
        return; }
    case 0x5A:
        if (pf2) { u64 q = xmm_load64(c, d); double v; memcpy(&v, &q, 8); X->f[0] = (float)v; }
        else if (pf3) { u32 q = xmm_load32(c, d); float v; memcpy(&v, &q, 4); X->g[0] = v; }
        else if (p66) { xmm_load(c, d, &s); X->f[0] = (float)s.g[0]; X->f[1] = (float)s.g[1]; X->q[1] = 0; }
        else { xmm_load(c, d, &s); double a = s.f[0], b = s.f[1]; X->g[0] = a; X->g[1] = b; }
        return;
    case 0x5B:
        xmm_load(c, d, &s);
        if (p66) for (int i = 0; i < 4; i++) X->sd[i] = cvt_d2i(c, s.f[i], 0);
        else if (pf3) for (int i = 0; i < 4; i++) X->sd[i] = cvt_d2i(c, s.f[i], 1);
        else for (int i = 0; i < 4; i++) X->f[i] = (float)s.sd[i];
        return;
    case 0xE6:
        xmm_load(c, d, &s);
        if (pf3) { double a = s.sd[0], b = s.sd[1]; X->g[0] = a; X->g[1] = b; }
        else if (pf2) { X->sd[0] = cvt_d2i(c, s.g[0], 0); X->sd[1] = cvt_d2i(c, s.g[1], 0); X->q[1] = 0; }
        else { X->sd[0] = cvt_d2i(c, s.g[0], 1); X->sd[1] = cvt_d2i(c, s.g[1], 1); X->q[1] = 0; }
        return;
    case 0xC2: {
        int n, dbl;
        if (pf2) { n = 1; dbl = 1; s.q[0] = xmm_load64(c, d); }
        else if (pf3) { n = 1; dbl = 0; s.d[0] = xmm_load32(c, d); }
        else if (p66) { n = 2; dbl = 1; xmm_load(c, d, &s); }
        else { n = 4; dbl = 0; xmm_load(c, d, &s); }
        int pred = fetch8(c);
        for (int i = 0; i < n; i++) {
            if (dbl) X->q[i] = cmp_pred(X->g[i], s.g[i], pred) ? ~0ull : 0;
            else X->d[i] = cmp_pred(X->f[i], s.f[i], pred) ? ~0u : 0;
        }
        return; }
    case 0xC6: {
        xmm_load(c, d, &s); int imm = fetch8(c); xmm_t r;
        if (p66) { r.q[0] = X->q[imm & 1]; r.q[1] = s.q[(imm >> 1) & 1]; }
        else { r.d[0] = X->d[imm & 3]; r.d[1] = X->d[(imm >> 2) & 3]; r.d[2] = s.d[(imm >> 4) & 3]; r.d[3] = s.d[(imm >> 6) & 3]; }
        *X = r; return; }
    }
    /* integer SSE2 (requires 66 prefix for xmm; MMX forms not supported) */
    if (!p66 && !(op == 0x6F || op == 0x7F || op == 0x7E || op == 0xD6 || op == 0x70) )
        cpu_fatal(c, "MMX op 0f %02x", op);
    switch (op) {
    case 0x60: xmm_load(c, d, &s); { xmm_t r; for (int i = 0; i < 8; i++) { r.b[2 * i] = X->b[i]; r.b[2 * i + 1] = s.b[i]; } *X = r; } return;
    case 0x61: xmm_load(c, d, &s); { xmm_t r; for (int i = 0; i < 4; i++) { r.w[2 * i] = X->w[i]; r.w[2 * i + 1] = s.w[i]; } *X = r; } return;
    case 0x62: xmm_load(c, d, &s); { xmm_t r; for (int i = 0; i < 2; i++) { r.d[2 * i] = X->d[i]; r.d[2 * i + 1] = s.d[i]; } *X = r; } return;
    case 0x6C: xmm_load(c, d, &s); { xmm_t r; r.q[0] = X->q[0]; r.q[1] = s.q[0]; *X = r; } return;
    case 0x68: xmm_load(c, d, &s); { xmm_t r; for (int i = 0; i < 8; i++) { r.b[2 * i] = X->b[8 + i]; r.b[2 * i + 1] = s.b[8 + i]; } *X = r; } return;
    case 0x69: xmm_load(c, d, &s); { xmm_t r; for (int i = 0; i < 4; i++) { r.w[2 * i] = X->w[4 + i]; r.w[2 * i + 1] = s.w[4 + i]; } *X = r; } return;
    case 0x6A: xmm_load(c, d, &s); { xmm_t r; for (int i = 0; i < 2; i++) { r.d[2 * i] = X->d[2 + i]; r.d[2 * i + 1] = s.d[2 + i]; } *X = r; } return;
    case 0x6D: xmm_load(c, d, &s); { xmm_t r; r.q[0] = X->q[1]; r.q[1] = s.q[1]; *X = r; } return;
    case 0x63: xmm_load(c, d, &s); { xmm_t r; for (int i = 0; i < 8; i++) { r.sb[i] = sat_s8(X->sw[i]); r.sb[8 + i] = sat_s8(s.sw[i]); } *X = r; } return;
    case 0x67: xmm_load(c, d, &s); { xmm_t r; for (int i = 0; i < 8; i++) { r.b[i] = sat_u8(X->sw[i]); r.b[8 + i] = sat_u8(s.sw[i]); } *X = r; } return;
    case 0x6B: xmm_load(c, d, &s); { xmm_t r; for (int i = 0; i < 4; i++) { r.sw[i] = sat_s16(X->sd[i]); r.sw[4 + i] = sat_s16(s.sd[i]); } *X = r; } return;
    case 0x64: xmm_load(c, d, &s); for (int i = 0; i < 16; i++) X->b[i] = X->sb[i] > s.sb[i] ? 0xff : 0; return;
    case 0x65: xmm_load(c, d, &s); for (int i = 0; i < 8; i++) X->w[i] = X->sw[i] > s.sw[i] ? 0xffff : 0; return;
    case 0x66: xmm_load(c, d, &s); for (int i = 0; i < 4; i++) X->d[i] = X->sd[i] > s.sd[i] ? ~0u : 0; return;
    case 0x74: xmm_load(c, d, &s); for (int i = 0; i < 16; i++) X->b[i] = X->b[i] == s.b[i] ? 0xff : 0; return;
    case 0x75: xmm_load(c, d, &s); for (int i = 0; i < 8; i++) X->w[i] = X->w[i] == s.w[i] ? 0xffff : 0; return;
    case 0x76: xmm_load(c, d, &s); for (int i = 0; i < 4; i++) X->d[i] = X->d[i] == s.d[i] ? ~0u : 0; return;
    case 0x6E: X->q[1] = 0; X->q[0] = d->mod == 3 ? c->r[d->rm] : rd32(d->addr); return;
    case 0x7E:
        if (pf3) { X->q[0] = xmm_load64(c, d); X->q[1] = 0; return; }   /* movq xmm, xmm/m64 */
        if (d->mod == 3) c->r[d->rm] = X->d[0]; else wr32(d->addr, X->d[0]); return;
    case 0xD6: if (d->mod == 3) { c->x[d->rm].q[0] = X->q[0]; c->x[d->rm].q[1] = 0; } else wr64(d->addr, X->q[0]); return;
    case 0x6F: if (!p66 && !pf3) cpu_fatal(c, "MMX movq"); xmm_load(c, d, X); return;
    case 0x7F: if (!p66 && !pf3) cpu_fatal(c, "MMX movq store"); if (d->mod == 3) c->x[d->rm] = *X; else memcpy(GM(d->addr), X, 16); return;
    case 0x70: {
        xmm_load(c, d, &s); int imm = fetch8(c); xmm_t r = s;
        if (p66) for (int i = 0; i < 4; i++) r.d[i] = s.d[(imm >> (2 * i)) & 3];
        else if (pf2) for (int i = 0; i < 4; i++) r.w[i] = s.w[(imm >> (2 * i)) & 3];
        else if (pf3) for (int i = 0; i < 4; i++) r.w[4 + i] = s.w[4 + ((imm >> (2 * i)) & 3)];
        else cpu_fatal(c, "pshufw");
        *X = r; return; }
    case 0x71: case 0x72: case 0x73: {
        xmm_t *R = &c->x[d->rm]; int imm = fetch8(c);
        if (op == 0x71) for (int i = 0; i < 8; i++) {
            if (d->reg == 2) R->w[i] = imm > 15 ? 0 : R->w[i] >> imm;
            else if (d->reg == 4) R->sw[i] = R->sw[i] >> (imm > 15 ? 15 : imm);
            else if (d->reg == 6) R->w[i] = imm > 15 ? 0 : R->w[i] << imm;
            else cpu_fatal(c, "psxw");
        } else if (op == 0x72) for (int i = 0; i < 4; i++) {
            if (d->reg == 2) R->d[i] = imm > 31 ? 0 : R->d[i] >> imm;
            else if (d->reg == 4) R->sd[i] = R->sd[i] >> (imm > 31 ? 31 : imm);
            else if (d->reg == 6) R->d[i] = imm > 31 ? 0 : R->d[i] << imm;
            else cpu_fatal(c, "psxd");
        } else {
            if (d->reg == 2) for (int i = 0; i < 2; i++) R->q[i] = imm > 63 ? 0 : R->q[i] >> imm;
            else if (d->reg == 6) for (int i = 0; i < 2; i++) R->q[i] = imm > 63 ? 0 : R->q[i] << imm;
            else if (d->reg == 3) { int n = imm > 16 ? 16 : imm; xmm_t r; memset(&r, 0, 16); for (int i = 0; i + n < 16; i++) r.b[i] = R->b[i + n]; *R = r; }
            else if (d->reg == 7) { int n = imm > 16 ? 16 : imm; xmm_t r; memset(&r, 0, 16); for (int i = n; i < 16; i++) r.b[i] = R->b[i - n]; *R = r; }
            else cpu_fatal(c, "psxq");
        }
        return; }
    case 0xD1: case 0xD2: case 0xD3: case 0xE1: case 0xE2: case 0xF1: case 0xF2: case 0xF3: {
        xmm_load(c, d, &s); u64 n = s.q[0];
        switch (op) {
        case 0xD1: for (int i = 0; i < 8; i++) X->w[i] = n > 15 ? 0 : X->w[i] >> n; break;
        case 0xD2: for (int i = 0; i < 4; i++) X->d[i] = n > 31 ? 0 : X->d[i] >> n; break;
        case 0xD3: for (int i = 0; i < 2; i++) X->q[i] = n > 63 ? 0 : X->q[i] >> n; break;
        case 0xE1: for (int i = 0; i < 8; i++) X->sw[i] = X->sw[i] >> (n > 15 ? 15 : n); break;
        case 0xE2: for (int i = 0; i < 4; i++) X->sd[i] = X->sd[i] >> (n > 31 ? 31 : n); break;
        case 0xF1: for (int i = 0; i < 8; i++) X->w[i] = n > 15 ? 0 : X->w[i] << n; break;
        case 0xF2: for (int i = 0; i < 4; i++) X->d[i] = n > 31 ? 0 : X->d[i] << n; break;
        case 0xF3: for (int i = 0; i < 2; i++) X->q[i] = n > 63 ? 0 : X->q[i] << n; break;
        }
        return; }
    case 0xC4: { u32 v = d->mod == 3 ? c->r[d->rm] : rd16(d->addr); int imm = fetch8(c); X->w[imm & 7] = (u16)v; return; }
    case 0xC5: { int imm = fetch8(c); c->r[d->reg] = c->x[d->rm].w[imm & 7]; return; }
    case 0xD7: { u32 m = 0; xmm_t *R = &c->x[d->rm]; for (int i = 0; i < 16; i++) if (R->b[i] & 0x80) m |= 1u << i; c->r[d->reg] = m; return; }
    case 0xD4: xmm_load(c, d, &s); X->q[0] += s.q[0]; X->q[1] += s.q[1]; return;
    case 0xFB: xmm_load(c, d, &s); X->q[0] -= s.q[0]; X->q[1] -= s.q[1]; return;
    case 0xD5: xmm_load(c, d, &s); for (int i = 0; i < 8; i++) X->sw[i] = (s16)((s32)X->sw[i] * s.sw[i]); return;
    case 0xE5: xmm_load(c, d, &s); for (int i = 0; i < 8; i++) X->sw[i] = (s16)(((s32)X->sw[i] * s.sw[i]) >> 16); return;
    case 0xE4: xmm_load(c, d, &s); for (int i = 0; i < 8; i++) X->w[i] = (u16)(((u32)X->w[i] * s.w[i]) >> 16); return;
    case 0xF4: xmm_load(c, d, &s); X->q[0] = (u64)X->d[0] * s.d[0]; X->q[1] = (u64)X->d[2] * s.d[2]; return;
    case 0xF5: xmm_load(c, d, &s); for (int i = 0; i < 4; i++) X->sd[i] = X->sw[2 * i] * s.sw[2 * i] + X->sw[2 * i + 1] * s.sw[2 * i + 1]; return;
    case 0xDB: xmm_load(c, d, &s); X->q[0] &= s.q[0]; X->q[1] &= s.q[1]; return;
    case 0xDF: xmm_load(c, d, &s); X->q[0] = ~X->q[0] & s.q[0]; X->q[1] = ~X->q[1] & s.q[1]; return;
    case 0xEB: xmm_load(c, d, &s); X->q[0] |= s.q[0]; X->q[1] |= s.q[1]; return;
    case 0xEF: xmm_load(c, d, &s); X->q[0] ^= s.q[0]; X->q[1] ^= s.q[1]; return;
    case 0xFC: xmm_load(c, d, &s); for (int i = 0; i < 16; i++) X->b[i] += s.b[i]; return;
    case 0xFD: xmm_load(c, d, &s); for (int i = 0; i < 8; i++) X->w[i] += s.w[i]; return;
    case 0xFE: xmm_load(c, d, &s); for (int i = 0; i < 4; i++) X->d[i] += s.d[i]; return;
    case 0xF8: xmm_load(c, d, &s); for (int i = 0; i < 16; i++) X->b[i] -= s.b[i]; return;
    case 0xF9: xmm_load(c, d, &s); for (int i = 0; i < 8; i++) X->w[i] -= s.w[i]; return;
    case 0xFA: xmm_load(c, d, &s); for (int i = 0; i < 4; i++) X->d[i] -= s.d[i]; return;
    case 0xEC: xmm_load(c, d, &s); for (int i = 0; i < 16; i++) X->sb[i] = sat_s8(X->sb[i] + s.sb[i]); return;
    case 0xED: xmm_load(c, d, &s); for (int i = 0; i < 8; i++) X->sw[i] = sat_s16(X->sw[i] + s.sw[i]); return;
    case 0xE8: xmm_load(c, d, &s); for (int i = 0; i < 16; i++) X->sb[i] = sat_s8(X->sb[i] - s.sb[i]); return;
    case 0xE9: xmm_load(c, d, &s); for (int i = 0; i < 8; i++) X->sw[i] = sat_s16(X->sw[i] - s.sw[i]); return;
    case 0xDC: xmm_load(c, d, &s); for (int i = 0; i < 16; i++) X->b[i] = sat_u8(X->b[i] + s.b[i]); return;
    case 0xDD: xmm_load(c, d, &s); for (int i = 0; i < 8; i++) X->w[i] = sat_u16(X->w[i] + s.w[i]); return;
    case 0xD8: xmm_load(c, d, &s); for (int i = 0; i < 16; i++) X->b[i] = sat_u8(X->b[i] - s.b[i]); return;
    case 0xD9: xmm_load(c, d, &s); for (int i = 0; i < 8; i++) X->w[i] = sat_u16(X->w[i] - s.w[i]); return;
    case 0xDA: xmm_load(c, d, &s); for (int i = 0; i < 16; i++) X->b[i] = X->b[i] < s.b[i] ? X->b[i] : s.b[i]; return;
    case 0xDE: xmm_load(c, d, &s); for (int i = 0; i < 16; i++) X->b[i] = X->b[i] > s.b[i] ? X->b[i] : s.b[i]; return;
    case 0xEA: xmm_load(c, d, &s); for (int i = 0; i < 8; i++) X->sw[i] = X->sw[i] < s.sw[i] ? X->sw[i] : s.sw[i]; return;
    case 0xEE: xmm_load(c, d, &s); for (int i = 0; i < 8; i++) X->sw[i] = X->sw[i] > s.sw[i] ? X->sw[i] : s.sw[i]; return;
    case 0xE0: xmm_load(c, d, &s); for (int i = 0; i < 16; i++) X->b[i] = (u8)((X->b[i] + s.b[i] + 1) >> 1); return;
    case 0xE3: xmm_load(c, d, &s); for (int i = 0; i < 8; i++) X->w[i] = (u16)((X->w[i] + s.w[i] + 1) >> 1); return;
    }
    cpu_fatal(c, "SSE op 0f %02x (66=%d f3=%d f2=%d)", op, p66, pf3, pf2);
}

/* ---------------- main loop ---------------- */
#define SZ (d.sz)

u64 cpu_run(cpu_t *c, u64 budget) {
    u64 n = 0;
    c->halted = 0;
    while (n < budget && !c->halted) {
        if (c->eip - THUNK_BASE < THUNK_SIZE) { g_thunk_handler(c, c->eip); n++; continue; }
        dec_t d; d.sz = 4; d.rep = 0; d.seg_fs = 0; d.opsize66 = 0;
        u32 start = c->eip;
        u8 op;
    prefix:
        op = fetch8(c);
        switch (op) {
        case 0x66: d.sz = 2; d.opsize66 = 1; goto prefix;
        case 0xF2: case 0xF3: d.rep = op; goto prefix;
        case 0xF0: case 0x2E: case 0x3E: case 0x26: case 0x36: case 0x65: goto prefix;
        case 0x64: d.seg_fs = 1; goto prefix;
        }
        n++;
        switch (op) {
        /* ALU r/m, r and r, r/m and acc, imm */
        case 0x00: case 0x01: case 0x02: case 0x03: case 0x08: case 0x09: case 0x0A: case 0x0B:
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x28: case 0x29: case 0x2A: case 0x2B:
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x38: case 0x39: case 0x3A: case 0x3B: {
            int aop = op >> 3, sz = (op & 1) ? SZ : 1, dir = op & 2;
            decode_modrm(c, &d);
            u32 a, b, r;
            if (dir) { a = getreg(c, d.reg, sz); b = get_rm(c, &d, sz); r = alu(c, aop, a, b, sz); if (aop != 7) setreg(c, d.reg, sz, r); }
            else { a = get_rm(c, &d, sz); b = getreg(c, d.reg, sz); r = alu(c, aop, a, b, sz); if (aop != 7) set_rm(c, &d, sz, r); }
            break; }
        case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C: {
            int aop = op >> 3; u32 r = alu(c, aop, c->r[EAX], fetch8(c), 1); if (aop != 7) setreg(c, EAX, 1, r); break; }
        case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D: {
            int aop = op >> 3; u32 imm = SZ == 4 ? fetch32(c) : fetch16(c);
            u32 r = alu(c, aop, c->r[EAX], imm, SZ); if (aop != 7) setreg(c, EAX, SZ, r); break; }
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: {
            u32 cf = c->flags & F_CF; u32 r = alu(c, 0, getreg(c, op & 7, SZ), 1, SZ); setreg(c, op & 7, SZ, r); c->flags = (c->flags & ~F_CF) | cf; break; }
        case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
            u32 cf = c->flags & F_CF; u32 r = alu(c, 5, getreg(c, op & 7, SZ), 1, SZ); setreg(c, op & 7, SZ, r); c->flags = (c->flags & ~F_CF) | cf; break; }
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: {
            u32 v = c->r[op & 7];
            if (SZ == 2) { c->r[ESP] -= 2; wr16(c->r[ESP], v); } else push32(c, v); break; }
        case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            if (SZ == 2) { u16 v = rd16(c->r[ESP]); c->r[ESP] += 2; setreg(c, op & 7, 2, v); }
            else { u32 v = pop32(c); c->r[op & 7] = v; }
            break;
        case 0x60: { u32 sp = c->r[ESP]; for (int i = 0; i < 8; i++) push32(c, i == ESP ? sp : c->r[i]); break; }
        case 0x61: { for (int i = 7; i >= 0; i--) { u32 v = pop32(c); if (i != ESP) c->r[i] = v; } break; }
        case 0x68: push32(c, SZ == 4 ? fetch32(c) : fetch16(c)); break;
        case 0x6A: push32(c, (u32)(s32)(s8)fetch8(c)); break;
        case 0x69: case 0x6B: {
            decode_modrm(c, &d);
            s64 a = SZ == 4 ? (s64)(s32)get_rm(c, &d, 4) : (s64)(s16)get_rm(c, &d, 2);
            s64 b = op == 0x6B ? (s8)fetch8(c) : (SZ == 4 ? (s64)(s32)fetch32(c) : (s64)(s16)fetch16(c));
            s64 r = a * b;
            int of = SZ == 4 ? (r != (s32)r) : (r != (s16)r);
            setreg(c, d.reg, SZ, (u32)r); setf(c, F_CF, of); setf(c, F_OF, of); set_szp(c, (u32)r, SZ);
            break; }
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            s8 rel = (s8)fetch8(c); if (cond(c, op & 15)) c->eip += rel; break; }
        case 0x80: case 0x81: case 0x82: case 0x83: {
            int sz = (op & 1) ? SZ : 1;
            decode_modrm(c, &d);
            u32 imm = op == 0x81 ? (SZ == 4 ? fetch32(c) : fetch16(c)) : (op == 0x83 ? (u32)(s32)(s8)fetch8(c) : fetch8(c));
            u32 r = alu(c, d.reg, get_rm(c, &d, sz), imm, sz);
            if (d.reg != 7) set_rm(c, &d, sz, r);
            break; }
        case 0x84: case 0x85: { int sz = (op & 1) ? SZ : 1; decode_modrm(c, &d); alu(c, 4, get_rm(c, &d, sz), getreg(c, d.reg, sz), sz); break; }
        case 0x86: case 0x87: { int sz = (op & 1) ? SZ : 1; decode_modrm(c, &d); u32 a = get_rm(c, &d, sz), b = getreg(c, d.reg, sz); set_rm(c, &d, sz, b); setreg(c, d.reg, sz, a); break; }
        case 0x88: case 0x89: { int sz = (op & 1) ? SZ : 1; decode_modrm(c, &d); set_rm(c, &d, sz, getreg(c, d.reg, sz)); break; }
        case 0x8A: case 0x8B: { int sz = (op & 1) ? SZ : 1; decode_modrm(c, &d); setreg(c, d.reg, sz, get_rm(c, &d, sz)); break; }
        case 0x8D: { int fs = d.seg_fs; d.seg_fs = 0; decode_modrm(c, &d); (void)fs; setreg(c, d.reg, SZ, d.addr); break; }
        case 0x8F: { decode_modrm(c, &d); u32 v = pop32(c); if (d.mod != 3 && (d.rm == 4 || 1)) { /* address computed before pop in real CPU when it uses esp; rare */ } set_rm(c, &d, 4, v); break; }
        case 0x90: break;
        case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
            u32 a = getreg(c, EAX, SZ), b = getreg(c, op & 7, SZ); setreg(c, EAX, SZ, b); setreg(c, op & 7, SZ, a); break; }
        case 0x98: if (SZ == 4) c->r[EAX] = (u32)(s32)(s16)c->r[EAX]; else setreg(c, EAX, 2, (u16)(s16)(s8)c->r[EAX]); break;
        case 0x99: if (SZ == 4) c->r[EDX] = (c->r[EAX] & 0x80000000u) ? 0xffffffffu : 0; else setreg(c, EDX, 2, (c->r[EAX] & 0x8000) ? 0xffff : 0); break;
        case 0x9C: push32(c, (c->flags & 0xfd5) | 2); break;
        case 0x9D: c->flags = (pop32(c) & 0xfd5) | 2; break;
        case 0x9E: c->flags = (c->flags & ~0xd5) | ((c->r[EAX] >> 8) & 0xd5); break;
        case 0x9F: setreg(c, 4, 1, (c->flags & 0xd5) | 2); break;
        case 0xA0: { u32 a = fetch32(c) + (d.seg_fs ? c->fsbase : 0); setreg(c, EAX, 1, rd8(a)); break; }
        case 0xA1: { u32 a = fetch32(c) + (d.seg_fs ? c->fsbase : 0); setreg(c, EAX, SZ, rdm(a, SZ)); break; }
        case 0xA2: { u32 a = fetch32(c) + (d.seg_fs ? c->fsbase : 0); wr8(a, (u8)c->r[EAX]); break; }
        case 0xA3: { u32 a = fetch32(c) + (d.seg_fs ? c->fsbase : 0); wrm(a, SZ, c->r[EAX]); break; }
        case 0xA4: case 0xA5: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xA6: case 0xA7: case 0xAE: case 0xAF: {
            int sz = (op & 1) ? SZ : 1; int step = (c->flags & F_DF) ? -sz : sz;
            int isrep = d.rep != 0;
            for (;;) {
                if (isrep && c->r[ECX] == 0) break;
                switch (op & ~1) {
                case 0xA4: wrm(c->r[EDI], sz, rdm(c->r[ESI], sz)); c->r[ESI] += step; c->r[EDI] += step; break;
                case 0xAA: wrm(c->r[EDI], sz, c->r[EAX]); c->r[EDI] += step; break;
                case 0xAC: setreg(c, EAX, sz, rdm(c->r[ESI], sz)); c->r[ESI] += step; break;
                case 0xA6: alu(c, 7, rdm(c->r[ESI], sz), rdm(c->r[EDI], sz), sz); c->r[ESI] += step; c->r[EDI] += step; break;
                case 0xAE: alu(c, 7, c->r[EAX], rdm(c->r[EDI], sz), sz); c->r[EDI] += step; break;
                }
                if (!isrep) break;
                c->r[ECX]--;
                if ((op & ~1) == 0xA6 || (op & ~1) == 0xAE) {
                    int zf = (c->flags & F_ZF) != 0;
                    if (d.rep == 0xF3 && !zf) break;
                    if (d.rep == 0xF2 && zf) break;
                }
            }
            break; }
        case 0xA8: alu(c, 4, c->r[EAX], fetch8(c), 1); break;
        case 0xA9: alu(c, 4, c->r[EAX], SZ == 4 ? fetch32(c) : fetch16(c), SZ); break;
        case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7: setreg(c, op & 7, 1, fetch8(c)); break;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF: setreg(c, op & 7, SZ, SZ == 4 ? fetch32(c) : fetch16(c)); break;
        case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
            int sz = (op & 1) ? SZ : 1; decode_modrm(c, &d);
            int cnt = (op <= 0xC1) ? fetch8(c) : (op <= 0xD1 ? 1 : (c->r[ECX] & 0xff));
            u32 v = get_rm(c, &d, sz);
            if ((cnt & 31) == 0) break;
            set_rm(c, &d, sz, shift_op(c, d.reg, v, cnt, sz));
            break; }
        case 0xC2: { u16 imm = fetch16(c); c->eip = pop32(c); c->r[ESP] += imm; break; }
        case 0xC3: c->eip = pop32(c); break;
        case 0xC6: case 0xC7: { int sz = (op & 1) ? SZ : 1; decode_modrm(c, &d); u32 imm = sz == 4 ? fetch32(c) : sz == 2 ? fetch16(c) : fetch8(c); set_rm(c, &d, sz, imm); break; }
        case 0xC8: { u16 sz = fetch16(c); u8 lvl = fetch8(c); push32(c, c->r[EBP]); u32 fp = c->r[ESP];
            for (int i = 1; i < lvl; i++) { c->r[EBP] -= 4; push32(c, rd32(c->r[EBP])); }
            if (lvl) push32(c, fp); c->r[EBP] = fp; c->r[ESP] -= sz; break; }
        case 0xC9: c->r[ESP] = c->r[EBP]; c->r[EBP] = pop32(c); break;
        case 0xCC: c->eip = start; cpu_fatal(c, "int3"); break;
        case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: x87(c, &d, op); break;
        case 0xE8: { s32 rel = (s32)fetch32(c); push32(c, c->eip); c->eip += rel; break; }
        case 0xE9: { s32 rel = (s32)fetch32(c); c->eip += rel; break; }
        case 0xEB: { s8 rel = (s8)fetch8(c); c->eip += rel; break; }
        case 0xF5: c->flags ^= F_CF; break;
        case 0xF6: case 0xF7: {
            int sz = (op & 1) ? SZ : 1; decode_modrm(c, &d);
            u32 v = get_rm(c, &d, sz);
            switch (d.reg) {
            case 0: case 1: alu(c, 4, v, sz == 4 ? fetch32(c) : sz == 2 ? fetch16(c) : fetch8(c), sz); break;
            case 2: set_rm(c, &d, sz, ~v); break;
            case 3: { u32 r = alu(c, 5, 0, v, sz); set_rm(c, &d, sz, r); setf(c, F_CF, (v & MASK[sz]) != 0); break; }
            case 4: {
                if (sz == 1) { u32 r = (c->r[EAX] & 0xff) * v; setreg(c, EAX, 2, r); setf(c, F_CF | F_OF, (r >> 8) != 0); }
                else if (sz == 2) { u32 r = (c->r[EAX] & 0xffff) * v; setreg(c, EAX, 2, r); setreg(c, EDX, 2, r >> 16); setf(c, F_CF | F_OF, (r >> 16) != 0); }
                else { u64 r = (u64)c->r[EAX] * v; c->r[EAX] = (u32)r; c->r[EDX] = (u32)(r >> 32); setf(c, F_CF | F_OF, (r >> 32) != 0); }
                break; }
            case 5: {
                if (sz == 1) { s32 r = (s8)c->r[EAX] * (s8)v; setreg(c, EAX, 2, r); setf(c, F_CF | F_OF, r != (s8)r); }
                else if (sz == 2) { s32 r = (s16)c->r[EAX] * (s16)v; setreg(c, EAX, 2, r); setreg(c, EDX, 2, (u32)r >> 16); setf(c, F_CF | F_OF, r != (s16)r); }
                else { s64 r = (s64)(s32)c->r[EAX] * (s32)v; c->r[EAX] = (u32)r; c->r[EDX] = (u32)((u64)r >> 32); setf(c, F_CF | F_OF, r != (s32)r); }
                set_szp(c, c->r[EAX], sz == 1 ? 1 : sz);
                break; }
            case 6: {
                if (!v) cpu_fatal(c, "divide by zero");
                if (sz == 1) { u32 a = c->r[EAX] & 0xffff; setreg(c, EAX, 1, a / v); setreg(c, 4, 1, a % v); }
                else if (sz == 2) { u32 a = ((c->r[EDX] & 0xffff) << 16) | (c->r[EAX] & 0xffff); setreg(c, EAX, 2, a / v); setreg(c, EDX, 2, a % v); }
                else { u64 a = ((u64)c->r[EDX] << 32) | c->r[EAX]; c->r[EAX] = (u32)(a / v); c->r[EDX] = (u32)(a % v); }
                break; }
            case 7: {
                if (!v) cpu_fatal(c, "divide by zero");
                if (sz == 1) { s32 a = (s16)c->r[EAX]; s32 b = (s8)v; setreg(c, EAX, 1, a / b); setreg(c, 4, 1, a % b); }
                else if (sz == 2) { s32 a = (s32)(((c->r[EDX] & 0xffff) << 16) | (c->r[EAX] & 0xffff)); s32 b = (s16)v; setreg(c, EAX, 2, a / b); setreg(c, EDX, 2, a % b); }
                else { s64 a = (s64)(((u64)c->r[EDX] << 32) | c->r[EAX]); s64 b = (s32)v; c->r[EAX] = (u32)(a / b); c->r[EDX] = (u32)(a % b); }
                break; }
            }
            break; }
        case 0xF8: c->flags &= ~F_CF; break;
        case 0xF9: c->flags |= F_CF; break;
        case 0xFA: case 0xFB: break;
        case 0xFC: c->flags &= ~F_DF; break;
        case 0xFD: c->flags |= F_DF; break;
        case 0xFE: case 0xFF: {
            int sz = op == 0xFE ? 1 : SZ; decode_modrm(c, &d);
            switch (d.reg) {
            case 0: { u32 cf = c->flags & F_CF; set_rm(c, &d, sz, alu(c, 0, get_rm(c, &d, sz), 1, sz)); c->flags = (c->flags & ~F_CF) | cf; break; }
            case 1: { u32 cf = c->flags & F_CF; set_rm(c, &d, sz, alu(c, 5, get_rm(c, &d, sz), 1, sz)); c->flags = (c->flags & ~F_CF) | cf; break; }
            case 2: { u32 t = get_rm(c, &d, 4); push32(c, c->eip); c->eip = t; break; }
            case 4: c->eip = get_rm(c, &d, 4); break;
            case 6: push32(c, get_rm(c, &d, 4)); break;
            default: c->eip = start; cpu_fatal(c, "FF /%d", d.reg);
            }
            break; }
        case 0x0F: {
            u8 op2 = fetch8(c);
            switch (op2) {
            case 0x0B: c->eip = start; cpu_fatal(c, "ud2"); break;
            case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F: decode_modrm(c, &d); break;
            case 0x31: { u64 t = c->icount + n; c->r[EAX] = (u32)t; c->r[EDX] = (u32)(t >> 32); break; }
            case 0xA2: { u32 leaf = c->r[EAX];
                if (leaf == 0) { c->r[EAX] = 1; c->r[EBX] = 0x756e6547; c->r[EDX] = 0x49656e69; c->r[ECX] = 0x6c65746e; }
                else { c->r[EAX] = 0x6f1; c->r[EBX] = 0; c->r[ECX] = 0; c->r[EDX] = (1u << 0) | (1u << 15) | (1u << 23) | (1u << 25) | (1u << 26); }
                break; }
            case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
            case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
                decode_modrm(c, &d); u32 v = get_rm(c, &d, SZ); if (cond(c, op2 & 15)) setreg(c, d.reg, SZ, v); break; }
            case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
            case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
                s32 rel = (s32)fetch32(c); if (cond(c, op2 & 15)) c->eip += rel; break; }
            case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
            case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
                decode_modrm(c, &d); set_rm(c, &d, 1, cond(c, op2 & 15)); break;
            case 0xA3: case 0xAB: case 0xB3: case 0xBB: case 0xBA: {
                decode_modrm(c, &d);
                int which; u32 bit;
                if (op2 == 0xBA) { which = d.reg; bit = fetch8(c); }
                else { which = op2 == 0xA3 ? 4 : op2 == 0xAB ? 5 : op2 == 0xB3 ? 6 : 7; bit = getreg(c, d.reg, SZ); }
                u32 addr = 0, v;
                if (d.mod == 3) { bit &= SZ * 8 - 1; v = getreg(c, d.rm, SZ); }
                else {
                    if (op2 == 0xBA) bit &= SZ * 8 - 1;
                    else { s32 sb = SZ == 4 ? (s32)bit : (s16)bit; addr = d.addr + ((sb >> 3) & ~(SZ - 1)); bit = (u32)sb & (SZ * 8 - 1); d.addr = addr; }
                    v = rdm(d.addr, SZ);
                }
                setf(c, F_CF, (v >> bit) & 1);
                if (which == 5) set_rm(c, &d, SZ, v | (1u << bit));
                else if (which == 6) set_rm(c, &d, SZ, v & ~(1u << bit));
                else if (which == 7) set_rm(c, &d, SZ, v ^ (1u << bit));
                break; }
            case 0xA4: case 0xA5: case 0xAC: case 0xAD: {
                decode_modrm(c, &d); int cnt = (op2 == 0xA4 || op2 == 0xAC) ? fetch8(c) : c->r[ECX]; cnt &= 31;
                if (!cnt) break;
                u32 dst = get_rm(c, &d, SZ), src = getreg(c, d.reg, SZ), r;
                if (SZ != 4) cpu_fatal(c, "shld16");
                if (op2 <= 0xA5) { r = (dst << cnt) | (src >> (32 - cnt)); setf(c, F_CF, (dst >> (32 - cnt)) & 1); }
                else { r = (dst >> cnt) | (src << (32 - cnt)); setf(c, F_CF, (dst >> (cnt - 1)) & 1); }
                set_szp(c, r, 4); set_rm(c, &d, SZ, r);
                break; }
            case 0xAF: {
                decode_modrm(c, &d);
                s64 a = SZ == 4 ? (s64)(s32)c->r[d.reg] : (s16)c->r[d.reg];
                s64 b = SZ == 4 ? (s64)(s32)get_rm(c, &d, 4) : (s16)get_rm(c, &d, 2);
                s64 r = a * b; int of = SZ == 4 ? r != (s32)r : r != (s16)r;
                setreg(c, d.reg, SZ, (u32)r); setf(c, F_CF, of); setf(c, F_OF, of); set_szp(c, (u32)r, SZ); break; }
            case 0xB0: case 0xB1: {
                int sz = op2 == 0xB0 ? 1 : SZ; decode_modrm(c, &d);
                u32 v = get_rm(c, &d, sz);
                alu(c, 7, c->r[EAX], v, sz);
                if (c->flags & F_ZF) set_rm(c, &d, sz, getreg(c, d.reg, sz)); else setreg(c, EAX, sz, v);
                break; }
            case 0xB6: decode_modrm(c, &d); setreg(c, d.reg, SZ, get_rm(c, &d, 1)); break;
            case 0xB7: decode_modrm(c, &d); setreg(c, d.reg, SZ, get_rm(c, &d, 2)); break;
            case 0xBE: decode_modrm(c, &d); setreg(c, d.reg, SZ, (u32)(s32)(s8)get_rm(c, &d, 1)); break;
            case 0xBF: decode_modrm(c, &d); setreg(c, d.reg, SZ, (u32)(s32)(s16)get_rm(c, &d, 2)); break;
            case 0xBC: case 0xBD: {
                decode_modrm(c, &d); u32 v = get_rm(c, &d, SZ);
                if (!v) { c->flags |= F_ZF; break; }
                c->flags &= ~F_ZF;
                int i;
                if (op2 == 0xBC) { i = 0; while (!((v >> i) & 1)) i++; }
                else { i = SZ * 8 - 1; while (!((v >> i) & 1)) i--; }
                setreg(c, d.reg, SZ, i);
                break; }
            case 0xC0: case 0xC1: {
                int sz = op2 == 0xC0 ? 1 : SZ; decode_modrm(c, &d);
                u32 a = get_rm(c, &d, sz), b = getreg(c, d.reg, sz);
                u32 r = alu(c, 0, a, b, sz); setreg(c, d.reg, sz, a); set_rm(c, &d, sz, r);
                break; }
            case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF: c->r[op2 & 7] = __builtin_bswap32(c->r[op2 & 7]); break;
            case 0xAE: { decode_modrm(c, &d);
                if (d.mod != 3 && d.reg == 2) c->mxcsr = rd32(d.addr);
                else if (d.mod != 3 && d.reg == 3) wr32(d.addr, c->mxcsr);
                /* fences: nothing */
                break; }
            default:
                sse_op(c, &d, op2);
            }
            break; }
        default:
            c->eip = start;
            cpu_fatal(c, "unimplemented opcode %02x", op);
        }
    }
    c->icount += n;
    return n;
}
