// testclap.c - a minimal CLAP host for the tests of the CLAP bridge (built as 32-bit and 64-bit):
//   testclap.exe <name>_xNN.clap
// The .clap loads the VST2 bridge <name>_xNN.dll next to it, which runs test/fakevst.c in the other bitness.
// Exit code = number of failures.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../clap/clap.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); fails++; } else printf("ok:   "); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static volatile LONG n_flush, n_callback, n_resize, n_latency, n_restart, last_w, last_h;
static void h_log(const clap_host_t* h, clap_log_severity s, const char* m) { (void)h; (void)s; printf("      plugin log: %s\n", m); }
static const clap_host_log_t ext_log = { h_log };
static void hp_rescan(const clap_host_t* h, clap_param_rescan_flags f) { (void)h; (void)f; }
static void hp_clear(const clap_host_t* h, clap_id id, clap_param_clear_flags f) { (void)h; (void)id; (void)f; }
static void hp_flush(const clap_host_t* h) { (void)h; InterlockedIncrement(&n_flush); }
static const clap_host_params_t ext_params = { hp_rescan, hp_clear, hp_flush };
static void hg_hints(const clap_host_t* h) { (void)h; }
static bool hg_resize(const clap_host_t* h, uint32_t w, uint32_t hh) { (void)h; InterlockedIncrement(&n_resize); last_w = (LONG)w; last_h = (LONG)hh; return true; }
static bool hg_show(const clap_host_t* h) { (void)h; return true; }
static void hg_closed(const clap_host_t* h, bool d) { (void)h; (void)d; }
static const clap_host_gui_t ext_gui = { hg_hints, hg_resize, hg_show, hg_show, hg_closed };
static void hl_changed(const clap_host_t* h) { (void)h; InterlockedIncrement(&n_latency); }
static const clap_host_latency_t ext_lat = { hl_changed };
static const void* host_ext(const clap_host_t* h, const char* id)
{
    (void)h;
    if (!strcmp(id, CLAP_EXT_LOG)) return &ext_log;
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &ext_params;
    if (!strcmp(id, CLAP_EXT_GUI)) return &ext_gui;
    if (!strcmp(id, CLAP_EXT_LATENCY)) return &ext_lat;
    return 0;
}
static void h_restart(const clap_host_t* h) { (void)h; InterlockedIncrement(&n_restart); }
static void h_process(const clap_host_t* h) { (void)h; }
static void h_callback(const clap_host_t* h) { (void)h; InterlockedIncrement(&n_callback); }

// events in and out
typedef struct { clap_input_events_t in; const clap_event_header_t* ev[16]; uint32_t n; } InEv;
static uint32_t in_size(const clap_input_events_t* l) { return ((InEv*)l->ctx)->n; }
static const clap_event_header_t* in_get(const clap_input_events_t* l, uint32_t i) { return ((InEv*)l->ctx)->ev[i]; }
typedef struct { clap_output_events_t out; int n, midi_on, value_idx[32], gestures; double value[32]; int nvalues; } OutEv;
static bool out_push(const clap_output_events_t* l, const clap_event_header_t* h)
{
    OutEv* o = (OutEv*)l->ctx; o->n++;
    if (h->type == CLAP_EVENT_MIDI && (((const clap_event_midi_t*)h)->data[0] & 0xF0) == 0x90) o->midi_on++;
    if (h->type == CLAP_EVENT_PARAM_VALUE && o->nvalues < 32) {
        const clap_event_param_value_t* v = (const clap_event_param_value_t*)h;
        o->value_idx[o->nvalues] = (int)v->param_id; o->value[o->nvalues++] = v->value;
    }
    if (h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || h->type == CLAP_EVENT_PARAM_GESTURE_END) o->gestures++;
    return true;
}
static void out_init(OutEv* o) { memset(o, 0, sizeof *o); o->out.ctx = o; o->out.try_push = out_push; }
static void in_init(InEv* e) { memset(e, 0, sizeof *e); e->in.ctx = e; e->in.size = in_size; e->in.get = in_get; }

