// clap.c - CLAP plugin around the VST2 bridge (VST2 inside, CLAP outside). This file only translates CLAP calls
// into VST2 calls in-process; the bridge (stub.c) does the rest. See docs/technical.md.
//   Windows 64-bit: copied as <plugin>_x64.clap next to a 32-bit plugin; loads <plugin>_x64.dll (vst2bridge-x64.dll)
//   Windows 32-bit: copied as <plugin>_x86.clap next to a 64-bit plugin; loads <plugin>_x86.dll (vst2bridge-x86.dll)
//     All .clap files of one bitness are the same file: it loads the VST2 DLL that has its own name (foo_x64.clap ->
//     foo_x64.dll in the same folder), so the process boundary, the helper and everything the VST2 bridge passes are
//     reused unchanged.
//   Linux / macOS: stub.c is linked in (vst2bridge-linux.clap, vst2bridge-macos); copied as <plugin>.clap next to
//     <plugin>.dll (macOS: a bundle <plugin>.clap/Contents/MacOS/<plugin>), it runs the plugin (32 or 64-bit) in
//     the helper of its bitness under Wine. Editor: Linux moves the helper's window into the host's X11 window,
//     macOS shows it as a floating window (a window of another process cannot be embedded there).
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "plat.h"
#include <wchar.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#ifndef _WIN32
#include <limits.h>
#include <unistd.h>
#if !defined(__APPLE__) && !defined(VB_FLOATING_GUI)
#include <xcb/xcb.h>
#endif
#endif
#include "clap/clap.h"
#include "vst2.h"
#include "protocol.h"

// the editor: embedded win32 child (Windows), helper window moved into the host's X11 window (Linux), floating
// window (macOS; VB_FLOATING_GUI builds that variant on Linux for the tests)
#if defined(_WIN32)
#define VB_GUI_WIN32
#elif defined(__APPLE__) || defined(VB_FLOATING_GUI)
#define VB_GUI_FLOATING
#else
#define VB_GUI_X11
#endif

#define VBC_MAGIC     0x4C434256u         // 'VBCL': state saved by the CLAP bridge
#define VBC_MAXEV     1024                // events given to the plugin per block (more are dropped)
#define VBC_QUEUE     512                 // parameter changes / gestures from the plugin waiting for the host
#define VBC_SYSEX_OUT 65536               // SysEx bytes out of the plugin per block

static char    g_stem8[256], g_id[320], g_name[256], g_vendor[256], g_version[32], g_descr[400];
static const char* g_features[4];
static clap_plugin_descriptor_t g_desc;
static int g_inits, g_probed, g_ok;
#ifdef _WIN32
static wchar_t g_dir[MAX_PATH], g_stem[MAX_PATH], g_self_name[MAX_PATH];
AEffect* vb_stub_main(AudioMasterCallback master, const wchar_t* dir, const wchar_t* stem, const wchar_t* self_name);   // stub.c, linked in
static CRITICAL_SECTION g_lock;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK init_lock(PINIT_ONCE o, PVOID a, PVOID* c) { (void)o; (void)a; (void)c; InitializeCriticalSection(&g_lock); return TRUE; }
static void glock(void) { EnterCriticalSection(&g_lock); }
static void gunlock(void) { LeaveCriticalSection(&g_lock); }
#else
static char g_dir[PATH_MAX], g_stem[256];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void glock(void) { pthread_mutex_lock(&g_lock); }
static void gunlock(void) { pthread_mutex_unlock(&g_lock); }
// stub.c, linked in
AEffect* vb_stub_main(AudioMasterCallback master, const char* dir, const char* stem);
intptr_t vb_stub_edit_open(AEffect* fx, int mode);
int vb_stub_edit_show(AEffect* fx, int show);
int vb_stub_edit_closed(AEffect* fx);
const char* vb_stub_token(AEffect* fx);
#endif

typedef struct { int kind; int32_t index; float value; } QEv;      // kind: 0 value, 1 gesture begin, 2 gesture end

typedef struct Inst {
    clap_plugin_t plugin;
    const clap_host_t* host;
    const clap_host_params_t* hparams;
    const clap_host_gui_t* hgui;
    const clap_host_latency_t* hlat;
    const clap_host_audio_ports_t* hports;
    AEffect* fx;
    float* defaults;
    int midi_in, midi_out, synth, active, processing, gui_made, gui_open, gui_w, gui_h;
    int nin, nout, ports_in, ports_out;       // VST2 channels and CLAP ports (pairs of channels) known to the host
    uint32_t latency;
    double rate;
    uint32_t maxf;
    float** vin; float** vout; float* bufs;   // channel pointer arrays and the spare channel buffers
    VstTimeInfo ti;
    volatile vb_tid proc_tid, set_tid;        // thread inside process / thread that is setting parameters
    const clap_output_events_t* cur_out;
    uint32_t cur_frames, sysex_used;
    uint8_t sysex_out[VBC_SYSEX_OUT];
    vb_mutex qlock; QEv q[VBC_QUEUE]; int qn;
    volatile int32_t want_reset, want_rescan, io_changed;
#ifndef _WIN32
    clap_id timer;                            // Linux: tells Wine where the window is; macOS: notices a closed window
#endif
#ifdef VB_GUI_X11
    xcb_connection_t* xc;
    xcb_window_t xparent, xchild, xroot;
    int last_x, last_y;
#endif
    char error[256];
} Inst;

static __thread Inst* t_creating;            // the instance whose VSTPluginMain is running on this thread

// ----------------------------------------------------------------------------- helpers

static void log_to_file(const char* m)
{
#ifdef _WIN32
    wchar_t t[MAX_PATH]; char f[MAX_PATH * 3];
    DWORD n = GetTempPathW(MAX_PATH, t);
    if (!n || n >= MAX_PATH) wcscpy(t, L"C:\\Windows\\Temp\\");
    snprintf(f, sizeof f, "%lsvst2bridge.log", t);
    FILE* fp = fopen(f, "a");
    if (fp) { fprintf(fp, "%ls: %s\n", g_self_name, m); fclose(fp); }
#else
    const char* t = getenv("TMPDIR");
    char f[PATH_MAX];
    snprintf(f, sizeof f, "%s/vst2bridge.log", t && *t ? t : "/tmp");
    FILE* fp = fopen(f, "a");
    if (fp) { fprintf(fp, "%s.clap: %s\n", g_stem, m); fclose(fp); }
#endif
}

static void log_error(Inst* p, const char* m)
{
    log_to_file(m);
    if (p && p->host && p->host->get_extension) {
        const clap_host_log_t* l = (const clap_host_log_t*)p->host->get_extension(p->host, CLAP_EXT_LOG);
        if (l && l->log) l->log(p->host, CLAP_LOG_ERROR, m);
    }
}

static intptr_t disp(Inst* p, int32_t op, int32_t idx, intptr_t val, void* ptr, float opt)
{
    return p->fx ? p->fx->dispatcher(p->fx, op, idx, val, ptr, opt) : 0;
}

static void set_param(Inst* p, int32_t i, float v)
{
    if (!p->fx || i < 0 || i >= p->fx->numParams) return;
    vb_tid me = vb_self(), was = p->set_tid;
    p->set_tid = me;                          // the plugin's audioMasterAutomate for this change is not sent back
    p->fx->setParameter(p->fx, i, v);
    p->set_tid = was;
}

static int can_do(Inst* p, const char* what) { return disp(p, effCanDo, 0, 0, (void*)what, 0) > 0; }

static void queue_push(Inst* p, int kind, int32_t index, float value)
{
    vb_lock(&p->qlock);
    if (p->qn < VBC_QUEUE) { QEv* e = &p->q[p->qn++]; e->kind = kind; e->index = index; e->value = value; }
    vb_unlock(&p->qlock);
}

