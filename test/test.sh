#!/bin/bash
# Tests the US-only DECtalk 4.99 drop-in on an x86_64 Linux machine (or a linux/amd64 Docker container).
# It only reads this folder and writes into "linux test output" next to it.
cd "$(dirname "$0")"
KIT="$PWD"
# Work in a Linux temp folder (this also works when the kit sits on a Windows drive under WSL);
# the WAV files are copied to "linux test output" at the end.
WORK=$(mktemp -d); OUT="$WORK"; mkdir -p "$OUT/dist"
cp "$KIT/memtest" "$WORK/memtest"; chmod +x "$WORK/memtest"
tar xzf "$KIT/ubuntu-latest.tar.gz" -C "$OUT/dist" || exit 1
cd "$OUT/dist"
T="Hello! This is DECtalk on your Mac, with the restored 4.99 engine and the classic 4.0 intonation."
ok=1
r() { echo; echo "== $1"; }
r "1. say (official program, through libtts.so and the new libtts_us.so)"
./say -fo ../1-say.wav -a "$T"; md5sum ../1-say.wav
[ "$(md5sum < ../1-say.wav | cut -c1-32)" = acb705c8709ec268bcaecff5dd6b32a1 ] && echo "   identical to the Windows engine" || { echo "   DIFFERENT"; ok=0; }
r "2. tools/say_demo_us (rebuilt with the new engine)"
./tools/say_demo_us -fo ../2-say_demo_us.wav -a "$T"; md5sum ../2-say_demo_us.wav
[ "$(md5sum < ../2-say_demo_us.wav | cut -c1-32)" = acb705c8709ec268bcaecff5dd6b32a1 ] && echo "   identical to the Windows engine" || { echo "   DIFFERENT"; ok=0; }
r "3. voices and singing"
./say -fo ../3-voices.wav -a "[:np] I'm Paul. [:nb] I'm Betty. [:nh] I'm Harry. [:nf] I'm Frank. [:nd] I'm Dennis. [:nk] I'm Kit. [:nu] I'm Ursula. [:nr] I'm Rita. [:nw] And I'm Wendy."
./say -fo ../4-numbers-and-fixes.wav -a "[:np][:rate 180] On 12/25/2026 I paid \$14.99 for 3 books. [:phoneme on][s<1,20>][s<1,20>] [:dial:1234] Still talking."
md5sum ../3-voices.wav ../4-numbers-and-fixes.wav
r "4. in-memory output and callbacks (through libtts.so like other programs)"
(cd "$OUT/dist" && LD_LIBRARY_PATH=lib ../memtest lib/libtts.so "Testing index marks.") || ok=0
r "5. the same, loading libtts_us.so directly"
(cd "$OUT/dist" && ../memtest lib/libtts_us.so "Testing index marks.") || ok=0
r "6. other languages still load (they are untouched)"
./say -l sp -fo ../5-spanish.wav -a "Hola, esto es una prueba." && ls -la ../5-spanish.wav
if command -v paplay >/dev/null || command -v aplay >/dev/null; then r "7. speaking out loud"; ./say -a "DECtalk is ready."; fi
mkdir -p "$KIT/linux test output" && cp "$OUT"/*.wav "$KIT/linux test output/" 2>/dev/null
echo; echo "WAV files copied to \"linux test output\" in the kit folder."
echo; [ $ok = 1 ] && echo "ALL GOOD" || echo "SOMETHING DIFFERED - please send Claude the output above"
