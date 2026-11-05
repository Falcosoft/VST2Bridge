#!/bin/bash
# Timing of the audio path under Wine (not a pass / fail test): microseconds per process call for 1 / 8 / 32 instances,
# one thread or one thread per instance. Wine on a busy machine is noisy: compare before / after on the same machine.
# Needs the same tools as run_tests.sh.  bash bridge/test/run_bench.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -z "${DISPLAY:-}" ]; then exec xvfb-run -a bash "$HERE/$(basename "$0")" "$@"; fi
cd "$HERE/.."
export WINEDEBUG=-all
make --no-print-directory vst2bridge-x64.dll vst2bridge-host32.exe >/dev/null
T="$(mktemp -d)"
trap 'wineserver -k 2>/dev/null || true; rm -rf "$T"' EXIT
CF="-O2 -Wall -Wextra -I."
i686-w64-mingw32-gcc   $CF -shared -o "$T/fake.dll" test/fakevst.c -static-libgcc -s
x86_64-w64-mingw32-gcc $CF -o "$T/bench64.exe" test/bench.c -static -s
cp vst2bridge-x64.dll "$T/fake_x64.dll"; cp vst2bridge-host32.exe "$T/"
for cfg in "1" "8" "8 mt" "32 mt"; do
  # shellcheck disable=SC2086
  wine "$T/bench64.exe" "$(winepath -w "$T/fake_x64.dll")" $cfg
done
