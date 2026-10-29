# DECtalk 4.99 for Linux: restored US English

A drop-in replacement for the official DECtalk 4.99 Linux release
(`ubuntu-latest.tar.gz` from [dectalk/dectalk](https://github.com/dectalk/dectalk/releases/tag/2023-10-30)).
Only American English changes. Everything else in the archive is byte-for-byte the same.

## Download

[`ubuntu-latest.tar.gz`](https://github.com/dgomez42/dectalk-linux/releases/download/4.99-us-1/ubuntu-latest.tar.gz)

```
sha256 c3fb51cc90fe55f3f6a5080f72af4a623de8883052575343a5a2bae24e844acd
```

If your Dockerfile or script downloads the official release, change the URL and checksum. Nothing else
needs to change. The folders, file names, `say`, `lib/libtts.so` and `DECtalk.conf` are all the same.

## What's different

Three files are replaced:

| File | Now |
|------|-----|
| `lib/libtts_us.so` | The restored US engine |
| `dic/dtalk_us.dic` | The matching US dictionary |
| `tools/say_demo_us` | Rebuilt with the restored engine. It takes the same options as before. |

The restored engine has:

- the classic DECtalk 4.0-style intonation
- voice definitions from the DECtalk user manual
- working singing
- fixes for crashes and squawks, for example `[:phoneme on][s<1,20>][s<1,20>]` and `[:dial:1234]`

The UK English, Spanish, Latin American Spanish, German and French engines and dictionaries are untouched.

## How it works

The restored engine is a 32-bit Windows build. Instead of porting its source, `libtts_us.so` runs that
exact engine in a small built-in x86 interpreter (`src/cpu.c`) with a minimal Win32 runtime
(`src/winrt.c`). As a result, the audio is sample-for-sample identical to the Windows engine.

`libtts_us.so` provides the full DECtalk API, so it works through the official multi-language
`libtts.so` loader and can also be linked directly. That includes:

- speaking, wave files (all four formats) and log files
- callbacks with index marks
- in-memory output (`TextToSpeechOpenInMemory` / `AddBuffer` / `ReturnBuffer`)
- user dictionaries and speaker parameters
- the event helpers `tunecheck_us` uses

It needs glibc 2.17 or later. The official library needs 2.34.

## Building

On x86_64 Linux with gcc, python3 and curl:

```
./build.sh
```

The script downloads three inputs pinned by commit and SHA-256: the engine, the dictionary, and the
official release. It then builds `out/libtts_us.so` and `out/say_demo_us`, and makes
`out/ubuntu-latest.tar.gz`. The library builds bit-for-bit reproducibly. The tarball's checksum differs
from the published one only because of file timestamps.

## Testing

`out/test.sh` (from `test/test.sh`) runs on x86_64 Linux or in a `linux/amd64` container. It checks:

- the official `say` through `libtts.so`, and `say_demo_us`, produce output byte-for-byte identical to the Windows engine
- in-memory output and index-mark callbacks work, both through `libtts.so` and when loading `libtts_us.so` directly
- the other languages still work
