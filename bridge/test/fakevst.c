// fakevst.c - a small VST2 plugin for the bridge tests, built as 32-bit and 64-bit. It exercises the paths of
// the bridge: strings, structures, parameters, chunks (small and large), audio with events, MIDI out, the
// editor with a nested sizeWindow, callbacks from the GUI thread, the audio thread and a thread of its own.
//   output = input * gain (parameter 0) + velocity/127 from the frame of the last note-on on
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "../vst2.h"

#define BIG_CHUNK (200 * 1024)
static AudioMasterCallback host;
static AEffect fx;
static float params[3] = { 0.5f, 0.0f, 0.0f };
static float level;                    // current note level
static ERect rect = { 0, 0, 200, 300 };
static unsigned char chunk[BIG_CHUNK];
static size_t chunk_len = 16;
static HWND editor;
static int ticks;
static double last_tempo;
static int plain;                      // FAKEVST_PLAIN=1: an old instrument (no synth flag, no effCanDo, no inputs, 4 outputs)

static void midi_out(int delta, int status, int d1, int d2)
{
    VstMidiEvent e; memset(&e, 0, sizeof e);
    e.type = kVstMidiType; e.byteSize = sizeof e; e.deltaFrames = delta;
    e.midiData[0] = (char)status; e.midiData[1] = (char)d1; e.midiData[2] = (char)d2;
    struct { int32_t n; intptr_t r; VstEvent* ev[2]; } list = { 1, 0, { (VstEvent*)&e, 0 } };
    host(&fx, audioMasterProcessEvents, 0, 0, &list, 0);
}

static int pending_delta = -1; static float pending_level;
static intptr_t VSTCALL dispatch(AEffect* e, int32_t op, int32_t idx, intptr_t val, void* ptr, float opt);

static void render(void** in, void** out, int n, int dbl)
{
    VstTimeInfo* t = (VstTimeInfo*)host(&fx, audioMasterGetTime, 0, 0, 0, 0);     // callback on the audio thread
    if (t) last_tempo = t->tempo;
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < n; i++) {
            float lv = (pending_delta >= 0 && i >= pending_delta) ? pending_level : level;
            if (dbl) ((double*)out[c])[i] = (plain ? 0 : ((double*)in[c])[i]) * params[0] + lv;
            else ((float*)out[c])[i] = (plain ? 0 : ((float*)in[c])[i]) * params[0] + lv;
        }
    }
    if (pending_delta >= 0) { level = pending_level; pending_delta = -1; midi_out(0, 0x90, 60, 1); }
}
static void VSTCALL proc_repl(AEffect* e, float** in, float** out, int32_t n) { (void)e; render((void**)in, (void**)out, n, 0); }
static void VSTCALL proc_dbl(AEffect* e, double** in, double** out, int32_t n) { (void)e; render((void**)in, (void**)out, n, 1); }
static void VSTCALL proc_acc(AEffect* e, float** in, float** out, int32_t n) { (void)e; (void)in; (void)out; (void)n; }
static void VSTCALL set_param(AEffect* e, int32_t i, float v) { (void)e; if (i >= 0 && i < 3) params[i] = v; }
static float VSTCALL get_param(AEffect* e, int32_t i) { (void)e; return i >= 0 && i < 3 ? params[i] : 0; }

static DWORD WINAPI worker(LPVOID a) { (void)a; host(&fx, audioMasterAutomate, 1, 0, 0, 0.25f); return 0; }

static LRESULT CALLBACK ed_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_TIMER) {
        ticks++;
        if (ticks == 2) {                   // the GUI thread calls the host on its own (no host call running)
            host(&fx, audioMasterBeginEdit, 2, 0, 0, 0);
            params[2] = 0.75f; host(&fx, audioMasterAutomate, 2, 0, 0, 0.75f);
            host(&fx, audioMasterEndEdit, 2, 0, 0, 0);
        }
        if (ticks == 4) { rect.right = 400; rect.bottom = 250; host(&fx, audioMasterSizeWindow, 400, 250, 0, 0); }
        return 0;
    }
    return DefWindowProcA(w, m, wp, lp);
}

