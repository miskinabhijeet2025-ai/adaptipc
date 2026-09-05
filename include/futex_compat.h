#ifndef FUTEX_COMPAT_H
#define FUTEX_COMPAT_H

/*
 * Cross-platform "futex-style" wait/wake on a shared atomic int flag.
 *
 * Linux:  the real futex(2) syscall on an _Atomic int that may live in
 *         shared memory (FUTEX_WAIT / FUTEX_WAKE, non-private so the flag
 *         can reside inside a POSIX shm object).
 * Apple:  no public futex; emulated with a PTHREAD_PROCESS_SHARED
 *         mutex/condvar pair that must be embedded next to the flag
 *         (see shm_ringbuffer.c for placement and initialization order).
 *
 * Protocol (classic lost-wake-safe pattern, used by the ring's flow
 * control):
 *   waiter:   for (;;) { if (condition met) break;
 *                        store flag = 1 (release);
 *                        if (condition met) { store flag = 0; break; }
 *                        futex_wait(&flag, 1); }
 *   waker:    if (exchange(flag, 0, acq_rel) == 1) futex_wake(&flag, all);
 *
 * Returns 0 on success, negative errno on failure (-EAGAIN from
 * futex_wait means the flag value changed and the waiter must recheck).
 */

#include <stdatomic.h>

#if defined(__linux__)

#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

static inline long futex_compat_wait(_Atomic int *flag, int expected)
{
    return (long)syscall(SYS_futex, (int *)flag, FUTEX_WAIT, expected,
                         NULL, NULL, 0);
}

static inline long futex_compat_wake(_Atomic int *flag, int nwaiters)
{
    return (long)syscall(SYS_futex, (int *)flag, FUTEX_WAKE, nwaiters,
                         NULL, NULL, 0);
}

#define FUTEX_COMPAT_HAVE_REAL_FUTEX 1

#elif defined(__APPLE__) || defined(__unix__)

/*
 * Emulated futex. The caller owns a struct that embeds the atomic flag
 * plus a process-shared mutex/condvar; these helpers operate on it.
 * Defined out-of-line in shm_ringbuffer.c because the storage lives in
 * shared memory initialized by the ring's producer.
 */

#include <pthread.h>

typedef struct futex_compat_emu {
    _Atomic int     flag;
    pthread_mutex_t mtx;
    pthread_cond_t  cnd;
    _Atomic int     ready; /* set release after mtx/cnd initialized */
} futex_compat_emu_t;

int futex_compat_emu_init(futex_compat_emu_t *f);
/* Returns 0 when the waiter should recheck its condition, -EAGAIN if the
 * flag was observed changed before sleeping, negative errno on error. */
int futex_compat_emu_wait(futex_compat_emu_t *f, int expected);
int futex_compat_emu_wake(futex_compat_emu_t *f);

#endif /* platform */

#endif /* FUTEX_COMPAT_H */
