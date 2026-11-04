#!/bin/bash
# Builds the bridge, the test plugin and the test hosts, and runs both directions under Wine.
# Needs: gcc-mingw-w64, wine (64-bit with 32-bit support), xvfb (or a DISPLAY). Run from anywhere:  bash bridge/test/run_tests.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SELF="$HERE/$(basename "$0")"
if [ -z "${DISPLAY:-}" ]; then exec xvfb-run -a bash "$SELF" "$@"; fi      # no X server: start one
cd "$HERE/.."
export WINEDEBUG=-all
make --no-print-directory
T="$(mktemp -d)"
# end Wine with the tests: a wineserver left running keeps a CI job from finishing ("Cleaning up orphan processes")
trap 'wineserver -k 2>/dev/null || true; rm -rf "$T"' EXIT
CF="-O2 -Wall -Wextra -I."
i686-w64-mingw32-gcc   $CF -shared -o "$T/fake32.dll" test/fakevst.c -static-libgcc -s
x86_64-w64-mingw32-gcc $CF -shared -o "$T/fake64.dll" test/fakevst.c -static-libgcc -s
i686-w64-mingw32-gcc   $CF -o "$T/testhost32.exe" test/testhost.c -static -s
x86_64-w64-mingw32-gcc $CF -o "$T/testhost64.exe" test/testhost.c -static -s
# the layout the patcher writes: the bridge and its helper next to the plugin
mkdir -p "$T/a" "$T/b"
cp "$T/fake32.dll" "$T/a/fake.dll"; cp vst2bridge-x64.dll "$T/a/fake_x64.dll"; cp vst2bridge-host32.exe "$T/a/"
cp "$T/fake64.dll" "$T/b/fake.dll"; cp vst2bridge-x86.dll "$T/b/fake_x86.dll"; cp vst2bridge-host64.exe "$T/b/"
st=0
echo "== 32-bit plugin in a 64-bit host (fake_x64.dll)"
wine "$T/testhost64.exe" "$(winepath -w "$T/a/fake_x64.dll")" || st=1
echo "== 64-bit plugin in a 32-bit host (fake_x86.dll)"
wine "$T/testhost32.exe" "$(winepath -w "$T/b/fake_x86.dll")" || st=1
[ $st = 0 ] && echo "ALL BRIDGE TESTS PASSED" || { echo "BRIDGE TESTS FAILED"; exit 1; }
