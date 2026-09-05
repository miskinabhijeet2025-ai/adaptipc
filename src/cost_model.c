/*
 * cost_model.c -- effective-cost routing (Phases 2, 3, 5).
 *
 * Effective cost of a transport T for a message of S bytes:
 *
 *   Score(T) = transport_cost(T, S)
 *            + queue_cost(T)                 (SHM only: predicted wait)
 *            + setup_cost(T)                 (SHM only, if unmapped)
 *            + switching_cost(T != current)
 *            + health_penalty(T)             (SHM only, FULL_ADAPTIVE)
 *            + latency_penalty(T, budget)
 *
 * Decision rule (stability guarantee):
 *   candidate = argmin Score(T)
 *   switch to candidate only if
 *       Score(candidate) + margin_us < Score(current)
 *   i.e. the improvement must exceed the margin. With per-side cost
 *   estimation error bounded by eps, measurement noise alone cannot
 *   flip the decision as long as margin_us > 2*eps, mirroring the
 *   hysteresis argument of the size-only policy.
 *
 * All quantities are microseconds. Priors are per-transport linear
 * models cost(S) = fixed + slope*S; the slopes/fixed terms are replaced
 * online by measured EWMA per size class (self-calibration, Phase 3),
 * and the learned crossover S* (where SHM becomes cheaper than UDS at
 * zero queue wait) is clamped to a bounded range and rate-limited.
 */
#define _POSIX_C_SOURCE 200809L

#include "cost_model.h"
#include "uds_fallback.h" /* UDS_MAX_DGRAM guard */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Names                                                                */
/* ------------------------------------------------------------------ */

const char *adapt_policy_name(adapt_policy_mode_t p)
{
    static const char *n[] = {
        "default", "size_only", "size_hysteresis",
        "queue_aware", "cost_aware", "full_adaptive"
    };
    return n[p <= ADAPT_POLICY_FULL_ADAPTIVE ? p : 0];
}

const char *adapt_qos_name(adapt_qos_t q)
{
    static const char *n[] = { "balanced", "latency", "throughput" };
    return n[q <= ADAPT_QOS_THROUGHPUT ? q : 0];
}

const char *adapt_health_name(adapt_health_state_t h)
{
    static const char *n[] = {
        "unavailable", "healthy", "degraded", "blocked", "recovering"
    };
    return n[h <= ADAPT_HEALTH_RECOVERING ? h : 0];
}

/* ------------------------------------------------------------------ */
/* Configuration: defaults + environment overrides                      */
/* ------------------------------------------------------------------ */

/*
 * Prior send-cost models in microseconds. Order of magnitude comes
 * from the project's own measurements (see benchmarks/cost_model_
 * constants.txt and results_summary.txt): UDS datagram send ~2-10 us
 * dominated by the socket layer; SHM ring push ~0.3-2 us dominated by
 * the payload copy. These are PRIORS: once >= learn_min_samples
 * measured costs exist per size class the learned fit replaces them.
 */
void adapt_cost_cfg_defaults(adapt_cost_cfg_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->uds_fixed_us = 3.0;
    cfg->uds_slope_us_per_b = 0.0015;   /* ~1.5 ns/B: socket copies   */
    cfg->shm_fixed_us = 0.3;
    cfg->shm_slope_us_per_b = 0.0004;   /* ~0.4 ns/B: ring memcpy     */

    cfg->switch_cost_us = 20.0;
    cfg->margin_us = 5.0;
    cfg->setup_cost_us = 52.0;          /* measured eager-map cost    */
    cfg->shm_notify_floor_us = 2000.0;  /* recv poll timeout (2 ms)   */
    cfg->shm_active_notify_us = 5.0;    /* active consumer: sched     */

    cfg->health_penalty_us[ADAPT_HEALTH_UNAVAILABLE] = 1e6;
    cfg->health_penalty_us[ADAPT_HEALTH_HEALTHY] = 0.0;
    cfg->health_penalty_us[ADAPT_HEALTH_DEGRADED] = 100.0;
    cfg->health_penalty_us[ADAPT_HEALTH_BLOCKED] = 1000.0;
    cfg->health_penalty_us[ADAPT_HEALTH_RECOVERING] = 25.0;

    cfg->qos = ADAPT_QOS_BALANCED;
    cfg->latency_budget_us = 0.0;       /* unlimited                  */
    cfg->latency_weight = 1.0;

    cfg->learn_min_samples = 32;
    cfg->learn_alpha = 0.1;
    cfg->crossover_min_b = 512.0;
    cfg->crossover_max_b = 1048576.0;
}

