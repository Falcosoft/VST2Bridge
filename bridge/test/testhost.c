// testhost.c - a minimal VST2 host for the bridge tests (built as 32-bit and 64-bit):  testhost.exe <bridge.dll>
// Loads the bridge around test/fakevst.c and checks every path of the protocol. Exit code = number of failures.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../vst2bridge.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); fails++; } else printf("ok:   "); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static volatile LONG n_version, n_iochanged, n_gettime, n_midiout, n_sizewin, n_automate1, n_automate2, n_begin, n_end;
static volatile LONG last_w, last_h;
static VstTimeInfo ti;
static int g_rate = 44100;              // what audioMasterGetSampleRate answers

static intptr_t VSTCALL master(AEffect* e, int32_t op, int32_t i, intptr_t v, void* p, float o)
{
    (void)e;
    switch (op) {
    case audioMasterVersion: InterlockedIncrement(&n_version); return 2400;
    case audioMasterIOChanged: InterlockedIncrement(&n_iochanged); return 1;
    case audioMasterGetTime: InterlockedIncrement(&n_gettime); memset(&ti, 0, sizeof ti); ti.tempo = 133.0; ti.sampleRate = 44100; return (intptr_t)&ti;
    case audioMasterProcessEvents: {
        const VstEvents* evs = (const VstEvents*)p;
        if (evs && evs->numEvents == 1 && evs->events[0]->type == kVstMidiType && ((unsigned char*)((VstMidiEvent*)evs->events[0])->midiData)[0] == 0x90)
            InterlockedIncrement(&n_midiout);
        return 1;
    }
    case audioMasterSizeWindow: InterlockedIncrement(&n_sizewin); last_w = i; last_h = (LONG)v; return 1;
    case audioMasterAutomate:
        if (i == 1 && fabsf(o - 0.25f) < 1e-6f) InterlockedIncrement(&n_automate1);
        if (i == 2 && fabsf(o - 0.75f) < 1e-6f) InterlockedIncrement(&n_automate2);
        return 1;
    case audioMasterBeginEdit: InterlockedIncrement(&n_begin); return 1;
    case audioMasterEndEdit: InterlockedIncrement(&n_end); return 1;
    case audioMasterGetSampleRate: return g_rate;
    case audioMasterGetVendorString: strcpy((char*)p, "testhost"); return 1;
    default: return 0;
    }
}

