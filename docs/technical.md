# How the bridge works

[Back to the README](../README.md)

## Files

| File | Built from | Role |
|---|---|---|
| `<name>_x64.dll` | `bridge/stub.c` (64-bit, `vst2bridge-x64.dll`) | Loaded by a 64-bit host in place of the 32-bit plugin `<name>.dll` |
| `<name>_x86.dll` | `bridge/stub.c` (32-bit, `vst2bridge-x86.dll`) | Loaded by a 32-bit host in place of the 64-bit plugin `<name>.dll` |
| `<name>_x64.clap` | `bridge/clap.c` (64-bit, `vst2bridge-x64.clap`) | CLAP plugin for 64-bit hosts; loads `<name>_x64.dll` |
| `<name>_x86.clap` | `bridge/clap.c` (32-bit, `vst2bridge-x86.clap`) | CLAP plugin for 32-bit hosts; loads `<name>_x86.dll` |
| `vst2bridge-host32.exe` | `bridge/host.c` (32-bit) | Helper that runs a 32-bit plugin |
| `vst2bridge-host64.exe` | `bridge/host.c` (64-bit) | Helper that runs a 64-bit plugin |

All bridge DLLs of one bitness are the same file; the bridge takes the plugin from its own name: it removes `.dll` and the suffix `_x64` (64-bit bridge) or `_x86` (32-bit bridge) and loads `<name>.dll` from its own folder. The helper must be in the same folder. The bridge exports `VSTPluginMain`, `main` (old hosts) and the marker `VST2BridgeStub`, by which the patcher recognises its own files (it also skips JBridge's files, which export `JBridgeBootstrap`).

## CLAP bridge

The CLAP bridge (`bridge/clap.c`) is a VST2 host in a CLAP plugin: it loads the VST2 file that has its own name from its own folder (`foo_x64.clap` → `foo_x64.dll`, the VST2 bridge) into the host's process and translates every CLAP call into VST2 calls. The process boundary, the helper and the passing of data are those of the VST2 bridge, unchanged. All CLAP bridges of one bitness are the same file; it exports `clap_entry` and the marker `VST2BridgeClap`.

| CLAP | VST2 |
|---|---|
| Descriptor | Name (`effGetEffectName`, else `effGetProductString`, else the file name), vendor, version (`effGetVendorVersion`), feature `instrument` (`effFlagsIsSynth`) or `audio-effect`; id `io.github.nightfright2k19.vst2bridge.<file name without suffix, lower case>`. To read them the bridge creates the plugin once when the host first asks; that instance is handed to the first `create_plugin`, so a host that scans and then loads starts the helper only once. |
| `activate` / `deactivate` | `effSetSampleRate`, `effSetBlockSize`, `effMainsChanged` |
| `start_processing` / `stop_processing` | `effStartProcess` / `effStopProcess` |
| Audio ports | VST2 channels in pairs: port 0 = channels 1-2 (main), port 1 = channels 3-4 ...; a last odd channel is a mono port |
| Note ports | MIDI in when the plugin is a synth or answers `effCanDo receiveVstEvents` / `receiveVstMidiEvent`; MIDI out for `sendVstEvents` / `sendVstMidiEvent` |
| `process` | CLAP MIDI, SysEx and note on / off events → `effProcessEvents` at their frame; parameter events → `setParameter` before the block; transport → `audioMasterGetTime`; `processReplacing`; MIDI and SysEx of the plugin (`audioMasterProcessEvents` during process) → output events. `reset` sends all notes off and reset controllers on 16 channels with the next block. |
| Parameters | id = VST2 index, range 0 … 1, name `effGetParamName`, default = value after loading; text = `effGetParamDisplay` + `effGetParamLabel` for the current value (other values as a number). `audioMasterAutomate` / `BeginEdit` / `EndEdit` from the plugin become `PARAM_VALUE` / gesture events (during process directly, otherwise queued, with `request_flush`); changes the host made itself are not sent back. `audioMasterUpdateDisplay` → `params.rescan`. |
| State | `'VBCL'`, version, kind, program, length, data: the plugin's bank chunk (`effGetChunk` / `effSetChunk`) when it has `effFlagsProgramChunks`, otherwise all parameter values |
| Latency | `initialDelay`, read after `effMainsChanged`; a change while active (`audioMasterIOChanged`) asks the host for a restart |
| GUI | Win32, embedded: `effEditGetRect`, `effEditOpen` with the host's window, `effEditClose`; `audioMasterSizeWindow` → `gui.request_resize`. Not resizable by the host. |

## Start

`VSTPluginMain` of the bridge creates one shared-memory block and a set of named events and a mutex (`Local\V2B<host pid>_<n>_*`), asks the host for `audioMasterCurrentId` (shell plugins), and starts the helper in the plugin's folder:

```
vst2bridge-hostNN.exe <plugin.dll> <object prefix> <shell id> <host pid>
```

The helper loads the plugin, calls its `VSTPluginMain`, writes the plugin's `AEffect` fields (`numInputs`, `flags`, `uniqueID` ...) into shared memory and signals "ready". The bridge copies those fields into its own `AEffect` and returns it to the host. The helper is put into a job object that ends it with the host, and it also watches the host process itself.

If anything fails, `VSTPluginMain` returns NULL (the host reports that the plugin cannot be loaded) and the reason goes to `%TEMP%\vst2bridge.log`.

## Calls across the process boundary

Every host → plugin call (`dispatcher`, `setParameter`, `getParameter`, `process*`) becomes a request frame in shared memory; the helper makes the real call and writes the reply into the same frame. The plugin's calls to the host (`audioMaster`) travel the other way. Calls nest: during `effEditOpen` a plugin may call `audioMasterSizeWindow`, and the host may call `effEditGetRect` while answering it. So each channel holds a stack of up to 8 frames and the side waiting for a reply serves the other side's nested requests meanwhile, on the same thread, just as the calls would run in one process.

Three channels connect the processes (`bridge/protocol.h`):

| Channel | Bridge side | Helper side | Who starts calls |
|---|---|---|---|
| main | the host's GUI / control threads | the main thread (runs the window message loop) | both; a named mutex decides who starts the outermost call |
| audio | the thread that calls `process` | an audio thread (time-critical priority) | the bridge only (the plugin's callbacks from its audio thread nest inside) |
| aux | a server thread | any other thread of the plugin | the helper only (one call at a time) |

Calls the host makes on the thread that last called `process` go through the audio channel, so they never wait for the GUI; all other calls go through the main channel. Editor calls always use the main channel, because the editor window belongs to the helper's main thread.

Calls the plugin starts on its own from its GUI thread (a knob turned: `audioMasterBeginEdit` / `Automate` / `EndEdit`) are answered by a server thread of the bridge; `audioMasterSizeWindow` from there is handed to the host's editor thread (where the editor was opened), because hosts expect window changes on that thread. While a thread of the bridge waits, it keeps answering messages sent to its windows, so cross-process window messages cannot deadlock.

## What is passed and how

The data behind `ptr` is copied into the frame (`bridge/marshal.h`):

| Kind | Opcodes |
|---|---|
| Value only | `effOpen`, `effSetProgram`, `effGetProgram`, `effSetSampleRate`, `effSetBlockSize`, `effMainsChanged`, `effEditClose`, `effCanBeAutomated`, `effGetPlugCategory`, `effSetBypass`, `effGetVendorVersion`, `effGetTailSize`, `effGetVstVersion`, `effEditKeyDown/Up`, `effStartProcess`, `effStopProcess`, `effSetProcessPrecision`, `effGetNumMidi*Channels`, ... and unknown opcodes without data |
| String in | `effSetProgramName`, `effString2Parameter`, `effCanDo` |
| String out | `effGetProgramName`, `effGetParamLabel/Display/Name`, `effGetProgramNameIndexed`, `effGetEffectName`, `effGetVendorString`, `effGetProductString`, `effShellGetNextPlugin` |
| Structure in / out | `effGetInput/OutputProperties`, `effGetParameterProperties`, `effGetMidiProgramName`, `effGetCurrentMidiProgram`, `effGetMidiProgramCategory`, `effGetMidiKeyName`, `effBeginLoadBank/Program` |
| Special | `effEditGetRect` (the bridge keeps a copy of the rectangle), `effEditOpen` (the helper creates a child window of the host's window), `effGetChunk` / `effSetChunk` (up to 64 KB in the frame, larger through a mapping of their own, any size), `effProcessEvents` (queued, sent with the next `process`), `effSetSpeakerArrangement` / `effGetSpeakerArrangement`, `effVendorSpecific` (only without data) |
| Answered by the bridge | `effEditIdle` (the helper gives the editor idle time itself, every 30 ms) |

Plugin → host: value-only calls (`audioMasterAutomate`, `Version`, `Idle`, `IOChanged`, `GetSampleRate`, `GetBlockSize`, `BeginEdit`, `EndEdit`, `UpdateDisplay`, ...), `CanDo` (string in), `GetVendorString` / `GetProductString` (string out), `GetTime` (the time info is copied back), `ProcessEvents` (MIDI and SysEx out of the plugin), `GetDirectory`, `SizeWindow` (the helper also resizes its window), `VendorSpecific` without data. `audioMasterCurrentId` is answered by the helper with the id the bridge got from the host. Before each callback and after each call the bridge refreshes its `AEffect` fields, so `audioMasterIOChanged` reports the plugin's new values.

## Audio

`processReplacing`, `process` (accumulating) and `processDoubleReplacing` all go through the audio channel: the inputs (up to 32 channels), the events queued by `effProcessEvents` and the frame count go into shared memory, the helper calls `effProcessEvents` and the plugin's process function, and the outputs come back. Blocks longer than 4096 frames are split; events are given to the piece they fall into. A plugin without `processReplacing` is run with `process` into cleared buffers. A call costs one round trip between the processes (two event signals).

## Failure

If the helper ends (the plugin crashed), every waiting call returns at once; from then on the bridge outputs silence and returns 0, so the host keeps running.
