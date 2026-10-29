/* say: speak text with DECtalk (Linux). Same command line as the DECtalk 4.99 "say" sample.
 * The DECtalk engine is built into this program; it only needs dtalk_us.dic. */
#include "compat.h"
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef unsigned int DWORD;
extern unsigned TextToSpeechStartup(void **, unsigned, DWORD, void *, long);
extern unsigned TextToSpeechShutdown(void *);
extern unsigned TextToSpeechSpeak(void *, char *, DWORD);
extern unsigned TextToSpeechSync(void *);
extern unsigned TextToSpeechSetRate(void *, DWORD);
extern unsigned TextToSpeechSetSpeaker(void *, DWORD);
extern unsigned TextToSpeechSetVolume(void *, int, int);
extern unsigned TextToSpeechOpenWaveOutFile(void *, char *, DWORD);
extern unsigned TextToSpeechCloseWaveOutFile(void *);
extern DWORD TextToSpeechVersion(char **);

static void usage(const char *p)
{
    fprintf(stderr,
        "\t%s will read text from stdin (default), the command line or a file\n"
        "\tand speak it through the sound card, or write it to a wave file.\n\n"
        "Usage: %s [-h] [-s #] [-r #] [-v #] [-d #] [-fo file] [-e #] [-a \"<text>\"] [-fi file] [-]\n"
        "          -a \"text\" speak text (quoted, multiple instances supported)\n"
        "          -d  #      Select the audio device (ignored)\n"
        "          -e  #      Output wave file encoding; specify:\n"
        "                       1   for PCM, 16 bit Mono 11KHz format\n"
        "                       2   for PCM,  8 bit Mono 11KHz format\n"
        "                       3   for MULAW 8 bit Mono  8KHz format\n"
        "          -fo file   Output wave file name, default: dtmemory.wav\n"
        "                     Also supports Sun .au and two virtual stdout streams:\n"
        "                     stdout:au Sun au with unknown length\n"
        "                     stdout:raw Raw PCM or MULAW samples in host byte order\n"
        "          -h         This help message\n"
        "          -r  #      Speaking rate ( 75 - 600 )\n"
        "          -s  #      Speaker number (0-8)\n"
        "          -v  #      Volume (0-100)\n"
        "          -fi file   Speak from a specified text file\n"
        "          -l lang    Language (only us is available)\n"
        "          -pre text  Text spoken (forced) before the normal input\n"
        "          -post text Text spoken (forced) after the normal input\n"
        "          -c         Clause mode (accepted; input is spoken line by line)\n"
        "          -          Read from stdin\n", p, p);
    exit(1);
}

static int num(const char *s) { int v = 0, neg = *s == '-'; if (neg || *s == '+') s++; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return neg ? -v : v; }

static char *append(char *a, const char *b)
{
    size_t la = a ? strlen(a) : 0, lb = strlen(b);
    a = realloc(a, la + lb + 2);
    if (la) a[la++] = ' ';
    memcpy(a + la, b, lb + 1);
    return a;
}

int main(int argc, char **argv)
{
    static const unsigned enc_fmt[3] = { 0x4, 0x1, 0x7 };
    char *cli = NULL, *pre = NULL, *post = NULL;
    const char *infile = NULL, *out = NULL;
    int rate = -1, spk = -1, vol = -1, enc = 1, use_stdin = 0, i;

    for (i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "-a") && more) cli = append(cli, argv[++i]);
        else if (!strcmp(a, "-fi") && more) infile = argv[++i];
        else if (!strcmp(a, "-fo")) out = (more && argv[i + 1][0] != '-') ? argv[++i] : "dtmemory.wav";
        else if (!strcmp(a, "-e") && more) enc = num(argv[++i]);
        else if (!strcmp(a, "-r") && more) rate = num(argv[++i]);
        else if (!strcmp(a, "-s") && more) spk = num(argv[++i]);
        else if (!strcmp(a, "-v") && more) vol = num(argv[++i]);
        else if (!strcmp(a, "-d") && more) i++;
        else if (!strcmp(a, "-l") && more)
        {
            if (strcasecmp(argv[++i], "us")) { fprintf(stderr, "DECtalk ML %s not supported.\n", argv[i]); return 1; }
        }
        else if (!strcmp(a, "-pre") && more) pre = argv[++i];
        else if (!strcmp(a, "-post") && more) post = argv[++i];
        else if (!strcmp(a, "-c")) ;
        else if (!strcmp(a, "-")) use_stdin = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) usage(argv[0]);
        else if (!strcmp(a, "--version")) { char *v; TextToSpeechVersion(&v); printf("%s\n", v); return 0; }
        else usage(argv[0]);
    }
    if (enc < 1 || enc > 3) enc = 1;
    if (!cli && !infile) use_stdin = 1;

    void *h = NULL;
    unsigned st = TextToSpeechStartup(&h, 0, out ? 0x80000000u : 0, NULL, 0);
    if (st) { fprintf(stderr, "\nmain: TextToSpeechStartup failed with code %u, exiting.\n", st); return 1; }
    if (spk >= 0) TextToSpeechSetSpeaker(h, (DWORD)spk);
    if (rate >= 0) TextToSpeechSetRate(h, (DWORD)rate);
    if (vol >= 0) TextToSpeechSetVolume(h, 1, vol);
    if (out && TextToSpeechOpenWaveOutFile(h, (char *)out, enc_fmt[enc - 1]))
    {
        fprintf(stderr, "say: cannot open %s for writing\n", out); TextToSpeechShutdown(h); return 1;
    }
    if (pre) { TextToSpeechSpeak(h, pre, 1); TextToSpeechSync(h); }
    if (cli) { TextToSpeechSpeak(h, cli, 1); TextToSpeechSync(h); }
    if (infile)
    {
        FILE *f = fopen(infile, "r");
        if (!f) { fprintf(stderr, "play_file: Cannot open %s for reading.\n", infile); TextToSpeechShutdown(h); return 1; }
        char line[4096];
        while (fgets(line, sizeof line, f)) TextToSpeechSpeak(h, line, 0);
        fclose(f);
        TextToSpeechSpeak(h, "", 1); TextToSpeechSync(h);
    }
    if (use_stdin)
    {
        char line[4096];
        while (fgets(line, sizeof line, stdin)) TextToSpeechSpeak(h, line, 1);
        TextToSpeechSync(h);
    }
    if (post) { TextToSpeechSpeak(h, post, 1); TextToSpeechSync(h); }
    if (out) TextToSpeechCloseWaveOutFile(h);
    TextToSpeechShutdown(h);
    return 0;
}