static double env_dbl(const char *name, double cur)
{
    const char *v = getenv(name);
    return v ? atof(v) : cur;
}

void adapt_cost_cfg_from_env(adapt_cost_cfg_t *cfg)
{
    if (!cfg) return;
    cfg->switch_cost_us = env_dbl("ADAPTIPC_SWITCH_COST_US",
                                  cfg->switch_cost_us);
    cfg->margin_us = env_dbl("ADAPTIPC_MARGIN_US", cfg->margin_us);
    cfg->setup_cost_us = env_dbl("ADAPTIPC_SETUP_COST_US",
                                 cfg->setup_cost_us);
    cfg->shm_notify_floor_us = env_dbl("ADAPTIPC_NOTIFY_FLOOR_US",
                                       cfg->shm_notify_floor_us);
    cfg->latency_budget_us = env_dbl("ADAPTIPC_LATENCY_BUDGET_US",
                                     cfg->latency_budget_us);
    cfg->learn_min_samples =
        (unsigned)env_dbl("ADAPTIPC_MIN_SAMPLES",
                          (double)cfg->learn_min_samples);
    cfg->learn_alpha = env_dbl("ADAPTIPC_LEARN_ALPHA", cfg->learn_alpha);
}

/* ------------------------------------------------------------------ */
/* Online cost calibration (Phase 3)                                    */
/* ------------------------------------------------------------------ */

void adapt_learn_init(adapt_learn_t *l, const adapt_cost_cfg_t *cfg)
{
    if (!l) return;
    memset(l, 0, sizeof(*l));
    /* Flat layout: UDS small=0, UDS large=1, SHM small=2, SHM large=3. */
    l->fixed_us[0] = l->fixed_us[1] = cfg->uds_fixed_us;
    l->slope_us_per_b[0] = l->slope_us_per_b[1] = cfg->uds_slope_us_per_b;
    l->fixed_us[2] = l->fixed_us[3] = cfg->shm_fixed_us;
    l->slope_us_per_b[2] = l->slope_us_per_b[3] = cfg->shm_slope_us_per_b;
    l->crossover_b = -1.0;              /* not yet learned            */
}

/* class index: 0 = small, 1 = large; transport offset: UDS 0/1,
 * SHM 2/3 in the flat arrays below. */
#define CLS_SMALL 0
#define CLS_LARGE 1
#define LIDX(t, c) (((t) == ADAPT_ROUTE_UDS ? 0 : 2) + (c))

static void observe_class(adapt_learn_t *l, const adapt_cost_cfg_t *cfg,
                          int idx, size_t size, double cost_us)
{
    const double a = cfg->learn_alpha;
    l->cls_mean_us[idx] = a * cost_us + (1.0 - a) * l->cls_mean_us[idx];
    l->cls_mean_b[idx] = a * (double)size + (1.0 - a) * l->cls_mean_b[idx];
    if (l->n[idx] < UINT64_MAX) l->n[idx]++;

    /* Refit this transport's linear model from the two class means. */
    const int other = (idx % 2 == 0) ? idx + 1 : idx - 1;
    const int base = (idx / 2) * 2;     /* 0 = UDS pair, 2 = SHM pair */
    if (l->n[base] >= cfg->learn_min_samples &&
        l->n[base + 1] >= cfg->learn_min_samples) {
        const double ds = l->cls_mean_b[idx] - l->cls_mean_b[other];
        if (fabs(ds) > 128.0) {
            const double slope = (l->cls_mean_us[idx] -
                                  l->cls_mean_us[other]) / ds;
            if (isfinite(slope) && slope > 1e-6) {
                const double fixed =
                    l->cls_mean_us[idx] - slope * l->cls_mean_b[idx];
                if (isfinite(fixed) && fixed >= 0.0) {
                    l->slope_us_per_b[idx] = slope;
                    l->fixed_us[idx] = fixed;
                    l->slope_us_per_b[other] = slope;
                    l->fixed_us[other] = fixed;
                }
            }
        }
    }
}

