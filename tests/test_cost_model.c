/*
 * test_cost_model.c -- unit tests for the context-aware policy engine.
 *
 * Covers:
 *  1. Queue-wait estimator robustness: empty queue, insufficient
 *     samples, stalled consumer, normal operation (no div-by-zero,
 *     no unstable estimates).
 *  2. Switching-margin stability: cost differences smaller than the
 *     margin never flip the route (noise immunity, H > 2*eps).
 *  3. Self-calibrating crossover: learns a bounded S* from measured
 *     costs, stays put on noise, clamps to the configured range.
 *  4. Health state machine: debounce on both degradation and recovery.
 *  5. QoS: a latency budget changes the decision vs BALANCED.
 *  6. Policy ladder: identical inputs produce the documented ablation
 *     differences (size_only vs size_hysteresis vs queue_aware).
 */
#define _POSIX_C_SOURCE 200809L

#include "cost_model.h"
#include "shm_ringbuffer.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fake a monotonic clock in microseconds-as-nanoseconds. */
static uint64_t fake_now;
static uint64_t tick(uint64_t ns) { fake_now += ns; return fake_now; }

static void test_queue_wait_robustness(void)
{
    adapt_rtctx_t rc;
    adapt_rtctx_init(&rc, 8);

    /* Empty queue: zero wait, always. */
    assert(adapt_rtctx_queue_wait_us(&rc, 1000, 0) == 0.0);

    /* No samples yet: conservative fallback, but finite and large. */
    double w = adapt_rtctx_queue_wait_us(&rc, 1000, 100000);
    assert(w > 0.0 && isfinite(w));

    /* Feed 32 samples with a healthy drain: 64 KB drained per ms. */
    fake_now = 1000000;
    size_t occ = 0;
    for (int i = 0; i < 64; i++) {
        adapt_rtctx_sample(&rc, 4096, occ, tick(1000000), 0.2);
        /* consumer drains everything between sends */
        occ = 0;
    }
    assert(rc.samples >= 8 && rc.drain_valid);
    /* occupancy small -> small wait */
    double w1 = adapt_rtctx_queue_wait_us(&rc, 4096, 8192);
    /* occupancy 10x -> wait ~10x */
    double w2 = adapt_rtctx_queue_wait_us(&rc, 4096, 81920);
    assert(w1 > 0.0 && isfinite(w1));
    assert(w2 > 9.0 * w1 && w2 < 11.0 * w1);

    /* Stalled consumer (occupancy keeps growing): the estimator must
     * still return a finite conservative value, not blow up. */
    adapt_rtctx_t rc2;
    adapt_rtctx_init(&rc2, 8);
    fake_now = 0;
    occ = 0;
    for (int i = 0; i < 100; i++) {
        adapt_rtctx_sample(&rc2, 65536, occ, tick(1000000), 0.2);
        occ += 65536; /* nothing ever drains */
    }
    assert(!rc2.drain_valid);
    double w3 = adapt_rtctx_queue_wait_us(&rc2, 65536, occ);
    assert(isfinite(w3) && w3 > 0.0);
    printf("  queue_wait: healthy=%.1f us 10x=%.1f us stalled=%.1f us\n",
           w1, w2, w3);
}

