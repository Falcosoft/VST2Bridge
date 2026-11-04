#!/bin/bash
# Builds the bridge, the test plugin and the test hosts, and runs both directions under Wine, then the Linux CLAP
# (native, helper under Wine) with a 32-bit and a 64-bit plugin and the macOS variant of the CLAP built on Linux
# (floating editor, -DVB_FLOATING_GUI).
# Needs: gcc-mingw-w64, gcc, libxcb1-dev, wine (64-bit with 32-bit support), xvfb (or a DISPLAY).
# Run from anywhere:  bash bridge/test/run_tests.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SELF="$HERE/$(basename "$0")"
if [ -z "${DISPLAY:-}" ]; then exec xvfb-run -a bash "$SELF" "$@"; fi      # no X server: start one
cd "$HERE/.."
export WINEDEBUG=-all
make --no-print-directory
make --no-print-directory linux
T="$(mktemp -d)"
# end Wine with the tests: a wineserver left running keeps a CI job from finishing ("Cleaning up orphan processes")
trap 'wineserver -k 2>/dev/null || true; rm -rf "$T"' EXIT
CF="-O2 -Wall -Wextra -I."
i686-w64-mingw32-gcc   $CF -shared -o "$T/fake32.dll" test/fakevst.c -static-libgcc -s
x86_64-w64-mingw32-gcc $CF -shared -o "$T/fake64.dll" test/fakevst.c -static-libgcc -s
i686-w64-mingw32-gcc   $CF -o "$T/testhost32.exe" test/testhost.c -static -s
x86_64-w64-mingw32-gcc $CF -o "$T/testhost64.exe" test/testhost.c -static -s
i686-w64-mingw32-gcc   $CF -o "$T/testclap32.exe" test/testclap.c -static -s
x86_64-w64-mingw32-gcc $CF -o "$T/testclap64.exe" test/testclap.c -static -s
gcc $CF -o "$T/testclap" test/testclap.c -lxcb -ldl
gcc $CF -Wno-format-truncation -DVB_FLOATING_GUI -shared -fPIC -fvisibility=hidden -o "$T/floating.clap" clap.c stub.c -lpthread
# the layout the patcher writes: the bridges (VST2 and CLAP) and the helper next to the plugin
mkdir -p "$T/a" "$T/b"
cp "$T/fake32.dll" "$T/a/fake.dll"; cp vst2bridge-x64.dll "$T/a/fake_x64.dll"; cp vst2bridge-x64.clap "$T/a/fake_x64.clap"; cp vst2bridge-host32.exe "$T/a/"
cp "$T/fake64.dll" "$T/b/fake.dll"; cp vst2bridge-x86.dll "$T/b/fake_x86.dll"; cp vst2bridge-x86.clap "$T/b/fake_x86.clap"; cp vst2bridge-host64.exe "$T/b/"
st=0
echo "== 32-bit plugin in a 64-bit host (fake_x64.dll)"
wine "$T/testhost64.exe" "$(winepath -w "$T/a/fake_x64.dll")" || st=1
echo "== 64-bit plugin in a 32-bit host (fake_x86.dll)"
wine "$T/testhost32.exe" "$(winepath -w "$T/b/fake_x86.dll")" || st=1
echo "== 32-bit plugin as CLAP in a 64-bit host (fake_x64.clap)"
wine "$T/testclap64.exe" "$(winepath -w "$T/a/fake_x64.clap")" || st=1
echo "== 64-bit plugin as CLAP in a 32-bit host (fake_x86.clap)"
wine "$T/testclap32.exe" "$(winepath -w "$T/b/fake_x86.clap")" || st=1
echo "== old instrument (no synth flag, no effCanDo) as CLAP, as Woof! checks it"
wine "$T/testclap64.exe" "$(winepath -w "$T/a/fake_x64.clap")" plain || st=1
wine "$T/testclap32.exe" "$(winepath -w "$T/b/fake_x86.clap")" plain || st=1
# Linux / macOS layout: <name>.clap next to <name>.dll and the helper of the plugin's bitness
mkdir -p "$T/l32" "$T/l64" "$T/m32"
cp "$T/fake32.dll" "$T/l32/fake.dll"; cp vst2bridge-linux.clap "$T/l32/fake.clap"; cp vst2bridge-host32.exe "$T/l32/"
cp "$T/fake64.dll" "$T/l64/fake.dll"; cp vst2bridge-linux.clap "$T/l64/fake.clap"; cp vst2bridge-host64.exe "$T/l64/"
cp "$T/fake32.dll" "$T/m32/fake.dll"; cp "$T/floating.clap" "$T/m32/fake.clap"; cp vst2bridge-host32.exe "$T/m32/"
echo "== Linux CLAP, 32-bit plugin under Wine"
"$T/testclap" "$T/l32/fake.clap" Fake32 || st=1
echo "== Linux CLAP, 64-bit plugin under Wine"
"$T/testclap" "$T/l64/fake.clap" Fake64 || st=1
echo "== macOS variant (floating editor), 32-bit plugin under Wine"
"$T/testclap" "$T/m32/fake.clap" Fake32 floating || st=1
echo "== Linux CLAP, old instrument (as Woof! checks it), 32-bit and 64-bit plugin"
"$T/testclap" "$T/l32/fake.clap" Fake32 plain || st=1
"$T/testclap" "$T/l64/fake.clap" Fake64 plain || st=1
[ -z "$(ls /dev/shm 2>/dev/null | grep '^vst2bridge_' || true)" ] || { echo "FAIL: shared files left in /dev/shm"; st=1; }
[ $st = 0 ] && echo "ALL BRIDGE TESTS PASSED" || { echo "BRIDGE TESTS FAILED"; exit 1; }
