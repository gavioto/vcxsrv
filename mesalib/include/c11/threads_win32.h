/*
 * C11 <threads.h> emulation library
 *
 * (C) Copyright yohhoy 2012.
 * Distributed under the Boost Software License, Version 1.0.
 *
 * Permission is hereby granted, free of charge, to any person or organization
 * obtaining a copy of the software and accompanying documentation covered by
 * this license (the "Software") to use, reproduce, display, distribute,
 * execute, and transmit the Software, and to prepare [[derivative work]]s of the
 * Software, and to permit third-parties to whom the Software is furnished to
 * do so, all subject to the following:
 *
 * The copyright notices in the Software and this entire statement, including
 * the above license grant, this restriction and the following disclaimer,
 * must be included in all copies of the Software, in whole or in part, and
 * all derivative works of the Software, unless such copies or derivative
 * works are solely in the form of machine-executable object code generated by
 * a source language processor.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
 * SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
 * FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <process.h>  // MSVCRT
#include <stdlib.h>

/*
Configuration macro:

  EMULATED_THREADS_USE_NATIVE_CALL_ONCE
    Use native WindowsAPI one-time initialization function.
    (requires WinVista or later)
    Otherwise emulate by mtx_trylock() + *busy loop* for WinXP.

  EMULATED_THREADS_USE_NATIVE_CV
    Use native WindowsAPI condition variable object.
    (requires WinVista or later)
    Otherwise use emulated implementation for WinXP.

  EMULATED_THREADS_TSS_DTOR_SLOTNUM
    Max registerable TSS dtor number.
*/

// XXX: Retain XP compatability
#if 0
#if _WIN32_WINNT >= 0x0600
// Prefer native WindowsAPI on newer environment.
#if !defined(__MINGW32__)
#define EMULATED_THREADS_USE_NATIVE_CALL_ONCE 
#endif
#define EMULATED_THREADS_USE_NATIVE_CV
#endif
#endif
#define EMULATED_THREADS_TSS_DTOR_SLOTNUM 64  // see TLS_MINIMUM_AVAILABLE


#include <windows.h>

// check configuration
#if defined(EMULATED_THREADS_USE_NATIVE_CALL_ONCE) && (_WIN32_WINNT < 0x0600)
#error EMULATED_THREADS_USE_NATIVE_CALL_ONCE requires _WIN32_WINNT>=0x0600
#endif

#if defined(EMULATED_THREADS_USE_NATIVE_CV) && (_WIN32_WINNT < 0x0600)
#error EMULATED_THREADS_USE_NATIVE_CV requires _WIN32_WINNT>=0x0600
#endif


/*---------------------------- macros ----------------------------*/
#ifdef EMULATED_THREADS_USE_NATIVE_CALL_ONCE
#define ONCE_FLAG_INIT INIT_ONCE_STATIC_INIT
#else
#define ONCE_FLAG_INIT {0}
#endif
#define TSS_DTOR_ITERATIONS 1

// FIXME: temporary non-standard hack to ease transition
#define _MTX_INITIALIZER_NP {(PCRITICAL_SECTION_DEBUG)-1, -1, 0, 0, 0, 0}

/*---------------------------- types ----------------------------*/
typedef struct cnd_t {
#ifdef EMULATED_THREADS_USE_NATIVE_CV
    CONDITION_VARIABLE condvar;
#else
    int blocked;
    int gone;
    int to_unblock;
    HANDLE sem_queue;
    HANDLE sem_gate;
    CRITICAL_SECTION monitor;
#endif
} cnd_t;

typedef HANDLE thrd_t;

typedef DWORD tss_t;

typedef CRITICAL_SECTION mtx_t;

#ifdef EMULATED_THREADS_USE_NATIVE_CALL_ONCE
typedef INIT_ONCE once_flag;
#else
typedef struct once_flag_t {
    volatile LONG status;
} once_flag;
#endif


static inline void * tss_get(tss_t key);
static inline void thrd_yield(void);
static inline int mtx_trylock(mtx_t *mtx);
static inline int mtx_lock(mtx_t *mtx);
static inline int mtx_unlock(mtx_t *mtx);

/*
Implementation limits:
  - Conditionally emulation for "Initialization functions"
    (see EMULATED_THREADS_USE_NATIVE_CALL_ONCE macro)
  - Emulated `mtx_timelock()' with mtx_trylock() + *busy loop*
*/
static void impl_tss_dtor_invoke(void);  // forward decl.

struct impl_thrd_param {
    thrd_start_t func;
    void *arg;
};

