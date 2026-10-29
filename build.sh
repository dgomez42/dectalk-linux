#!/bin/bash
# Builds the US English DECtalk 4.99 library (libtts_us.so) and say_demo_us for x86_64 Linux,
# then makes the drop-in ubuntu-latest.tar.gz from the official dectalk/dectalk release.
# Needs: gcc, python3, curl, sha256sum. Output goes to ./out
set -e
cd "$(dirname "$0")"
mkdir -p out/fetch
get() {   # url sha256 file
  [ -f "$3" ] && echo "$2  $3" | sha256sum -c --quiet 2>/dev/null && return 0
  curl -fL --retry 3 -o "$3" "$1"
  echo "$2  $3" | sha256sum -c --quiet || { echo "checksum mismatch: $3"; exit 1; }
}
# The restored DECtalk 4.99 engine (32-bit Windows build) and its US dictionary, pinned by commit and checksum
get https://raw.githubusercontent.com/dgomez42/dectalk-nvda/3a079000696a07146b6ca2ee07f31078ff802649/addon/synthDrivers32/dectalkRuntime/IA32/DECtalk.dll \
    8ab2b76ba3e76fcadec7923ccd857f5a04e4eefbc3524ab871eb2caa27ab382b out/fetch/DECtalk.dll
get https://raw.githubusercontent.com/dgomez42/80speak/aa5f5282b9ea40afda6a88aab9eef278a68a6ddb/dectalk/dtalk_us.dic \
    426567620baa6f21e3681204f0a004320b1009b26e4c2f7a5d66b6b51ca70ae7 out/fetch/dtalk_us.dic
# The official 4.99 Linux release this replaces the US files in
get https://github.com/dectalk/dectalk/releases/download/2023-10-30/ubuntu-latest.tar.gz \
    185d94dbb5f51f6f866a4d4521b39eba486095753d21d398578cc573604e3b0c out/fetch/ubuntu-latest.tar.gz

# Embed the engine in the library
python3 - out/fetch/DECtalk.dll out/engine_gen.c <<'P'
import sys
d=open(sys.argv[1],'rb').read()
with open(sys.argv[2],'w') as f:
    f.write('const unsigned long engine_dll_len=%d;\n__attribute__((aligned(16))) const unsigned char engine_dll[]={'%len(d))
    for i in range(0,len(d),32): f.write(','.join(map(str,d[i:i+32]))+',\n')
    f.write('};\n')
P
CF="-O2 -D__NO_INLINE__ -fPIC -fvisibility=hidden -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -Isrc -w"
for f in cpu winrt dtcore audio api opevent; do gcc $CF -c src/$f.c -o out/$f.o; done
gcc -O1 -fPIC -fvisibility=hidden -c out/engine_gen.c -o out/engine_gen.o
LIBS="-Wl,--no-as-needed /usr/lib/x86_64-linux-gnu/libdl.so.2 /usr/lib/x86_64-linux-gnu/libpthread.so.0 -lm -Wl,--as-needed"
O="out/api.o out/opevent.o out/dtcore.o out/audio.o out/winrt.o out/cpu.o out/engine_gen.o"
gcc -shared -o out/libtts_us.so -Wl,-soname,libtts_us.so $O $LIBS
gcc $CF -fno-pie -c src/say.c -o out/say.o
gcc -O2 -no-pie -nostartfiles -o out/say_demo_us src/start.S out/say.o ${O/out\/opevent.o /} $LIBS
strip --strip-unneeded out/libtts_us.so out/say_demo_us
gcc -O1 test/memtest.c -o out/memtest -ldl

# Drop-in tarball: the official release with only the 3 US files replaced
python3 - <<'P'
import tarfile, io, time
rep={'./lib/libtts_us.so':'out/libtts_us.so','./dic/dtalk_us.dic':'out/fetch/dtalk_us.dic','./tools/say_demo_us':'out/say_demo_us'}
now=int(time.time())
with tarfile.open('out/fetch/ubuntu-latest.tar.gz','r:gz') as a, tarfile.open('out/ubuntu-latest.tar.gz','w:gz',format=a.format) as b:
    for m in a:
        if m.name in rep:
            d=open(rep[m.name],'rb').read(); m.size=len(d); m.mtime=now; b.addfile(m,io.BytesIO(d))
        elif m.isfile(): b.addfile(m,a.extractfile(m))
        else: b.addfile(m)
P
cp test/test.sh out/test.sh; chmod +x out/test.sh
echo "Built out/libtts_us.so, out/say_demo_us and out/ubuntu-latest.tar.gz (test with out/test.sh on x86_64 Linux)"
