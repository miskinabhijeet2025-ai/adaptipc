/*
 * runtime_context.c -- producer-side context collector (Phase 1).
 *
 * All state is sampled from the producer's adapt_send() path only, so
 * the struct is single-thread-owned by construction (SPSC: exactly one
 * producer). Estimates are EWMAs chosen for robustness:
 *
 *   - arrival rate : payload bytes / elapsed time since the previous
 *     send (offers a floor of zero; idle gaps simply produce small
 *     rates, never division by zero because dt==0 is skipped).
 *   - drain rate   : between two send observations the consumer must
 *     have drained (prev_occ_after_push) - occ_now bytes; a positive
 *     delta over dt yields one rate sample. A negative delta (queue
 *     grew) yields no sample rather than a bogus negative rate.
 *   - queue wait   : (occ + incoming) / drain_rate; every unstable
 *     case (too few samples, stalled consumer, rate <= floor) falls
 *     back to the conservative ADAPT_DRAIN_FLOOR_BPS, which
 *     deliberately OVERestimates the wait -- routing away from SHM on
 *     uncertainty is the safe direction for latency.
 */
#define _POSIX_C_SOURCE 200809L

#include "cost_model.h"

#include <limits.h>
#include <math.h>
#include <time.h>
#include <string.h>

void adapt_rtctx_init(adapt_rtctx_t *rc, unsigned min_samples)
{
    if (!rc) return;
    memset(rc, 0, sizeof(*rc));
    rc->min_samples = min_samples ? min_samples : 8;
}

void adapt_rtctx_sample(adapt_rtctx_t *rc, size_t payload,
                        size_t occ_before_push, uint64_t now_ns,
                        double alpha)
{
    if (!rc) return;
    const double dt_ns = (rc->last_ns && now_ns > rc->last_ns)
                             ? (double)(now_ns - rc->last_ns)
                             : 0.0;

    if (dt_ns > 0) {
        /* arrival rate: this message over the idle gap */
        const double a = (double)payload / (dt_ns / 1e9);
        rc->ewma_arrival_bps = alpha * a +
            (1.0 - alpha) * rc->ewma_arrival_bps;

        /* drain rate: what the consumer removed since the previous
         * observation. prev_occ already includes the previous payload
         * (post-push occupancy). */
        const double expected = rc->prev_occ_bytes;
        if (occ_before_push < expected) {
            const double drained = expected - (double)occ_before_push;
            const double d = drained / (dt_ns / 1e9);
            rc->ewma_drain_bps = alpha * d +
                (1.0 - alpha) * rc->ewma_drain_bps;
            if (rc->samples < UINT_MAX) rc->samples++;
            if (drained > 0) {
                rc->drain_valid = 1;
                rc->last_drain_ns = now_ns;
            }
        }
        /* else: queue grew -- no drain sample this interval */
    }

    /* Occupancy is smoothed four times slower than the size EWMA: the
     * queue-wait estimate feeds a routing decision whose switching
     * cost must not be paid on every transient backlog spike. */
    const double occ_a = alpha * 0.25;
    rc->ewma_occ_bytes = occ_a * (double)occ_before_push +
        (1.0 - occ_a) * rc->ewma_occ_bytes;

    rc->prev_occ_bytes = (double)occ_before_push + (double)payload;
    rc->last_ns = now_ns;
}

double adapt_rtctx_queue_wait_us(const adapt_rtctx_t *rc, size_t incoming,
                                 size_t occ_bytes)
{
    (void)incoming; /* wait is determined by bytes ahead, not own size */
    const double bytes = (double)occ_bytes;
    if (bytes <= 0.0) return 0.0;          /* empty queue: no wait */

    /* Staleness guard: a drain estimate from a consumer that has since
     * stopped draining is worse than no estimate (v2.1 finding -- the
     * EWMA otherwise retains a stale fast rate and underestimates the
     * wait by orders of magnitude). Treat the estimate as reliable
     * only if a positive drain was observed within the stale window. */
    int fresh = 0;
    if (rc && rc->last_drain_ns) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull +
                       (uint64_t)ts.tv_nsec;
        fresh = now >= rc->last_drain_ns &&
                now - rc->last_drain_ns < ADAPT_DRAIN_STALE_NS;
    }
    int reliable = rc && rc->samples >= rc->min_samples &&
                   rc->drain_valid && fresh;
    double rate = reliable ? rc->ewma_drain_bps : 0.0;
    if (rate <= ADAPT_DRAIN_FLOOR_BPS)
        rate = ADAPT_DRAIN_FLOOR_BPS;      /* conservative fallback */

    const double us = bytes / rate * 1e6;
    return isfinite(us) ? us : 0.0;
}