static void test_switching_margin(void)
{
    adapt_cost_cfg_t cfg;
    adapt_cost_cfg_defaults(&cfg);
    cfg.switch_cost_us = 20.0;
    cfg.margin_us = 5.0;

    adapt_rtctx_t rc;
    adapt_rtctx_init(&rc, 1);
    adapt_learn_t learn;
    adapt_learn_init(&learn, &cfg);

    /* Costs: SHM slightly cheaper than UDS (difference 2 us < margin). */
    double c_uds = adapt_cost_uds(&cfg, 4096);
    double c_shm = adapt_cost_shm(&cfg, 4096, 0, 0, 1);
    assert(c_shm < c_uds);

    /* Current route UDS: SHM looks cheaper but the margin blocks the
     * switch (score_shm + switch_cost + margin > score_uds). */
    adapt_route_t r = adapt_policy_decide(
        ADAPT_POLICY_COST_AWARE, &cfg, &rc, &learn, NULL, 1,
        4096.0, ADAPT_ROUTE_UDS, 4096, tick(1000), NULL);
    assert(r == ADAPT_ROUTE_UDS); /* sticky within margin */

    /* Same inputs, already on SHM: stays (no switch cost either way). */
    r = adapt_policy_decide(ADAPT_POLICY_COST_AWARE, &cfg, &rc, &learn,
                            NULL, 1, 4096.0, ADAPT_ROUTE_SHM, 4096,
                            tick(1000), NULL);
    assert(r == ADAPT_ROUTE_SHM);

    /* A decisive cost difference DOES switch: huge SHM queue wait. */
    r = adapt_policy_decide(ADAPT_POLICY_COST_AWARE, &cfg, &rc, &learn,
                            NULL, 1, 4096.0, ADAPT_ROUTE_SHM, 4096,
                            tick(1000), NULL);
    /* make SHM far more expensive via the queue-wait input */
    adapt_rtctx_t busy;
    adapt_rtctx_init(&busy, 1);
    busy.samples = 100;
    busy.drain_valid = 1;
    busy.ewma_drain_bps = 1e9; /* 1 GB/s: 40 MB backlog = 40 ms wait */
    busy.ewma_occ_bytes = 40e6;
    r = adapt_policy_decide(ADAPT_POLICY_COST_AWARE, &cfg, &busy, &learn,
                            NULL, 1, 4096.0, ADAPT_ROUTE_SHM, 4096,
                            tick(1000), NULL);
    assert(r == ADAPT_ROUTE_UDS); /* 40 ms wait >> any margin */
    printf("  margin: sticky=%.1f+%.1f vs %.1f (blocked switch ok)\n",
           c_shm, cfg.switch_cost_us, c_uds);
}

static void test_crossover_learning(void)
{
    adapt_cost_cfg_t cfg;
    adapt_cost_cfg_defaults(&cfg);
    adapt_learn_t learn;
    adapt_learn_init(&learn, &cfg);

    /* Ground truth: UDS cost = 3us + 2ns/B; SHM = 1us + 0.5ns/B.
     * Crossover: (1-3)/(2-0.5) ns/B -> negative => clamped low. Use a
     * truth with a real crossover: UDS = 2 + 2ns/B, SHM = 4 + 0.5ns/B
     * -> S* = (4-2)/1.5ns = 1333 B. */
    unsigned seed = 7;
    for (int i = 0; i < 400; i++) {
        size_t s_small = 128 + (size_t)(rand_r(&seed) % 256);
        size_t s_large = 8192 + (size_t)(rand_r(&seed) % 4096);
        adapt_learn_observe(&learn, &cfg, ADAPT_ROUTE_UDS, s_small,
                            2.0 + 0.002 * (double)s_small);
        adapt_learn_observe(&learn, &cfg, ADAPT_ROUTE_UDS, s_large,
                            2.0 + 0.002 * (double)s_large);
        adapt_learn_observe(&learn, &cfg, ADAPT_ROUTE_SHM, s_small,
                            4.0 + 0.0005 * (double)s_small);
        adapt_learn_observe(&learn, &cfg, ADAPT_ROUTE_SHM, s_large,
                            4.0 + 0.0005 * (double)s_large);
    }
    const double s_star = adapt_learn_crossover(&learn, &cfg);
    assert(s_star > 0.0);
    /* truth ~1333 B; tolerance 25% for EWMA noise */
    assert(s_star > 1000.0 && s_star < 1700.0);

    /* Noise immunity: wildly noisy observations must not move S*
     * outside the configured clamp. */
    for (int i = 0; i < 400; i++) {
        adapt_learn_observe(&learn, &cfg, ADAPT_ROUTE_UDS, 512,
                            2.0 + (double)(rand_r(&seed) % 1000));
        adapt_learn_observe(&learn, &cfg, ADAPT_ROUTE_SHM, 512,
                            4.0 + (double)(rand_r(&seed) % 1000));
    }
    const double s2 = adapt_learn_crossover(&learn, &cfg);
    assert(s2 >= cfg.crossover_min_b && s2 <= cfg.crossover_max_b);
    printf("  crossover: learned S*=%.0f B (truth 1333), "
           "updates=%u, noisy=%.0f B\n",
           s_star, learn.crossover_updates, s2);
}

