// plat.h - the few system primitives that stub.c and clap.c use on Windows and on Linux / macOS (POSIX):
// locks, thread ids, atomics and the folder for the log file.
#ifndef VB_PLAT_H
#define VB_PLAT_H
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef CRITICAL_SECTION vb_mutex;                      // recursive
static inline void vb_mutex_init(vb_mutex* m) { InitializeCriticalSection(m); }
static inline void vb_mutex_free(vb_mutex* m) { DeleteCriticalSection(m); }
static inline void vb_lock(vb_mutex* m) { EnterCriticalSection(m); }
static inline void vb_unlock(vb_mutex* m) { LeaveCriticalSection(m); }
typedef DWORD vb_tid;
static inline vb_tid vb_self(void) { return GetCurrentThreadId(); }
#define vb_barrier()        MemoryBarrier()
#define vb_xchg(p, v)       InterlockedExchange((volatile LONG*)(p), (LONG)(v))
#define vb_load(p)          (*(volatile LONG*)(p))
#define vb_inc(p)           InterlockedIncrement((volatile LONG*)(p))
#define vb_cas(p, old, new) (InterlockedCompareExchange((volatile LONG*)(p), (LONG)(new), (LONG)(old)) == (LONG)(old))
#define VB_EXPORT           __declspec(dllexport)
#else
#include <pthread.h>
typedef pthread_mutex_t vb_mutex;                       // recursive, like a critical section
static inline void vb_mutex_init(vb_mutex* m)
{
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a); pthread_mutexattr_destroy(&a);
}
static inline void vb_mutex_free(vb_mutex* m) { pthread_mutex_destroy(m); }
static inline void vb_lock(vb_mutex* m) { pthread_mutex_lock(m); }
static inline void vb_unlock(vb_mutex* m) { pthread_mutex_unlock(m); }
typedef uintptr_t vb_tid;                               // 0 = no thread
static inline vb_tid vb_self(void) { return (vb_tid)pthread_self(); }
#define vb_barrier()        __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define vb_xchg(p, v)       __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define vb_load(p)          __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define vb_inc(p)           __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
static inline int vb_cas_(volatile int32_t* p, int32_t old, int32_t nw) { return __atomic_compare_exchange_n(p, &old, nw, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
#define vb_cas(p, old, new) vb_cas_((volatile int32_t*)(p), (old), (new))
#define VB_EXPORT           __attribute__((visibility("default")))
#endif
#endif