typedef struct { uint8_t buf[300 * 1024]; uint32_t len, pos; } Mem;
static int64_t m_write(const clap_ostream_t* s, const void* b, uint64_t n) { Mem* m = s->ctx; if (m->len + n > sizeof m->buf) return -1; memcpy(m->buf + m->len, b, n); m->len += (uint32_t)n; return (int64_t)n; }
static int64_t m_read(const clap_istream_t* s, void* b, uint64_t n) { Mem* m = s->ctx; uint64_t r = m->len - m->pos < n ? m->len - m->pos : n; memcpy(b, m->buf + m->pos, r); m->pos += (uint32_t)r; return (int64_t)r; }

static float inL[512], inR[512], outL[512], outR[512];
static void run(const clap_plugin_t* pl, InEv* e, OutEv* o, uint32_t frames, const clap_event_transport_t* tr)
{
    float* ic[2] = { inL, inR }; float* oc[2] = { outL, outR };
    clap_audio_buffer_t in = { ic, 0, 2, 0, 0 }, out = { oc, 0, 2, 0, 0 };
    clap_process_t pr; memset(&pr, 0, sizeof pr);
    pr.frames_count = frames; pr.steady_time = -1; pr.transport = tr;
    pr.audio_inputs = &in; pr.audio_inputs_count = 1; pr.audio_outputs = &out; pr.audio_outputs_count = 1;
    pr.in_events = &e->in; pr.out_events = &o->out;
    pl->process(pl, &pr);
}

