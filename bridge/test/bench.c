// bench.c - measures the cost of one process call through the bridge:  bench.exe <bridge.dll> [instances] [mt]
// Loads <instances> bridges around test/fakevst.c and times processReplacing for several block sizes.
// "mt" runs one audio thread per instance (like a multi-threaded host), otherwise one thread calls them in turn.
// Prints the mean and the 99th percentile in microseconds per call. Not part of run_tests.sh (timing, not pass / fail):
//   bash bridge/test/run_bench.sh
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../vst2bridge.h"

static intptr_t VSTCALL master(AEffect* e, int32_t op, int32_t i, intptr_t v, void* p, float o)
{
    (void)e; (void)i; (void)v; (void)p; (void)o;
    return op == audioMasterVersion ? 2400 : 0;
}
typedef AEffect* (VSTCALL *Entry)(AudioMasterCallback);
typedef struct { AEffect* fx; double mean, p99; int block, iters; } Job;
static float bufs[2][2][4096];

static double now_us(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return c.QuadPart * 1e6 / (double)f.QuadPart; }
static int cmp(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return x < y ? -1 : x > y; }

static void run(Job* j, int slot)
{
    float* in[2] = { bufs[slot][0], bufs[slot][0] + 2048 }; float* out[2] = { bufs[slot][1], bufs[slot][1] + 2048 };
    double* t = (double*)malloc(sizeof(double) * (size_t)j->iters), sum = 0;
    for (int i = 0; i < 50; i++) j->fx->processReplacing(j->fx, in, out, j->block);         // warm up
    for (int i = 0; i < j->iters; i++) {
        double a = now_us(); j->fx->processReplacing(j->fx, in, out, j->block); t[i] = now_us() - a; sum += t[i];
    }
    qsort(t, (size_t)j->iters, sizeof(double), cmp);
    j->mean = sum / j->iters; j->p99 = t[(int)(j->iters * 0.99)];
    free(t);
}
typedef struct { Job* j; int slot; } Arg;
static DWORD WINAPI thread(LPVOID a) { Arg* g = (Arg*)a; run(g->j, g->slot); return 0; }

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: bench <bridge.dll> [instances] [mt]\n"); return 99; }
    int n = argc > 2 ? atoi(argv[2]) : 1, mt = argc > 3 && !strcmp(argv[3], "mt");
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    HMODULE m = LoadLibraryA(argv[1]);
    Entry entry = m ? (Entry)(void*)GetProcAddress(m, "VSTPluginMain") : 0;
    if (!entry) { printf("cannot load %s\n", argv[1]); return 1; }
    Job* jobs = (Job*)calloc((size_t)n, sizeof *jobs);
    for (int i = 0; i < n; i++) {
        jobs[i].fx = entry(master);
        if (!jobs[i].fx) { printf("instance %d failed\n", i); return 1; }
        jobs[i].fx->dispatcher(jobs[i].fx, effOpen, 0, 0, 0, 0);
        jobs[i].fx->dispatcher(jobs[i].fx, effSetSampleRate, 0, 0, 0, 44100.0f);
        jobs[i].fx->dispatcher(jobs[i].fx, effMainsChanged, 0, 1, 0, 0);
    }
    printf("bench: %d instance(s), %s, bridge %s\n", n, mt ? "one thread each" : "one thread", argv[1]);
    static const int blocks[] = { 64, 128, 512, 2048 };
    for (unsigned b = 0; b < sizeof blocks / sizeof *blocks; b++) {
        for (int i = 0; i < n; i++) { jobs[i].block = blocks[b]; jobs[i].iters = 2000 / (mt ? 1 : n) + 200; }
        double mean = 0, p99 = 0;
        if (mt) {
            HANDLE h[64]; Arg a[64];
            for (int i = 0; i < n; i++) { a[i].j = &jobs[i]; a[i].slot = i & 1; h[i] = CreateThread(0, 0, thread, &a[i], 0, 0); }
            WaitForMultipleObjects((DWORD)n, h, TRUE, INFINITE);
            for (int i = 0; i < n; i++) CloseHandle(h[i]);
        } else for (int i = 0; i < n; i++) run(&jobs[i], 0);
        for (int i = 0; i < n; i++) { mean += jobs[i].mean; if (jobs[i].p99 > p99) p99 = jobs[i].p99; }
        printf("  block %4d: mean %7.1f us   p99 %7.1f us   (budget %7.1f us)\n", blocks[b], mean / n, p99, blocks[b] * 1e6 / 44100.0);
        fflush(stdout);
    }
    for (int i = 0; i < n; i++) { jobs[i].fx->dispatcher(jobs[i].fx, effMainsChanged, 0, 0, 0, 0); jobs[i].fx->dispatcher(jobs[i].fx, effClose, 0, 0, 0, 0); }
    return 0;
}
