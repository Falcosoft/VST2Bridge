// vst2.h - the VST 2.4 declarations VST2Bridge needs, written from the public interface description
// (no Steinberg SDK file is used). Pointer-sized fields use intptr_t, so the same header gives the right
// layout for 32-bit and 64-bit builds.
#ifndef VB_VST2_H
#define VB_VST2_H
#include <stdint.h>

#ifdef _WIN32
#define VSTCALL __cdecl
#else
#define VSTCALL
#endif

typedef struct AEffect AEffect;
typedef intptr_t (VSTCALL *AudioMasterCallback)(AEffect*, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
typedef intptr_t (VSTCALL *DispatcherProc)(AEffect*, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
typedef void     (VSTCALL *ProcessProc)(AEffect*, float**, float**, int32_t);
typedef void     (VSTCALL *ProcessDoubleProc)(AEffect*, double**, double**, int32_t);
typedef void     (VSTCALL *SetParameterProc)(AEffect*, int32_t, float);
typedef float    (VSTCALL *GetParameterProc)(AEffect*, int32_t);

struct AEffect {
    int32_t magic;                      // kEffectMagic
    DispatcherProc dispatcher;
    ProcessProc process;                // accumulating (deprecated, still called by some hosts)
    SetParameterProc setParameter;
    GetParameterProc getParameter;
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void* object; void* user;
    int32_t uniqueID, version;
    ProcessProc processReplacing;
    ProcessDoubleProc processDoubleReplacing;
    char future[56];
};

#define kEffectMagic 0x56737450         // 'VstP'

enum {
    effFlagsHasEditor = 1 << 0, effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5,
    effFlagsIsSynth = 1 << 8, effFlagsNoSoundInStop = 1 << 9, effFlagsCanDoubleReplacing = 1 << 12
};

enum {                                  // dispatcher opcodes (host -> plugin)
    effOpen = 0, effClose, effSetProgram, effGetProgram, effSetProgramName, effGetProgramName,
    effGetParamLabel, effGetParamDisplay, effGetParamName, effGetVu, effSetSampleRate, effSetBlockSize,
    effMainsChanged, effEditGetRect, effEditOpen, effEditClose, effEditDraw, effEditMouse, effEditKey,
    effEditIdle, effEditTop, effEditSleep, effIdentify, effGetChunk, effSetChunk, effProcessEvents,
    effCanBeAutomated, effString2Parameter, effGetNumProgramCategories, effGetProgramNameIndexed,
    effCopyProgram, effConnectInput, effConnectOutput, effGetInputProperties, effGetOutputProperties,
    effGetPlugCategory, effGetCurrentPosition, effGetDestinationBuffer, effOfflineNotify, effOfflinePrepare,
    effOfflineRun, effProcessVarIo, effSetSpeakerArrangement, effSetBlockSizeAndSampleRate, effSetBypass,
    effGetEffectName, effGetErrorText, effGetVendorString, effGetProductString, effGetVendorVersion,
    effVendorSpecific, effCanDo, effGetTailSize, effIdle, effGetIcon, effSetViewPosition,
    effGetParameterProperties, effKeysRequired, effGetVstVersion, effEditKeyDown, effEditKeyUp,
    effSetEditKnobMode, effGetMidiProgramName, effGetCurrentMidiProgram, effGetMidiProgramCategory,
    effHasMidiProgramsChanged, effGetMidiKeyName, effBeginSetProgram, effEndSetProgram,
    effGetSpeakerArrangement, effShellGetNextPlugin, effStartProcess, effStopProcess,
    effSetTotalSampleToProcess, effSetPanLaw, effBeginLoadBank, effBeginLoadProgram,
    effSetProcessPrecision, effGetNumMidiInputChannels, effGetNumMidiOutputChannels
};

enum {                                  // audioMaster opcodes (plugin -> host)
    audioMasterAutomate = 0, audioMasterVersion, audioMasterCurrentId, audioMasterIdle,
    audioMasterPinConnected, audioMasterWantMidi = 6, audioMasterGetTime, audioMasterProcessEvents,
    audioMasterSetTime, audioMasterTempoAt, audioMasterGetNumAutomatableParameters,
    audioMasterGetParameterQuantization, audioMasterIOChanged, audioMasterNeedIdle, audioMasterSizeWindow,
    audioMasterGetSampleRate, audioMasterGetBlockSize, audioMasterGetInputLatency, audioMasterGetOutputLatency,
    audioMasterGetPreviousPlug, audioMasterGetNextPlug, audioMasterWillReplaceOrAccumulate,
    audioMasterGetCurrentProcessLevel, audioMasterGetAutomationState, audioMasterOfflineStart,
    audioMasterOfflineRead, audioMasterOfflineWrite, audioMasterOfflineGetCurrentPass,
    audioMasterOfflineGetCurrentMetaPass, audioMasterSetOutputSampleRate,
    audioMasterGetOutputSpeakerArrangement, audioMasterGetVendorString, audioMasterGetProductString,
    audioMasterGetVendorVersion, audioMasterVendorSpecific, audioMasterSetIcon, audioMasterCanDo,
    audioMasterGetLanguage, audioMasterOpenWindow, audioMasterCloseWindow, audioMasterGetDirectory,
    audioMasterUpdateDisplay, audioMasterBeginEdit, audioMasterEndEdit, audioMasterOpenFileSelector,
    audioMasterCloseFileSelector, audioMasterEditFile, audioMasterGetChunkFile,
    audioMasterGetInputSpeakerArrangement
};

typedef struct { int16_t top, left, bottom, right; } ERect;

enum { kVstMidiType = 1, kVstSysExType = 6 };

typedef struct { int32_t type, byteSize, deltaFrames, flags; char data[16]; } VstEvent;

typedef struct {
    int32_t numEvents; intptr_t reserved; VstEvent* events[2];  // events[] really has numEvents entries
} VstEvents;

typedef struct {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    char midiData[4]; char detune, noteOffVelocity, reserved1, reserved2;
} VstMidiEvent;

typedef struct {
    int32_t type, byteSize, deltaFrames, flags, dumpBytes;
    intptr_t resvd1; char* sysexDump; intptr_t resvd2;
} VstMidiSysexEvent;

typedef struct {                        // 88 bytes, no pointers
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;

// sizes of the structures passed by pointer that contain no pointers (identical for 32 and 64 bits)
#define VST_PIN_PROPERTIES_SIZE      128    // effGet{Input,Output}Properties
#define VST_PARAM_PROPERTIES_SIZE    152    // effGetParameterProperties
#define VST_MIDI_PROGRAM_NAME_SIZE    80    // effGetMidiProgramName, effGetCurrentMidiProgram
#define VST_MIDI_PROGRAM_CAT_SIZE     76    // effGetMidiProgramCategory
#define VST_MIDI_KEY_NAME_SIZE        80    // effGetMidiKeyName
#define VST_PATCH_CHUNK_INFO_SIZE     64    // effBeginLoadBank, effBeginLoadProgram
#define VST_SPEAKER_PROPS_SIZE       112    // one VstSpeakerProperties
#define VST_SPEAKER_ARR_HEADER         8    // VstSpeakerArrangement: type, numChannels, then speakers[] (at least 8)
#endif