static void push_param_event(const clap_output_events_t* out, int kind, int32_t index, float value, uint32_t time)
{
    if (kind == 0) {
        clap_event_param_value_t e; memset(&e, 0, sizeof e);
        e.header.size = sizeof e; e.header.time = time; e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type = CLAP_EVENT_PARAM_VALUE;
        e.param_id = (clap_id)index; e.note_id = -1; e.port_index = -1; e.channel = -1; e.key = -1; e.value = value;
        out->try_push(out, &e.header);
    } else {
        clap_event_param_gesture_t e; memset(&e, 0, sizeof e);
        e.header.size = sizeof e; e.header.time = time; e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type = kind == 1 ? CLAP_EVENT_PARAM_GESTURE_BEGIN : CLAP_EVENT_PARAM_GESTURE_END;
        e.param_id = (clap_id)index;
        out->try_push(out, &e.header);
    }
}

static void queue_drain(Inst* p, const clap_output_events_t* out)
{
    QEv tmp[VBC_QUEUE]; int n;
    vb_lock(&p->qlock);
    n = p->qn; memcpy(tmp, p->q, n * sizeof *tmp); p->qn = 0;
    vb_unlock(&p->qlock);
    if (out) for (int i = 0; i < n; i++) push_param_event(out, tmp[i].kind, tmp[i].index, tmp[i].value, 0);
}

static void request_flush(Inst* p)
{
    if (p->hparams && p->hparams->request_flush) p->hparams->request_flush(p->host);
}

// MIDI out of the plugin: only during process (the bridge runs those callbacks on the thread that called process)
static void midi_out(Inst* p, const VstEvents* evs)
{
    const clap_output_events_t* out = p->cur_out;
    if (!out || !evs) return;
    for (int i = 0; i < evs->numEvents; i++) {
        const VstEvent* ve = evs->events[i];
        if (!ve) continue;
        uint32_t t = ve->deltaFrames < 0 ? 0 : (uint32_t)ve->deltaFrames;
        if (p->cur_frames && t >= p->cur_frames) t = p->cur_frames - 1;
        if (ve->type == kVstMidiType) {
            const VstMidiEvent* m = (const VstMidiEvent*)ve;
            clap_event_midi_t e; memset(&e, 0, sizeof e);
            e.header.size = sizeof e; e.header.time = t; e.header.space_id = CLAP_CORE_EVENT_SPACE_ID; e.header.type = CLAP_EVENT_MIDI;
            for (int k = 0; k < 3; k++) e.data[k] = (uint8_t)m->midiData[k];
            out->try_push(out, &e.header);
        } else if (ve->type == kVstSysExType) {
            const VstMidiSysexEvent* s = (const VstMidiSysexEvent*)ve;
            if (s->dumpBytes <= 0 || !s->sysexDump || p->sysex_used + (uint32_t)s->dumpBytes > VBC_SYSEX_OUT) continue;
            uint8_t* b = p->sysex_out + p->sysex_used;              // must stay valid until process returns
            memcpy(b, s->sysexDump, (size_t)s->dumpBytes); p->sysex_used += (uint32_t)s->dumpBytes;
            clap_event_midi_sysex_t e; memset(&e, 0, sizeof e);
            e.header.size = sizeof e; e.header.time = t; e.header.space_id = CLAP_CORE_EVENT_SPACE_ID; e.header.type = CLAP_EVENT_MIDI_SYSEX;
            e.buffer = b; e.size = (uint32_t)s->dumpBytes;
            out->try_push(out, &e.header);
        }
    }
}

// ----------------------------------------------------------------------------- the VST2 host side

static intptr_t VSTCALL master(AEffect* e, int32_t op, int32_t idx, intptr_t val, void* ptr, float opt)
{
    Inst* p = e && e->resvd1 ? (Inst*)e->resvd1 : t_creating;
    vb_tid me = vb_self();
    (void)val;
    switch (op) {
    case audioMasterVersion: return 2400;
    case audioMasterCurrentId: return 0;
    case audioMasterIdle: return 0;
    case audioMasterGetSampleRate: return p ? (intptr_t)(p->rate + 0.5) : 44100;
    case audioMasterGetBlockSize: return p ? (intptr_t)p->maxf : 512;
    case audioMasterGetCurrentProcessLevel: return p && p->proc_tid == me ? 2 : 1;
    case audioMasterGetAutomationState: return 1;
    case audioMasterWantMidi: return 1;
    case audioMasterGetTime: return p ? (intptr_t)&p->ti : 0;
    case audioMasterGetVendorString: if (ptr) strcpy((char*)ptr, "VST2Bridge"); return 1;
    case audioMasterGetProductString: if (ptr) strcpy((char*)ptr, "VST2Bridge CLAP"); return 1;
    case audioMasterGetVendorVersion: return 1;
    case audioMasterCanDo: {
        static const char* yes[] = { "sendVstEvents", "sendVstMidiEvent", "sendVstTimeInfo", "receiveVstEvents",
                                     "receiveVstMidiEvent", "sizeWindow", "supplyIdle", 0 };
        if (!ptr) return 0;
        for (int i = 0; yes[i]; i++) if (!strcmp((const char*)ptr, yes[i])) return 1;
        return 0;
    }
    case audioMasterProcessEvents:
        if (p && p->proc_tid == me) midi_out(p, (const VstEvents*)ptr);
        return 1;
    case audioMasterAutomate:
    case audioMasterBeginEdit:
    case audioMasterEndEdit:
        if (!p || !p->fx || p->set_tid == me) return 1;     // our own change coming back
        if (idx < 0 || idx >= p->fx->numParams) return 0;
        {
            int kind = op == audioMasterAutomate ? 0 : op == audioMasterBeginEdit ? 1 : 2;
            if (p->proc_tid == me && p->cur_out) push_param_event(p->cur_out, kind, idx, opt, 0);
            else { queue_push(p, kind, idx, opt); if (p->proc_tid != me) request_flush(p); }
        }
        return 1;
    case audioMasterSizeWindow:
        if (!p) return 0;
        p->gui_w = idx; p->gui_h = (int)val;
#ifndef VB_GUI_FLOATING                       // a floating window is sized by the helper itself
        if (p->gui_open && p->hgui && p->hgui->request_resize) p->hgui->request_resize(p->host, (uint32_t)idx, (uint32_t)val);
#endif
        return 1;
    case audioMasterIOChanged:
        if (p && p->host) { vb_xchg(&p->io_changed, 1); p->host->request_callback(p->host); }
        return 1;
    case audioMasterUpdateDisplay:
        if (p && p->host) { vb_xchg(&p->want_rescan, 1); p->host->request_callback(p->host); }
        return 1;
    default: return 0;
    }
}


static Inst* inst_alloc(void)
{
    Inst* p = (Inst*)calloc(1, sizeof *p);
    if (!p) return 0;
    vb_mutex_init(&p->qlock);
#ifndef _WIN32
    p->timer = CLAP_INVALID_ID;
#endif
    p->rate = 44100; p->maxf = 512;
    p->ti.sampleRate = 44100; p->ti.tempo = 120; p->ti.timeSigNumerator = 4; p->ti.timeSigDenominator = 4;
    p->ti.flags = (1 << 10) | (1 << 13);     // kVstTempoValid | kVstTimeSigValid
    return p;
}

