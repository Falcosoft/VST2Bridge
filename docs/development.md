# Development

[Back to the README](../README.md)

## Layout

| Path | Contents |
|---|---|
| `bridge/` | The bridge in C: `stub.c` (bridge DLL), `host.c` (helper exe), `protocol.h` (shared memory), `marshal.h` (how each opcode's data is copied), `vst2.h` (VST 2.4 declarations written from the public interface description, no Steinberg SDK file), `stub.def` (exports), `Makefile` |
| `bridge/test/` | `fakevst.c` (test plugin), `testhost.c` (test host), `run_tests.sh` (both directions under Wine) |
| `patcher.py`, `patcher_i18n.py` | The patcher (Tkinter window and `--cli`); texts in 29 languages (the same as SXG-Create-NF; a missing key falls back to English) |
| `utility/bridges.py` | Finding plugins and writing bridges |
| `utility/pe.py` | Reading machine type and exports of a DLL |
| `utility/selfupdate.py` | Self-update from the GitHub releases (adapted from SXG-Create-NF) |
| `docs/versions.md` | Version history, shown by the patcher's **What's new?** |

## Building

The bridge is built with mingw-w64 (cross-compiled on Linux; also works in MSYS2 on Windows):

```
sudo apt install gcc-mingw-w64
make -C bridge          # vst2bridge-x64.dll, vst2bridge-x86.dll, vst2bridge-host32.exe, vst2bridge-host64.exe
```

The patcher runs from source with Python 3.10+ and Tkinter (`python patcher.py`, or `python patcher.py --cli <folder>`); it takes the bridge files from `bridge/`.

## Tests

```
sudo dpkg --add-architecture i386 && sudo apt update && sudo apt install wine wine64 wine32:i386 xvfb
bash bridge/test/run_tests.sh
```

It builds the test plugin and the test host in both bitnesses and checks both directions (32-bit plugin in a 64-bit host, 64-bit plugin in a 32-bit host): strings, structures, parameters, small and large chunks, audio with MIDI and SysEx at the right frame, long blocks, accumulating and double processing, MIDI out, callbacks from the audio thread, the GUI thread and another thread, the editor with nested and GUI-started `sizeWindow`. The workflow `check.yml` runs it on every pull request, together with the patcher's `--selftest` and `--cli`.

## Releases

Every push to `main` that touches the bridge or the patcher runs `build.yml`: bridge build and tests, the patcher frozen with PyInstaller on Windows (with the bridge files and `docs/versions.md`), `--selftest`, and a release `autobuild-r<N>` with `VST2Bridge-Patcher-r<N>-windows.exe`. The tag, the asset name and `--selftest-out` are a contract with `utility/selfupdate.py`.

Add a row to `docs/versions.md` for every change that ends up in a release: `| next | | <change> |`, newest on top. The workflow replaces `next` with the release number and date.
