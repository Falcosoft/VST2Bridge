// host.c - the helper process. Built twice from this file: vst2bridge-host32.exe (runs 32-bit plugins for the
// 64-bit bridge) and vst2bridge-host64.exe (runs 64-bit plugins for the 32-bit bridge).
//   vst2bridge-hostNN.exe <plugin.dll> <object prefix> <shell id> <host pid>
// Loads one plugin and serves the bridge's calls (protocol.h): the main thread runs the window message loop and the
// calls of the host's GUI / control thread, a second thread the calls of the host's audio thread.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "vst2.h"
#include "protocol.h"
#include "marshal.h"

static VbShared* sh;
static HANDLE   ev_ready, mutex, ev_main_h, ev_main_s, ev_main_v, ev_audio_h, ev_audio_s, ev_aux_h, ev_aux_s;
static AEffect* fx;
static DWORD    main_tid, audio_tid;
static int      main_active, audio_active;      // >0: the thread takes part in a call on its channel
static CRITICAL_SECTION aux_cs;
static volatile LONG quitting, closing;
static int32_t  shell_id;
static void*    chunk_ptr; static size_t chunk_size;    // result of the last effGetChunk
static HWND     editor_wnd;
static int      editor_open, ed_w, ed_h;
static VstTimeInfo time_info[3];                // per channel: the plugin keeps the pointer until its next call
static char     directory[VB_STR_MAX];

typedef struct {
    int32_t op, index; int64_t value; float opt;
    const void* in; uint32_t in_len;
    void* out; uint32_t out_cap; uint32_t out_len;
    int64_t ret;
} Req;

static void serve_req(VbFrame* f);

static void fill_info(void)
{
    VbInfo* i = &sh->info;
    if (!fx) return;
    i->numPrograms = fx->numPrograms; i->numParams = fx->numParams; i->numInputs = fx->numInputs; i->numOutputs = fx->numOutputs;
    i->flags = fx->flags; i->initialDelay = fx->initialDelay; i->uniqueID = fx->uniqueID; i->version = fx->version;
    i->ioRatio = fx->ioRatio; i->has_replacing = fx->processReplacing != 0; i->has_double = fx->processDoubleReplacing != 0;
}

// ----------------------------------------------------------------------------- channel plumbing

static int hwait(HANDLE ev, int pump)
{
    for (;;) {
        DWORD r = pump ? MsgWaitForMultipleObjects(1, &ev, FALSE, INFINITE, QS_SENDMESSAGE) : WaitForSingleObject(ev, INFINITE);
        if (r == WAIT_OBJECT_0) return 1;
        if (pump && r == WAIT_OBJECT_0 + 1) { MSG m; PeekMessageW(&m, 0, 0, 0, PM_NOREMOVE); continue; }
        return 0;
    }
}

static HANDLE main_to_stub(void) { return sh->main.owner == VB_OWNER_HELPER ? ev_main_v : ev_main_s; }

// serves the request on top of ch and replies
static void serve_top(VbChan* ch, int* active, HANDLE to)
{
    MemoryBarrier();
    if (ch->msg != VB_MSG_REQ || ch->depth < 1) return;
    (*active)++;
    serve_req(&ch->f[ch->depth - 1]);
    (*active)--;
    fill_info();
    MemoryBarrier();
    ch->msg = VB_MSG_REPLY;
    SetEvent(to);
}

// one call to the bridge; serves its nested calls meanwhile
static int hexchange(VbChan* ch, HANDLE to, HANDLE in, int pump, int* active, Req* r)
{
    int d = ch->depth;
    if (d >= VB_MAXDEPTH || r->in_len > VB_FRAME_DATA) return 0;
    VbFrame* f = &ch->f[d];
    f->kind = VB_K_MASTER; f->op = r->op; f->index = r->index; f->flags = 0;
    f->value = r->value; f->opt = r->opt; f->ret = 0; f->len = r->in_len;
    if (r->in_len) memcpy(f->data, r->in, r->in_len);
    fill_info();
    ch->depth = d + 1;
    MemoryBarrier();
    ch->msg = VB_MSG_REQ;
    (*active)++;
    SetEvent(to);
    int ok = 1;
    for (;;) {
        if (!hwait(in, pump)) { ok = 0; break; }
        MemoryBarrier();
        if (ch->msg == VB_MSG_REPLY && ch->depth == d + 1) break;
        if (ch->msg == VB_MSG_REQ && ch->depth > d + 1) serve_top(ch, active, to);
    }
    (*active)--;
    r->ret = f->ret;
    r->out_len = f->len < r->out_cap ? f->len : r->out_cap;
    if (ok && r->out && r->out_len) memcpy(r->out, f->data, r->out_len);
    ch->depth = d;
    return ok;
}