static unsigned __stdcall impl_thrd_routine(void *p)
{
    struct impl_thrd_param pack;
    int code;
    memcpy(&pack, p, sizeof(struct impl_thrd_param));
    free(p);
    code = pack.func(pack.arg);
    impl_tss_dtor_invoke();
    return (unsigned)code;
}

static DWORD impl_xtime2msec(const xtime *xt)
{
    return (DWORD)((xt->sec * 1000u) + (xt->nsec / 1000));
}

#ifdef EMULATED_THREADS_USE_NATIVE_CALL_ONCE
struct impl_call_once_param { void (*func)(void); };
static BOOL CALLBACK impl_call_once_callback(PINIT_ONCE InitOnce, PVOID Parameter, PVOID *Context)
{
    struct impl_call_once_param *param = (struct impl_call_once_param*)Parameter;
    (param->func)();
    ((void)InitOnce); ((void)Context);  // suppress warning
    return TRUE;
}
#endif  // ifdef EMULATED_THREADS_USE_NATIVE_CALL_ONCE

#ifndef EMULATED_THREADS_USE_NATIVE_CV
/*
Note:
  The implementation of condition variable is ported from Boost.Interprocess
  See http://www.boost.org/boost/interprocess/sync/windows/condition.hpp
*/
static void impl_cond_do_signal(cnd_t *cond, int broadcast)
{
    int nsignal = 0;

    EnterCriticalSection(&cond->monitor);
    if (cond->to_unblock != 0) {
        if (cond->blocked == 0) {
            LeaveCriticalSection(&cond->monitor);
            return;
        }
        if (broadcast) {
            cond->to_unblock += nsignal = cond->blocked;
            cond->blocked = 0;
        } else {
            nsignal = 1;
            cond->to_unblock++;
            cond->blocked--;
        }
    } else if (cond->blocked > cond->gone) {
        WaitForSingleObject(cond->sem_gate, INFINITE);
        if (cond->gone != 0) {
            cond->blocked -= cond->gone;
            cond->gone = 0;
        }
        if (broadcast) {
            nsignal = cond->to_unblock = cond->blocked;
            cond->blocked = 0;
        } else {
            nsignal = cond->to_unblock = 1;
            cond->blocked--;
        }
    }
    LeaveCriticalSection(&cond->monitor);

    if (0 < nsignal)
        ReleaseSemaphore(cond->sem_queue, nsignal, NULL);
}

static int impl_cond_do_wait(cnd_t *cond, mtx_t *mtx, const xtime *xt)
{
    int nleft = 0;
    int ngone = 0;
    int timeout = 0;
    DWORD w;

    WaitForSingleObject(cond->sem_gate, INFINITE);
    cond->blocked++;
    ReleaseSemaphore(cond->sem_gate, 1, NULL);

    mtx_unlock(mtx);

    w = WaitForSingleObject(cond->sem_queue, xt ? impl_xtime2msec(xt) : INFINITE);
    timeout = (w == WAIT_TIMEOUT);
 
    EnterCriticalSection(&cond->monitor);
    if ((nleft = cond->to_unblock) != 0) {
        if (timeout) {
            if (cond->blocked != 0) {
                cond->blocked--;
            } else {
                cond->gone++;
            }
        }
        if (--cond->to_unblock == 0) {
            if (cond->blocked != 0) {
                ReleaseSemaphore(cond->sem_gate, 1, NULL);
                nleft = 0;
            }
            else if ((ngone = cond->gone) != 0) {
                cond->gone = 0;
            }
        }
    } else if (++cond->gone == INT_MAX/2) {
        WaitForSingleObject(cond->sem_gate, INFINITE);
        cond->blocked -= cond->gone;
        ReleaseSemaphore(cond->sem_gate, 1, NULL);
        cond->gone = 0;
    }
    LeaveCriticalSection(&cond->monitor);

    if (nleft == 1) {
        while (ngone--)
            WaitForSingleObject(cond->sem_queue, INFINITE);
        ReleaseSemaphore(cond->sem_gate, 1, NULL);
    }

    mtx_lock(mtx);
    return timeout ? thrd_busy : thrd_success;
}
#endif  // ifndef EMULATED_THREADS_USE_NATIVE_CV

static struct impl_tss_dtor_entry {
    tss_t key;
    tss_dtor_t dtor;
} impl_tss_dtor_tbl[EMULATED_THREADS_TSS_DTOR_SLOTNUM];