// creates the VST2 plugin (through the bridge, which starts the helper)
static int inst_load(Inst* p)
{
#ifdef _WIN32
    t_creating = p;
    AEffect* fx = vb_stub_main(master, g_dir, g_stem, g_self_name);
    t_creating = 0;
    if (!fx || fx->magic != kEffectMagic) { snprintf(p->error, sizeof p->error, "the plugin could not be started (see the line above in this log)"); return 0; }
#else
    t_creating = p;
    AEffect* fx = vb_stub_main(master, g_dir, g_stem);
    t_creating = 0;
    if (!fx) { snprintf(p->error, sizeof p->error, "the plugin could not be started (see the line above in this log)"); return 0; }
#endif
    fx->resvd1 = (intptr_t)p;                 // resvd1 belongs to the host: the way back from the callback to us
    p->fx = fx;
    disp(p, effOpen, 0, 0, 0, 0);
    // MIDI in is always offered: VST2 hosts send MIDI to every plugin, and many plugins answer no effCanDo at all.
    // Instrument: flagged as synth, category synth, no audio inputs, or it says it receives MIDI (old instruments
    // such as S-YXG50 set no flag; CLAP hosts like Woof! only offer instruments).
    p->midi_in = 1;
    p->synth = (fx->flags & effFlagsIsSynth) || disp(p, effGetPlugCategory, 0, 0, 0, 0) == 2 /* kPlugCategSynth */
               || (fx->numInputs == 0 && fx->numOutputs > 0) || can_do(p, "receiveVstEvents") || can_do(p, "receiveVstMidiEvent");
    p->midi_out = can_do(p, "sendVstEvents") || can_do(p, "sendVstMidiEvent");
    if (fx->numParams > 0) {
        p->defaults = (float*)calloc((size_t)fx->numParams, sizeof(float));
        if (p->defaults) for (int i = 0; i < fx->numParams; i++) p->defaults[i] = fx->getParameter(fx, i);
    }
    p->nin = fx->numInputs; p->nout = fx->numOutputs;
    p->ports_in = (p->nin + 1) / 2; p->ports_out = (p->nout + 1) / 2;
    return 1;
}

static void gui_release(Inst* p);

static void inst_free(Inst* p)
{
    gui_release(p);
    if (p->fx) {
        if (p->gui_open) disp(p, effEditClose, 0, 0, 0, 0);
        if (p->active) disp(p, effMainsChanged, 0, 0, 0, 0);
        disp(p, effClose, 0, 0, 0, 0);
    }
    free(p->defaults); free(p->vin); free(p->vout); free(p->bufs);
    vb_mutex_free(&p->qlock);
    free(p);
}

// ----------------------------------------------------------------------------- descriptor (from the plugin itself)

static Inst* g_spare;                        // the instance made for the descriptor, handed to the first create_plugin

static void copy_str(char* dst, size_t n, const char* src) { snprintf(dst, n, "%s", src); }

static void probe(void)
{
    glock();
    if (g_probed) { gunlock(); return; }
    g_probed = 1;
    gunlock();

    Inst* p = inst_alloc();
    if (!p) return;
    if (!inst_load(p)) { log_error(0, p->error); inst_free(p); return; }
    char s[256];
    memset(s, 0, sizeof s); disp(p, effGetEffectName, 0, 0, s, 0);
    if (!s[0]) { memset(s, 0, sizeof s); disp(p, effGetProductString, 0, 0, s, 0); }
    copy_str(g_name, sizeof g_name, s[0] ? s : g_stem8);
    memset(s, 0, sizeof s); disp(p, effGetVendorString, 0, 0, s, 0);
    copy_str(g_vendor, sizeof g_vendor, s[0] ? s : "VST2Bridge");
    int v = (int)disp(p, effGetVendorVersion, 0, 0, 0, 0);
    if (v > 0) snprintf(g_version, sizeof g_version, "%d", v); else copy_str(g_version, sizeof g_version, "1.0");
#ifdef _WIN32
    snprintf(g_descr, sizeof g_descr, "%s (VST2 plugin of the other bitness, bridged by VST2Bridge)", g_name);
#else
    snprintf(g_descr, sizeof g_descr, "%s (Windows VST2 plugin, run under Wine by VST2Bridge)", g_name);
#endif
    int n = 0;
    if (p->synth) { g_features[n++] = CLAP_PLUGIN_FEATURE_INSTRUMENT; g_features[n++] = CLAP_PLUGIN_FEATURE_SYNTHESIZER; }
    else g_features[n++] = CLAP_PLUGIN_FEATURE_AUDIO_EFFECT;
    if (p->nout >= 2) g_features[n++] = CLAP_PLUGIN_FEATURE_STEREO;           // the main output port is stereo
    else if (p->nout == 1) g_features[n++] = CLAP_PLUGIN_FEATURE_MONO;
    g_features[n] = 0;
    memset(&g_desc, 0, sizeof g_desc);
    g_desc.clap_version = (clap_version_t)CLAP_VERSION_INIT;
    g_desc.id = g_id; g_desc.name = g_name; g_desc.vendor = g_vendor;
    g_desc.url = "https://github.com/NightFright2k19/VST2Bridge";
    g_desc.manual_url = g_desc.url; g_desc.support_url = g_desc.url;
    g_desc.version = g_version; g_desc.description = g_descr; g_desc.features = g_features;
    glock();
    g_spare = p; g_ok = 1;
    gunlock();
}

// ----------------------------------------------------------------------------- plugin

#define INST(c) ((Inst*)(c)->plugin_data)

static bool pl_init(const clap_plugin_t* c)
{
    Inst* p = INST(c);
    const clap_host_t* h = p->host;
    if (h->get_extension) {
        p->hparams = (const clap_host_params_t*)h->get_extension(h, CLAP_EXT_PARAMS);
        p->hgui = (const clap_host_gui_t*)h->get_extension(h, CLAP_EXT_GUI);
        p->hlat = (const clap_host_latency_t*)h->get_extension(h, CLAP_EXT_LATENCY);
        p->hports = (const clap_host_audio_ports_t*)h->get_extension(h, CLAP_EXT_AUDIO_PORTS);
    }
    if (!p->fx && !inst_load(p)) { log_error(p, p->error); return false; }
    return true;
}

static void pl_destroy(const clap_plugin_t* c) { inst_free(INST(c)); }

static bool pl_activate(const clap_plugin_t* c, double sr, uint32_t minf, uint32_t maxf)
{
    (void)minf;
    Inst* p = INST(c);
    if (!p->fx) return false;
    if (maxf == 0) maxf = 512;
    p->rate = sr; p->maxf = maxf; p->ti.sampleRate = sr; p->ti.samplePos = 0;
    disp(p, effSetSampleRate, 0, 0, 0, (float)sr);
    disp(p, effSetBlockSize, 0, (intptr_t)maxf, 0, 0);
    disp(p, effMainsChanged, 0, 1, 0, 0);
    // channels as the host knows them (the ports); the plugin gets at least as many as it has now
    int nin = p->fx->numInputs > p->ports_in * 2 ? p->fx->numInputs : p->ports_in * 2;
    int nout = p->fx->numOutputs > p->ports_out * 2 ? p->fx->numOutputs : p->ports_out * 2;
    free(p->vin); free(p->vout); free(p->bufs);
    p->vin = (float**)calloc((size_t)nin + 1, sizeof(float*));
    p->vout = (float**)calloc((size_t)nout + 1, sizeof(float*));
    p->bufs = (float*)calloc((size_t)(nin + nout) * maxf + 1, sizeof(float));
    if (!p->vin || !p->vout || !p->bufs) { disp(p, effMainsChanged, 0, 0, 0, 0); return false; }
    p->nin = nin; p->nout = nout;
    uint32_t lat = p->fx->initialDelay > 0 ? (uint32_t)p->fx->initialDelay : 0;
    if (lat != p->latency) { p->latency = lat; if (p->hlat && p->hlat->changed) p->hlat->changed(p->host); }
    vb_xchg(&p->io_changed, 0);
    p->active = 1;
    return true;
}

static void pl_deactivate(const clap_plugin_t* c)
{
    Inst* p = INST(c);
    if (!p->active) return;
    disp(p, effMainsChanged, 0, 0, 0, 0);
    p->active = 0;
}

