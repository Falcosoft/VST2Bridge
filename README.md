# VST2Bridge

A free, universal VST2 bridge for Windows: use **32-bit VST2 plugins in 64-bit hosts** and **64-bit plugins in 32-bit hosts**, as VST2 or as **CLAP**. On **Linux and macOS** (Apple Silicon and Intel) it makes Windows VST2 plugins usable as **CLAP** plugins, run under Wine. No installation, no registry, free for everybody (GPL-3.0).

## How it works

The patcher writes a small bridge DLL and a CLAP bridge next to each plugin. The host loads the bridge like any plugin; the bridge starts a helper process of the plugin's bitness, which runs the real plugin, and passes everything between the two: audio, MIDI and SysEx, parameters, presets, the plugin's own window and the plugin's calls to the host.

| Plugin | Bridge written next to it | For |
|---|---|---|
| `syxg50.dll` (32-bit) | `syxg50_x64.dll` (VST2) + `syxg50_x64.clap` (CLAP) + `vst2bridge-host32.exe` | 64-bit hosts |
| `Emu-SC8850.dll` (64-bit) | `Emu-SC8850_x86.dll` (VST2) + `Emu-SC8850_x86.clap` (CLAP) + `vst2bridge-host64.exe` | 32-bit hosts |

The CLAP bridge is a CLAP plugin around the VST2 bridge of the same name, so it offers the same plugin with the same features to CLAP hosts.

### Linux and macOS

Linux and macOS cannot load Windows DLLs, so there the patcher writes **only CLAP bridges**, for plugins of either bitness. The CLAP is a native plugin (Linux x86-64; macOS universal, Apple Silicon and Intel) that runs the Windows plugin in the same helper programs under **Wine**:

| Plugin | Written next to it (Linux / macOS) |
|---|---|
| `syxg50.dll` (32-bit) | `syxg50.clap` + `vst2bridge-host32.exe` |
| `Emu-SC8850.dll` (64-bit) | `Emu-SC8850.clap` + `vst2bridge-host64.exe` |

On macOS the `.clap` is a bundle (a folder), signed ad hoc by the patcher. **Wine must be installed** (Wine 9 or newer; on Apple Silicon Wine runs under Rosetta 2): `wine` in the path, on macOS also the usual install folders (Homebrew, Wine Stable / Staging / Devel apps, CrossOver), or set `VB_WINE=/path/to/wine`. Start Wine once by hand first so that its prefix (`~/.wine` or `WINEPREFIX`) exists. The plugin's window is embedded in the host's window on Linux (X11 hosts; XWayland works, Wayland-native embedding does not); on macOS it opens as a floating window of its own, because a window of another process cannot be embedded there.

## Usage

1. Download the patcher from the [latest release](https://github.com/NightFright2k19/VST2Bridge/releases/latest): `VST2Bridge-Patcher-r<N>-windows.exe`, `...-linux.tar.gz` (extract, run `VST2Bridge-Patcher`), `...-macos-applesilicon.zip` (M1 and newer) or `...-macos-intel.zip` (extract, open the app).
2. Put it into a folder with your VST2 plugins (subfolders such as `Input` are fine), or start it anywhere and click **Choose…** to pick the plugin folder.
3. The patcher finds the plugins and tells whether each is 32-bit or 64-bit. Click **Create bridges**.
4. Windows: load `<plugin>_x64.dll` / `<plugin>_x64.clap` in a 64-bit host or `<plugin>_x86.dll` / `<plugin>_x86.clap` in a 32-bit host. CLAP hosts look in `C:\Program Files\Common Files\CLAP` by default: add the plugin folder to the host's CLAP folders. Keep the original plugin, both bridge files and the helper exe in the same folder (the `.clap` cannot be moved on its own).
   Linux / macOS: load `<plugin>.clap`; add the plugin folder to the host's CLAP folders (default folders: `~/.clap` on Linux, `~/Library/Audio/Plug-Ins/CLAP` on macOS; `CLAP_PATH` also works). Keep the `.clap`, the plugin DLL and the helper exe together. If a plugin does not load, the reason is in `vst2bridge.log` in the temp folder (`$TMPDIR` or `/tmp`); `VB_DEBUG=1` shows Wine's own messages.

Hosts running at an unusual sample rate (e.g. 32000 Hz) get the plugin at 44100 Hz, converted by the bridge; `vst2bridge.ini` next to the plugin can change that (see [Sample rates](docs/technical.md#sample-rates)).

The patcher speaks 29 languages (the system language, or pick one at the top right), updates itself (**Update** appears when a new release is out) and shows the changes with **What's new?**. Without a window: `VST2Bridge-Patcher.exe --cli [folder]` (Linux / macOS: `VST2Bridge-Patcher --cli [folder]`).

## More documentation

| Document | Contents |
|---|---|
| [How the bridge works](docs/technical.md) | Processes, shared memory, threads, which VST2 calls are passed and how |
| [Limitations](docs/limitations.md) | What does not work (yet) |
| [Development](docs/development.md) | Building, tests, releases, project layout |
| [Version history](docs/versions.md) | Changes of every release |
