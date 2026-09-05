#ifndef COST_MODEL_H
#define COST_MODEL_H

/*
 * cost_model.h -- context-aware, cost-aware routing for AdaptIPC.
 *
 * Pipeline (per adapt_send()):
 *
 *   payload size, ring occupancy, timestamps
 *        |
 *   [runtime_context]  EWMA arrival/drain rates, occupancy, latency
 *        |
 *   [cost model]       effective per-transport cost estimates
 *        |
 *   [policy decision]  min-cost + switching margin + health + QoS
 *        |
 *   route (UDS / SHM), optional decision-log record
 *
 * All costs are in MICROSECONDS so numbers are directly interpretable
 * and the same units flow through queueing estimates, measured send
 * costs, switching penalties and latency budgets.
 *
 * Synchronization: every structure here is owned by the producer-side
 * call path (adapt_send); no cross-thread sharing, no atomics needed.
 */

#include <stddef.h>
#include <stdint.h>

#include "adapt_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Policy / QoS / health enums                                          */
/* ------------------------------------------------------------------ */

typedef enum {
    ADAPT_POLICY_DEFAULT = 0,        /* == SIZE_HYSTERESIS (compat)      */
    ADAPT_POLICY_SIZE_ONLY = 1,      /* raw EWMA threshold, no deadband  */
    ADAPT_POLICY_SIZE_HYSTERESIS = 2,/* original validated policy        */
    ADAPT_POLICY_QUEUE_AWARE = 3,    /* + queue-wait penalty on SHM      */
    ADAPT_POLICY_COST_AWARE = 4,     /* + measured costs + switch margin */
    ADAPT_POLICY_FULL_ADAPTIVE = 5   /* + health + learned crossover+QoS */
} adapt_policy_mode_t;

typedef enum {
    ADAPT_QOS_BALANCED = 0,          /* combined cost (default)          */
    ADAPT_QOS_LATENCY = 1,           /* double weight on latency terms   */
    ADAPT_QOS_THROUGHPUT = 2         /* ignore latency budget            */
} adapt_qos_t;

typedef enum {
    ADAPT_HEALTH_UNAVAILABLE = 0,    /* not mapped / not negotiated      */
    ADAPT_HEALTH_HEALTHY = 1,
    ADAPT_HEALTH_DEGRADED = 2,       /* high occupancy or slow drain     */
    ADAPT_HEALTH_BLOCKED = 3,        /* ~full ring, producer parked      */
    ADAPT_HEALTH_RECOVERING = 4      /* drained, probation before HEA.   */
} adapt_health_state_t;

const char *adapt_policy_name(adapt_policy_mode_t p);
const char *adapt_qos_name(adapt_qos_t q);
const char *adapt_health_name(adapt_health_state_t h);

/* ------------------------------------------------------------------ */
/* Runtime context collector (Phase 1)                                  */
/* ------------------------------------------------------------------ */

typedef struct adapt_rtctx {
    /* EWMA estimates (all producer-side, sampled once per adapt_send) */
    double ewma_arrival_bps;   /* offered bytes/s                          */
    double ewma_drain_bps;     /* bytes/s drained between observations     */
    double ewma_occ_bytes;     /* ring occupancy at send time              */
    double ewma_latency_us;    /* recent send-side cost (measured, us)     */

    /* drain-rate bookkeeping: between two send() observations the
     * consumer drained (prev_occ + prev_size) - occ bytes in dt ns. */
    double    prev_occ_bytes;  /* occupancy *after* previous push          */
    uint64_t  last_ns;
    uint64_t  last_drain_ns;   /* last observation of positive drain       */
    unsigned  samples;
    int       drain_valid;     /* >= 1 positive drain observation          */

    /* Minimum samples before rate-based decisions are trusted. Until
     * then queue_wait_us() reports the conservative fallback. */
    unsigned  min_samples;
} adapt_rtctx_t;

void   adapt_rtctx_init(adapt_rtctx_t *rc, unsigned min_samples);

/* Record one observation. `occ_before_push` is the ring occupancy seen
 * before this message is pushed; `payload` is its size. alpha in (0,1]. */
void   adapt_rtctx_sample(adapt_rtctx_t *rc, size_t payload,
                          size_t occ_before_push, uint64_t now_ns,
                          double alpha);

/*
 * Estimated queueing wait (us) for a message of `incoming` bytes given
 * the current occupancy. Robustness rules:
 *   - occupancy 0                      -> 0 (empty queue)
 *   - drain rate unknown (<min samples)-> conservative fallback using
 *     the DRAIN_FLOOR constant below (never divide by ~0)
 *   - drain rate <= 0 or stalled       -> same fallback
 */
double adapt_rtctx_queue_wait_us(const adapt_rtctx_t *rc, size_t incoming,
                                 size_t occ_bytes);

/* Conservative drain-rate floor (bytes/s) used when the consumer's rate
 * is unknown or stalled. Chosen well below any measured rate so the
 * fallback OVERestimates queue wait (safe direction for routing). */
#define ADAPT_DRAIN_FLOOR_BPS (50.0e6)

/* Consumer considered "active" if a drain was observed within this
 * window; else SHM delivery to a poll-blocked consumer incurs the
 * notification floor (see cost model). */
#define ADAPT_ACTIVE_WINDOW_NS (20ull * 1000 * 1000)

/* ------------------------------------------------------------------ */
/* Transport health (Phase 4)                                           */
/* ------------------------------------------------------------------ */

typedef struct adapt_health {
    adapt_health_state_t state;
    unsigned bad_streak;       /* consecutive degraded samples            */
    unsigned good_streak;      /* consecutive healthy samples             */
    unsigned transitions;
    /* Debounce: state changes need this many consecutive bad/good
     * samples, so one outlier never flips the state. */
    unsigned debounce;
} adapt_health_t;