static void pump(const clap_plugin_t* pl, int ms)
{
    DWORD t0 = GetTickCount();
    while ((int)(GetTickCount() - t0) < ms) {
        MSG m;
        while (PeekMessageA(&m, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
        if (n_callback) { InterlockedExchange(&n_callback, 0); pl->on_main_thread(pl); }
        MsgWaitForMultipleObjects(0, 0, FALSE, 10, QS_ALLINPUT);
    }
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: testclap <bridge.clap>\n"); return 99; }
    const char* other = sizeof(void*) == 8 ? "Fake32" : "Fake64";
    printf("host is %d-bit, CLAP %s\n", (int)sizeof(void*) * 8, argv[1]);
    HMODULE m = LoadLibraryA(argv[1]);
    CHECK(m != 0, "load the CLAP");
    if (!m) return 1;
    CHECK(GetProcAddress(m, "VST2BridgeClap") != 0, "exports VST2BridgeClap");
    const clap_plugin_entry_t* entry = (const clap_plugin_entry_t*)(void*)GetProcAddress(m, "clap_entry");
    CHECK(entry && entry->init(argv[1]), "clap_entry init");
    if (!entry) return 1;
    const clap_plugin_factory_t* f = (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    CHECK(f && f->get_plugin_count(f) == 1, "factory with one plugin");
    if (!f || f->get_plugin_count(f) != 1) return 1;
    const clap_plugin_descriptor_t* d = f->get_plugin_descriptor(f, 0);
    printf("      id=%s name=%s vendor=%s version=%s\n", d->id, d->name, d->vendor, d->version);
    CHECK(!strcmp(d->name, "FakeVST") && !strcmp(d->vendor, other), "descriptor from the plugin in the other bitness (%s, %s)", d->name, d->vendor);
    CHECK(!strcmp(d->id, "io.github.nightfright2k19.vst2bridge.fake"), "id from the file name");
    CHECK(d->features && d->features[0] && !strcmp(d->features[0], CLAP_PLUGIN_FEATURE_INSTRUMENT), "feature instrument (effFlagsIsSynth)");

    clap_host_t host = { CLAP_VERSION_INIT, 0, "testclap", "VST2Bridge", "", "1", host_ext, h_restart, h_process, h_callback };
    const clap_plugin_t* pl = f->create_plugin(f, &host, d->id);
    CHECK(pl && pl->init(pl), "create and init");
    if (!pl) return 1;

    const clap_plugin_audio_ports_t* ap = pl->get_extension(pl, CLAP_EXT_AUDIO_PORTS);
    clap_audio_port_info_t pi;
    CHECK(ap && ap->count(pl, true) == 1 && ap->count(pl, false) == 1 && ap->get(pl, 0, false, &pi) && pi.channel_count == 2,
          "audio ports: stereo in, stereo out");
    const clap_plugin_note_ports_t* np = pl->get_extension(pl, CLAP_EXT_NOTE_PORTS);
    CHECK(np && np->count(pl, true) == 1 && np->count(pl, false) == 1, "note ports: MIDI in and out");
    const clap_plugin_params_t* pp = pl->get_extension(pl, CLAP_EXT_PARAMS);
    clap_param_info_t info; double v = 0; char txt[64] = "";
    CHECK(pp && pp->count(pl) == 3 && pp->get_info(pl, 1, &info) && !strcmp(info.name, "Param1"), "parameters: 3, name %s", pp ? info.name : "");
    CHECK(pp->get_info(pl, 0, &info) && fabs(info.default_value - 0.5) < 1e-6, "default value %.2f", info.default_value);
    CHECK(pp->get_value(pl, 0, &v) && fabs(v - 0.5) < 1e-6 && pp->value_to_text(pl, 0, v, txt, sizeof txt) && !strcmp(txt, "0.50 dB"),
          "value %.2f shown as '%s'", v, txt);
    CHECK(pl->get_extension(pl, CLAP_EXT_GUI) && pl->get_extension(pl, CLAP_EXT_STATE), "gui and state extensions");

    CHECK(pl->activate(pl, 44100, 1, 512), "activate");
    const clap_plugin_latency_t* lat = pl->get_extension(pl, CLAP_EXT_LATENCY);
    CHECK(lat && lat->get(pl) == 64 && n_latency == 1, "latency 64 reported during activate");
    CHECK(pl->start_processing(pl), "start processing");

    for (int i = 0; i < 512; i++) { inL[i] = 0.2f; inR[i] = -0.2f; }
    InEv e; OutEv o; in_init(&e); out_init(&o);
    clap_event_midi_t on = { { sizeof on, 100, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 }, 0, { 0x90, 60, 127 } };
    uint8_t gmon[6] = { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 };
    clap_event_midi_sysex_t sx = { { sizeof sx, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI_SYSEX, 0 }, 0, gmon, 6 };
    e.ev[0] = &sx.header; e.ev[1] = &on.header; e.n = 2;
    clap_event_transport_t tr; memset(&tr, 0, sizeof tr);
    tr.header.size = sizeof tr; tr.header.type = CLAP_EVENT_TRANSPORT; tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_IS_PLAYING; tr.tempo = 133;
    run(pl, &e, &o, 512, &tr);
    CHECK(fabsf(outL[0] - 0.1f) < 1e-6f && fabsf(outR[99] + 0.1f) < 1e-6f, "audio in -> out (gain 0.5): %f %f", outL[0], outR[99]);
    CHECK(fabsf(outL[100] - 1.1f) < 1e-6f && fabsf(outL[511] - 1.1f) < 1e-6f, "MIDI note-on at its frame: %f -> %f", outL[99], outL[100]);
    CHECK(o.midi_on == 1, "MIDI out of the plugin as a CLAP event");
    CHECK(pp->get_value(pl, 1, &v) && fabs(v - 0.5) < 1e-6, "SysEx reached the plugin");

    clap_event_param_value_t pv; memset(&pv, 0, sizeof pv);
    pv.header.size = sizeof pv; pv.header.space_id = CLAP_CORE_EVENT_SPACE_ID; pv.header.type = CLAP_EVENT_PARAM_VALUE;
    pv.param_id = 0; pv.note_id = -1; pv.port_index = -1; pv.channel = -1; pv.key = -1; pv.value = 1.0;
    clap_event_note_t off; memset(&off, 0, sizeof off);
    off.header.size = sizeof off; off.header.time = 10; off.header.space_id = CLAP_CORE_EVENT_SPACE_ID; off.header.type = CLAP_EVENT_NOTE_ON;
    off.note_id = -1; off.port_index = 0; off.channel = 0; off.key = 60; off.velocity = 0.5;
    in_init(&e); out_init(&o); e.ev[0] = &pv.header; e.ev[1] = &off.header; e.n = 2;
    run(pl, &e, &o, 512, 0);
    CHECK(fabsf(outL[0] - 1.2f) < 1e-6f, "parameter event (gain 1.0): %f", outL[0]);
    CHECK(fabsf(outL[20] - (0.2f + 64 / 127.0f)) < 1e-4f, "CLAP note event (velocity 0.5): %f", outL[20]);
    CHECK(o.nvalues == 0, "the host's own change is not sent back");

    // editor: nested sizeWindow inside effEditOpen, then begin/automate/end and sizeWindow from the plugin's GUI
    WNDCLASSA wc; memset(&wc, 0, sizeof wc); wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(0); wc.lpszClassName = "TestClapWnd";
    RegisterClassA(&wc);
    HWND w = CreateWindowA("TestClapWnd", "testclap", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 600, 400, 0, 0, GetModuleHandleA(0), 0);
    const clap_plugin_gui_t* g = pl->get_extension(pl, CLAP_EXT_GUI);
    uint32_t gw = 0, gh = 0;
    CHECK(g->is_api_supported(pl, CLAP_WINDOW_API_WIN32, false) && g->create(pl, CLAP_WINDOW_API_WIN32, false) && g->get_size(pl, &gw, &gh),
          "gui create, size %ux%u", gw, gh);
    CHECK(gw == 300 && gh == 200, "editor size before open");
    clap_window_t win; win.api = CLAP_WINDOW_API_WIN32; win.win32 = w;
    CHECK(g->set_parent(pl, &win) && g->show(pl), "editor opened in the host window");
    CHECK(n_resize >= 1 && last_w == 320 && last_h == 240, "resize request from inside effEditOpen (%ldx%ld)", last_w, last_h);
    pump(pl, 800);
    CHECK(last_w == 400 && last_h == 250, "resize request started by the plugin's GUI (%ldx%ld)", last_w, last_h);
    CHECK(n_flush >= 1, "request_flush for a change made in the plugin's GUI");
    out_init(&o); in_init(&e);
    run(pl, &e, &o, 64, 0);
    CHECK(o.nvalues == 1 && o.value_idx[0] == 2 && fabs(o.value[0] - 0.75) < 1e-6 && o.gestures == 2,
          "the change reaches the host as gesture begin / value / end (%d values, %d gestures)", o.nvalues, o.gestures);
    g->destroy(pl);
    DestroyWindow(w);

    // state: the plugin's chunk
    const clap_plugin_state_t* st = pl->get_extension(pl, CLAP_EXT_STATE);
    static Mem mem; clap_ostream_t os = { &mem, m_write }; clap_istream_t is = { &mem, m_read };
    CHECK(st->save(pl, &os) && mem.len == 20 + 16, "state save (%u bytes)", mem.len);
    CHECK(st->load(pl, &is), "state load");
    mem.pos = 0; mem.buf[0] ^= 1;
    CHECK(!st->load(pl, &is), "state with a wrong header is refused");

    // a second instance next to the first (its own helper)
    const clap_plugin_t* pl2 = f->create_plugin(f, &host, d->id);
    CHECK(pl2 && pl2->init(pl2) && pl2->activate(pl2, 48000, 1, 512), "second instance");
    if (pl2) {
        in_init(&e); out_init(&o);
        run(pl2, &e, &o, 256, 0);
        CHECK(fabsf(outL[5] - 0.1f) < 1e-6f, "second instance has its own state (gain 0.5): %f", outL[5]);
        pl2->deactivate(pl2); pl2->destroy(pl2);
    }

    pl->stop_processing(pl);
    pl->deactivate(pl);
    pl->destroy(pl);
    entry->deinit();
    printf("RESULT: %s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails;
}
