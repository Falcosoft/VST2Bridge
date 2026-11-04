// stub.c - the bridge DLL. Built twice from this file:
//   64-bit: copied as <plugin>_x64.dll next to a 32-bit plugin; runs it in vst2bridge-host32.exe
//   32-bit: copied as <plugin>_x86.dll next to a 64-bit plugin; runs it in vst2bridge-host64.exe
// The host loads the bridge like any VST2 plugin. The bridge finds the real plugin from its own file name
// (syxg50_x64.dll -> syxg50.dll in the same folder), starts the helper of the plugin's bitness and passes every
// call through shared memory (protocol.h); the plugin's calls to the host come back the same way.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "vst2.h"
#include "protocol.h"
#include "marshal.h"

#ifdef _WIN64
#define VB_SUFFIX   L"_x64"
#define VB_HELPER   L"vst2bridge-host32.exe"
#else
#define VB_SUFFIX   L"_x86"
#define VB_HELPER   L"vst2bridge-host64.exe"
#endif
#define VB_SIZEWINDOW_MSG (WM_APP + 1)

static wchar_t g_dir[MAX_PATH], g_stem[MAX_PATH], g_self_name[MAX_PATH];
static HMODULE g_self;
static volatile LONG g_counter;

typedef struct {
    AEffect fx;                         // first member: the host's AEffect* is also the Inst*
    AudioMasterCallback master;
    VbShared* sh;
    volatile LONG dead, stopping, loading;
    HANDLE proc, job, map, ready, mutex, main_h, main_s, main_v, audio_h, audio_s, aux_h, aux_s;
    HANDLE srv_thread, aux_thread;
    volatile DWORD main_tid;            // bridge thread taking part in the current main-channel call
    volatile DWORD process_tid;         // thread that last called process: its calls use the audio channel
    CRITICAL_SECTION audio_cs, ev_cs;
    char prefix[64];
    HWND msgwnd;                        // on the host's editor thread: sizeWindow requests are run there
    ERect rect;
    uint8_t* chunk; size_t chunk_cap;
    uint8_t* spk[2]; size_t spk_cap[2];
    uint32_t n_ev, sysex_n;             // events of effProcessEvents wait here for the next process call
    VbEvent ev[VB_MAXEVENTS];
    uint8_t sysex[VB_SYSEX_MAX];
    char error[256];
} Inst;

typedef struct {
    int32_t kind, op, index, flags;
    int64_t value;
    float opt;
    const void* in; uint32_t in_len;    // request data
    void* out; uint32_t out_cap;        // where the reply data goes
    uint32_t out_len;
    int64_t ret; float ret_opt;
} Req;

static void serve_master(Inst* p, VbFrame* f, int on_server);

// ----------------------------------------------------------------------------- helpers

static void log_error(const char* m)
{
    wchar_t t[MAX_PATH]; char f[MAX_PATH * 3];
    DWORD n = GetTempPathW(MAX_PATH, t);
    if (!n || n >= MAX_PATH) wcscpy(t, L"C:\\Windows\\Temp\\");
    snprintf(f, sizeof f, "%lsvst2bridge.log", t);
    FILE* fp = fopen(f, "a");
    if (fp) { fprintf(fp, "%ls: %s\n", g_self_name, m); fclose(fp); }
}