static bool pl_start(const clap_plugin_t* c) { Inst* p = INST(c); disp(p, effStartProcess, 0, 0, 0, 0); p->processing = 1; return true; }
static void pl_stop(const clap_plugin_t* c) { Inst* p = INST(c); disp(p, effStopProcess, 0, 0, 0, 0); p->processing = 0; }
static void pl_reset(const clap_plugin_t* c) { vb_xchg(&INST(c)->want_reset, 1); }

static void set_transport(Inst* p, const clap_event_transport_t* t)
{
    VstTimeInfo* ti = &p->ti;
    int32_t fl = (1 << 10) | (1 << 13) | (1 << 9) | (1 << 11);      // tempo, time signature, ppq, bars valid
    if (t->flags & CLAP_TRANSPORT_HAS_TEMPO) ti->tempo = t->tempo;
    if (t->flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE) { ti->timeSigNumerator = t->tsig_num; ti->timeSigDenominator = t->tsig_denom; }
    if (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) {
        ti->ppqPos = (double)t->song_pos_beats / (double)CLAP_BEATTIME_FACTOR;
        ti->barStartPos = (double)t->bar_start / (double)CLAP_BEATTIME_FACTOR;
        ti->cycleStartPos = (double)t->loop_start_beats / (double)CLAP_BEATTIME_FACTOR;
        ti->cycleEndPos = (double)t->loop_end_beats / (double)CLAP_BEATTIME_FACTOR;
        fl |= 1 << 12;                                              // kVstCyclePosValid
    }
    if (t->flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE)
        ti->samplePos = (double)t->song_pos_seconds / (double)CLAP_SECTIME_FACTOR * p->rate;
    if (t->flags & CLAP_TRANSPORT_IS_PLAYING) fl |= 1 << 1;         // kVstTransportPlaying
    if (t->flags & CLAP_TRANSPORT_IS_RECORDING) fl |= 1 << 3;
    if (t->flags & CLAP_TRANSPORT_IS_LOOP_ACTIVE) fl |= 1 << 2;
    ti->flags = fl;
}

typedef struct { int32_t numEvents; intptr_t reserved; VstEvent* events[VBC_MAXEV]; } EvList;

static void add_midi(VstMidiEvent* mev, EvList* l, uint32_t t, uint8_t a, uint8_t b, uint8_t c3)
{
    if (l->numEvents >= VBC_MAXEV) return;
    VstMidiEvent* m = &mev[l->numEvents];
    memset(m, 0, sizeof *m);
    m->type = kVstMidiType; m->byteSize = sizeof *m; m->deltaFrames = (int32_t)t; m->flags = 1;    // kVstMidiEventIsRealtime
    m->midiData[0] = (char)a; m->midiData[1] = (char)b; m->midiData[2] = (char)c3;
    l->events[l->numEvents++] = (VstEvent*)m;
}

static void param_events(Inst* p, const clap_input_events_t* in)
{
    uint32_t n = in ? in->size(in) : 0;
    for (uint32_t i = 0; i < n; i++) {
        const clap_event_header_t* h = in->get(in, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) {
            const clap_event_param_value_t* v = (const clap_event_param_value_t*)h;
            set_param(p, (int32_t)v->param_id, (float)v->value);
        }
    }
}

static clap_process_status pl_process(const clap_plugin_t* c, const clap_process_t* pr)
{
    Inst* p = INST(c);
    uint32_t frames = pr->frames_count;
    if (frames > p->maxf) frames = p->maxf;
    if (!p->fx || !p->active || !frames) {
        for (uint32_t o = 0; o < pr->audio_outputs_count; o++)
            for (uint32_t k = 0; k < pr->audio_outputs[o].channel_count; k++)
                if (pr->audio_outputs[o].data32) memset(pr->audio_outputs[o].data32[k], 0, pr->frames_count * sizeof(float));
        return CLAP_PROCESS_CONTINUE;
    }
    static __thread VstMidiEvent mev[VBC_MAXEV];
    static __thread VstMidiSysexEvent sev[VBC_MAXEV];
    static __thread EvList list;
    p->proc_tid = vb_self(); p->cur_out = pr->out_events; p->cur_frames = frames; p->sysex_used = 0;
    if (pr->transport) set_transport(p, pr->transport);

    list.numEvents = 0;
    if (vb_xchg(&p->want_reset, 0))                    // all notes off, reset controllers
        for (int ch = 0; ch < 16; ch++) { add_midi(mev, &list, 0, 0xB0 | ch, 123, 0); add_midi(mev, &list, 0, 0xB0 | ch, 121, 0); }
    uint32_t ne = pr->in_events ? pr->in_events->size(pr->in_events) : 0;
    for (uint32_t i = 0; i < ne; i++) {
        const clap_event_header_t* h = pr->in_events->get(pr->in_events, i);
        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
        uint32_t t = h->time < frames ? h->time : frames - 1;
        switch (h->type) {
        case CLAP_EVENT_PARAM_VALUE: {                              // at the start of the block
            const clap_event_param_value_t* v = (const clap_event_param_value_t*)h;
            set_param(p, (int32_t)v->param_id, (float)v->value);
            break;
        }
        case CLAP_EVENT_MIDI: {
            const clap_event_midi_t* m = (const clap_event_midi_t*)h;
            add_midi(mev, &list, t, m->data[0], m->data[1], m->data[2]);
            break;
        }
        case CLAP_EVENT_MIDI_SYSEX: {
            const clap_event_midi_sysex_t* m = (const clap_event_midi_sysex_t*)h;
            if (!m->buffer || !m->size || list.numEvents >= VBC_MAXEV) break;
            VstMidiSysexEvent* s = &sev[list.numEvents];
            memset(s, 0, sizeof *s);
            s->type = kVstSysExType; s->byteSize = sizeof *s; s->deltaFrames = (int32_t)t;
            s->dumpBytes = (int32_t)m->size; s->sysexDump = (char*)m->buffer;
            list.events[list.numEvents++] = (VstEvent*)s;
            break;
        }
        case CLAP_EVENT_NOTE_ON: case CLAP_EVENT_NOTE_OFF: case CLAP_EVENT_NOTE_CHOKE: {
            const clap_event_note_t* n = (const clap_event_note_t*)h;
            int ch = n->channel < 0 ? 0 : n->channel & 15, key = n->key < 0 ? 60 : n->key & 127;
            if (h->type == CLAP_EVENT_NOTE_ON) {
                int v = (int)(n->velocity * 127.0 + 0.5);
                add_midi(mev, &list, t, 0x90 | ch, key, v < 1 ? 1 : v > 127 ? 127 : v);
            } else add_midi(mev, &list, t, 0x80 | ch, key, (int)(n->velocity * 127.0 + 0.5) & 127);
            break;
        }
        default: break;
        }
    }
    if (list.numEvents) disp(p, effProcessEvents, 0, 0, &list, 0);

    // channels: CLAP port k = VST2 channels 2k, 2k+1; channels the host did not give use the spare buffers
    float* spare = p->bufs;
    for (int i = 0; i < p->nin; i++) {
        uint32_t port = (uint32_t)i / 2, ch = (uint32_t)i % 2;
        float* b = 0;
        if (port < pr->audio_inputs_count && ch < pr->audio_inputs[port].channel_count && pr->audio_inputs[port].data32)
            b = pr->audio_inputs[port].data32[ch];
        if (!b) { b = spare + (size_t)i * p->maxf; memset(b, 0, frames * sizeof(float)); }
        p->vin[i] = b;
    }
    spare += (size_t)p->nin * p->maxf;
    for (int i = 0; i < p->nout; i++) {
        uint32_t port = (uint32_t)i / 2, ch = (uint32_t)i % 2;
        float* b = 0;
        if (port < pr->audio_outputs_count && ch < pr->audio_outputs[port].channel_count && pr->audio_outputs[port].data32)
            b = pr->audio_outputs[port].data32[ch];
        p->vout[i] = b ? b : spare + (size_t)i * p->maxf;
    }
    p->fx->processReplacing(p->fx, p->vin, p->vout, (int32_t)frames);   // the bridge always has processReplacing
    // host channels the plugin does not have stay silent
    for (uint32_t o = 0; o < pr->audio_outputs_count; o++)
        for (uint32_t k = 0; k < pr->audio_outputs[o].channel_count; k++)
            if (o * 2 + k >= (uint32_t)p->nout && pr->audio_outputs[o].data32) memset(pr->audio_outputs[o].data32[k], 0, pr->frames_count * sizeof(float));
    queue_drain(p, pr->out_events);
    p->ti.samplePos += frames;
    p->cur_out = 0; p->proc_tid = 0;
    return CLAP_PROCESS_CONTINUE;
}