void adapt_learn_observe(adapt_learn_t *l, const adapt_cost_cfg_t *cfg,
                         adapt_route_t transport, size_t size,
                         double cost_us)
{
    if (!l || !cfg || !isfinite(cost_us) || cost_us < 0.0) return;
    if (transport != ADAPT_ROUTE_UDS && transport != ADAPT_ROUTE_SHM)
        return;
    const int cls = size <= ADAPT_LEARN_SMALL_MAX ? CLS_SMALL : CLS_LARGE;
    observe_class(l, cfg, LIDX(transport, cls), size, cost_us);
}

double adapt_learn_crossover(adapt_learn_t *l, const adapt_cost_cfg_t *cfg)
{
    if (!l || !cfg) return -1.0;
    /* small-class indices: UDS=0, SHM=2; large-class: 1 and 3. */
    if (l->n[0] < cfg->learn_min_samples ||
        l->n[2] < cfg->learn_min_samples ||
        l->n[1] < cfg->learn_min_samples ||
        l->n[3] < cfg->learn_min_samples)
        return l->crossover_b;          /* keep previous (or -1)      */

    const double dfs = l->slope_us_per_b[0] - l->slope_us_per_b[2];
    if (dfs <= 1e-9) return l->crossover_b;  /* no per-byte advantage */

    double s = (l->fixed_us[2] - l->fixed_us[0]) / dfs;
    if (!isfinite(s)) return l->crossover_b;
    if (s < cfg->crossover_min_b) s = cfg->crossover_min_b;
    if (s > cfg->crossover_max_b) s = cfg->crossover_max_b;

    /* Rate-limit threshold movement (EWMA) to prevent oscillation. */
    if (l->crossover_b < 0.0) l->crossover_b = s;
    else l->crossover_b += cfg->learn_alpha * (s - l->crossover_b);
    l->crossover_updates++;
    return l->crossover_b;
}

/* ------------------------------------------------------------------ */
/* Decision logging                                                     */
/* ------------------------------------------------------------------ */

void adapt_declog_init(adapt_declog_t *dl)
{
    if (dl) memset(dl, 0, sizeof(*dl));
}

void adapt_declog_record(adapt_declog_t *dl, const adapt_decision_t *d)
{
    if (!dl || !dl->enabled || !d) return;
    dl->ring[dl->next % ADAPT_DECISION_LOG_N] = *d;
    dl->next++;
    if (dl->count < ADAPT_DECISION_LOG_N) dl->count++;
}