static int file_exists(const wchar_t* f) { DWORD a = GetFileAttributesW(f); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

static void sync_info(Inst* p)
{
    const VbInfo* i = &p->sh->info;
    AEffect* fx = &p->fx;
    fx->numPrograms = i->numPrograms; fx->numParams = i->numParams;
    fx->numInputs = i->numInputs; fx->numOutputs = i->numOutputs;
    fx->flags = (i->flags | effFlagsCanReplacing) & ~(i->has_double ? 0 : effFlagsCanDoubleReplacing);
    fx->initialDelay = i->initialDelay; fx->uniqueID = i->uniqueID; fx->version = i->version; fx->ioRatio = i->ioRatio;
}

// waits for ev; answers messages sent to this thread's windows meanwhile (pump). 0 = the helper is gone.
static int chan_wait(Inst* p, HANDLE ev, int pump)
{
    HANDLE w[2] = { ev, p->proc };
    for (;;) {
        DWORD r = pump ? MsgWaitForMultipleObjects(2, w, FALSE, INFINITE, QS_SENDMESSAGE) : WaitForMultipleObjects(2, w, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0) return !p->dead;
        if (r == WAIT_OBJECT_0 + 2) { MSG m; PeekMessageW(&m, 0, 0, 0, PM_NOREMOVE); continue; }   // runs the sent messages
        InterlockedExchange(&p->dead, 1);
        return 0;
    }
}

// one call: push r on ch, signal `to`, wait on `in` for the reply and serve the helper's nested calls meanwhile
static int exchange(Inst* p, VbChan* ch, HANDLE to, HANDLE in, int pump, Req* r)
{
    if (p->dead) return 0;
    int d = ch->depth;
    if (d >= VB_MAXDEPTH || r->in_len > VB_FRAME_DATA) return 0;
    VbFrame* f = &ch->f[d];
    f->kind = r->kind; f->op = r->op; f->index = r->index; f->flags = r->flags;
    f->value = r->value; f->opt = r->opt; f->ret = 0;
    f->len = r->in_len;
    if (r->in_len) memcpy(f->data, r->in, r->in_len);
    ch->depth = d + 1;
    MemoryBarrier();
    ch->msg = VB_MSG_REQ;
    SetEvent(to);
    for (;;) {
        if (!chan_wait(p, in, pump)) return 0;
        MemoryBarrier();
        if (ch->msg == VB_MSG_REPLY && ch->depth == d + 1) break;
        if (ch->msg == VB_MSG_REQ && ch->depth > d + 1) {           // the plugin calls the host
            serve_master(p, &ch->f[ch->depth - 1], 0);
            MemoryBarrier();
            ch->msg = VB_MSG_REPLY;
            SetEvent(to);
        }
    }
    sync_info(p);
    r->ret = f->ret; r->ret_opt = f->opt;
    r->out_len = f->len < r->out_cap ? f->len : r->out_cap;
    if (r->out && r->out_len) memcpy(r->out, f->data, r->out_len);
    ch->depth = d;
    return 1;
}

static int lock_main(Inst* p)
{
    HANDLE w[2] = { p->mutex, p->proc };
    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(2, w, FALSE, INFINITE, QS_SENDMESSAGE);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED_0) return 1;
        if (r == WAIT_OBJECT_0 + 2) { MSG m; PeekMessageW(&m, 0, 0, 0, PM_NOREMOVE); continue; }
        InterlockedExchange(&p->dead, 1);
        return 0;
    }
}

static int call_main(Inst* p, Req* r)
{
    VbChan* ch = &p->sh->main;
    DWORD me = GetCurrentThreadId();
    if (p->main_tid == me)                                          // nested in a call this thread takes part in
        return exchange(p, ch, p->main_h, ch->owner == VB_OWNER_HELPER ? p->main_v : p->main_s, 1, r);
    if (!lock_main(p)) return 0;
    p->main_tid = me; ch->owner = VB_OWNER_STUB;
    int ok = exchange(p, ch, p->main_h, p->main_s, 1, r);
    ch->owner = VB_OWNER_NONE; p->main_tid = 0;
    ReleaseMutex(p->mutex);
    return ok;
}

static int call_audio(Inst* p, Req* r)
{
    EnterCriticalSection(&p->audio_cs);                             // recursive: nested calls of the same thread pass
    int ok = exchange(p, &p->sh->audio, p->audio_h, p->audio_s, 0, r);
    LeaveCriticalSection(&p->audio_cs);
    return ok;
}

// calls made on the thread that runs process go to the helper's audio thread, all others to its main thread
static int call(Inst* p, Req* r, int force_main)
{
    if (p->dead) return 0;
    if (!force_main && GetCurrentThreadId() == p->process_tid) return call_audio(p, r);
    return call_main(p, r);
}

// ----------------------------------------------------------------------------- plugin -> host