void adapt_health_init(adapt_health_t *h, unsigned debounce);
/* Update from a SHM observation; returns 1 if the state changed. */
int  adapt_health_update_shm(adapt_health_t *h, size_t occ_bytes,
                             size_t capacity, int drain_valid,
                             double arrival_bps, double drain_bps);

/* ------------------------------------------------------------------ */
/* Cost model + decision (Phases 2, 3, 5)                               */
/* ------------------------------------------------------------------ */

typedef struct adapt_cost_cfg {
    /* Linear per-transport send-cost priors, microseconds:
     *   cost_T(S) = fixed_T + slope_T * S
     * Replaced online by measured EWMA per transport/size-class once
     * enough samples exist (self-calibration, Phase 3). */
    double uds_fixed_us, uds_slope_us_per_b;
    double shm_fixed_us, shm_slope_us_per_b;

    /* Penalty applied to the transport we would switch AWAY from. */
    double switch_cost_us;
    /* Decision margin: switch only if
     *   cost(new) + margin < cost(current) - switch_cost
     * Noise-stability: with estimation error eps per side, margin H
     * prevents noise-driven flips whenever H > 2*eps. */
    double margin_us;
    /* One-time SHM setup cost when the ring is not yet mapped. */
    double setup_cost_us;
    /* SHM notification latency model: a consumer blocked in the recv
     * poll loop notices ring arrivals only after the poll timeout
     * unless it is actively draining (then only a scheduling delay
     * remains). This is why small messages legitimately belong on UDS
     * -- modeled and measured, not hardcoded in the policy. */
    double shm_notify_floor_us;
    double shm_active_notify_us;

    /* Health penalties per SHM state (us, additive to SHM cost). */
    double health_penalty_us[5]; /* indexed by adapt_health_state_t */

    adapt_qos_t qos;
    double latency_budget_us;    /* 0 = unlimited                       */
    double latency_weight;       /* 1.0 balanced, 2.0 latency mode      */

    /* Learning parameters (Phase 3). */
    unsigned learn_min_samples;  /* per transport/size-class            */
    double   learn_alpha;        /* EWMA on measured costs              */
    double   crossover_min_b;    /* learned-threshold clamp             */
    double   crossover_max_b;
} adapt_cost_cfg_t;

/* Defaults + env overrides (ADAPTIPC_* variables), centralized here. */
void adapt_cost_cfg_defaults(adapt_cost_cfg_t *cfg);
void adapt_cost_cfg_from_env(adapt_cost_cfg_t *cfg);

/* Learned per-transport cost state (online calibration).
 * Flat layout: index = transport_offset + class, transport_offset
 * UDS=0 / SHM=2, class small=0 / large=1. Each class tracks the EWMA
 * of observed (size, cost) pairs; the per-transport linear model
 * (fixed, slope) is refit from the two class means. */
typedef struct adapt_learn {
    double   cls_mean_us[4];
    double   cls_mean_b[4];
    uint64_t n[4];
    double   fixed_us[4];          /* [0]=uds,[1]=uds-large,[2]=shm,[3]=shm-large */
    double   slope_us_per_b[4];
    double   crossover_b;          /* learned S* (bytes), observable */
    unsigned crossover_updates;
} adapt_learn_t;

#define ADAPT_LEARN_SMALL_MAX 1024u

void adapt_learn_init(adapt_learn_t *l, const adapt_cost_cfg_t *cfg);
/* Feed one measured send cost (us) for transport T and payload S. */
void adapt_learn_observe(adapt_learn_t *l, const adapt_cost_cfg_t *cfg,
                         adapt_route_t transport, size_t size,
                         double cost_us);
/* Recompute the learned crossover from current linear fits. Returns
 * the (clamped) crossover in bytes; -1 if not yet learnable. */
double adapt_learn_crossover(adapt_learn_t *l, const adapt_cost_cfg_t *cfg);

/* One routing decision (instrumentation record). */
typedef struct adapt_decision {
    uint64_t   seq;
    uint64_t   ts_ns;
    size_t     payload;
    double     occ_bytes;
    adapt_route_t current, selected;
    double     cost_uds, cost_shm;
    double     queue_wait_shm_us;
    double     switch_cost_us, setup_cost_us, health_penalty_us;
    double     latency_penalty_us;
    double     score_uds, score_shm;
    char       reason[24];
} adapt_decision_t;

/* Keep the last N decisions in a ring for dumping. */
#define ADAPT_DECISION_LOG_N 512

typedef struct adapt_declog {
    int enabled;
    unsigned count, next;        /* total recorded, ring cursor         */
    adapt_decision_t ring[ADAPT_DECISION_LOG_N];
} adapt_declog_t;

void adapt_declog_init(adapt_declog_t *dl);
void adapt_declog_record(adapt_declog_t *dl, const adapt_decision_t *d);
/* Append the full log as CSV to `path` (called from adapt_shutdown). */
void adapt_declog_dump(const adapt_declog_t *dl, const char *path);

/*
 * The decision function. Inputs: policy mode, config, runtime context,
 * learned costs, SHM health/mapped state, EWMA size, current route,
 * payload size. Returns the route to use and (optionally) fills `d`
 * with the full scoring breakdown for instrumentation.
 */
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
                                  adapt_decision_t *d /* may be NULL */);

/* Cost primitives, exposed for tests and the ablation harness. */
double adapt_cost_uds(const adapt_cost_cfg_t *cfg, size_t size);
double adapt_cost_shm(const adapt_cost_cfg_t *cfg, size_t size,
                      double queue_wait_us, double health_penalty_us,
                      int shm_mapped);

#ifdef __cplusplus
}
#endif

#endif /* COST_MODEL_H */