// main thread, outside a call: take the mutex; serve the bridge's calls while waiting for it
static int lock_main(void)
{
    HANDLE w[2] = { mutex, ev_main_h };
    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(2, w, FALSE, INFINITE, QS_SENDMESSAGE);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED_0) return 1;
        if (r == WAIT_OBJECT_0 + 1) { serve_top(&sh->main, &main_active, main_to_stub()); continue; }
        if (r == WAIT_OBJECT_0 + 2) { MSG m; PeekMessageW(&m, 0, 0, 0, PM_NOREMOVE); continue; }
        return 0;
    }
}

static int call_host(Req* r)
{
    DWORD me = GetCurrentThreadId();
    if (me == main_tid) {
        if (main_active) return hexchange(&sh->main, main_to_stub(), ev_main_h, 1, &main_active, r);
        if (!lock_main()) return 0;
        sh->main.owner = VB_OWNER_HELPER;
        int ok = hexchange(&sh->main, ev_main_v, ev_main_h, 1, &main_active, r);
        sh->main.owner = VB_OWNER_NONE;
        ReleaseMutex(mutex);
        return ok;
    }
    if (me == audio_tid && audio_active) return hexchange(&sh->audio, ev_audio_s, ev_audio_h, 0, &audio_active, r);
    EnterCriticalSection(&aux_cs);                                  // any other thread of the plugin
    int dummy = 0;
    int ok = hexchange(&sh->aux, ev_aux_s, ev_aux_h, 0, &dummy, r);
    LeaveCriticalSection(&aux_cs);
    return ok;
}

// ----------------------------------------------------------------------------- plugin -> host