static LRESULT CALLBACK msg_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == VB_SIZEWINDOW_MSG) {
        Inst* p = (Inst*)GetWindowLongPtrW(w, GWLP_USERDATA);
        return p ? (LRESULT)p->master(&p->fx, audioMasterSizeWindow, (int32_t)wp, (intptr_t)lp, 0, 0) : 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

static void make_msgwnd(Inst* p)
{
    static int registered;
    if (p->msgwnd) return;
    if (!registered) {
        WNDCLASSW wc; memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = msg_proc; wc.hInstance = g_self; wc.lpszClassName = L"VST2BridgeMsg";
        RegisterClassW(&wc); registered = 1;
    }
    p->msgwnd = CreateWindowExW(0, L"VST2BridgeMsg", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, g_self, 0);
    if (p->msgwnd) SetWindowLongPtrW(p->msgwnd, GWLP_USERDATA, (LONG_PTR)p);
}

static void serve_master(Inst* p, VbFrame* f, int on_server)
{
    int size, m = master_marshal(f->op, &size);
    AEffect* fx = p->loading ? 0 : &p->fx;                          // plugins pass NULL while they are being created
    intptr_t ret = 0, val = (intptr_t)f->value;
    uint32_t in_len = f->len;
    sync_info(p);
    f->len = 0;
    switch (m) {
    case M_NONE: ret = p->master(fx, f->op, f->index, val, 0, f->opt); break;
    case M_STR_IN:
        if (in_len) f->data[in_len - 1] = 0;
        ret = p->master(fx, f->op, f->index, val, in_len ? f->data : 0, f->opt);
        break;
    case M_STR_OUT: {
        char buf[VB_STR_MAX]; memset(buf, 0, sizeof buf);
        ret = p->master(fx, f->op, f->index, val, buf, f->opt);
        buf[VB_STR_MAX - 1] = 0;
        f->len = (uint32_t)strlen(buf) + 1; memcpy(f->data, buf, f->len);
        break;
    }
    case M_SPECIAL:
        switch (f->op) {
        case audioMasterGetTime: {
            VstTimeInfo* t = (VstTimeInfo*)p->master(fx, f->op, f->index, val, 0, f->opt);
            if (t) { memcpy(f->data, t, sizeof *t); f->len = sizeof *t; ret = 1; }
            break;
        }
        case audioMasterGetDirectory: {
            const char* s = (const char*)p->master(fx, f->op, f->index, val, 0, f->opt);
            if (s) { size_t n = strnlen(s, VB_STR_MAX - 1); memcpy(f->data, s, n); f->data[n] = 0; f->len = (uint32_t)n + 1; ret = 1; }
            break;
        }
        case audioMasterProcessEvents: {                            // MIDI sent by the plugin
            uint32_t n = 0;
            if (in_len >= 4) memcpy(&n, f->data, 4);
            if (n > VB_MAXEVENTS || 4 + (size_t)n * sizeof(VbEvent) > in_len) break;
            const VbEvent* e = (const VbEvent*)(f->data + 4);
            const uint8_t* sx = f->data + 4 + n * sizeof(VbEvent);
            size_t sx_len = in_len - 4 - n * sizeof(VbEvent);
            VstEvents* list = (VstEvents*)calloc(1, sizeof(VstEvents) + n * sizeof(VstEvent*));
            VstMidiEvent* me = (VstMidiEvent*)calloc(n ? n : 1, sizeof *me);
            VstMidiSysexEvent* se = (VstMidiSysexEvent*)calloc(n ? n : 1, sizeof *se);
            if (list && me && se) {
                int cnt = 0;
                for (uint32_t i = 0; i < n; i++) {
                    if (e[i].type == kVstSysExType) {
                        if ((uint64_t)e[i].sysex_off + e[i].sysex_len > sx_len) continue;
                        se[i].type = kVstSysExType; se[i].byteSize = sizeof se[i]; se[i].deltaFrames = e[i].delta; se[i].flags = e[i].flags;
                        se[i].dumpBytes = (int32_t)e[i].sysex_len; se[i].sysexDump = (char*)sx + e[i].sysex_off;
                        list->events[cnt++] = (VstEvent*)&se[i];
                    } else {
                        me[i].type = kVstMidiType; me[i].byteSize = sizeof me[i]; me[i].deltaFrames = e[i].delta; me[i].flags = e[i].flags;
                        me[i].noteLength = e[i].note_length; me[i].noteOffset = e[i].note_offset;
                        memcpy(me[i].midiData, e[i].midi, 4); me[i].detune = e[i].detune; me[i].noteOffVelocity = (char)e[i].off_velocity;
                        list->events[cnt++] = (VstEvent*)&me[i];
                    }
                }
                list->numEvents = cnt;
                ret = p->master(fx, f->op, f->index, val, list, f->opt);
            }
            free(list); free(me); free(se);
            break;
        }
        case audioMasterVendorSpecific:                             // the helper sends only calls with ptr = NULL
            ret = p->master(fx, f->op, f->index, val, 0, f->opt);
            break;
        case audioMasterSizeWindow:
            // from the server thread: run it on the host's editor thread, where hosts expect window changes
            if (on_server && p->msgwnd) {
                DWORD_PTR res = 0;
                DWORD gui = GetWindowThreadProcessId(p->msgwnd, 0);
                DWORD prev = p->main_tid;
                p->main_tid = gui;                                  // calls the host makes meanwhile nest in this call
                if (!SendMessageTimeoutW(p->msgwnd, VB_SIZEWINDOW_MSG, (WPARAM)f->index, (LPARAM)val, SMTO_NORMAL, 5000, &res)) res = 0;
                p->main_tid = prev;
                ret = (intptr_t)res;
            } else ret = p->master(fx, f->op, f->index, val, 0, f->opt);
            break;
        }
        break;
    default: break;                                                 // M_REFUSE (the helper does not send these)
    }
    f->ret = ret;
}

// helper-started calls on the main channel (the plugin's GUI, timers ...) and calls from other plugin threads
static DWORD WINAPI server_thread(LPVOID a)
{
    Inst* p = (Inst*)a;
    VbChan* ch = &p->sh->main;
    while (chan_wait(p, p->main_v, 0) && !p->stopping) {
        MemoryBarrier();
        if (ch->msg != VB_MSG_REQ || ch->depth < 1) continue;
        p->main_tid = GetCurrentThreadId();
        serve_master(p, &ch->f[ch->depth - 1], 1);
        p->main_tid = 0;
        MemoryBarrier();
        ch->msg = VB_MSG_REPLY;
        SetEvent(p->main_h);
    }
    return 0;
}

static DWORD WINAPI aux_thread(LPVOID a)
{
    Inst* p = (Inst*)a;
    VbChan* ch = &p->sh->aux;
    while (chan_wait(p, p->aux_s, 0) && !p->stopping) {
        MemoryBarrier();
        if (ch->msg != VB_MSG_REQ || ch->depth < 1) continue;
        serve_master(p, &ch->f[ch->depth - 1], 0);
        MemoryBarrier();
        ch->msg = VB_MSG_REPLY;
        SetEvent(p->aux_h);
    }
    return 0;
}

// ----------------------------------------------------------------------------- start / stop

static int start(Inst* p)
{
    wchar_t dll[MAX_PATH], exe[MAX_PATH], cmd[4 * MAX_PATH], wpfx[64];
    _snwprintf(dll, MAX_PATH, L"%ls\\%ls.dll", g_dir, g_stem);
    _snwprintf(exe, MAX_PATH, L"%ls\\%ls", g_dir, VB_HELPER);
    if (!file_exists(dll)) { snprintf(p->error, sizeof p->error, "plugin %ls not found next to the bridge", dll); return 0; }
    if (!file_exists(exe)) { snprintf(p->error, sizeof p->error, "%ls not found next to the bridge", VB_HELPER); return 0; }

    snprintf(p->prefix, sizeof p->prefix, "Local\\V2B%lu_%ld_", GetCurrentProcessId(), InterlockedIncrement(&g_counter));
    MultiByteToWideChar(CP_ACP, 0, p->prefix, -1, wpfx, 64);
    char nm[128];
    #define OBJ(sfx) (snprintf(nm, sizeof nm, "%s%s", p->prefix, sfx), nm)
    p->map = CreateFileMappingA(INVALID_HANDLE_VALUE, 0, PAGE_READWRITE, 0, (DWORD)sizeof(VbShared), OBJ(VB_OBJ_MAP));
    p->ready = CreateEventA(0, FALSE, FALSE, OBJ(VB_OBJ_READY));
    p->mutex = CreateMutexA(0, FALSE, OBJ(VB_OBJ_MUTEX));
    p->main_h = CreateEventA(0, FALSE, FALSE, OBJ(VB_OBJ_MAIN_H));  p->main_s = CreateEventA(0, FALSE, FALSE, OBJ(VB_OBJ_MAIN_S));
    p->main_v = CreateEventA(0, FALSE, FALSE, OBJ(VB_OBJ_MAIN_V));
    p->audio_h = CreateEventA(0, FALSE, FALSE, OBJ(VB_OBJ_AUDIO_H)); p->audio_s = CreateEventA(0, FALSE, FALSE, OBJ(VB_OBJ_AUDIO_S));
    p->aux_h = CreateEventA(0, FALSE, FALSE, OBJ(VB_OBJ_AUX_H));    p->aux_s = CreateEventA(0, FALSE, FALSE, OBJ(VB_OBJ_AUX_S));
    #undef OBJ
    if (!p->map || !p->ready || !p->mutex || !p->main_h || !p->main_s || !p->main_v || !p->audio_h || !p->audio_s || !p->aux_h || !p->aux_s) {
        snprintf(p->error, sizeof p->error, "cannot create shared objects (error %lu)", GetLastError()); return 0;
    }
    p->sh = (VbShared*)MapViewOfFile(p->map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!p->sh) { snprintf(p->error, sizeof p->error, "cannot map shared memory"); return 0; }
    p->sh->magic = VB_MAGIC; p->sh->version = VB_VERSION;

    // shell plugins ask for the id of the sub-plugin while they are created; the helper answers with this one
    int32_t shell_id = p->master ? (int32_t)p->master(0, audioMasterCurrentId, 0, 0, 0, 0) : 0;
    _snwprintf(cmd, 4 * MAX_PATH, L"\"%ls\" \"%ls\" %ls %ld %lu", exe, dll, wpfx, (long)shell_id, GetCurrentProcessId());

    p->job = CreateJobObjectW(0, 0);                                // the helper ends with the host, even if the host crashes
    if (p->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li; memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(p->job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    STARTUPINFOW si; PROCESS_INFORMATION pi; memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (!CreateProcessW(exe, cmd, 0, 0, FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB | CREATE_SUSPENDED, 0, g_dir, &si, &pi)
        && !CreateProcessW(exe, cmd, 0, 0, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, 0, g_dir, &si, &pi)) {
        snprintf(p->error, sizeof p->error, "cannot start %ls (error %lu)", VB_HELPER, GetLastError()); return 0;
    }
    if (p->job) AssignProcessToJobObject(p->job, pi.hProcess);
    p->proc = pi.hProcess;
    p->srv_thread = CreateThread(0, 0, server_thread, p, 0, 0);
    p->aux_thread = CreateThread(0, 0, aux_thread, p, 0, 0);
    ResumeThread(pi.hThread); CloseHandle(pi.hThread);

    HANDLE w[2] = { p->ready, p->proc };
    DWORD r = WaitForMultipleObjects(2, w, FALSE, 120000);          // big sample sets: loading can take a while
    if (r != WAIT_OBJECT_0) {
        snprintf(p->error, sizeof p->error, r == WAIT_OBJECT_0 + 1 ? "the helper ended while loading the plugin" : "the plugin took too long to load");
        return 0;
    }
    if (!p->sh->ok) { snprintf(p->error, sizeof p->error, "%s", p->sh->error[0] ? p->sh->error : "the plugin could not be loaded"); return 0; }
    return 1;
}

static void stop(Inst* p)
{
    if (p->proc && !p->dead && p->sh && p->sh->ok) {
        Req r; memset(&r, 0, sizeof r); r.kind = VB_K_QUIT;
        call_main(p, &r);
        if (WaitForSingleObject(p->proc, 5000) != WAIT_OBJECT_0) TerminateProcess(p->proc, 0);
    } else if (p->proc) TerminateProcess(p->proc, 0);
    InterlockedExchange(&p->stopping, 1);
    InterlockedExchange(&p->dead, 1);
    if (p->main_v) SetEvent(p->main_v);
    if (p->aux_s) SetEvent(p->aux_s);
    if (p->srv_thread) { WaitForSingleObject(p->srv_thread, 3000); CloseHandle(p->srv_thread); }
    if (p->aux_thread) { WaitForSingleObject(p->aux_thread, 3000); CloseHandle(p->aux_thread); }
    if (p->sh) UnmapViewOfFile(p->sh);
    HANDLE* h[] = { &p->proc, &p->job, &p->map, &p->ready, &p->mutex, &p->main_h, &p->main_s, &p->main_v,
                    &p->audio_h, &p->audio_s, &p->aux_h, &p->aux_s };
    for (size_t i = 0; i < sizeof h / sizeof *h; i++) if (*h[i]) { CloseHandle(*h[i]); *h[i] = 0; }
    p->sh = 0;
    if (p->msgwnd) {
        SetWindowLongPtrW(p->msgwnd, GWLP_USERDATA, 0);
        if (GetWindowThreadProcessId(p->msgwnd, 0) == GetCurrentThreadId()) DestroyWindow(p->msgwnd);
        else PostMessageW(p->msgwnd, WM_CLOSE, 0, 0);
        p->msgwnd = 0;
    }
}

// a named mapping for one chunk larger than a frame
static HANDLE chunk_map(Inst* p, size_t n, char* name, size_t name_n, void** view)
{
    snprintf(name, name_n, "%s%s%ld", p->prefix, VB_OBJ_CHUNK, InterlockedIncrement(&g_counter));
    HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, 0, PAGE_READWRITE, (DWORD)((uint64_t)n >> 32), (DWORD)n, name);
    *view = m ? MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0) : 0;
    if (m && !*view) { CloseHandle(m); m = 0; }
    return m;
}

// ----------------------------------------------------------------------------- host -> plugin

static void queue_events(Inst* p, const VstEvents* evs)
{
    EnterCriticalSection(&p->ev_cs);
    for (int i = 0; i < evs->numEvents; i++) {
        const VstEvent* e = evs->events[i];
        if (!e || p->n_ev >= VB_MAXEVENTS) continue;
        VbEvent* q = &p->ev[p->n_ev];
        memset(q, 0, sizeof *q);
        q->type = e->type; q->delta = e->deltaFrames < 0 ? 0 : e->deltaFrames; q->flags = e->flags;
        if (e->type == kVstMidiType) {
            const VstMidiEvent* m = (const VstMidiEvent*)e;
            memcpy(q->midi, m->midiData, 4); q->note_length = m->noteLength; q->note_offset = m->noteOffset;
            q->detune = m->detune; q->off_velocity = (uint8_t)m->noteOffVelocity;
        } else if (e->type == kVstSysExType) {
            const VstMidiSysexEvent* s = (const VstMidiSysexEvent*)e;
            uint32_t n = s->dumpBytes > 0 ? (uint32_t)s->dumpBytes : 0;
            if (!s->sysexDump || !n || p->sysex_n + n > VB_SYSEX_MAX) continue;
            q->sysex_off = p->sysex_n; q->sysex_len = n;
            memcpy(p->sysex + p->sysex_n, s->sysexDump, n); p->sysex_n += n;
        } else continue;                                            // other event types are not passed on
        p->n_ev++;
    }
    LeaveCriticalSection(&p->ev_cs);
}

static void process(Inst* p, void** in, void** out, int32_t frames, int mode)
{
    size_t es = mode == VB_P_DOUBLE ? sizeof(double) : sizeof(float);
    int nin = p->fx.numInputs < VB_MAXCH ? p->fx.numInputs : VB_MAXCH;
    int nout = p->fx.numOutputs < VB_MAXCH ? p->fx.numOutputs : VB_MAXCH;
    uint32_t total = frames > 0 ? (uint32_t)frames : 0, pos = 0, ei = 0;
    EnterCriticalSection(&p->audio_cs);
    EnterCriticalSection(&p->ev_cs);
    p->process_tid = GetCurrentThreadId();
    VbAudio* a = &p->sh->au;
    while (pos < total) {
        uint32_t n = total - pos > VB_MAXFRAMES ? VB_MAXFRAMES : total - pos;
        int ok = !p->dead;
        if (ok) {
            a->nin = nin; a->nout = nout; a->n_events = 0; a->sysex_bytes = 0;
            for (int c = 0; c < nin; c++) {
                if (in && in[c]) memcpy(a->in[c], (char*)in[c] + pos * es, n * es);
                else memset(a->in[c], 0, n * es);
            }
            while (ei < p->n_ev) {                                  // the events that fall into this piece
                const VbEvent* e = &p->ev[ei];
                if (pos + n < total && (uint32_t)e->delta >= pos + n) break;
                VbEvent* d = &a->events[a->n_events++];
                *d = *e; d->delta = (uint32_t)e->delta > pos ? e->delta - (int32_t)pos : 0;
                if ((uint32_t)d->delta >= n) d->delta = (int32_t)n - 1;
                if (e->type == kVstSysExType) {
                    d->sysex_off = a->sysex_bytes;
                    memcpy(a->sysex + a->sysex_bytes, p->sysex + e->sysex_off, e->sysex_len);
                    a->sysex_bytes += e->sysex_len;
                }
                ei++;
            }
            Req r; memset(&r, 0, sizeof r);
            r.kind = VB_K_PROCESS; r.op = mode; r.value = n;
            ok = call_audio(p, &r);
        }
        for (int c = 0; c < p->fx.numOutputs; c++) {
            if (!out || !out[c]) continue;
            char* o = (char*)out[c] + pos * es;
            if (!ok || c >= nout) { if (mode != VB_P_ACCUMULATE) memset(o, 0, n * es); }
            else if (mode == VB_P_ACCUMULATE) { float* of = (float*)o; const float* s = (const float*)a->out[c]; for (uint32_t i = 0; i < n; i++) of[i] += s[i]; }
            else memcpy(o, a->out[c], n * es);
        }
        pos += n;
    }
    p->n_ev = 0; p->sysex_n = 0;
    LeaveCriticalSection(&p->ev_cs);
    LeaveCriticalSection(&p->audio_cs);
}

static void VSTCALL vst_process(AEffect* fx, float** in, float** out, int32_t n) { process((Inst*)fx, (void**)in, (void**)out, n, VB_P_ACCUMULATE); }
static void VSTCALL vst_process_replacing(AEffect* fx, float** in, float** out, int32_t n) { process((Inst*)fx, (void**)in, (void**)out, n, VB_P_REPLACING); }
static void VSTCALL vst_process_double(AEffect* fx, double** in, double** out, int32_t n) { process((Inst*)fx, (void**)in, (void**)out, n, VB_P_DOUBLE); }

static void VSTCALL vst_set_parameter(AEffect* fx, int32_t i, float v)
{
    Req r; memset(&r, 0, sizeof r); r.kind = VB_K_SETPARAM; r.index = i; r.opt = v;
    call((Inst*)fx, &r, 0);
}
static float VSTCALL vst_get_parameter(AEffect* fx, int32_t i)
{
    Req r; memset(&r, 0, sizeof r); r.kind = VB_K_GETPARAM; r.index = i;
    return call((Inst*)fx, &r, 0) ? r.ret_opt : 0.0f;
}

static intptr_t special(Inst* p, Req* r, void* ptr, intptr_t value)
{
    switch (r->op) {
    case effClose:
        stop(p);
        DeleteCriticalSection(&p->audio_cs); DeleteCriticalSection(&p->ev_cs);
        free(p->chunk); free(p->spk[0]); free(p->spk[1]); free(p);
        return 1;
    case effProcessEvents:
        if (ptr) queue_events(p, (const VstEvents*)ptr);
        return 1;
    case effEditGetRect: {
        ERect rc;
        r->out = &rc; r->out_cap = sizeof rc;
        if (!call(p, r, 1)) return 0;
        if (r->out_len == sizeof rc) { p->rect = rc; if (ptr) *(ERect**)ptr = &p->rect; }
        return (intptr_t)r->ret;
    }
    case effEditOpen:
        if (!ptr) return 0;
        make_msgwnd(p);
        r->value = (int64_t)(uint32_t)(uintptr_t)ptr;               // window handles have 32 significant bits in 32 and 64-bit processes
        return call(p, r, 1) ? (intptr_t)r->ret : 0;
    case effGetChunk: {
        if (!ptr) return 0;
        size_t need = VB_FRAME_DATA;
        if (need > p->chunk_cap) { free(p->chunk); p->chunk = (uint8_t*)malloc(need); p->chunk_cap = p->chunk ? need : 0; }
        if (!p->chunk) return 0;
        r->out = p->chunk; r->out_cap = (uint32_t)p->chunk_cap;
        if (!call(p, r, 0) || r->ret <= 0) return 0;
        size_t n = (size_t)r->ret;
        if (r->out_len != n) {                                      // too big for a frame: through a mapping of its own
            char name[96]; void* view; HANDLE m = chunk_map(p, n, name, sizeof name, &view);
            if (!m) return 0;
            Req c; memset(&c, 0, sizeof c); c.kind = VB_K_READ_CHUNK; c.value = (int64_t)n;
            c.in = name; c.in_len = (uint32_t)strlen(name) + 1;
            int ok = call(p, &c, 0) && c.ret;
            if (ok && n > p->chunk_cap) { free(p->chunk); p->chunk = (uint8_t*)malloc(n); p->chunk_cap = p->chunk ? n : 0; }
            if (ok && p->chunk) memcpy(p->chunk, view, n); else n = 0;
            UnmapViewOfFile(view); CloseHandle(m);
            if (!n) return 0;
        }
        *(void**)ptr = p->chunk;
        return (intptr_t)n;
    }
    case effSetChunk: {
        if (!ptr || value <= 0) return 0;
        size_t n = (size_t)value;
        if (n <= VB_FRAME_DATA) { r->in = ptr; r->in_len = (uint32_t)n; return call(p, r, 0) ? (intptr_t)r->ret : 0; }
        char name[96]; void* view; HANDLE m = chunk_map(p, n, name, sizeof name, &view);
        if (!m) return 0;
        memcpy(view, ptr, n);
        r->flags = VB_F_MAPPED; r->in = name; r->in_len = (uint32_t)strlen(name) + 1;
        intptr_t ret = call(p, r, 0) ? (intptr_t)r->ret : 0;
        UnmapViewOfFile(view); CloseHandle(m);
        return ret;
    }
    case effSetSpeakerArrangement: {                                // value = input arrangement, ptr = output arrangement
        if (!value || !ptr) return 0;
        int32_t si = speaker_arr_size((void*)value), so = speaker_arr_size(ptr);
        uint8_t* buf = (uint8_t*)malloc(4 + si + so);
        if (!buf) return 0;
        memcpy(buf, &si, 4); memcpy(buf + 4, (void*)value, si); memcpy(buf + 4 + si, ptr, so);
        r->in = buf; r->in_len = 4 + si + so; r->value = 0;
        intptr_t ret = call(p, r, 0) ? (intptr_t)r->ret : 0;
        free(buf);
        return ret;
    }
    case effGetSpeakerArrangement: {                                // value / ptr = where the plugin puts its pointers
        uint8_t* buf = (uint8_t*)malloc(VB_FRAME_DATA);
        if (!buf) return 0;
        r->out = buf; r->out_cap = VB_FRAME_DATA; r->value = 0;
        intptr_t ret = call(p, r, 0) ? (intptr_t)r->ret : 0;
        uint32_t off = 0;
        for (int k = 0; k < 2; k++) {                               // [size][arrangement] for input, then output; size 0 = NULL
            int32_t n = 0;
            if (off + 4 <= r->out_len) memcpy(&n, buf + off, 4);
            off += 4;
            void* res = 0;
            if (n > 0 && off + (uint32_t)n <= r->out_len) {
                if ((size_t)n > p->spk_cap[k]) { free(p->spk[k]); p->spk[k] = (uint8_t*)malloc(n); p->spk_cap[k] = p->spk[k] ? (size_t)n : 0; }
                if (p->spk[k]) { memcpy(p->spk[k], buf + off, n); res = p->spk[k]; }
                off += (uint32_t)n;
            }
            void** dst = k == 0 ? (void**)value : (void**)ptr;
            if (dst) *dst = res;
        }
        free(buf);
        return ret;
    }
    case effVendorSpecific:                                         // only without data (e.g. the mouse wheel 'stCA' / 'Whee')
        if (ptr) return 0;
        return call(p, r, 0) ? (intptr_t)r->ret : 0;
    }
    return 0;
}

static intptr_t VSTCALL vst_dispatch(AEffect* fx, int32_t op, int32_t index, intptr_t value, void* ptr, float opt)
{
    Inst* p = (Inst*)fx;
    int size, m = disp_marshal(op, &size);
    Req r; memset(&r, 0, sizeof r);
    r.kind = VB_K_DISPATCH; r.op = op; r.index = index; r.value = (int64_t)value; r.opt = opt;
    if (op != effClose && p->dead) return 0;
    int editor = op == effEditClose || op == effEditKeyDown || op == effEditKeyUp || op == effSetEditKnobMode;
    switch (m) {
    case M_NONE:
        if (ptr && op > effGetNumMidiOutputChannels) return 0;      // unknown opcode with data we cannot pass
        break;
    case M_STR_IN:
        if (ptr) { r.in = ptr; r.in_len = (uint32_t)strnlen((const char*)ptr, VB_STR_MAX - 1) + 1; }
        break;
    case M_STR_OUT:
        if (ptr) { r.out = ptr; r.out_cap = VB_STR_MAX; }
        break;
    case M_IN:
        if (ptr) { r.in = ptr; r.in_len = (uint32_t)size; }
        break;
    case M_INOUT:
        if (!ptr) return 0;
        r.in = ptr; r.in_len = (uint32_t)size; r.out = ptr; r.out_cap = (uint32_t)size;
        break;
    case M_SPECIAL:
        return special(p, &r, ptr, value);
    default:                                                        // M_LOCAL, M_REFUSE
        return 0;
    }
    return call(p, &r, editor) ? (intptr_t)r.ret : 0;
}

// ----------------------------------------------------------------------------- entry

static void set_names(void)
{
    wchar_t full[MAX_PATH];
    if (!GetModuleFileNameW(g_self, full, MAX_PATH)) wcscpy(full, L".\\x.dll");
    for (wchar_t* q = full; *q; q++) if (*q == L'/') *q = L'\\';
    wchar_t* slash = wcsrchr(full, L'\\');
    wchar_t* base = slash ? slash + 1 : full;
    wcsncpy(g_self_name, base, MAX_PATH - 1);
    if (slash) { *slash = 0; wcsncpy(g_dir, full, MAX_PATH - 1); } else wcscpy(g_dir, L".");
    wcsncpy(g_stem, base, MAX_PATH - 1);
    wchar_t* dot = wcsrchr(g_stem, L'.'); if (dot) *dot = 0;
    size_t n = wcslen(g_stem);                                      // syxg50_x64 -> syxg50
    if (n > 4 && !_wcsicmp(g_stem + n - 4, VB_SUFFIX)) g_stem[n - 4] = 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) { (void)r; if (reason == DLL_PROCESS_ATTACH) g_self = (HMODULE)h; return TRUE; }

AEffect* VSTPluginMain(AudioMasterCallback master)
{
    if (!g_stem[0]) set_names();
    Inst* p = (Inst*)calloc(1, sizeof *p);
    if (!p) return 0;
    InitializeCriticalSection(&p->audio_cs); InitializeCriticalSection(&p->ev_cs);
    p->master = master; p->loading = 1;
    AEffect* fx = &p->fx;
    fx->magic = kEffectMagic; fx->dispatcher = vst_dispatch; fx->process = vst_process;
    fx->setParameter = vst_set_parameter; fx->getParameter = vst_get_parameter;
    fx->processReplacing = vst_process_replacing; fx->processDoubleReplacing = vst_process_double;
    fx->object = p;
    if (!start(p)) {
        log_error(p->error);
        stop(p);
        DeleteCriticalSection(&p->audio_cs); DeleteCriticalSection(&p->ev_cs);
        free(p);
        return 0;
    }
    sync_info(p);
    p->loading = 0;
    return fx;
}

AEffect* vb_main(AudioMasterCallback master) { return VSTPluginMain(master); }     // exported as "main" (old hosts)
int VST2BridgeStub(void) { return VB_VERSION; }                                     // marker for the patcher
