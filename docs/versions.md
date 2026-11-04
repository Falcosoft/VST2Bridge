# Version history

What changed in each Autobuild release, newest first. Releases are numbered `r<N>` and can be downloaded from the [Releases](https://github.com/NightFright2k19/VST2Bridge/releases) page; the number in the patcher's file name tells you which one you have. Numbers in brackets are issues / pull requests.

| Release | Date | Changes |
|---|---|---|
| next | | Linux and macOS (Apple Silicon and Intel): patchers for both systems that write CLAP bridges only (`<name>.clap` + the helper of the plugin's bitness); the CLAP runs the Windows plugin (32-bit or 64-bit) under Wine. The plugin's window is embedded on Linux (X11) and floating on macOS. The patcher updates itself on all three systems. |
| next | | CLAP bridges: next to every VST2 bridge the patcher also writes a CLAP bridge of the same name (`<name>_x64.clap` for 64-bit hosts, `<name>_x86.clap` for 32-bit hosts) with audio, MIDI, parameters, presets, latency and the plugin's window. Bridges written by r1 or r2 show as "update" and get their `.clap` with **Create bridges**. |
| r2 | 2026-11-04 | Patcher: the same 29 languages as the SXG-Create-NF patcher (was English and German only); the system language is chosen automatically, the menu at the top right switches it. |
| r1 | 2026-11-04 | First version (#1): the patcher finds 32-bit and 64-bit VST2 plugins and writes a bridge next to each (`<name>_x64.dll` for 64-bit hosts, `<name>_x86.dll` for 32-bit hosts). The bridge runs the plugin in a helper process and passes audio, MIDI and SysEx, parameters, presets (chunks), the plugin's own window and the plugin's calls to the host. |