static void test_health_debounce(void)
{
    adapt_health_t h;
    adapt_health_init(&h, 4);
    const size_t cap = 65536;

    /* Unavailable -> first observation under threshold -> HEALTHY. */
    adapt_health_update_shm(&h, 0, cap, 1, 1e6, 1e6);
    assert(h.state == ADAPT_HEALTH_HEALTHY);

    /* One bad sample does NOT degrade (debounce). */
    adapt_health_update_shm(&h, cap, cap, 1, 1e6, 1e6);
    assert(h.state == ADAPT_HEALTH_HEALTHY);
    unsigned t0 = h.transitions;

    /* Sustained full occupancy -> BLOCKED after `debounce` samples. */
    for (int i = 0; i < 4; i++)
        adapt_health_update_shm(&h, cap, cap, 1, 1e6, 1e6);
    assert(h.state == ADAPT_HEALTH_BLOCKED);
    assert(h.transitions > t0);

    /* Recovery: drained below LW for the debounce streak goes through
     * RECOVERING then HEALTHY, not in one jump. */
    t0 = h.transitions;
    for (int i = 0; i < 4; i++)
        adapt_health_update_shm(&h, 0, cap, 1, 1e6, 1e6);
    assert(h.state == ADAPT_HEALTH_RECOVERING);
    for (int i = 0; i < 4; i++)
        adapt_health_update_shm(&h, 0, cap, 1, 1e6, 1e6);
    assert(h.state == ADAPT_HEALTH_HEALTHY);
    assert(h.transitions > t0);
    printf("  health: transitions=%u (blocked->recovering->healthy)\n",
           h.transitions);
}

static void test_qos_budget_changes_decision(void)
{
    adapt_cost_cfg_t cfg;
    adapt_cost_cfg_defaults(&cfg);
    adapt_rtctx_t rc;
    adapt_rtctx_init(&rc, 1);
    adapt_learn_t learn;
    adapt_learn_init(&learn, &cfg);

    /* Warm the learned costs so the decision is the full cost
     * comparison, not the cold-start size prior. */
    for (int i = 0; i < 200; i++) {
        adapt_learn_observe(&learn, &cfg, ADAPT_ROUTE_UDS, 256, 2.5);
        adapt_learn_observe(&learn, &cfg, ADAPT_ROUTE_UDS, 16384, 30.0);
        adapt_learn_observe(&learn, &cfg, ADAPT_ROUTE_SHM, 256, 0.5);
        adapt_learn_observe(&learn, &cfg, ADAPT_ROUTE_SHM, 16384, 8.0);
    }

    /* Idle consumer (no recent drain): SHM delivery incurs the 2 ms
     * notification floor, but that floor is a LATENCY term, not a
     * transport-cost term. BALANCED therefore keeps SHM (cheapest
     * cost); LATENCY with a 500 us budget sees the 2 ms overshoot and
     * switches to UDS. This is exactly the tradeoff the budget knob
     * is meant to express. */
    rc.ewma_occ_bytes = 0;      /* empty queue */
    rc.last_drain_ns = 0;       /* consumer idle */
    const uint64_t now = 1000000;

    adapt_route_t balanced = adapt_policy_decide(
        ADAPT_POLICY_FULL_ADAPTIVE, &cfg, &rc, &learn, NULL, 1,
        4096.0, ADAPT_ROUTE_SHM, 4096, now, NULL);

    adapt_cost_cfg_t lat = cfg;
    lat.qos = ADAPT_QOS_LATENCY;
    lat.latency_budget_us = 500.0;
    lat.latency_weight = 2.0;
    adapt_route_t latency = adapt_policy_decide(
        ADAPT_POLICY_FULL_ADAPTIVE, &lat, &rc, &learn, NULL, 1,
        4096.0, ADAPT_ROUTE_SHM, 4096, now, NULL);

    assert(balanced == ADAPT_ROUTE_SHM);
    assert(latency == ADAPT_ROUTE_UDS);
    printf("  qos: balanced(idle consumer)=SHM, "
           "latency(budget 500us, notify 2ms)=UDS\n");
}