static int impl_tss_dtor_register(tss_t key, tss_dtor_t dtor)
{
    int i;
    for (i = 0; i < EMULATED_THREADS_TSS_DTOR_SLOTNUM; i++) {
        if (!impl_tss_dtor_tbl[i].dtor)
            break;
    }
    if (i == EMULATED_THREADS_TSS_DTOR_SLOTNUM)
        return 1;
    impl_tss_dtor_tbl[i].key = key;
    impl_tss_dtor_tbl[i].dtor = dtor;
    return 0;
}

static void impl_tss_dtor_invoke()
{
    int i;
    for (i = 0; i < EMULATED_THREADS_TSS_DTOR_SLOTNUM; i++) {
        if (impl_tss_dtor_tbl[i].dtor) {
            void* val = tss_get(impl_tss_dtor_tbl[i].key);
            if (val)
                (impl_tss_dtor_tbl[i].dtor)(val);
        }
    }
}


/*--------------- 7.25.2 Initialization functions ---------------*/
// 7.25.2.1
static inline void
call_once(once_flag *flag, void (*func)(void))
{
    assert(!flag && !func);
#ifdef EMULATED_THREADS_USE_NATIVE_CALL_ONCE
    {
    struct impl_call_once_param param;
    param.func = func;
    InitOnceExecuteOnce(flag, impl_call_once_callback, (PVOID)&param, NULL);
    }
#else
    if (InterlockedCompareExchange(&flag->status, 1, 0) == 0) {
        (func)();
        InterlockedExchange(&flag->status, 2);
    } else {
        while (flag->status == 1) {
            // busy loop!
            thrd_yield();
        }
    }
#endif
}


/*------------- 7.25.3 Condition variable functions -------------*/
// 7.25.3.1
static inline int
cnd_broadcast(cnd_t *cond)
{
    if (!cond) return thrd_error;
#ifdef EMULATED_THREADS_USE_NATIVE_CV
    WakeAllConditionVariable(&cond->condvar);
#else
    impl_cond_do_signal(cond, 1);
#endif
    return thrd_success;
}

// 7.25.3.2
static inline void
cnd_destroy(cnd_t *cond)
{
    assert(cond);
#ifdef EMULATED_THREADS_USE_NATIVE_CV
    // do nothing
#else
    CloseHandle(cond->sem_queue);
    CloseHandle(cond->sem_gate);
    DeleteCriticalSection(&cond->monitor);
#endif
}

// 7.25.3.3
static inline int
cnd_init(cnd_t *cond)
{
    if (!cond) return thrd_error;
#ifdef EMULATED_THREADS_USE_NATIVE_CV
    InitializeConditionVariable(&cond->condvar);
#else
    cond->blocked = 0;
    cond->gone = 0;
    cond->to_unblock = 0;
    cond->sem_queue = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    cond->sem_gate = CreateSemaphore(NULL, 1, 1, NULL);
    InitializeCriticalSection(&cond->monitor);
#endif
    return thrd_success;
}

// 7.25.3.4
static inline int
cnd_signal(cnd_t *cond)
{
    if (!cond) return thrd_error;
#ifdef EMULATED_THREADS_USE_NATIVE_CV
    WakeConditionVariable(&cond->condvar);
#else
    impl_cond_do_signal(cond, 0);
#endif
    return thrd_success;
}

// 7.25.3.5
static inline int
cnd_timedwait(cnd_t *cond, mtx_t *mtx, const xtime *xt)
{
    if (!cond || !mtx || !xt) return thrd_error;
#ifdef EMULATED_THREADS_USE_NATIVE_CV
    if (SleepConditionVariableCS(&cond->condvar, mtx, impl_xtime2msec(xt)))
        return thrd_success;
    return (GetLastError() == ERROR_TIMEOUT) ? thrd_busy : thrd_error;
#else
    return impl_cond_do_wait(cond, mtx, xt);
#endif
}

// 7.25.3.6
static inline int
cnd_wait(cnd_t *cond, mtx_t *mtx)
{
    if (!cond || !mtx) return thrd_error;
#ifdef EMULATED_THREADS_USE_NATIVE_CV
    SleepConditionVariableCS(&cond->condvar, mtx, INFINITE);
#else
    impl_cond_do_wait(cond, mtx, NULL);
#endif
    return thrd_success;
}


/*-------------------- 7.25.4 Mutex functions --------------------*/
// 7.25.4.1
static inline void
mtx_destroy(mtx_t *mtx)
{
    assert(mtx);
    DeleteCriticalSection(mtx);
}