static void pl_on_main(const clap_plugin_t* c)
{
    Inst* p = INST(c);
    if (!p->fx) return;
    if (vb_xchg(&p->want_rescan, 0) && p->hparams && p->hparams->rescan)
        p->hparams->rescan(p->host, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
    if (vb_xchg(&p->io_changed, 0)) {
        int pin = (p->fx->numInputs + 1) / 2, pout = (p->fx->numOutputs + 1) / 2;
        uint32_t lat = p->fx->initialDelay > 0 ? (uint32_t)p->fx->initialDelay : 0;
        if (p->active) { if (lat != p->latency || pin > p->ports_in || pout > p->ports_out) p->host->request_restart(p->host); }
        else if ((pin != p->ports_in || pout != p->ports_out) && p->hports && p->hports->rescan
                 && (!p->hports->is_rescan_flag_supported || p->hports->is_rescan_flag_supported(p->host, CLAP_AUDIO_PORTS_RESCAN_LIST))) {
            p->ports_in = pin; p->ports_out = pout;
            p->hports->rescan(p->host, CLAP_AUDIO_PORTS_RESCAN_LIST);
        }
    }
}

// ----------------------------------------------------------------------------- audio and note ports

static uint32_t ap_count(const clap_plugin_t* c, bool is_input)
{
    Inst* p = INST(c);
    if (!p->active && p->fx) { p->ports_in = (p->fx->numInputs + 1) / 2; p->ports_out = (p->fx->numOutputs + 1) / 2; }
    return (uint32_t)(is_input ? p->ports_in : p->ports_out);
}
static bool ap_get(const clap_plugin_t* c, uint32_t i, bool is_input, clap_audio_port_info_t* info)
{
    Inst* p = INST(c);
    int ports = is_input ? p->ports_in : p->ports_out;
    int nch = p->fx ? (is_input ? p->fx->numInputs : p->fx->numOutputs) : 0;
    if ((int)i >= ports) return false;
    int left = nch - (int)i * 2;
    memset(info, 0, sizeof *info);
    info->id = i;
    info->channel_count = left >= 2 || left <= 0 ? 2 : 1;
    if (i == 0) { snprintf(info->name, sizeof info->name, is_input ? "Input" : "Output"); info->flags = CLAP_AUDIO_PORT_IS_MAIN; }
    else if (info->channel_count == 2) snprintf(info->name, sizeof info->name, "%s %u-%u", is_input ? "Input" : "Output", i * 2 + 1, i * 2 + 2);
    else snprintf(info->name, sizeof info->name, "%s %u", is_input ? "Input" : "Output", i * 2 + 1);
    info->port_type = info->channel_count == 2 ? CLAP_PORT_STEREO : CLAP_PORT_MONO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
static const clap_plugin_audio_ports_t ext_audio = { ap_count, ap_get };

static uint32_t np_count(const clap_plugin_t* c, bool is_input) { Inst* p = INST(c); return (is_input ? p->midi_in : p->midi_out) ? 1 : 0; }
static bool np_get(const clap_plugin_t* c, uint32_t i, bool is_input, clap_note_port_info_t* info)
{
    if (i || !np_count(c, is_input)) return false;
    memset(info, 0, sizeof *info);
    info->id = 0; snprintf(info->name, sizeof info->name, is_input ? "MIDI in" : "MIDI out");
    info->supported_dialects = is_input ? CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_CLAP : CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
    return true;
}
static const clap_plugin_note_ports_t ext_notes = { np_count, np_get };

// ----------------------------------------------------------------------------- parameters

static uint32_t pa_count(const clap_plugin_t* c) { Inst* p = INST(c); return p->fx && p->fx->numParams > 0 ? (uint32_t)p->fx->numParams : 0; }
static bool pa_info(const clap_plugin_t* c, uint32_t i, clap_param_info_t* info)
{
    Inst* p = INST(c);
    if (i >= pa_count(c)) return false;
    memset(info, 0, sizeof *info);
    info->id = i; info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    char s[256]; memset(s, 0, sizeof s);
    disp(p, effGetParamName, (int32_t)i, 0, s, 0);
    if (!s[0]) snprintf(s, sizeof s, "Parameter %u", i + 1);
    snprintf(info->name, sizeof info->name, "%s", s);
    info->min_value = 0; info->max_value = 1;
    info->default_value = p->defaults ? p->defaults[i] : 0;
    if (info->default_value < 0 || info->default_value > 1) info->default_value = 0;
    return true;
}
static bool pa_value(const clap_plugin_t* c, clap_id id, double* v)
{
    Inst* p = INST(c);
    if (id >= pa_count(c)) return false;
    *v = p->fx->getParameter(p->fx, (int32_t)id);
    return true;
}
static bool pa_to_text(const clap_plugin_t* c, clap_id id, double v, char* out, uint32_t n)
{
    Inst* p = INST(c);
    if (id >= pa_count(c) || !n) return false;
    // VST2 can only show the current value: other values (automation lanes ...) are shown as a number
    if (fabs(v - p->fx->getParameter(p->fx, (int32_t)id)) < 1e-6) {
        char d[256], l[256]; memset(d, 0, sizeof d); memset(l, 0, sizeof l);
        disp(p, effGetParamDisplay, (int32_t)id, 0, d, 0);
        disp(p, effGetParamLabel, (int32_t)id, 0, l, 0);
        if (d[0]) { snprintf(out, n, l[0] ? "%s %s" : "%s", d, l); return true; }
    }
    snprintf(out, n, "%.3f", v);
    return true;
}
static bool pa_from_text(const clap_plugin_t* c, clap_id id, const char* text, double* v)
{
    if (id >= pa_count(c) || !text) return false;
    char* end; double d = strtod(text, &end);
    if (end == text || d < 0 || d > 1) return false;
    *v = d; return true;
}
static void pa_flush(const clap_plugin_t* c, const clap_input_events_t* in, const clap_output_events_t* out)
{
    Inst* p = INST(c);
    param_events(p, in);
    queue_drain(p, out);
}
static const clap_plugin_params_t ext_params = { pa_count, pa_info, pa_value, pa_to_text, pa_from_text, pa_flush };

// ----------------------------------------------------------------------------- state

// 'VBCL', version 1, kind (1 = the plugin's bank chunk, 0 = all parameter values as floats), program, length, data
static int write_all(const clap_ostream_t* s, const void* d, uint64_t n)
{
    const char* b = (const char*)d;
    while (n) { int64_t w = s->write(s, b, n); if (w <= 0) return 0; b += w; n -= (uint64_t)w; }
    return 1;
}
static int read_all(const clap_istream_t* s, void* d, uint64_t n)
{
    char* b = (char*)d;
    while (n) { int64_t r = s->read(s, b, n); if (r <= 0) return 0; b += r; n -= (uint64_t)r; }
    return 1;
}
static bool st_save(const clap_plugin_t* c, const clap_ostream_t* s)
{
    Inst* p = INST(c);
    if (!p->fx) return false;
    uint32_t hdr[5] = { VBC_MAGIC, 1, 0, (uint32_t)disp(p, effGetProgram, 0, 0, 0, 0), 0 };
    if (p->fx->flags & effFlagsProgramChunks) {
        void* data = 0;
        intptr_t n = disp(p, effGetChunk, 0, 0, &data, 0);
        if (n < 0 || (n > 0 && !data)) return false;
        hdr[2] = 1; hdr[4] = (uint32_t)n;
        return write_all(s, hdr, sizeof hdr) && write_all(s, data, (uint64_t)n);
    }
    uint32_t np = pa_count(c);
    hdr[4] = np * 4;
    if (!write_all(s, hdr, sizeof hdr)) return false;
    for (uint32_t i = 0; i < np; i++) { float v = p->fx->getParameter(p->fx, (int32_t)i); if (!write_all(s, &v, 4)) return false; }
    return true;
}
static bool st_load(const clap_plugin_t* c, const clap_istream_t* s)
{
    Inst* p = INST(c);
    uint32_t hdr[5];
    if (!p->fx || !read_all(s, hdr, sizeof hdr) || hdr[0] != VBC_MAGIC || hdr[1] != 1 || hdr[4] > (1u << 30)) return false;
    uint8_t* data = (uint8_t*)malloc(hdr[4] + 1);
    if (!data) return false;
    if (!read_all(s, data, hdr[4])) { free(data); return false; }
    vb_tid was = p->set_tid; p->set_tid = vb_self();     // changes made by loading are not sent back
    if (hdr[2] == 1) {
        disp(p, effBeginSetProgram, 0, 0, 0, 0);
        disp(p, effSetChunk, 0, (intptr_t)hdr[4], data, 0);
        disp(p, effEndSetProgram, 0, 0, 0, 0);
        if (p->fx->numPrograms > 1 && (int32_t)hdr[3] < p->fx->numPrograms) disp(p, effSetProgram, 0, (intptr_t)hdr[3], 0, 0);
    } else {
        if (p->fx->numPrograms > 1 && (int32_t)hdr[3] < p->fx->numPrograms) disp(p, effSetProgram, 0, (intptr_t)hdr[3], 0, 0);
        uint32_t np = hdr[4] / 4;
        for (uint32_t i = 0; i < np && i < pa_count(c); i++) { float v; memcpy(&v, data + i * 4, 4); set_param(p, (int32_t)i, v); }
    }
    p->set_tid = was;
    free(data);
    if (p->hparams && p->hparams->rescan) p->hparams->rescan(p->host, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
    return true;
}
static const clap_plugin_state_t ext_state = { st_save, st_load };

// ----------------------------------------------------------------------------- latency

static uint32_t lat_get(const clap_plugin_t* c) { return INST(c)->latency; }
static const clap_plugin_latency_t ext_latency = { lat_get };

// ----------------------------------------------------------------------------- editor

static void read_rect(Inst* p)
{
    ERect* r = 0;
    disp(p, effEditGetRect, 0, 0, &r, 0);
    if (r && r->right > r->left && r->bottom > r->top) { p->gui_w = r->right - r->left; p->gui_h = r->bottom - r->top; }
}

#ifdef VB_GUI_WIN32
// embedded Win32 window
static void gui_release(Inst* p) { (void)p; }
static bool gui_supported(const clap_plugin_t* c, const char* api, bool floating)
{
    Inst* p = INST(c);
    return !floating && api && !strcmp(api, CLAP_WINDOW_API_WIN32) && p->fx && (p->fx->flags & effFlagsHasEditor);
}
static bool gui_preferred(const clap_plugin_t* c, const char** api, bool* floating)
{
    if (!gui_supported(c, CLAP_WINDOW_API_WIN32, false)) return false;
    *api = CLAP_WINDOW_API_WIN32; *floating = false; return true;
}
static bool gui_create(const clap_plugin_t* c, const char* api, bool floating)
{
    Inst* p = INST(c);
    if (!gui_supported(c, api, floating)) return false;
    read_rect(p);
    if (p->gui_w <= 0 || p->gui_h <= 0) { p->gui_w = 640; p->gui_h = 480; }
    p->gui_made = 1; return true;
}
static void gui_destroy(const clap_plugin_t* c)
{
    Inst* p = INST(c);
    if (p->gui_open) disp(p, effEditClose, 0, 0, 0, 0);
    p->gui_open = 0; p->gui_made = 0;
}
static bool gui_scale(const clap_plugin_t* c, double s) { (void)c; (void)s; return false; }
static bool gui_size(const clap_plugin_t* c, uint32_t* w, uint32_t* h)
{
    Inst* p = INST(c);
    if (!p->gui_made) return false;
    *w = (uint32_t)p->gui_w; *h = (uint32_t)p->gui_h; return true;
}
static bool gui_can_resize(const clap_plugin_t* c) { (void)c; return false; }
static bool gui_hints(const clap_plugin_t* c, clap_gui_resize_hints_t* h) { (void)c; (void)h; return false; }
static bool gui_adjust(const clap_plugin_t* c, uint32_t* w, uint32_t* h) { return gui_size(c, w, h); }
static bool gui_set_size(const clap_plugin_t* c, uint32_t w, uint32_t h) { Inst* p = INST(c); return (int)w == p->gui_w && (int)h == p->gui_h; }
static bool gui_set_parent(const clap_plugin_t* c, const clap_window_t* win)
{
    Inst* p = INST(c);
    if (!p->gui_made || !win || !win->win32 || p->gui_open) return false;
    int w0 = p->gui_w, h0 = p->gui_h;
    p->gui_open = 1;                          // before the call: a sizeWindow inside effEditOpen goes to the host
    if (!disp(p, effEditOpen, 0, 0, win->win32, 0)) {
        // many plugins return 0 although the editor is open: trust the child window instead
        if (!GetWindow((HWND)win->win32, GW_CHILD)) { p->gui_open = 0; return false; }
    }
    read_rect(p);
    if ((p->gui_w != w0 || p->gui_h != h0) && p->hgui && p->hgui->request_resize)
        p->hgui->request_resize(p->host, (uint32_t)p->gui_w, (uint32_t)p->gui_h);
    return true;
}
static bool gui_transient(const clap_plugin_t* c, const clap_window_t* w) { (void)c; (void)w; return false; }
static void gui_title(const clap_plugin_t* c, const char* t) { (void)c; (void)t; }
static bool gui_show(const clap_plugin_t* c) { return INST(c)->gui_open; }
static bool gui_hide(const clap_plugin_t* c) { return INST(c)->gui_open; }
static const clap_plugin_gui_t ext_gui = {
    gui_supported, gui_preferred, gui_create, gui_destroy, gui_scale, gui_size, gui_can_resize, gui_hints,
    gui_adjust, gui_set_size, gui_set_parent, gui_transient, gui_title, gui_show, gui_hide
};

#else
// Linux / macOS: the editor is a window of the helper (Wine)

static const clap_host_timer_support_t* host_timer(Inst* p)
{
    return p->host && p->host->get_extension ? (const clap_host_timer_support_t*)p->host->get_extension(p->host, CLAP_EXT_TIMER_SUPPORT) : 0;
}
static void timer_start(Inst* p, uint32_t ms)
{
    const clap_host_timer_support_t* ts = host_timer(p);
    if (p->timer == CLAP_INVALID_ID && ts && ts->register_timer) ts->register_timer(p->host, ms, &p->timer);
}
static void timer_stop(Inst* p)
{
    const clap_host_timer_support_t* ts = p->timer != CLAP_INVALID_ID ? host_timer(p) : 0;
    if (ts && ts->unregister_timer) ts->unregister_timer(p->host, p->timer);
    p->timer = CLAP_INVALID_ID;
}
#ifdef VB_GUI_FLOATING
#define GUI_API      CLAP_WINDOW_API_COCOA
#define GUI_FLOATING true
#else
#define GUI_API      CLAP_WINDOW_API_X11
#define GUI_FLOATING false
#endif

static bool gui_supported(const clap_plugin_t* c, const char* api, bool floating)
{
    Inst* p = INST(c);
    return floating == GUI_FLOATING && api && !strcmp(api, GUI_API) && p->fx && (p->fx->flags & effFlagsHasEditor);
}
static bool gui_preferred(const clap_plugin_t* c, const char** api, bool* floating)
{
    if (!gui_supported(c, GUI_API, GUI_FLOATING)) return false;
    *api = GUI_API; *floating = GUI_FLOATING; return true;
}
static bool gui_create(const clap_plugin_t* c, const char* api, bool floating)
{
    Inst* p = INST(c);
    if (!gui_supported(c, api, floating)) return false;
    read_rect(p);
    if (p->gui_w <= 0 || p->gui_h <= 0) { p->gui_w = 640; p->gui_h = 480; }
    p->gui_made = 1; return true;
}
static void gui_destroy(const clap_plugin_t* c)
{
    Inst* p = INST(c);
    gui_release(p);
    if (p->gui_open) disp(p, effEditClose, 0, 0, 0, 0);
    p->gui_open = 0; p->gui_made = 0;
}
static bool gui_scale(const clap_plugin_t* c, double s) { (void)c; (void)s; return false; }
static bool gui_size(const clap_plugin_t* c, uint32_t* w, uint32_t* h)
{
    Inst* p = INST(c);
    if (!p->gui_made) return false;
    *w = (uint32_t)p->gui_w; *h = (uint32_t)p->gui_h; return true;
}
static bool gui_can_resize(const clap_plugin_t* c) { (void)c; return false; }
static bool gui_hints(const clap_plugin_t* c, clap_gui_resize_hints_t* h) { (void)c; (void)h; return false; }
static bool gui_adjust(const clap_plugin_t* c, uint32_t* w, uint32_t* h) { return gui_size(c, w, h); }
static bool gui_set_size(const clap_plugin_t* c, uint32_t w, uint32_t h) { Inst* p = INST(c); return (int)w == p->gui_w && (int)h == p->gui_h; }
static bool gui_transient(const clap_plugin_t* c, const clap_window_t* w) { (void)c; (void)w; return false; }
static void gui_title(const clap_plugin_t* c, const char* t) { (void)c; (void)t; }

#ifdef VB_GUI_FLOATING
// macOS: show opens the plugin's window (with a title bar, always on top) or shows it again, hide hides it; closing it
// by hand only hides it, and the timer tells the host
static void gui_release(Inst* p) { timer_stop(p); }
static bool gui_set_parent(const clap_plugin_t* c, const clap_window_t* win) { (void)c; (void)win; return false; }
static bool gui_show(const clap_plugin_t* c)
{
    Inst* p = INST(c);
    if (!p->gui_made || !p->fx) return false;
    if (p->gui_open) vb_stub_edit_show(p->fx, 1);
    else {
        p->gui_open = 1;
        if (!vb_stub_edit_open(p->fx, VB_F_FLOATING)) { p->gui_open = 0; return false; }
        read_rect(p);
    }
    timer_start(p, 250);
    return true;
}
static bool gui_hide(const clap_plugin_t* c)
{
    Inst* p = INST(c);
    if (!p->gui_open) return false;
    vb_stub_edit_show(p->fx, 0);
    return true;
}
static void on_timer(const clap_plugin_t* c, clap_id id)
{
    Inst* p = INST(c);
    if (id != p->timer || !p->gui_open || !p->fx || !vb_stub_edit_closed(p->fx)) return;
    const clap_host_gui_t* hg = p->hgui;
    if (hg && hg->closed) hg->closed(p->host, false);
}
#else
// Linux: the helper opens the editor in a bare window titled "VST2Bridge <token>"; it is found by that title and
// moved into the host's X11 window (XReparentWindow), as other Wine bridges do
static xcb_atom_t atom(xcb_connection_t* c, const char* name)
{
    xcb_intern_atom_reply_t* r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, (uint16_t)strlen(name), name), 0);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE; free(r); return a;
}
static int has_title(xcb_connection_t* c, xcb_window_t w, xcb_atom_t prop, xcb_atom_t type, const char* title)
{
    xcb_get_property_reply_t* r = xcb_get_property_reply(c, xcb_get_property(c, 0, w, prop, type, 0, 64), 0);
    int hit = 0;
    size_t tl = strlen(title);
    if (r) { int n = xcb_get_property_value_length(r); const char* v = (const char*)xcb_get_property_value(r);
             for (int i = 0; i + (int)tl <= n; i++) if (!strncmp(v + i, title, tl)) hit = 1; }
    free(r); return hit;
}
static xcb_window_t find_window(xcb_connection_t* c, xcb_window_t w, const char* title, xcb_atom_t net_name, xcb_atom_t utf8, int depth)
{
    if (has_title(c, w, XCB_ATOM_WM_NAME, XCB_ATOM_ANY, title) || has_title(c, w, net_name, utf8, title)) return w;
    if (depth <= 0) return 0;
    xcb_query_tree_reply_t* t = xcb_query_tree_reply(c, xcb_query_tree(c, w), 0);
    xcb_window_t found = 0;
    if (t) { xcb_window_t* ch = xcb_query_tree_children(t);
             for (int i = 0; i < xcb_query_tree_children_length(t) && !found; i++) found = find_window(c, ch[i], title, net_name, utf8, depth - 1);
             free(t); }
    return found;
}
// tell Wine where its window is on the screen (it does not see the host's window moving), so that clicks hit
static void send_position(Inst* p)
{
    if (!p->xc || !p->xchild) return;
    xcb_translate_coordinates_reply_t* r = xcb_translate_coordinates_reply(p->xc, xcb_translate_coordinates(p->xc, p->xchild, p->xroot, 0, 0), 0);
    if (!r) return;
    int x = r->dst_x, y = r->dst_y; free(r);
    if (x == p->last_x && y == p->last_y) return;
    p->last_x = x; p->last_y = y;
    xcb_configure_notify_event_t ev; memset(&ev, 0, sizeof ev);
    ev.response_type = XCB_CONFIGURE_NOTIFY; ev.event = p->xchild; ev.window = p->xchild;
    ev.x = (int16_t)x; ev.y = (int16_t)y; ev.width = (uint16_t)p->gui_w; ev.height = (uint16_t)p->gui_h;
    xcb_send_event(p->xc, 0, p->xchild, XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char*)&ev);
    xcb_flush(p->xc);
}
static void gui_release(Inst* p)
{
    timer_stop(p);
    if (p->xc) { xcb_disconnect(p->xc); p->xc = 0; }
    p->xchild = 0;
}
static bool gui_set_parent(const clap_plugin_t* c, const clap_window_t* win)
{
    Inst* p = INST(c);
    if (!p->gui_made || !win || !win->x11 || p->gui_open || !p->fx) return false;
    int w0 = p->gui_w, h0 = p->gui_h;
    p->gui_open = 1;                          // before the call: a sizeWindow inside effEditOpen goes to the host
    if (!vb_stub_edit_open(p->fx, VB_F_TOPLEVEL)) { p->gui_open = 0; return false; }
    read_rect(p);
    p->xc = xcb_connect(0, 0);
    if (!p->xc || xcb_connection_has_error(p->xc)) { if (p->xc) xcb_disconnect(p->xc); p->xc = 0; return false; }
    p->xparent = (xcb_window_t)win->x11;
    p->xroot = xcb_setup_roots_iterator(xcb_get_setup(p->xc)).data->root;
    xcb_atom_t net_name = atom(p->xc, "_NET_WM_NAME"), utf8 = atom(p->xc, "UTF8_STRING");
    char title[64]; snprintf(title, sizeof title, "VST2Bridge %s", vb_stub_token(p->fx));
    for (int i = 0; i < 100 && !p->xchild; i++) {                  // the window appears a moment after the call
        p->xchild = find_window(p->xc, p->xroot, title, net_name, utf8, 3);
        if (!p->xchild) usleep(50000);
    }
    if (!p->xchild) { log_error(p, "the editor window of the helper was not found (X11 only, no Wayland)"); return false; }
    xcb_reparent_window(p->xc, p->xchild, p->xparent, 0, 0);
    xcb_map_window(p->xc, p->xchild);
    xcb_flush(p->xc);
    p->last_x = p->last_y = -100000;
    send_position(p);
    timer_start(p, 100);
    if ((p->gui_w != w0 || p->gui_h != h0) && p->hgui && p->hgui->request_resize)
        p->hgui->request_resize(p->host, (uint32_t)p->gui_w, (uint32_t)p->gui_h);
    return true;
}
static bool gui_show(const clap_plugin_t* c)
{
    Inst* p = INST(c);
    if (!p->gui_open) return false;
    vb_stub_edit_show(p->fx, 1);
    if (p->xc) { xcb_map_window(p->xc, p->xchild); xcb_flush(p->xc); send_position(p); }
    return true;
}
static bool gui_hide(const clap_plugin_t* c)
{
    Inst* p = INST(c);
    if (!p->gui_open) return false;
    if (p->xc) { xcb_unmap_window(p->xc, p->xchild); xcb_flush(p->xc); }
    vb_stub_edit_show(p->fx, 0);
    return true;
}
static void on_timer(const clap_plugin_t* c, clap_id id)
{
    Inst* p = INST(c);
    if (id == p->timer && p->gui_open) send_position(p);
}
#endif
static const clap_plugin_gui_t ext_gui = {
    gui_supported, gui_preferred, gui_create, gui_destroy, gui_scale, gui_size, gui_can_resize, gui_hints,
    gui_adjust, gui_set_size, gui_set_parent, gui_transient, gui_title, gui_show, gui_hide
};
static const clap_plugin_timer_support_t ext_timer = { on_timer };
#endif