static intptr_t VSTCALL master(AEffect* e, int32_t op, int32_t idx, intptr_t val, void* ptr, float opt)
{
    (void)e;
    if (op == audioMasterCurrentId) return shell_id;
    if (op == audioMasterVersion && closing) return 2400;
    if (closing || !sh) return 0;
    int size, m = master_marshal(op, &size);
    Req r; memset(&r, 0, sizeof r);
    r.op = op; r.index = idx; r.value = (int64_t)val; r.opt = opt;
    int ch = GetCurrentThreadId() == main_tid ? 0 : GetCurrentThreadId() == audio_tid ? 1 : 2;
    switch (m) {
    case M_NONE:
        return call_host(&r) ? (intptr_t)r.ret : 0;
    case M_STR_IN:
        if (ptr) { r.in = ptr; r.in_len = (uint32_t)strnlen((const char*)ptr, VB_STR_MAX - 1) + 1; }
        return call_host(&r) ? (intptr_t)r.ret : 0;
    case M_STR_OUT:
        if (ptr) { r.out = ptr; r.out_cap = 256; }                  // kVstMaxVendorStrLen / kVstMaxProductStrLen are 64
        return call_host(&r) ? (intptr_t)r.ret : 0;
    case M_SPECIAL:
        switch (op) {
        case audioMasterGetTime:
            r.out = &time_info[ch]; r.out_cap = sizeof(VstTimeInfo);
            return call_host(&r) && r.ret && r.out_len == sizeof(VstTimeInfo) ? (intptr_t)&time_info[ch] : 0;
        case audioMasterGetDirectory:
            r.out = directory; r.out_cap = sizeof directory - 1;
            if (!call_host(&r) || !r.ret) return 0;
            directory[r.out_len] = 0;
            return (intptr_t)directory;
        case audioMasterVendorSpecific:
            return ptr ? 0 : call_host(&r) ? (intptr_t)r.ret : 0;
        case audioMasterSizeWindow:
            if (editor_wnd && idx > 0 && val > 0) {
                ed_w = idx; ed_h = (int)val;
                SetWindowPos(editor_wnd, 0, 0, 0, ed_w, ed_h, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            }
            return call_host(&r) ? (intptr_t)r.ret : 0;
        case audioMasterProcessEvents: {                            // MIDI out of the plugin
            const VstEvents* evs = (const VstEvents*)ptr;
            if (!evs || evs->numEvents <= 0) return 0;
            uint8_t* buf = (uint8_t*)malloc(VB_FRAME_DATA);
            if (!buf) return 0;
            uint32_t n = 0, cnt = evs->numEvents > VB_MAXEVENTS ? VB_MAXEVENTS : (uint32_t)evs->numEvents;
            VbEvent* out = (VbEvent*)(buf + 4);
            size_t sx_room = VB_FRAME_DATA - 4 - cnt * sizeof(VbEvent), sx_used = 0;
            uint8_t* sx = buf + 4 + cnt * sizeof(VbEvent);
            for (uint32_t i = 0; i < cnt; i++) {
                const VstEvent* ev = evs->events[i];
                if (!ev) continue;
                VbEvent* q = &out[n]; memset(q, 0, sizeof *q);
                q->type = ev->type; q->delta = ev->deltaFrames; q->flags = ev->flags;
                if (ev->type == kVstMidiType) {
                    const VstMidiEvent* me = (const VstMidiEvent*)ev;
                    memcpy(q->midi, me->midiData, 4); q->note_length = me->noteLength; q->note_offset = me->noteOffset;
                    q->detune = me->detune; q->off_velocity = (uint8_t)me->noteOffVelocity;
                } else if (ev->type == kVstSysExType) {
                    const VstMidiSysexEvent* se = (const VstMidiSysexEvent*)ev;
                    size_t len = se->dumpBytes > 0 ? (size_t)se->dumpBytes : 0;
                    if (!se->sysexDump || !len || sx_used + len > sx_room) continue;
                    q->sysex_off = (uint32_t)sx_used; q->sysex_len = (uint32_t)len;
                    memcpy(sx + sx_used, se->sysexDump, len); sx_used += len;
                } else continue;
                n++;
            }
            // pack: the events, then the SysEx bytes right behind them
            if (n < cnt) memmove(buf + 4 + n * sizeof(VbEvent), sx, sx_used);
            memcpy(buf, &n, 4);
            r.in = buf; r.in_len = (uint32_t)(4 + n * sizeof(VbEvent) + sx_used);
            intptr_t ret = call_host(&r) ? (intptr_t)r.ret : 0;
            free(buf);
            return ret;
        }
        }
        return 0;
    default:
        return 0;                                                   // M_REFUSE: not supported across processes
    }
}

// ----------------------------------------------------------------------------- editor window

static LRESULT CALLBACK ed_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_TIMER && fx && editor_open) { fx->dispatcher(fx, effEditIdle, 0, 0, 0, 0); return 0; }
    return DefWindowProcW(w, m, wp, lp);
}

static void edit_rect(void)
{
    ERect* r = 0;
    fx->dispatcher(fx, effEditGetRect, 0, 0, &r, 0);
    if (r && r->right > r->left && r->bottom > r->top) { ed_w = r->right - r->left; ed_h = r->bottom - r->top; }
}