static void test_policy_ladder(void)
{
    adapt_cost_cfg_t cfg;
    adapt_cost_cfg_defaults(&cfg);
    adapt_learn_t learn;
    adapt_learn_init(&learn, &cfg);

    /* Inside the deadband (2048 B), size_only flips on the threshold
     * while size_hysteresis sticks to the current route. */
    adapt_route_t so = adapt_policy_decide(
        ADAPT_POLICY_SIZE_ONLY, &cfg, NULL, NULL, NULL, 1,
        2048.0, ADAPT_ROUTE_UDS, 2048, 0, NULL);
    adapt_route_t sh = adapt_policy_decide(
        ADAPT_POLICY_SIZE_HYSTERESIS, &cfg, NULL, NULL, NULL, 1,
        2048.0, ADAPT_ROUTE_UDS, 2048, 0, NULL);
    assert(so == ADAPT_ROUTE_UDS);  /* 2048 < TAU_HIGH */
    assert(sh == ADAPT_ROUTE_UDS);  /* sticky in deadband */

    so = adapt_policy_decide(ADAPT_POLICY_SIZE_ONLY, &cfg, NULL, NULL,
                             NULL, 1, 2048.0, ADAPT_ROUTE_SHM, 2048, 0,
                             NULL);
    sh = adapt_policy_decide(ADAPT_POLICY_SIZE_HYSTERESIS, &cfg, NULL,
                             NULL, NULL, 1, 2048.0, ADAPT_ROUTE_SHM,
                             2048, 0, NULL);
    assert(so == ADAPT_ROUTE_UDS);  /* threshold, ignores current */
    assert(sh == ADAPT_ROUTE_SHM);  /* sticky, ignores threshold  */

    /* Queue pressure: queue_aware escapes SHM when the wait exceeds
     * the UDS cost; size_hysteresis cannot. */
    adapt_rtctx_t rc;
    adapt_rtctx_init(&rc, 1);
    rc.samples = 100;
    rc.drain_valid = 1;
    rc.ewma_drain_bps = 1e9;
    rc.ewma_occ_bytes = 40e6; /* 40 ms wait for a 4 KB message */
    /* 2 KB payload (fits UDS_MAX_DGRAM) with a 40 ms SHM backlog. */
    adapt_route_t qa = adapt_policy_decide(
        ADAPT_POLICY_QUEUE_AWARE, &cfg, &rc, NULL, NULL, 1,
        8192.0, ADAPT_ROUTE_SHM, 2048, 0, NULL);
    adapt_route_t sh2 = adapt_policy_decide(
        ADAPT_POLICY_SIZE_HYSTERESIS, &cfg, &rc, NULL, NULL, 1,
        8192.0, ADAPT_ROUTE_SHM, 2048, 0, NULL);
    assert(qa == ADAPT_ROUTE_UDS);
    assert(sh2 == ADAPT_ROUTE_SHM);
    printf("  ladder: size_only flips at threshold, hysteresis sticks, "
           "queue_aware escapes 40ms backlog\n");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("cost model unit tests:\n");
    test_queue_wait_robustness();
    test_switching_margin();
    test_crossover_learning();
    test_health_debounce();
    test_qos_budget_changes_decision();
    test_policy_ladder();
    printf("test_cost_model: PASS\n");
    return 0;
}