static const void* pl_ext(const clap_plugin_t* c, const char* id)
{
    Inst* p = INST(c);
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &ext_audio;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &ext_notes;
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &ext_params;
    if (!strcmp(id, CLAP_EXT_STATE)) return &ext_state;
    if (!strcmp(id, CLAP_EXT_LATENCY)) return &ext_latency;
    if (!strcmp(id, CLAP_EXT_GUI) && p->fx && (p->fx->flags & effFlagsHasEditor)) return &ext_gui;
#ifndef _WIN32
    if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &ext_timer;
#endif
    return 0;
}

// ----------------------------------------------------------------------------- factory and entry

static uint32_t fac_count(const clap_plugin_factory_t* f) { (void)f; probe(); return g_ok ? 1 : 0; }
static const clap_plugin_descriptor_t* fac_desc(const clap_plugin_factory_t* f, uint32_t i) { (void)f; probe(); return g_ok && i == 0 ? &g_desc : 0; }
static const clap_plugin_t* fac_create(const clap_plugin_factory_t* f, const clap_host_t* host, const char* id)
{
    (void)f;
    probe();
    if (!g_ok || !id || strcmp(id, g_id) || !host) return 0;
    glock();
    Inst* p = g_spare; g_spare = 0;          // the plugin made for the descriptor saves one start of the helper
    gunlock();
    if (!p && !(p = inst_alloc())) return 0;
    p->host = host;
    p->plugin.desc = &g_desc; p->plugin.plugin_data = p;
    p->plugin.init = pl_init; p->plugin.destroy = pl_destroy; p->plugin.activate = pl_activate;
    p->plugin.deactivate = pl_deactivate; p->plugin.start_processing = pl_start; p->plugin.stop_processing = pl_stop;
    p->plugin.reset = pl_reset; p->plugin.process = pl_process; p->plugin.get_extension = pl_ext;
    p->plugin.on_main_thread = pl_on_main;
    return &p->plugin;
}
static const clap_plugin_factory_t g_factory = { fac_count, fac_desc, fac_create };