static intptr_t VSTCALL dispatch(AEffect* e, int32_t op, int32_t idx, intptr_t val, void* ptr, float opt)
{
    (void)e; (void)opt;
    switch (op) {
    case effGetEffectName: strcpy((char*)ptr, "FakeVST"); return 1;
    case effGetVendorString: strcpy((char*)ptr, sizeof(void*) == 8 ? "Fake64" : "Fake32"); return 1;
    case effGetProductString: strcpy((char*)ptr, "Bridge test"); return 1;
    case effGetVendorVersion: return 1234;
    case effGetVstVersion: return 2400;
    case effGetPlugCategory: return plain ? 0 : 2;
    case effGetParamName: snprintf((char*)ptr, 32, "Param%d", idx); return 0;
    case effGetParamDisplay: snprintf((char*)ptr, 32, "%.2f", idx >= 0 && idx < 3 ? params[idx] : 0); return 0;
    case effGetParamLabel: strcpy((char*)ptr, "dB"); return 0;
    case effString2Parameter: if (ptr && idx >= 0 && idx < 3) params[idx] = (float)atof((char*)ptr); return 1;
    case effCanDo: if (plain) return 0;
        return ptr && (!strcmp((char*)ptr, "receiveVstEvents") || !strcmp((char*)ptr, "sendVstMidiEvent")) ? 1 : -1;
    case effGetParameterProperties: {
        unsigned char* p = (unsigned char*)ptr;   // stepFloat, smallStepFloat, largeStepFloat, label[64] ...
        float s = 0.125f; memcpy(p, &s, 4); strcpy((char*)p + 12, "Gain");
        return 1;
    }
    case effMainsChanged:
        if (val) { fx.initialDelay = 64; host(&fx, audioMasterIOChanged, 0, 0, 0, 0); }
        return 0;
    case effSetSampleRate: return 0;
    case effSetProgram:
        if (val == 99) { HANDLE t = CreateThread(0, 0, worker, 0, 0, 0); WaitForSingleObject(t, 5000); CloseHandle(t); }
        return 0;
    case effProcessEvents: {
        const VstEvents* evs = (const VstEvents*)ptr;
        for (int i = 0; i < evs->numEvents; i++) {
            const VstMidiEvent* m = (const VstMidiEvent*)evs->events[i];
            if (m->type == kVstMidiType && (m->midiData[0] & 0xF0) == 0x90) { pending_delta = m->deltaFrames; pending_level = (unsigned char)m->midiData[2] / 127.0f; }
            if (m->type == kVstSysExType) { const VstMidiSysexEvent* s = (const VstMidiSysexEvent*)m; if (s->dumpBytes == 6 && (unsigned char)s->sysexDump[1] == 0x7E) params[1] = 0.5f; }
        }
        return 1;
    }
    case effGetChunk: *(void**)ptr = chunk; return (intptr_t)chunk_len;
    case effSetChunk: if (val > 0 && val <= BIG_CHUNK) { memcpy(chunk, ptr, (size_t)val); chunk_len = (size_t)val; } return 1;
    case effEditGetRect: *(ERect**)ptr = &rect; return 1;
    case effEditOpen: {
        WNDCLASSA wc; memset(&wc, 0, sizeof wc); wc.lpfnWndProc = ed_proc; wc.hInstance = GetModuleHandleA(0); wc.lpszClassName = "FakeEd";
        RegisterClassA(&wc);
        editor = CreateWindowA("FakeEd", "", WS_CHILD | WS_VISIBLE, 0, 0, 300, 200, (HWND)ptr, 0, GetModuleHandleA(0), 0);
        rect.right = 320; rect.bottom = 240;
        host(&fx, audioMasterSizeWindow, 320, 240, 0, 0);   // nested: inside the host's effEditOpen
        SetTimer(editor, 1, 50, 0);
        return 1;
    }
    case effEditClose: if (editor) { DestroyWindow(editor); editor = 0; } return 0;
    default: return 0;
    }
}

__declspec(dllexport) AEffect* VSTPluginMain(AudioMasterCallback m)
{
    host = m;
    if (!host(0, audioMasterVersion, 0, 0, 0, 0)) return 0;
    for (size_t i = 0; i < sizeof chunk; i++) chunk[i] = (unsigned char)(i * 7);
    fx.magic = kEffectMagic; fx.dispatcher = dispatch; fx.process = proc_acc; fx.processReplacing = proc_repl;
    fx.processDoubleReplacing = proc_dbl; fx.setParameter = set_param; fx.getParameter = get_param;
    fx.numPrograms = 1; fx.numParams = 3; fx.numInputs = 2; fx.numOutputs = 2;
    fx.flags = effFlagsHasEditor | effFlagsCanReplacing | effFlagsProgramChunks | effFlagsIsSynth | effFlagsCanDoubleReplacing;
    fx.uniqueID = 0x466B5673; fx.version = 1;
    char env[8];
    if (GetEnvironmentVariableA("FAKEVST_PLAIN", env, sizeof env) && env[0] == '1') {
        plain = 1; fx.flags &= ~effFlagsIsSynth; fx.numInputs = 0; fx.numOutputs = 4;
    }
    return &fx;
}