void adapt_declog_dump(const adapt_declog_t *dl, const char *path)
{
    if (!dl || !dl->enabled || !path || dl->count == 0) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "seq,ts_ns,payload,occ_bytes,current,"
               "cost_uds_us,cost_shm_us,queue_wait_shm_us,"
               "switch_cost_us,setup_cost_us,health_penalty_us,"
               "latency_penalty_us,score_uds_us,score_shm_us,"
               "selected,reason\n");
    const unsigned total = dl->count;
    for (unsigned k = 0; k < total; k++) {
        const adapt_decision_t *d =
            &dl->ring[(dl->next - total + k) % ADAPT_DECISION_LOG_N];
        fprintf(f,
                "%llu,%llu,%zu,%.0f,%s,%.2f,%.2f,%.2f,"
                "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%s,%s\n",
                (unsigned long long)d->seq,
                (unsigned long long)d->ts_ns,
                d->payload, d->occ_bytes,
                adapt_route_name(d->current),
                d->cost_uds, d->cost_shm,
                d->queue_wait_shm_us,
                d->switch_cost_us, d->setup_cost_us,
                d->health_penalty_us, d->latency_penalty_us,
                d->score_uds, d->score_shm,
                adapt_route_name(d->selected), d->reason);
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Cost primitives                                                      */
/* ------------------------------------------------------------------ */

double adapt_cost_uds(const adapt_cost_cfg_t *cfg, size_t size)
{
    return cfg->uds_fixed_us + cfg->uds_slope_us_per_b * (double)size;
}

double adapt_cost_shm(const adapt_cost_cfg_t *cfg, size_t size,
                      double queue_wait_us, double health_penalty_us,
                      int shm_mapped)
{
    double c = cfg->shm_fixed_us + cfg->shm_slope_us_per_b * (double)size;
    if (queue_wait_us > 0.0) c += queue_wait_us;
    if (health_penalty_us > 0.0) c += health_penalty_us;
    if (!shm_mapped) c += cfg->setup_cost_us;
    return c;
}

/* ------------------------------------------------------------------ */
/* The decision function                                                */
/* ------------------------------------------------------------------ */

static void snprintf_reason(adapt_decision_t *d, const char *r)
{
    snprintf(d->reason, sizeof(d->reason), "%s", r);
}

adapt_route_t adapt_policy_decide(adapt_policy_mode_t policy,
                                  const adapt_cost_cfg_t *cfg,
                                  const adapt_rtctx_t *rt,
                                  const adapt_learn_t *learn,
                                  const adapt_health_t *health,
                                  int shm_mapped,
                                  double ewma_size,
                                  adapt_route_t current,
                                  size_t payload,
                                  uint64_t now_ns,
                                  adapt_decision_t *d)
{
    adapt_decision_t local;
    if (!d) d = &local;
    memset(d, 0, sizeof(*d));
    d->ts_ns = now_ns;
    d->payload = payload;
    d->current = current;

    /* ---- size-only family -------------------------------------- */
    if (policy == ADAPT_POLICY_SIZE_ONLY) {
        d->selected = ewma_size >= ADAPT_TAU_HIGH ? ADAPT_ROUTE_SHM
                                                  : ADAPT_ROUTE_UDS;
        snprintf_reason(d, "SIZE_THRESHOLD");
        d->score_uds = d->cost_uds = adapt_cost_uds(cfg, payload);
        d->score_shm = d->cost_shm = adapt_cost_shm(cfg, payload,
                                                    0, 0, shm_mapped);
        return d->selected;
    }
    if (policy == ADAPT_POLICY_SIZE_HYSTERESIS ||
        (policy == ADAPT_POLICY_FULL_ADAPTIVE && learn &&
         (learn->n[0] < cfg->learn_min_samples ||
          learn->n[2] < cfg->learn_min_samples))) {
        /* Original validated policy; also the cold-start fallback of
         * FULL_ADAPTIVE (safe defaults during learning). Backlog
         * safety needs no learned costs, so the queue-pressure escape
         * applies here too once the runtime context has samples. */
        adapt_route_t r;
        if (ewma_size >= ADAPT_TAU_HIGH) r = ADAPT_ROUTE_SHM;
        else if (ewma_size <= ADAPT_TAU_LOW) r = ADAPT_ROUTE_UDS;
        else r = current == ADAPT_ROUTE_SHM ? ADAPT_ROUTE_SHM
                                            : ADAPT_ROUTE_UDS;
        d->selected = r;
        snprintf_reason(d, policy == ADAPT_POLICY_SIZE_HYSTERESIS
                               ? "SIZE_HYSTERESIS" : "COLD_START");
        if (policy == ADAPT_POLICY_FULL_ADAPTIVE && rt &&
            r == ADAPT_ROUTE_SHM && ewma_size >= ADAPT_TAU_HIGH) {
            const size_t occ = (size_t)rt->ewma_occ_bytes;
            const double q = adapt_rtctx_queue_wait_us(rt, payload, occ);
            double hp = 0.0;
            if (health && shm_mapped)
                hp = cfg->health_penalty_us[health->state];
            /* Escape only when the predicted wait (plus any health
             * penalty) beats the whole UDS send cost PLUS the
             * switching cost: transient backlog spikes must not pay
             * the switch. */
            if (q + hp > adapt_cost_uds(cfg, payload) +
                             cfg->switch_cost_us &&
                payload + 1 <= UDS_MAX_DGRAM) {
                r = ADAPT_ROUTE_UDS;
                d->selected = r;
                snprintf_reason(d, hp > 0 ? "HEALTH_ESCAPE"
                                          : "QUEUE_PRESSURE");
            }
            d->queue_wait_shm_us = q;
            d->health_penalty_us = hp;
        }
        d->score_uds = d->cost_uds = adapt_cost_uds(cfg, payload);
        d->score_shm = d->cost_shm = adapt_cost_shm(cfg, payload,
                                                    d->queue_wait_shm_us,
                                                    0, shm_mapped);
        return r;
    }

    /* ---- queue occupancy (shared by queue-aware and up) --------- */
    const size_t occ = rt ? (size_t)rt->ewma_occ_bytes : 0;
    d->occ_bytes = (double)occ;
    const double q_shm = policy >= ADAPT_POLICY_QUEUE_AWARE
        ? adapt_rtctx_queue_wait_us(rt, payload, occ)
        : 0.0;
    d->queue_wait_shm_us = q_shm;

    if (policy == ADAPT_POLICY_QUEUE_AWARE) {
        /* Size-hysteresis decision, but escape SHM when the predicted
         * queue wait exceeds the whole UDS send cost and the message
         * physically fits UDS. Back to size rules once the queue
         * drains. */
        adapt_route_t r;
        if (ewma_size >= ADAPT_TAU_HIGH) {
            r = ADAPT_ROUTE_SHM;
            /* Same switching-cost threshold as the cold-start path. */
            if (q_shm > adapt_cost_uds(cfg, payload) +
                        cfg->switch_cost_us &&
                payload + 1 <= UDS_MAX_DGRAM)
                r = ADAPT_ROUTE_UDS;
        } else if (ewma_size <= ADAPT_TAU_LOW) {
            r = ADAPT_ROUTE_UDS;
        } else {
            r = current == ADAPT_ROUTE_SHM ? ADAPT_ROUTE_SHM
                                           : ADAPT_ROUTE_UDS;
        }
        d->selected = r;
        snprintf_reason(d, r == ADAPT_ROUTE_UDS &&
                            ewma_size >= ADAPT_TAU_HIGH
                            ? "QUEUE_PRESSURE" : "SIZE_HYSTERESIS");
        d->cost_uds = adapt_cost_uds(cfg, payload);
        d->cost_shm = adapt_cost_shm(cfg, payload, q_shm, 0, shm_mapped);
        d->score_uds = d->cost_uds;
        d->score_shm = d->cost_shm;
        return r;
    }

    /* ---- full effective-cost comparison (COST_AWARE and up) ----- */
    double fixed_uds = cfg->uds_fixed_us, slope_uds = cfg->uds_slope_us_per_b;
    double fixed_shm = cfg->shm_fixed_us, slope_shm = cfg->shm_slope_us_per_b;
    if (learn) {
        fixed_uds = learn->fixed_us[0];  slope_uds = learn->slope_us_per_b[0];
        fixed_shm = learn->fixed_us[2];  slope_shm = learn->slope_us_per_b[2];
    }
    d->cost_uds = fixed_uds + slope_uds * (double)payload;
    d->cost_shm = fixed_shm + slope_shm * (double)payload + q_shm;

    /* Health penalty (FULL_ADAPTIVE only). */
    if (policy == ADAPT_POLICY_FULL_ADAPTIVE && health &&
        shm_mapped) {
        d->health_penalty_us =
            cfg->health_penalty_us[health->state];
    }
    d->cost_shm += d->health_penalty_us;

    /* Setup cost: comparing SHM before it is mapped must include the
     * one-time negotiation (lazy establishment, Phase F evidence). */
    if (!shm_mapped) {
        d->setup_cost_us = cfg->setup_cost_us;
        d->cost_shm += d->setup_cost_us;
    }

    /* Notification floor: an idle consumer notices ring arrivals only
     * after its poll timeout; UDS wakes poll() immediately. When the
     * consumer is actively draining the floor collapses. This term is
     * what makes small-message UDS routing emerge from measurement
     * rather than a hardcoded size rule. */
    const int consumer_active =
        rt && rt->last_drain_ns != 0 &&
        now_ns >= rt->last_drain_ns &&
        (now_ns - rt->last_drain_ns) < ADAPT_ACTIVE_WINDOW_NS;
    const double notify = shm_mapped
        ? (consumer_active ? cfg->shm_active_notify_us
                           : cfg->shm_notify_floor_us)
        : 0.0;

    /* Latency terms and QoS weighting. Each transport is penalized by
     * its own predicted-latency overshoot beyond the budget (zero when
     * no budget is set or QoS is THROUGHPUT). */
    const double lat_uds = d->cost_uds;
    const double lat_shm = q_shm + notify +
        fixed_shm + slope_shm * (double)payload;
    d->latency_penalty_us = 0.0;
    if (cfg->qos != ADAPT_QOS_THROUGHPUT &&
        cfg->latency_budget_us > 0.0) {
        const double over_uds = lat_uds > cfg->latency_budget_us
            ? lat_uds - cfg->latency_budget_us : 0.0;
        const double over_shm = lat_shm > cfg->latency_budget_us
            ? lat_shm - cfg->latency_budget_us : 0.0;
        d->latency_penalty_us =
            cfg->latency_weight * (over_uds > over_shm ? over_uds
                                                       : over_shm);
    }

    /* Switching cost applies to the transport we would move TO. */
    d->switch_cost_us = cfg->switch_cost_us;
    d->score_uds = d->cost_uds +
        (current == ADAPT_ROUTE_UDS ? 0.0 : cfg->switch_cost_us);
    d->score_shm = d->cost_shm +
        (current == ADAPT_ROUTE_SHM ? 0.0 : cfg->switch_cost_us);
    if (d->latency_penalty_us > 0.0) {
        /* Charge the overshoot to the transport whose predicted
         * latency overshoots (the larger one). */
        if (lat_shm > lat_uds) d->score_shm += d->latency_penalty_us;
        else                   d->score_uds += d->latency_penalty_us;
    }

    adapt_route_t candidate =
        d->score_shm <= d->score_uds ? ADAPT_ROUTE_SHM : ADAPT_ROUTE_UDS;

    /* Stability rule: switch only on a real improvement. The very first
     * decision (current == NONE) is exempt: there is no route to
     * protect, and leaving last_route at NONE would break the
     * escalation guards downstream. */
    if (current == ADAPT_ROUTE_NONE) {
        d->selected = candidate;
        snprintf_reason(d, "INITIAL_ROUTE");
        return candidate;
    }
    if (candidate != current) {
        const double cand = candidate == ADAPT_ROUTE_SHM
                                ? d->score_shm : d->score_uds;
        const double curr = current == ADAPT_ROUTE_SHM
                                ? d->score_shm : d->score_uds;
        if (cand + cfg->margin_us < curr) {
            d->selected = candidate;
            snprintf_reason(d,
                !shm_mapped && candidate == ADAPT_ROUTE_SHM
                    ? "SETUP_WORTHWHILE"
                    : (q_shm > cfg->switch_cost_us
                           ? "QUEUE_PRESSURE" : "COST_WIN"));
        } else {
            d->selected = current;
            snprintf_reason(d, "STICKY_MARGIN");
        }
    } else {
        d->selected = current;
        snprintf_reason(d, "COST_STABLE");
    }

    /* Learned crossover observability (Phase 3). */
    if (policy == ADAPT_POLICY_FULL_ADAPTIVE && learn)
        adapt_learn_crossover((adapt_learn_t *)learn, cfg);
    return d->selected;
}
