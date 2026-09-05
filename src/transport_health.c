/*
 * transport_health.c -- lightweight debounced transport health (Phase 4).
 *
 * States are derived from producer-observable signals only:
 *   - ring occupancy fraction (backlog / producer parked)
 *   - drain-rate deficit (consumer not keeping up)
 * Transitions require `debounce` consecutive bad (or good) samples, so
 * a single outlier sample never changes the state -- the same
 * anti-thrashing reasoning as the routing hysteresis.
 *
 * State machine (SHM):
 *   HEALTHY      -- normal
 *   -> DEGRADED  -- occupancy >= DEGRADED_OCC or drain deficit for
 *                   `debounce` consecutive samples
 *   -> BLOCKED   -- occupancy >= BLOCKED_OCC (~full, producer parking)
 *   -> RECOVERING-- occupancy <= RECOVERED_OCC for `debounce` samples
 *   -> HEALTHY   -- further RECOVERING streak of `debounce` samples
 * UDS health is not modeled separately: its only failure mode visible
 * here is send failure, which adapt_send already surfaces as errno.
 */
#define _POSIX_C_SOURCE 200809L

#include "cost_model.h"

#include <string.h>

#define DEGRADED_OCC_FRAC 0.80   /* matches the flow-control HW */
#define BLOCKED_OCC_FRAC  0.95
#define RECOVERED_OCC_FRAC 0.20
/* Consumer considered too slow when it drains less than this fraction
 * of the observed arrival rate while the queue is non-empty. */
#define DRAIN_DEFICIT_FRAC 0.5

void adapt_health_init(adapt_health_t *h, unsigned debounce)
{
    if (!h) return;
    memset(h, 0, sizeof(*h));
    h->state = ADAPT_HEALTH_UNAVAILABLE;
    h->debounce = debounce ? debounce : 8;
}

static int set_state(adapt_health_t *h, adapt_health_state_t s)
{
    if (h->state != s) {
        h->state = s;
        h->transitions++;
        h->bad_streak = 0;
        h->good_streak = 0;
        return 1;
    }
    return 0;
}

int adapt_health_update_shm(adapt_health_t *h, size_t occ_bytes,
                            size_t capacity, int drain_valid,
                            double arrival_bps, double drain_bps)
{
    if (!h || capacity == 0) return 0;

    const double occ_frac = (double)occ_bytes / (double)capacity;

    if (occ_frac >= BLOCKED_OCC_FRAC) {
        h->good_streak = 0;
        if (++h->bad_streak >= h->debounce ||
            h->state == ADAPT_HEALTH_DEGRADED)
            return set_state(h, ADAPT_HEALTH_BLOCKED);
    } else if (occ_frac >= DEGRADED_OCC_FRAC ||
               (occ_bytes > 0 && drain_valid && arrival_bps > 0 &&
                drain_bps < DRAIN_DEFICIT_FRAC * arrival_bps)) {
        h->good_streak = 0;
        if (h->state == ADAPT_HEALTH_BLOCKED) {
            /* blocked -> recovering first; only degrade if backlog
             * persists at the degraded level. */
            if (occ_frac >= DEGRADED_OCC_FRAC &&
                ++h->bad_streak >= h->debounce)
                return set_state(h, ADAPT_HEALTH_DEGRADED);
        } else if (++h->bad_streak >= h->debounce) {
            return set_state(h, ADAPT_HEALTH_DEGRADED);
        }
    } else if (occ_frac <= RECOVERED_OCC_FRAC) {
        h->bad_streak = 0;
        if (h->state == ADAPT_HEALTH_UNAVAILABLE) {
            /* first mapping: straight to HEALTHY */
            return set_state(h, ADAPT_HEALTH_HEALTHY);
        } else if (h->state != ADAPT_HEALTH_HEALTHY &&
                   ++h->good_streak >= h->debounce) {
            return set_state(h, h->state == ADAPT_HEALTH_BLOCKED
                             ? ADAPT_HEALTH_RECOVERING
                             : ADAPT_HEALTH_HEALTHY);
        }
    }
    return 0;
}