static intptr_t edit_open(HWND parent)
{
    static int registered;
    if (editor_open) return 1;
    if (!(fx->flags & effFlagsHasEditor)) return 0;
    if (!registered) {
        WNDCLASSW wc; memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = ed_proc; wc.hInstance = GetModuleHandleW(0); wc.lpszClassName = L"VST2BridgeEditor";
        wc.hCursor = LoadCursorW(0, (LPCWSTR)IDC_ARROW);
        RegisterClassW(&wc); registered = 1;
    }
    edit_rect();
    if (ed_w <= 0) { ed_w = 640; ed_h = 480; }
    // a child of the host's window, although that window belongs to another process (Windows allows this)
    editor_wnd = CreateWindowExW(0, L"VST2BridgeEditor", L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                 0, 0, ed_w, ed_h, parent, 0, GetModuleHandleW(0), 0);
    if (!editor_wnd) return 0;
    intptr_t ret = fx->dispatcher(fx, effEditOpen, 0, 0, editor_wnd, 0);
    editor_open = 1;
    edit_rect();                                                    // some plugins know their size only once open
    SetWindowPos(editor_wnd, 0, 0, 0, ed_w, ed_h, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    SetTimer(editor_wnd, 1, 30, 0);
    ShowWindow(editor_wnd, SW_SHOWNA);
    return ret ? ret : 1;
}

static void edit_close(void)
{
    if (!editor_open) return;
    KillTimer(editor_wnd, 1);
    fx->dispatcher(fx, effEditClose, 0, 0, 0, 0);
    editor_open = 0;
    DestroyWindow(editor_wnd); editor_wnd = 0;
}

// ----------------------------------------------------------------------------- host -> plugin

static void do_process(VbFrame* f)
{
    static VstMidiEvent mev[VB_MAXEVENTS];
    static VstMidiSysexEvent sev[VB_MAXEVENTS];
    static char evbuf[sizeof(VstEvents) + VB_MAXEVENTS * sizeof(VstEvent*)];
    static double zero[VB_MAXFRAMES], sink[VB_MAXFRAMES];
    static void* ins[256]; static void* outs[256];
    VbAudio* a = &sh->au;
    int n = (int)f->value, dbl = f->op == VB_P_DOUBLE;
    size_t es = dbl ? sizeof(double) : sizeof(float);
    if (n < 0) n = 0;
    if (n > VB_MAXFRAMES) n = VB_MAXFRAMES;

    uint32_t ne = a->n_events > VB_MAXEVENTS ? VB_MAXEVENTS : a->n_events, cnt = 0;
    VstEvents* list = (VstEvents*)evbuf;
    for (uint32_t i = 0; i < ne; i++) {
        const VbEvent* e = &a->events[i];
        if (e->type == kVstSysExType) {
            if ((uint64_t)e->sysex_off + e->sysex_len > VB_SYSEX_MAX) continue;
            memset(&sev[i], 0, sizeof sev[i]);
            sev[i].type = kVstSysExType; sev[i].byteSize = sizeof sev[i]; sev[i].deltaFrames = e->delta; sev[i].flags = e->flags;
            sev[i].dumpBytes = (int32_t)e->sysex_len; sev[i].sysexDump = (char*)a->sysex + e->sysex_off;
            list->events[cnt++] = (VstEvent*)&sev[i];
        } else {
            memset(&mev[i], 0, sizeof mev[i]);
            mev[i].type = kVstMidiType; mev[i].byteSize = sizeof mev[i]; mev[i].deltaFrames = e->delta; mev[i].flags = e->flags;
            mev[i].noteLength = e->note_length; mev[i].noteOffset = e->note_offset;
            memcpy(mev[i].midiData, e->midi, 4); mev[i].detune = e->detune; mev[i].noteOffVelocity = (char)e->off_velocity;
            list->events[cnt++] = (VstEvent*)&mev[i];
        }
    }
    if (cnt) { list->numEvents = (int32_t)cnt; list->reserved = 0; fx->dispatcher(fx, effProcessEvents, 0, 0, list, 0); }

    int nin = fx->numInputs < 256 ? fx->numInputs : 256, nout = fx->numOutputs < 256 ? fx->numOutputs : 256;
    memset(zero, 0, n * es);
    for (int c = 0; c < nin; c++) ins[c] = c < a->nin ? (void*)a->in[c] : (void*)zero;
    for (int c = 0; c < nout; c++) { outs[c] = c < VB_MAXCH ? (void*)a->out[c] : (void*)sink; memset(outs[c], 0, n * es); }
    if (dbl && fx->processDoubleReplacing) fx->processDoubleReplacing(fx, (double**)ins, (double**)outs, n);
    else if (fx->processReplacing && (fx->flags & effFlagsCanReplacing)) fx->processReplacing(fx, (float**)ins, (float**)outs, n);
    else if (fx->process) fx->process(fx, (float**)ins, (float**)outs, n);        // accumulates into the cleared outputs
    f->len = 0;
}

static void* open_view(const char* name, HANDLE* m)
{
    *m = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    void* v = *m ? MapViewOfFile(*m, FILE_MAP_ALL_ACCESS, 0, 0, 0) : 0;
    if (*m && !v) { CloseHandle(*m); *m = 0; }
    return v;
}

static void do_dispatch(VbFrame* f)
{
    int size, m = disp_marshal(f->op, &size);
    uint8_t* d = f->data;
    intptr_t ret = 0, val = (intptr_t)f->value;
    uint32_t in_len = f->len;
    f->len = 0;
    if (f->op == effEditClose) { edit_close(); f->ret = 0; return; }
    switch (m) {
    case M_NONE: ret = fx->dispatcher(fx, f->op, f->index, val, 0, f->opt); break;
    case M_STR_IN:
        if (in_len) d[in_len - 1] = 0;
        ret = fx->dispatcher(fx, f->op, f->index, val, in_len ? d : 0, f->opt);
        break;
    case M_STR_OUT: {
        char buf[VB_STR_MAX]; memset(buf, 0, sizeof buf);
        ret = fx->dispatcher(fx, f->op, f->index, val, buf, f->opt);
        buf[VB_STR_MAX - 1] = 0;
        f->len = (uint32_t)strlen(buf) + 1; memcpy(d, buf, f->len);
        break;
    }
    case M_IN: ret = fx->dispatcher(fx, f->op, f->index, val, in_len >= (uint32_t)size ? d : 0, f->opt); break;
    case M_INOUT:
        if (in_len < (uint32_t)size) memset(d + in_len, 0, size - in_len);
        ret = fx->dispatcher(fx, f->op, f->index, val, d, f->opt);
        f->len = (uint32_t)size;
        break;
    case M_SPECIAL:
        switch (f->op) {
        case effEditGetRect: {
            ERect* r = 0;
            ret = fx->dispatcher(fx, f->op, 0, 0, &r, 0);
            if (r) { memcpy(d, r, sizeof *r); f->len = sizeof *r; }
            break;
        }
        case effEditOpen: ret = edit_open((HWND)(uintptr_t)(uint32_t)f->value); break;
        case effGetChunk: {
            void* cp = 0;
            ret = fx->dispatcher(fx, f->op, f->index, 0, &cp, 0);
            chunk_ptr = cp; chunk_size = ret > 0 && cp ? (size_t)ret : 0;
            if (chunk_size && chunk_size <= VB_FRAME_DATA) { memcpy(d, cp, chunk_size); f->len = (uint32_t)chunk_size; }
            break;
        }
        case effSetChunk:
            if (f->flags & VB_F_MAPPED) {
                if (in_len) d[in_len - 1] = 0;
                HANDLE mh; void* v = open_view((const char*)d, &mh);
                if (v) { ret = fx->dispatcher(fx, f->op, f->index, val, v, 0); UnmapViewOfFile(v); CloseHandle(mh); }
            } else ret = fx->dispatcher(fx, f->op, f->index, (intptr_t)in_len, d, 0);
            break;
        case effSetSpeakerArrangement: {
            int32_t si = 0;
            if (in_len >= 4) memcpy(&si, d, 4);
            if (si <= 0 || 4 + (uint32_t)si >= in_len) break;
            ret = fx->dispatcher(fx, f->op, f->index, (intptr_t)(d + 4), d + 4 + si, f->opt);
            break;
        }
        case effGetSpeakerArrangement: {
            void* pin = 0; void* pout = 0;
            ret = fx->dispatcher(fx, f->op, f->index, (intptr_t)&pin, &pout, f->opt);
            uint32_t off = 0;
            void* src[2] = { pin, pout };
            for (int k = 0; k < 2; k++) {
                int32_t n = src[k] ? speaker_arr_size(src[k]) : 0;
                if (off + 4 + (uint32_t)n > VB_FRAME_DATA) n = 0;
                memcpy(d + off, &n, 4); off += 4;
                if (n) { memcpy(d + off, src[k], n); off += (uint32_t)n; }
            }
            f->len = off;
            break;
        }
        case effVendorSpecific: ret = fx->dispatcher(fx, f->op, f->index, val, 0, f->opt); break;
        }
        break;
    default: break;
    }
    f->ret = ret;
}

static void serve_req(VbFrame* f)
{
    switch (f->kind) {
    case VB_K_DISPATCH: do_dispatch(f); break;
    case VB_K_SETPARAM: fx->setParameter(fx, f->index, f->opt); f->len = 0; break;
    case VB_K_GETPARAM: f->opt = fx->getParameter(fx, f->index); f->len = 0; break;
    case VB_K_PROCESS:  do_process(f); break;
    case VB_K_READ_CHUNK: {
        f->ret = 0;
        if (f->len) f->data[f->len - 1] = 0;
        HANDLE mh; void* v = open_view((const char*)f->data, &mh);
        if (v) {
            if (chunk_ptr && chunk_size == (size_t)f->value) { memcpy(v, chunk_ptr, chunk_size); f->ret = 1; }
            UnmapViewOfFile(v); CloseHandle(mh);
        }
        f->len = 0;
        break;
    }
    case VB_K_QUIT: InterlockedExchange(&quitting, 1); f->ret = 1; f->len = 0; break;
    default: f->ret = 0; f->len = 0; break;
    }
}

// ----------------------------------------------------------------------------- threads

static DWORD WINAPI audio_thread(LPVOID a)
{
    (void)a;
    audio_tid = GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    while (hwait(ev_audio_h, 0) && !quitting) serve_top(&sh->audio, &audio_active, ev_audio_s);
    return 0;
}

static DWORD WINAPI watchdog(LPVOID a)                  // the host is gone (crashed): end as well
{
    HANDLE h = (HANDLE)a;
    WaitForSingleObject(h, INFINITE);
    ExitProcess(0);
    return 0;
}

static void fail(const char* msg)
{
    snprintf(sh->error, sizeof sh->error, "%s", msg);
    sh->ok = 0;
    SetEvent(ev_ready);
}

static HANDLE open_obj(const char* prefix, const char* suffix, int kind)
{
    char name[160];
    snprintf(name, sizeof name, "%s%s", prefix, suffix);
    if (kind == 0) return OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, name);
    if (kind == 1) return OpenMutexA(MUTEX_MODIFY_STATE | SYNCHRONIZE, FALSE, name);
    return OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
}

