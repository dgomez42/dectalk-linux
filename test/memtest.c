#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
typedef struct { char *lpData; void *ph; void *ix; unsigned maxlen, maxph, maxix, len, nph, nix, res; } BUF;
typedef struct { unsigned v, sample, res; } IDX;
#define F(n) void *n = dlsym(L, #n); if (!n) { printf("missing %s\n", #n); return 1; }
static FILE *mem; static long total; static int nbuf, nidx_cb, nidx_buf;
static void *gh; static unsigned (*AddBuffer)(void*, BUF*);
static void cb(long l1, long l2, unsigned user, unsigned msg)
{
    if (msg == 9) { BUF *b = (BUF *)l2; fwrite(b->lpData, 1, b->len, mem); total += b->len; nbuf++;
        IDX *x = b->ix; for (unsigned i = 0; i < b->nix; i++) { printf("  buffer index mark %u at sample %u\n", x[i].v, x[i].sample); nidx_buf++; }
        b->len = 0; AddBuffer(gh, b); }
    else if (msg == 1) { printf("  index-mark callback: %ld %ld user=%u\n", l1, l2, user); nidx_cb++; }
    else printf("  msg %u %ld %ld\n", msg, l1, l2);
}
int main(int c, char **v)
{
    void *L = dlopen(v[1], RTLD_NOW); if (!L) { printf("%s\n", dlerror()); return 1; }
    F(TextToSpeechStartupEx) F(TextToSpeechSpeak) F(TextToSpeechSync) F(TextToSpeechOpenWaveOutFile) F(TextToSpeechCloseWaveOutFile)
    F(TextToSpeechShutdown) F(TextToSpeechOpenInMemory) F(TextToSpeechCloseInMemory) F(TextToSpeechAddBuffer) F(TextToSpeechVersion)
    unsigned (*Startup)(void**, unsigned, unsigned, void*, long) = TextToSpeechStartupEx;
    unsigned (*Speak)(void*, const char*, unsigned) = TextToSpeechSpeak;
    unsigned (*Sync)(void*) = TextToSpeechSync, (*CloseW)(void*) = TextToSpeechCloseWaveOutFile, (*Shut)(void*) = TextToSpeechShutdown;
    unsigned (*OpenW)(void*, const char*, unsigned) = TextToSpeechOpenWaveOutFile;
    unsigned (*OpenM)(void*, unsigned) = TextToSpeechOpenInMemory, (*CloseM)(void*) = TextToSpeechCloseInMemory;
    unsigned (*Ver)(char**) = TextToSpeechVersion;
    AddBuffer = TextToSpeechAddBuffer;
    char *vs; unsigned vv = Ver(&vs); printf("version %08x \"%s\"\n", vv, vs);
    void *wrap[2] = {0};   /* like libtts.so: the language library stores its handle in wrapper[0] */
    unsigned r = Startup(&wrap[0], 0, 0x80000000u, cb, 1234); if (r) return 1; printf("startup %u\n", r); gh = wrap[0];
    const char *T = v[2];
    r = OpenW(gh, "file.wav", 4); printf("openwave %u\n", r);
    char T1[4096]; snprintf(T1, sizeof T1, "[:index mark 3] %s [:index mark 4]", T); Speak(gh, T1, 1); Sync(gh); CloseW(gh);
    mem = fopen("mem.raw", "wb");
    r = OpenM(gh, 4); printf("openinmemory %u\n", r);
    static BUF b[4]; static char d[4][8192]; static IDX ix[4][32];
    for (int i = 0; i < 4; i++) { b[i].lpData = d[i]; b[i].maxlen = 8192; b[i].ix = ix[i]; b[i].maxix = 32; printf("addbuffer %u\n", AddBuffer(gh, &b[i])); }
    char T2[4096]; snprintf(T2, sizeof T2, "%s [:index mark 7] And done.", T);
    Speak(gh, T2, 1); Sync(gh);
    CloseM(gh); fclose(mem);
    printf("memory bytes %ld in %d buffers, index marks: %d by callback, %d in buffers\n", total, nbuf, nidx_cb, nidx_buf);
    Shut(gh);
    return 0;
}