static void make_id(void)
{
    char low[256]; snprintf(low, sizeof low, "%s", g_stem8);
    for (char* q = low; *q; q++) { if (*q >= 'A' && *q <= 'Z') *q += 32; else if (*q == ' ') *q = '-'; }
    snprintf(g_id, sizeof g_id, "io.github.nightfright2k19.vst2bridge.%s", low);
}

#ifdef _WIN32
static bool entry_init(const char* path)
{
    InitOnceExecuteOnce(&g_once, init_lock, 0, 0);
    if (g_inits) { g_inits++; return true; }
    wchar_t full[MAX_PATH];
    if (!path || !MultiByteToWideChar(CP_UTF8, 0, path, -1, full, MAX_PATH)) return false;
    for (wchar_t* q = full; *q; q++) if (*q == L'/') *q = L'\\';  // hosts mix both separators
    wchar_t* slash = wcsrchr(full, L'\\');
    wcsncpy(g_self_name, slash ? slash + 1 : full, MAX_PATH - 1);
    // foo_x64.clap -> the plugin foo.dll in the same folder (the bridge is part of the CLAP); id and fallback name from foo
    if (slash) { wchar_t keep = *slash; *slash = 0; wcsncpy(g_dir, full, MAX_PATH - 1); *slash = keep; } else wcscpy(g_dir, L".");
    wcsncpy(g_stem, g_self_name, MAX_PATH - 1); g_stem[MAX_PATH - 1] = 0;
    wchar_t* dot = wcsrchr(g_stem, L'.'); if (dot) *dot = 0;
    size_t n = wcslen(g_stem);
    if (n > 4 && (!_wcsicmp(g_stem + n - 4, L"_x64") || !_wcsicmp(g_stem + n - 4, L"_x86"))) g_stem[n - 4] = 0;
    WideCharToMultiByte(CP_UTF8, 0, g_stem, -1, g_stem8, sizeof g_stem8, 0, 0);
    make_id();
    g_inits = 1;
    return true;
}
#else
// foo.clap -> foo.dll in the same folder (macOS: hosts pass the bundle foo.clap or its foo.clap/Contents/MacOS/foo)
static bool entry_init(const char* path)
{
    if (g_inits) { g_inits++; return true; }
    if (!path || !*path) return false;
    char full[PATH_MAX];
    snprintf(full, sizeof full, "%s", path);
    char* b = strstr(full, ".clap/Contents/MacOS");
    if (b) b[5] = 0;
    size_t n = strlen(full); while (n > 1 && full[n - 1] == '/') full[--n] = 0;
    char* slash = strrchr(full, '/');
    const char* base = slash ? slash + 1 : full;
    snprintf(g_stem, sizeof g_stem, "%s", base);
    char* dot = strrchr(g_stem, '.'); if (dot && dot != g_stem) *dot = 0;
    if (slash) { *slash = 0; snprintf(g_dir, sizeof g_dir, "%s", full[0] ? full : "/"); } else snprintf(g_dir, sizeof g_dir, ".");
    snprintf(g_stem8, sizeof g_stem8, "%s", g_stem);
    make_id();
    g_inits = 1;
    return true;
}
#endif
static void entry_deinit(void)
{
    if (!g_inits || --g_inits) return;
    if (g_spare) { inst_free(g_spare); g_spare = 0; }
    g_probed = 0; g_ok = 0;
    // the VST2 bridge stays loaded: a host may still hold threads of it for a moment
}
static const void* entry_factory(const char* id) { return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &g_factory : 0; }

VB_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, entry_init, entry_deinit, entry_factory };
VB_EXPORT int VST2BridgeClap(void) { return 1; }                 // marker for the patcher