typedef AEffect* (VSTCALL *VstMain)(AudioMasterCallback);

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    (void)hi; (void)hp; (void)cmd; (void)show;
    int argc; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 5) return 2;
    char prefix[128];
    WideCharToMultiByte(CP_ACP, 0, argv[2], -1, prefix, sizeof prefix, 0, 0);
    shell_id = (int32_t)wcstol(argv[3], 0, 10);
    DWORD host_pid = (DWORD)wcstoul(argv[4], 0, 10);

    HANDLE map = open_obj(prefix, VB_OBJ_MAP, 2);
    ev_ready = open_obj(prefix, VB_OBJ_READY, 0); mutex = open_obj(prefix, VB_OBJ_MUTEX, 1);
    ev_main_h = open_obj(prefix, VB_OBJ_MAIN_H, 0); ev_main_s = open_obj(prefix, VB_OBJ_MAIN_S, 0); ev_main_v = open_obj(prefix, VB_OBJ_MAIN_V, 0);
    ev_audio_h = open_obj(prefix, VB_OBJ_AUDIO_H, 0); ev_audio_s = open_obj(prefix, VB_OBJ_AUDIO_S, 0);
    ev_aux_h = open_obj(prefix, VB_OBJ_AUX_H, 0); ev_aux_s = open_obj(prefix, VB_OBJ_AUX_S, 0);
    if (!map || !ev_ready || !mutex || !ev_main_h || !ev_main_s || !ev_main_v || !ev_audio_h || !ev_audio_s || !ev_aux_h || !ev_aux_s) return 3;
    sh = (VbShared*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!sh || sh->magic != VB_MAGIC || sh->version != VB_VERSION) return 3;
    InitializeCriticalSection(&aux_cs);
    main_tid = GetCurrentThreadId();
    HANDLE hp_ = OpenProcess(SYNCHRONIZE, FALSE, host_pid);
    if (hp_) CreateThread(0, 0, watchdog, hp_, 0, 0);

    // the plugin finds its files next to itself (the bridge also starts the helper in the plugin's folder)
    HMODULE dll = LoadLibraryExW(argv[1], 0, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) { char m[200]; snprintf(m, sizeof m, "cannot load the plugin (error %lu)", GetLastError()); fail(m); return 4; }
    VstMain entry = (VstMain)(void*)GetProcAddress(dll, "VSTPluginMain");
    if (!entry) entry = (VstMain)(void*)GetProcAddress(dll, "main");
    if (!entry) { fail("the DLL has no VSTPluginMain: not a VST2 plugin"); return 4; }
    fx = entry(master);
    if (!fx || fx->magic != kEffectMagic) { fail("the plugin did not start (VSTPluginMain returned no plugin)"); return 4; }
    fill_info();
    sh->ok = 1;
    HANDLE at = CreateThread(0, 0, audio_thread, 0, 0, 0);
    SetEvent(ev_ready);

    while (!quitting) {
        DWORD r = MsgWaitForMultipleObjects(1, &ev_main_h, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) serve_top(&sh->main, &main_active, main_to_stub());
        else if (r == WAIT_OBJECT_0 + 1) {
            MSG msg;
            while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        } else break;
    }
    edit_close();
    InterlockedExchange(&closing, 1);
    InterlockedExchange(&quitting, 1);
    SetEvent(ev_audio_h);
    WaitForSingleObject(at, 2000);
    fx->dispatcher(fx, effClose, 0, 0, 0, 0);
    return 0;
}
