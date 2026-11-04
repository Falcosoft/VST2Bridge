# Limitations

[Back to the README](../README.md)

- **Windows only**, x86 and x64. ARM64 plugins are not detected.
- **Not passed across processes** (the bridge answers 0): offline processing (`effOffline*`, `audioMasterOffline*`), `effProcessVarIo`, `audioMasterOpenFileSelector` / `CloseFileSelector` (plugins then fall back to their own file dialogs, if they have one), and the deprecated calls that return pointers (`audioMasterGetPreviousPlug` / `GetNextPlug`, `GetOutputSpeakerArrangement`, `effGetCurrentPosition`, `effGetDestinationBuffer`, `effGetIcon`).
- **`effVendorSpecific` / `audioMasterVendorSpecific`** are passed only when `ptr` is NULL (e.g. the mouse wheel extension); their data has no defined format.
- **Limits per call:** 32 audio channels each way (more are silent), 1024 MIDI events and 64 KB of SysEx per 4096 frames, strings up to 1 KB.
- **CLAP bridge:** it is a VST2 plugin seen through CLAP, so it offers what VST2 has: parameters are 0 … 1 with text only for the current value, parameter changes take effect at the start of a block, no polyphonic modulation or note expressions, no 64-bit audio, the editor is not resizable by the host, a shell plugin (several plugins in one DLL) shows only its first plugin. Its name, vendor and category come from the plugin, so the CLAP starts the plugin once when the host scans it (that instance is reused for the first plugin the host creates).
- **Speed:** every call is a round trip between two processes. Hosts that ask for every parameter value many times a second (`getParameter`) see more CPU load than with the plugin in their own process.
- **Untested in real hosts so far**: the tests run a test plugin and a test host under Wine (`bridge/test/`). Feedback with host and plugin names is welcome.