static void pump(int ms)
{
    DWORD t0 = GetTickCount();
    while ((int)(GetTickCount() - t0) < ms) {
        MSG m;
        while (PeekMessageA(&m, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
        MsgWaitForMultipleObjects(0, 0, FALSE, 10, QS_ALLINPUT);
    }
}

typedef AEffect* (VSTCALL *Entry)(AudioMasterCallback);
typedef struct { int32_t numEvents; intptr_t reserved; VstEvent* ev[4]; } Evs;
static float inL[12000], inR[12000], outL[12000], outR[12000];
static double dinL[512], dinR[512], doutL[512], doutR[512];

static void test_direct(const char* bridge, const char* path, const char* other)
{
    HMODULE module = LoadLibraryA(bridge);
    CHECK(module, "load a bridge DLL with no sibling plugin");
    if (!module) return;
    typedef AEffect* (VSTCALL *EntryA)(AudioMasterCallback, const char*);
    typedef AEffect* (VSTCALL *EntryW)(AudioMasterCallback, const wchar_t*);
    EntryA entry_a = (EntryA)(void*)GetProcAddress(module, "VST2BridgeCreateA");
    EntryW entry_w = (EntryW)(void*)GetProcAddress(module, "VST2BridgeCreateW");
    CHECK(entry_a && entry_w, "exports both direct-loading entry points");
    if (!entry_a || !entry_w) { FreeLibrary(module); return; }

    wchar_t wide[MAX_PATH];
    int converted = MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    CHECK(converted, "convert the explicit plugin path to UTF-16");
    if (!converted) { FreeLibrary(module); return; }

    AEffect* direct[] = { entry_a(master, path), entry_w(master, wide) };
    for (int i = 0; i < 2; i++) {
        AEffect* fx = direct[i];
        CHECK(fx && fx->magic == kEffectMagic, "load plugin by path through VST2BridgeCreate%c", i ? 'W' : 'A');
        if (!fx || fx->magic != kEffectMagic) continue;
        fx->dispatcher(fx, effOpen, 0, 0, 0, 0);
        char vendor[256] = "";
        fx->dispatcher(fx, effGetVendorString, 0, 0, vendor, 0);
        CHECK(!strcmp(vendor, other), "direct-loaded plugin runs in the other bitness (%s)", vendor);
        fx->dispatcher(fx, effClose, 0, 0, 0, 0);
    }
    FreeLibrary(module);
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: testhost <bridge.dll> [other bitness] [direct bridge.dll direct plugin.dll]\n"); return 99; }
    const char* other = argc > 2 ? argv[2] : (sizeof(void*) == 8 ? "Fake32" : "Fake64");
    printf("host is %d-bit, bridge %s\n", (int)sizeof(void*) * 8, argv[1]);
    HMODULE m = LoadLibraryA(argv[1]);
    CHECK(m != 0, "load the bridge");
    if (!m) return 1;
    CHECK(GetProcAddress(m, "main") && GetProcAddress(m, "VST2BridgeStub"), "exports main and VST2BridgeStub");
    Entry entry = (Entry)(void*)GetProcAddress(m, "VSTPluginMain");
    AEffect* fx = entry ? entry(master) : 0;
    CHECK(fx && fx->magic == kEffectMagic, "plugin created through the helper");
    if (!fx) return 1;
    CHECK(n_version > 0, "callback while the plugin is created (audioMasterVersion)");
    CHECK(fx->numInputs == 2 && fx->numOutputs == 2 && fx->numParams == 3, "2 in, 2 out, 3 parameters");
    CHECK((fx->flags & effFlagsHasEditor) && (fx->flags & effFlagsProgramChunks) && (fx->flags & effFlagsCanDoubleReplacing), "flags: editor, chunks, double");
    CHECK(fx->uniqueID == 0x466B5673, "unique id");

    char s[256] = "";
    fx->dispatcher(fx, effOpen, 0, 0, 0, 0);
    fx->dispatcher(fx, effGetEffectName, 0, 0, s, 0); CHECK(!strcmp(s, "FakeVST"), "effGetEffectName = %s", s);
    fx->dispatcher(fx, effGetVendorString, 0, 0, s, 0); CHECK(!strcmp(s, other), "plugin runs in the other bitness (%s)", s);
    CHECK(fx->dispatcher(fx, effGetVendorVersion, 0, 0, 0, 0) == 1234, "effGetVendorVersion");
    CHECK(fx->dispatcher(fx, effCanDo, 0, 0, (void*)"receiveVstEvents", 0) == 1, "effCanDo receiveVstEvents");
    CHECK(fx->dispatcher(fx, effCanDo, 0, 0, (void*)"nothing", 0) == -1, "effCanDo unknown = -1");
    fx->dispatcher(fx, effGetParamName, 1, 0, s, 0); CHECK(!strcmp(s, "Param1"), "effGetParamName = %s", s);
    unsigned char pp[152]; memset(pp, 0, sizeof pp);
    fx->dispatcher(fx, effGetParameterProperties, 0, 0, pp, 0);
    float step; memcpy(&step, pp, 4);
    CHECK(step == 0.125f && !strcmp((char*)pp + 12, "Gain"), "effGetParameterProperties (structure in and out)");
    fx->dispatcher(fx, effString2Parameter, 1, 0, (void*)"0.3", 0);
    CHECK(fabsf(fx->getParameter(fx, 1) - 0.3f) < 1e-6f, "effString2Parameter (string in)");
    fx->setParameter(fx, 0, 0.5f);
    CHECK(fx->getParameter(fx, 0) == 0.5f, "setParameter / getParameter");

    DWORD t0 = GetTickCount();
    for (int i = 0; i < 20000; i++) fx->getParameter(fx, 0);
    DWORD dt = GetTickCount() - t0;
    printf("      20000 getParameter calls: %lu ms (%.1f us each)\n", dt, dt * 1000.0 / 20000);

    fx->dispatcher(fx, effSetSampleRate, 0, 0, 0, 44100.0f);
    fx->dispatcher(fx, effSetBlockSize, 0, 512, 0, 0);
    fx->dispatcher(fx, effMainsChanged, 0, 1, 0, 0);
    CHECK(n_iochanged == 1 && fx->initialDelay == 64, "audioMasterIOChanged inside effMainsChanged, initialDelay = %d", fx->initialDelay);

    // audio with a note-on at frame 100 and a SysEx
    for (int i = 0; i < 12000; i++) { inL[i] = 0.2f; inR[i] = -0.2f; }
    VstMidiEvent on; memset(&on, 0, sizeof on);
    on.type = kVstMidiType; on.byteSize = sizeof on; on.deltaFrames = 100; on.midiData[0] = (char)0x90; on.midiData[1] = 60; on.midiData[2] = 127;
    char gmon[6] = { (char)0xF0, 0x7E, 0x7F, 0x09, 0x01, (char)0xF7 };
    VstMidiSysexEvent sx; memset(&sx, 0, sizeof sx);
    sx.type = kVstSysExType; sx.byteSize = sizeof sx; sx.dumpBytes = 6; sx.sysexDump = gmon;
    Evs ev; memset(&ev, 0, sizeof ev); ev.numEvents = 2; ev.ev[0] = (VstEvent*)&on; ev.ev[1] = (VstEvent*)&sx;
    fx->dispatcher(fx, effProcessEvents, 0, 0, &ev, 0);
    float* in[2] = { inL, inR }; float* out[2] = { outL, outR };
    fx->processReplacing(fx, in, out, 512);
    CHECK(fabsf(outL[0] - 0.1f) < 1e-6f && fabsf(outR[99] + 0.1f) < 1e-6f, "audio in -> out (gain 0.5): %f %f", outL[0], outR[99]);
    CHECK(fabsf(outL[100] - 1.1f) < 1e-6f && fabsf(outL[511] - 1.1f) < 1e-6f, "note-on at its frame: %f -> %f", outL[99], outL[100]);
    CHECK(fx->getParameter(fx, 1) == 0.5f, "SysEx reached the plugin");
    CHECK(n_gettime > 0, "audioMasterGetTime from the plugin's audio thread");
    CHECK(n_midiout == 1, "MIDI out of the plugin (audioMasterProcessEvents)");
    fx->processReplacing(fx, in, out, 10000);                       // longer than a piece of the bridge
    CHECK(fabsf(outL[9999] - 1.1f) < 1e-6f && fabsf(outR[5000] - 0.9f) < 1e-6f, "long block (split by the bridge)");
    for (int i = 0; i < 512; i++) { outL[i] = 1.0f; }
    fx->process(fx, in, out, 512);
    CHECK(fabsf(outL[10] - 2.1f) < 1e-6f, "accumulating process: %f", outL[10]);
    for (int i = 0; i < 512; i++) { dinL[i] = 0.4; dinR[i] = 0.4; }
    double* din[2] = { dinL, dinR }; double* dout[2] = { doutL, doutR };
    fx->processDoubleReplacing(fx, din, dout, 512);
    CHECK(fabs(doutL[3] - 1.2) < 1e-6, "processDoubleReplacing: %f", doutL[3]);

    t0 = GetTickCount();
    for (int i = 0; i < 2000; i++) fx->processReplacing(fx, in, out, 64);
    dt = GetTickCount() - t0;
    printf("      2000 process calls of 64 frames: %lu ms (%.1f us each)\n", dt, dt * 1000.0 / 2000);

    // chunks: small (inside a frame) and large (own mapping)
    void* cp = 0;
    intptr_t n = fx->dispatcher(fx, effGetChunk, 0, 0, &cp, 0);
    CHECK(n == 16 && cp && ((unsigned char*)cp)[3] == 21, "effGetChunk small (%d bytes)", (int)n);
    static unsigned char big[200 * 1024];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (unsigned char)(i * 13 + 5);
    CHECK(fx->dispatcher(fx, effSetChunk, 0, sizeof big, big, 0) == 1, "effSetChunk large (%d bytes)", (int)sizeof big);
    cp = 0; n = fx->dispatcher(fx, effGetChunk, 0, 0, &cp, 0);
    CHECK(n == (intptr_t)sizeof big && cp && !memcmp(cp, big, sizeof big), "effGetChunk large returns the same data");

    // a callback from a thread of the plugin's own
    fx->dispatcher(fx, effSetProgram, 0, 99, 0, 0);
    CHECK(n_automate1 == 1, "callback from another plugin thread (aux channel)");

    // editor
    WNDCLASSA wc; memset(&wc, 0, sizeof wc); wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(0); wc.lpszClassName = "TestHostWnd";
    RegisterClassA(&wc);
    HWND w = CreateWindowA("TestHostWnd", "testhost", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 600, 400, 0, 0, GetModuleHandleA(0), 0);
    ERect* r = 0;
    fx->dispatcher(fx, effEditGetRect, 0, 0, &r, 0);
    CHECK(r && r->right == 300 && r->bottom == 200, "effEditGetRect before open: %dx%d", r ? r->right : 0, r ? r->bottom : 0);
    CHECK(fx->dispatcher(fx, effEditOpen, 0, 0, w, 0) == 1, "effEditOpen");
    CHECK(n_sizewin == 1 && last_w == 320 && last_h == 240, "nested sizeWindow inside effEditOpen (%ldx%ld)", last_w, last_h);
    r = 0; fx->dispatcher(fx, effEditGetRect, 0, 0, &r, 0);
    CHECK(r && r->right == 320 && r->bottom == 240, "effEditGetRect after open: %dx%d", r ? r->right : 0, r ? r->bottom : 0);
    pump(800);
    CHECK(n_automate2 == 1 && n_begin == 1 && n_end == 1, "callbacks started by the plugin's GUI (begin/automate/end)");
    CHECK(n_sizewin == 2 && last_w == 400 && last_h == 250, "sizeWindow started by the plugin's GUI, run on the host's GUI thread (%ldx%ld)", last_w, last_h);
    CHECK(fx->getParameter(fx, 2) == 0.75f, "parameter changed in the GUI");
    fx->dispatcher(fx, effEditClose, 0, 0, 0, 0);
    DestroyWindow(w);

    fx->dispatcher(fx, effMainsChanged, 0, 0, 0, 0);
    fx->dispatcher(fx, effClose, 0, 0, 0, 0);

    // the host's sample rate reaches the plugin unchanged, whatever it is (no conversion in the bridge)
    {
        static const int rates[] = { 32000, 22050, 44100, 96000 };
        for (unsigned k = 0; k < sizeof rates / sizeof rates[0]; k++) {
            g_rate = rates[k];
            AEffect* r2 = entry(master);
            if (!r2) { CHECK(0, "another instance for the rate test"); continue; }
            r2->dispatcher(r2, effOpen, 0, 0, 0, 0);
            r2->dispatcher(r2, effSetSampleRate, 0, 0, 0, (float)rates[k]);
            r2->dispatcher(r2, effSetBlockSize, 0, 512, 0, 0);
            r2->dispatcher(r2, effMainsChanged, 0, 1, 0, 0);
            intptr_t r0 = r2->dispatcher(r2, effGetTailSize, 0, 0, 0, 0), r1 = r2->dispatcher(r2, effGetTailSize, 1, 0, 0, 0);
            CHECK(r0 == rates[k] && r1 == rates[k], "host at %d Hz: plugin set to %d Hz, audioMasterGetSampleRate answers %d", rates[k], (int)r0, (int)r1);
            CHECK(r2->initialDelay == 64, "latency unchanged at %d Hz: %d", rates[k], r2->initialDelay);
            r2->dispatcher(r2, effMainsChanged, 0, 0, 0, 0);
            r2->dispatcher(r2, effClose, 0, 0, 0, 0);
        }
    }

    if (argc > 4) test_direct(argv[3], argv[4], other);

    printf("RESULT: %s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails;
}