// 7.25.4.2
static inline int
mtx_init(mtx_t *mtx, int type)
{
    if (!mtx) return thrd_error;
    if (type != mtx_plain && type != mtx_timed && type != mtx_try
      && type != (mtx_plain|mtx_recursive)
      && type != (mtx_timed|mtx_recursive)
      && type != (mtx_try|mtx_recursive))
        return thrd_error;
    InitializeCriticalSection(mtx);
    return thrd_success;
}

// 7.25.4.3
static inline int
mtx_lock(mtx_t *mtx)
{
    if (!mtx) return thrd_error;
    EnterCriticalSection(mtx);
    return thrd_success;
}

// 7.25.4.4
static inline int
mtx_timedlock(mtx_t *mtx, const xtime *xt)
{
    time_t expire, now;
    if (!mtx || !xt) return thrd_error;
    expire = time(NULL);
    expire += xt->sec;
    while (mtx_trylock(mtx) != thrd_success) {
        now = time(NULL);
        if (expire < now)
            return thrd_busy;
        // busy loop!
        thrd_yield();
    }
    return thrd_success;
}

// 7.25.4.5
static inline int
mtx_trylock(mtx_t *mtx)
{
    if (!mtx) return thrd_error;
    return TryEnterCriticalSection(mtx) ? thrd_success : thrd_busy;
}

// 7.25.4.6
static inline int
mtx_unlock(mtx_t *mtx)
{
    if (!mtx) return thrd_error;
    LeaveCriticalSection(mtx);
    return thrd_success;
}


/*------------------- 7.25.5 Thread functions -------------------*/
// 7.25.5.1
static inline int
thrd_create(thrd_t *thr, thrd_start_t func, void *arg)
{
    struct impl_thrd_param *pack;
    uintptr_t handle;
    if (!thr) return thrd_error;
    pack = (struct impl_thrd_param *)malloc(sizeof(struct impl_thrd_param));
    if (!pack) return thrd_nomem;
    pack->func = func;
    pack->arg = arg;
    handle = _beginthreadex(NULL, 0, impl_thrd_routine, pack, 0, NULL);
    if (handle == 0) {
        if (errno == EAGAIN || errno == EACCES)
            return thrd_nomem;
        return thrd_error;
    }
    *thr = (thrd_t)handle;
    return thrd_success;
}

// 7.25.5.2
static inline thrd_t
thrd_current(void)
{
    return GetCurrentThread();
}

// 7.25.5.3
static inline int
thrd_detach(thrd_t thr)
{
    CloseHandle(thr);
    return thrd_success;
}

// 7.25.5.4
static inline int
thrd_equal(thrd_t thr0, thrd_t thr1)
{
    return (thr0 == thr1);
}

// 7.25.5.5
static inline void
thrd_exit(int res)
{
    impl_tss_dtor_invoke();
    _endthreadex((unsigned)res);
}

// 7.25.5.6
static inline int
thrd_join(thrd_t thr, int *res)
{
    DWORD w, code;
    w = WaitForSingleObject(thr, INFINITE);
    if (w != WAIT_OBJECT_0)
        return thrd_error;
    if (res) {
        if (!GetExitCodeThread(thr, &code)) {
            CloseHandle(thr);
            return thrd_error;
        }
        *res = (int)code;
    }
    CloseHandle(thr);
    return thrd_success;
}

// 7.25.5.7
static inline void
thrd_sleep(const xtime *xt)
{
    assert(xt);
    Sleep(impl_xtime2msec(xt));
}

// 7.25.5.8
static inline void
thrd_yield(void)
{
    SwitchToThread();
}


/*----------- 7.25.6 Thread-specific storage functions -----------*/
// 7.25.6.1
static inline int
tss_create(tss_t *key, tss_dtor_t dtor)
{
    if (!key) return thrd_error;
    *key = TlsAlloc();
    if (dtor) {
        if (impl_tss_dtor_register(*key, dtor)) {
            TlsFree(*key);
            return thrd_error;
        }
    }
    return (*key != 0xFFFFFFFF) ? thrd_success : thrd_error;
}

// 7.25.6.2
static inline void
tss_delete(tss_t key)
{
    TlsFree(key);
}

// 7.25.6.3
static inline void *
tss_get(tss_t key)
{
    return TlsGetValue(key);
}

// 7.25.6.4
static inline int
tss_set(tss_t key, void *val)
{
    return TlsSetValue(key, val) ? thrd_success : thrd_error;
}


/*-------------------- 7.25.7 Time functions --------------------*/
// 7.25.6.1
static inline int
xtime_get(xtime *xt, int base)
{
    if (!xt) return 0;
    if (base == TIME_UTC) {
        xt->sec = time(NULL);
        xt->nsec = 0;
        return base;
    }
    return 0;
}
