// marshal.h - how the data behind `ptr` (and for two opcodes `value`) of every VST2 opcode crosses the process
// boundary. Used by both sides: stub.c (dispatcher out, audioMaster in) and host.c (dispatcher in, audioMaster out).
#ifndef VB_MARSHAL_H
#define VB_MARSHAL_H
#include "vst2.h"

enum {
    M_NONE,         // no data; ptr is passed as NULL (the call is refused when the host gave a non-NULL ptr to an unknown opcode)
    M_STR_IN,       // ptr = zero-terminated string read by the callee (may be NULL)
    M_STR_OUT,      // ptr = buffer the callee writes a string into
    M_IN,           // ptr = fixed-size structure read by the callee
    M_INOUT,        // ptr = fixed-size structure read and written by the callee
    M_SPECIAL,      // handled by name in stub.c / host.c
    M_LOCAL,        // answered by the bridge itself
    M_REFUSE        // not supported across processes: returns 0
};

#define VB_STR_MAX 1024                 // largest string copied back (VST limits are 8 ... 256)

static int disp_marshal(int op, int* size)
{
    *size = 0;
    switch (op) {
    case effSetProgramName: case effString2Parameter: case effCanDo:
        return M_STR_IN;
    case effGetProgramName: case effGetParamLabel: case effGetParamDisplay: case effGetParamName:
    case effGetProgramNameIndexed: case effGetEffectName: case effGetErrorText: case effGetVendorString:
    case effGetProductString: case effShellGetNextPlugin:
        return M_STR_OUT;
    case effGetInputProperties: case effGetOutputProperties: *size = VST_PIN_PROPERTIES_SIZE; return M_INOUT;
    case effGetParameterProperties: *size = VST_PARAM_PROPERTIES_SIZE; return M_INOUT;
    case effGetMidiProgramName: case effGetCurrentMidiProgram: *size = VST_MIDI_PROGRAM_NAME_SIZE; return M_INOUT;
    case effGetMidiProgramCategory: *size = VST_MIDI_PROGRAM_CAT_SIZE; return M_INOUT;
    case effGetMidiKeyName: *size = VST_MIDI_KEY_NAME_SIZE; return M_INOUT;
    case effBeginLoadBank: case effBeginLoadProgram: *size = VST_PATCH_CHUNK_INFO_SIZE; return M_IN;
    case effClose: case effEditGetRect: case effEditOpen: case effGetChunk: case effSetChunk:
    case effProcessEvents: case effSetSpeakerArrangement: case effGetSpeakerArrangement: case effVendorSpecific:
        return M_SPECIAL;
    case effEditIdle:                   // the helper gives the editor its idle time itself
        return M_LOCAL;
    case effEditDraw: case effEditMouse: case effEditKey: case effGetCurrentPosition: case effGetDestinationBuffer:
    case effOfflineNotify: case effOfflinePrepare: case effOfflineRun: case effProcessVarIo: case effGetIcon:
        return M_REFUSE;
    default:
        return M_NONE;
    }
}

static int master_marshal(int op, int* size)
{
    *size = 0;
    switch (op) {
    case audioMasterCanDo: return M_STR_IN;
    case audioMasterGetVendorString: case audioMasterGetProductString: return M_STR_OUT;
    case audioMasterGetTime: case audioMasterProcessEvents: case audioMasterGetDirectory:
    case audioMasterVendorSpecific: case audioMasterSizeWindow:
        return M_SPECIAL;
    case audioMasterSetTime: case audioMasterGetPreviousPlug: case audioMasterGetNextPlug:
    case audioMasterOfflineStart: case audioMasterOfflineRead: case audioMasterOfflineWrite:
    case audioMasterGetOutputSpeakerArrangement: case audioMasterSetIcon: case audioMasterOpenWindow:
    case audioMasterCloseWindow: case audioMasterOpenFileSelector: case audioMasterCloseFileSelector:
    case audioMasterEditFile: case audioMasterGetChunkFile: case audioMasterGetInputSpeakerArrangement:
        return M_REFUSE;
    default:
        return M_NONE;
    }
}

// size of a VstSpeakerArrangement as it is stored (speakers[] has at least 8 entries)
static int speaker_arr_size(const void* a)
{
    int32_t n = ((const int32_t*)a)[1];
    if (n < 8) n = 8;
    if (n > 64) n = 64;
    return VST_SPEAKER_ARR_HEADER + n * VST_SPEAKER_PROPS_SIZE;
}
#endif
