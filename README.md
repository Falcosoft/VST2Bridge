# VST2Bridge

A free, universal VST2 bridge for Windows: use **32-bit VST2 plugins in 64-bit hosts** and **64-bit plugins in 32-bit hosts**, as VST2 or as **CLAP**. No installation, no registry, free for everybody (GPL-3.0).

## How it works

The patcher writes a small bridge DLL and a CLAP bridge next to each plugin. The host loads the bridge like any plugin; the bridge starts a helper process of the plugin's bitness, which runs the real plugin, and passes everything between the two: audio, MIDI and SysEx, parameters, presets, the plugin's own window and the plugin's calls to the host.

| Plugin | Bridge written next to it | For |
|---|---|---|
| `syxg50.dll` (32-bit) | `syxg50_x64.dll` (VST2) + `syxg50_x64.clap` (CLAP) + `vst2bridge-host32.exe` | 64-bit hosts |
| `Emu-SC8850.dll` (64-bit) | `Emu-SC8850_x86.dll` (VST2) + `Emu-SC8850_x86.clap` (CLAP) + `vst2bridge-host64.exe` | 32-bit hosts |

The CLAP bridge is a CLAP plugin around the VST2 bridge of the same name, so it offers the same plugin with the same features to CLAP hosts.

## Usage

1. Download `VST2Bridge-Patcher-r<N>-windows.exe` from the [latest release](https://github.com/NightFright2k19/VST2Bridge/releases/latest).
2. Put it into a folder with your VST2 plugins (subfolders such as `Input` are fine), or start it anywhere and click **Choose…** to pick the plugin folder.
3. The patcher finds the plugins and tells whether each is 32-bit or 64-bit. Click **Create bridges**.
4. Load `<plugin>_x64.dll` / `<plugin>_x64.clap` in a 64-bit host or `<plugin>_x86.dll` / `<plugin>_x86.clap` in a 32-bit host. CLAP hosts look in `C:\Program Files\Common Files\CLAP` by default: add the plugin folder to the host's CLAP folders. Keep the original plugin, both bridge files and the helper exe in the same folder (the `.clap` cannot be moved on its own).

The patcher speaks 29 languages (the system language, or pick one at the top right), updates itself (**Update** appears when a new release is out) and shows the changes with **What's new?**. Without a window: `VST2Bridge-Patcher.exe --cli [folder]`.

## More documentation

| Document | Contents |
|---|---|
| [How the bridge works](docs/technical.md) | Processes, shared memory, threads, which VST2 calls are passed and how |
| [Limitations](docs/limitations.md) | What does not work (yet) |
| [Development](docs/development.md) | Building, tests, releases, project layout |
| [Version history](docs/versions.md) | Changes of every release |
