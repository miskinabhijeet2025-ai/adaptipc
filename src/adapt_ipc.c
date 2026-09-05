/*
 * adapt_ipc.c -- adaptive IPC router with EWMA-based transport selection.
 *
 * Wire format on both transports:
 *   [1 byte transport tag][payload...]
 * The tag lets adapt_recv() demultiplex when sender and receiver disagree
 * on the route for consecutive messages (e.g., during EWMA transitions).
 */
#define _POSIX_C_SOURCE 200809L

#include "adapt_ipc.h"
#include "shm_ringbuffer.h"
#include "uds_fallback.h"
#include "cost_model.h"

#include <errno.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Wire-format tag byte (first byte of every framed record):
 *   1 = SHM payload, 2 = UDS payload, 0 = control (never a data tag). */
#define ADAPT_WIRE_TAG_CTRL 0x00

/* Control message sub-types carried inside a ctrl frame:
 *   [tag=0][type][payload]
 *   SHM_SETUP_REQ payload: u32 LE ring capacity, then NUL-terminated name
 *   SHM_SETUP_ACK payload: u8 status (0 = ok) */
#define ADAPT_CTRL_SHM_SETUP_REQ 1
#define ADAPT_CTRL_SHM_SETUP_ACK 2

/* Producer waits at most this long for the receiver's ACK before falling
 * back to UDS for the current message (fallback stays active throughout,
 * so no message is ever dropped during the asynchronous handshake). */
#define ADAPT_NEGOT_DEADLINE_MS 250
#define ADAPT_NEGOT_POLL_MS     5
#define ADAPT_NEGOT_RESEND_MS   100

static inline uint64_t stats_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#define STATS_T0(ctx) uint64_t t0_ = (ctx->stats_on ? stats_now_ns() : 0)
#define STATS_ACC(field) do { if (ctx->stats_on) ctx->st.field += \
                              stats_now_ns() - t0_; } while (0)


struct adapt_ctx {
    adapt_role_t    role;
    adapt_config_t  cfg;
    double          ewma_size;
    adapt_route_t   last_route;
    shm_ring_t     *ring;       /* NULL until lazily negotiated/mapped */
    uds_endpoint_t *uds;
    unsigned char  *rxbuf;      /* framed-record scratch (grows to ring cap) */
    size_t          rxbuf_cap;
    int             stats_on;
    adapt_stats_t   st;
    /* retry-resume state: classification of the in-flight message */
    int             retry_pending;
    size_t          pending_size;
    adapt_route_t   pending_route;
    double          alpha;   /* EWMA smoothing factor; ADAPTIPC_ALPHA env
                                 overrides ADAPT_EWMA_ALPHA default */
    /* lazy SHM negotiation state (producer side) */
    int             negot_done;     /* ring mapped on both sides          */
    char            neg_name[64];   /* shm object name being negotiated   */
    uint64_t        last_req_ns;    /* last SHM_SETUP_REQ send time       */
    int             eager_shm;      /* ADAPTIPC_EAGER_SHM=1 restores eager */

    /* context-aware policy state (producer side, single-threaded) */
    adapt_policy_mode_t policy;
    adapt_cost_cfg_t    cmcfg;
    adapt_rtctx_t       rt;
    adapt_learn_t       learn;
    adapt_health_t      health;
    adapt_declog_t      declog;
    const char         *declog_path;
    unsigned long       decision_seq;
};

static inline double ewma_update(double ewma, double alpha, size_t current_size)
{
    /* ewma_size = (alpha * current) + ((1 - alpha) * ewma) */
    return alpha * (double)current_size + (1.0 - alpha) * ewma;
}

static int grow_rxbuf(adapt_ctx_t *ctx, size_t cap)
{
    if (ctx->rxbuf_cap >= cap) return 0;
    unsigned char *nb = realloc(ctx->rxbuf, cap);
    if (!nb) return -ENOMEM;
    ctx->rxbuf = nb;
    ctx->rxbuf_cap = cap;
    return 0;
}

/*
 * Lazy SHM negotiation (producer side). Sends SHM_SETUP_REQ over the UDS
 * control channel naming a fresh shm object, then waits for SHM_SETUP_ACK.
 * The UDS fallback remains fully usable during the handshake, so the
 * caller can degrade to UDS while the handshake is in flight -- no
 * messages are dropped. Returns 0 once the ring is mapped on both sides,
 * -ETIMEDOUT if the deadline passed, negative errno otherwise.
 */
static void generate_shm_name(char *buf, size_t bufsz)
{
    static _Atomic unsigned neg_counter;
    snprintf(buf, bufsz, "/adaptipc_shm_%ld_%u",
             (long)getpid(),
             atomic_fetch_add_explicit(&neg_counter, 1,
                                       memory_order_relaxed) + 1u);
}

static int producer_map_ring(adapt_ctx_t *ctx)
{
    shm_ring_role_t rrole = SHM_RING_ROLE_PRODUCER;
    /* Attach (no O_TRUNC): the consumer already created/sized the object
     * in response to our REQ. As the producer role we still initialize
     * the header with release semantics; the consumer's pops simply see
     * an empty ring until that becomes visible. */
    int rc = shm_ring_attach(ctx->neg_name, ctx->cfg.shm_capacity,
                             rrole, &ctx->ring);
    if (rc != 0) return rc;
    rc = grow_rxbuf(ctx, ctx->cfg.shm_capacity);
    if (rc != 0) { shm_ring_close(ctx->ring, 0); ctx->ring = NULL; }
    return rc;
}

static int producer_ensure_shm(adapt_ctx_t *ctx, uint64_t now_ns)
{
    if (ctx->negot_done) return 0;

    if (ctx->neg_name[0] == '\0')
        generate_shm_name(ctx->neg_name, sizeof(ctx->neg_name));

    /* Send (or, while pending, periodically re-send) the setup request. */
    if (now_ns - ctx->last_req_ns >
        (uint64_t)ADAPT_NEGOT_RESEND_MS * 1000000u) {
        unsigned char cbuf[6 + 64];
        uint32_t cap32 = (uint32_t)ctx->cfg.shm_capacity;
        cbuf[0] = ADAPT_WIRE_TAG_CTRL;
        cbuf[1] = ADAPT_CTRL_SHM_SETUP_REQ;
        memcpy(cbuf + 2, &cap32, sizeof(cap32));
        memcpy(cbuf + 6, ctx->neg_name, strlen(ctx->neg_name) + 1);
        int rc = uds_send(ctx->uds, ctx->cfg.peer_sock, cbuf,
                          6 + (int)strlen(ctx->neg_name) + 1);
        if (rc != 0) return rc;
        if (ctx->stats_on) ctx->st.shm_setup_reqs++;
        ctx->last_req_ns = now_ns;
    }

    /* Wait for the ACK, tolerating a bounded number of poll timeouts. */
    const uint64_t deadline =
        now_ns + (uint64_t)ADAPT_NEGOT_DEADLINE_MS * 1000000u;
    for (;;) {
        uint64_t t0 = ctx->stats_on ? stats_now_ns() : 0;
        int n = uds_recv_timeout(ctx->uds, ctx->rxbuf, ctx->rxbuf_cap,
                                 ADAPT_NEGOT_POLL_MS);
        if (ctx->stats_on) ctx->st.negot_wait_ns += stats_now_ns() - t0;
        if (n == -EAGAIN) {
            if (stats_now_ns() > deadline) return -ETIMEDOUT;
            continue;
        }
        if (n < 0) return n;
        if (n >= 3 && ctx->rxbuf[0] == ADAPT_WIRE_TAG_CTRL &&
            ctx->rxbuf[1] == ADAPT_CTRL_SHM_SETUP_ACK &&
            ctx->rxbuf[2] == 0) {
            if (ctx->stats_on) ctx->st.shm_setup_acks++;
            int rc = producer_map_ring(ctx);
            if (rc != 0) return rc;
            ctx->negot_done = 1;
            return 0;
        }
        /* Anything else (e.g. a duplicate REQ echoed back) is ignored. */
    }
}

/* Consumer side of the control channel, invoked from adapt_recv(). */
static void handle_control(adapt_ctx_t *ctx, const unsigned char *frame,
                           int frame_len)
{
    if (frame_len < 2) return;
    if (frame[1] == ADAPT_CTRL_SHM_SETUP_REQ) {
        if (frame_len < 7) return;
        uint32_t cap32;
        memcpy(&cap32, frame + 2, sizeof(cap32));
        const char *name = (const char *)frame + 6;
        if (cap32 == 0 || cap32 > (1u << 30)) return;
        if (!memchr(name, '\0', (size_t)frame_len - 6)) return;

        int status = 0;
        if (!ctx->ring) {
            shm_ring_role_t rrole = SHM_RING_ROLE_CONSUMER;
            /* Attach without waiting for producer header init: we must
             * ACK first, otherwise the producer (waiting on this ACK)
             * and we (waiting on its header init) would deadlock. */
            int rc = shm_ring_attach(name, cap32, rrole, &ctx->ring);
            if (rc != 0) {
                status = (uint8_t)(-rc);
            } else if (grow_rxbuf(ctx, cap32) != 0) {
                shm_ring_close(ctx->ring, 0);
                ctx->ring = NULL;
                status = (uint8_t)-ENOMEM;
            }
        }
        unsigned char ack[3] = {
            ADAPT_WIRE_TAG_CTRL, ADAPT_CTRL_SHM_SETUP_ACK, (unsigned char)status
        };
        (void)uds_send(ctx->uds, ctx->cfg.peer_sock, ack, sizeof(ack));
        if (ctx->stats_on && status == 0) ctx->st.shm_setup_acks++;
    }
    /* SETUP_ACK is consumed by the producer handshake, not here. */
}

int adapt_init(adapt_role_t role, const adapt_config_t *cfg, adapt_ctx_t **out)
{
    if (!cfg || !cfg->local_sock || !cfg->peer_sock || !out) return -EINVAL;
    int rc;

    adapt_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return -ENOMEM;

    ctx->role = role;
    ctx->ewma_size = 0.0;
    ctx->last_route = ADAPT_ROUTE_NONE;
    ctx->cfg.shm_name =
        cfg->shm_name ? cfg->shm_name : ADAPT_SHM_NAME_DEFAULT;
    ctx->cfg.local_sock = cfg->local_sock;
    ctx->cfg.peer_sock = cfg->peer_sock;
    ctx->cfg.shm_capacity = cfg->shm_capacity ? cfg->shm_capacity : 65536;
    ctx->cfg.nonblocking_send = cfg->nonblocking_send;
    ctx->stats_on = getenv("ADAPTIPC_STATS") != NULL;
    ctx->eager_shm = getenv("ADAPTIPC_EAGER_SHM") != NULL;
    {
        const char *as = getenv("ADAPTIPC_ALPHA");
        double a = as ? atof(as) : (double)0.0;
        ctx->alpha = (a > 0.0 && a <= 1.0) ? a : (double)0.2;
    }

    /* Policy / QoS resolution: explicit config wins, then env, then the
     * validated size+hysteresis default (backward compatible). */
    ctx->policy = cfg->policy;
    if (ctx->policy == ADAPT_POLICY_DEFAULT) {
        const char *pv = getenv("ADAPTIPC_POLICY");
        ctx->policy = ADAPT_POLICY_SIZE_HYSTERESIS;
        if (pv) {
            if (!strcmp(pv, "size_only"))
                ctx->policy = ADAPT_POLICY_SIZE_ONLY;
            else if (!strcmp(pv, "size_hysteresis"))
                ctx->policy = ADAPT_POLICY_SIZE_HYSTERESIS;
            else if (!strcmp(pv, "queue_aware"))
                ctx->policy = ADAPT_POLICY_QUEUE_AWARE;
            else if (!strcmp(pv, "cost_aware"))
                ctx->policy = ADAPT_POLICY_COST_AWARE;
            else if (!strcmp(pv, "full_adaptive"))
                ctx->policy = ADAPT_POLICY_FULL_ADAPTIVE;
        }
    }
    adapt_cost_cfg_defaults(&ctx->cmcfg);
    ctx->cmcfg.qos = cfg->qos;
    ctx->cmcfg.latency_budget_us = cfg->latency_budget_us;
    adapt_cost_cfg_from_env(&ctx->cmcfg);
    if (ctx->cmcfg.qos == ADAPT_QOS_LATENCY &&
        !getenv("ADAPTIPC_LATENCY_WEIGHT"))
        ctx->cmcfg.latency_weight = 2.0;
    adapt_rtctx_init(&ctx->rt,
                     ctx->cmcfg.learn_min_samples > 8
                         ? 8 : ctx->cmcfg.learn_min_samples);
    adapt_learn_init(&ctx->learn, &ctx->cmcfg);
    adapt_health_init(&ctx->health, 8);
    adapt_declog_init(&ctx->declog);
    if (role == ADAPT_ROLE_PRODUCER)
        ctx->declog_path = getenv("ADAPTIPC_DECISION_LOG");
    if (ctx->declog_path) ctx->declog.enabled = 1;

    if (ctx->eager_shm) {
        shm_ring_role_t rrole = (role == ADAPT_ROLE_PRODUCER)
                                    ? SHM_RING_ROLE_PRODUCER
                                    : SHM_RING_ROLE_CONSUMER;
        /* Legacy eager path: both sides pass create=1 for idempotent
         * setup; header-init races are guarded by the `initialized`
         * flag inside shm_ring_create(). */
        /* Consumers attach without the header-init spin: in harnesses
         * the consumer may initialize before the producer exists, and
         * an uninitialized ring simply reads as empty (head == tail)
         * until the producer's release-published init lands. */
        if (rrole == SHM_RING_ROLE_CONSUMER)
            rc = shm_ring_attach(ctx->cfg.shm_name, ctx->cfg.shm_capacity,
                                 rrole, &ctx->ring);
        else
            rc = shm_ring_create(ctx->cfg.shm_name, ctx->cfg.shm_capacity,
                                 1, rrole, &ctx->ring);
        if (rc != 0) goto fail;
        ctx->negot_done = 1;
        rc = grow_rxbuf(ctx, ctx->cfg.shm_capacity);
        if (rc != 0) goto fail;
    }

    rc = uds_open(cfg->local_sock, &ctx->uds);
    if (rc != 0) goto fail;

    /* Lazy path: only a small UDS-sized scratch buffer up front; the
     * SHM mapping (and the grow of rxbuf) is deferred until the EWMA
     * classifier first routes to SHM. */
    rc = grow_rxbuf(ctx, UDS_MAX_DGRAM);
    if (rc != 0) goto fail;

    *out = ctx;
    return 0;

fail:
    if (ctx->ring) shm_ring_close(ctx->ring, role == ADAPT_ROLE_PRODUCER);
    free(ctx->rxbuf);
    free(ctx);
    return rc;
}

int adapt_send(adapt_ctx_t *ctx, const void *data, size_t size)
{
    if (!ctx || !data || size == 0) return -EINVAL;
    if (ctx->role != ADAPT_ROLE_PRODUCER) return -EPERM;

    /* Retry resumption: if the previous attempt of THIS logical message
     * failed with -EAGAIN, reuse its classification. Re-applying the EWMA
     * on every retry would let backlog pressure distort the classifier
     * (bulk retries inflate it to S_t; control retries decay it through
     * the deadband), flipping routes under load -- a real bug observed in
     * measurement. */
    /* Context sampling + health monitoring run on EVERY attempt,
     * including -EAGAIN retries: backpressure is exactly when the
     * health monitor and rate estimator must keep observing. */
    const uint64_t now = stats_now_ns();
    const size_t occ = shm_ring_used_bytes(ctx->ring);
    adapt_rtctx_sample(&ctx->rt, size, occ, now, ctx->alpha);
    if (ctx->ring) {
        const size_t cap = occ + shm_ring_free_bytes(ctx->ring);
        adapt_health_update_shm(&ctx->health, occ, cap,
                                ctx->rt.drain_valid,
                                ctx->rt.ewma_arrival_bps,
                                ctx->rt.ewma_drain_bps);
        if (ctx->stats_on)
            ctx->st.health_transitions = ctx->health.transitions;
    }

    adapt_route_t route;
    if (ctx->retry_pending && ctx->pending_size == size &&
        ctx->pending_route != ADAPT_ROUTE_NONE) {
        route = ctx->pending_route;
    } else {
        STATS_T0(ctx);
        ctx->ewma_size = ewma_update(ctx->ewma_size, ctx->alpha, size);
        STATS_ACC(router_ns);

        adapt_decision_t d;
        route = adapt_policy_decide(
            ctx->policy, &ctx->cmcfg, &ctx->rt,
            ctx->policy >= ADAPT_POLICY_COST_AWARE ? &ctx->learn : NULL,
            ctx->policy == ADAPT_POLICY_FULL_ADAPTIVE ? &ctx->health
                                                      : NULL,
            ctx->ring != NULL, ctx->ewma_size, ctx->last_route,
            size, now, ctx->declog.enabled ? &d : NULL);
        if (ctx->declog.enabled) {
            d.seq = ++ctx->decision_seq;
            adapt_declog_record(&ctx->declog, &d);
        }
        if (ctx->stats_on) ctx->st.decisions++;

        if (ctx->last_route != ADAPT_ROUTE_NONE &&
            route != ctx->last_route && ctx->stats_on)
            ctx->st.route_switches++;
        /* Remember the DECIDED route (not the per-message override). */
        ctx->last_route = route;
        ctx->retry_pending = 0;
        ctx->pending_route = ADAPT_ROUTE_NONE;
    }
    if (ctx->stats_on) ctx->st.sends++;

    /* Payload-size guard rails: if the classified route cannot physically
     * carry this message, escalate/fall back to the one that can.
     * This is a per-message transport override ONLY -- it must not move
     * the sticky EWMA state (last_route), otherwise deadband oscillation
     * would masquerade as adaptation. */
    if (route != ADAPT_ROUTE_SHM && size + 1 > UDS_MAX_DGRAM) {
        /* route == UDS or an undecided NONE: anything that cannot fit
         * a datagram must escalate to the ring. */
        route = ADAPT_ROUTE_SHM;
        if (ctx->stats_on) ctx->st.send_escalations++;
    }
    if (route == ADAPT_ROUTE_SHM && size + 1 > ctx->cfg.shm_capacity)
        return -EMSGSIZE;
    ctx->pending_size = size;
    ctx->pending_route = route;

    /* Lazy negotiation: the first SHM-classified send triggers the
     * SHM_SETUP_REQ/ACK handshake. Until it completes, the UDS fallback
     * carries the traffic (if it fits); oversized payloads must wait. */
    if (route == ADAPT_ROUTE_SHM && !ctx->ring) {
        int hrc = producer_ensure_shm(ctx, stats_now_ns());
        if (hrc != 0) {
            if (size + 1 <= UDS_MAX_DGRAM) {
                route = ADAPT_ROUTE_UDS; /* degrade; fallback stays active */
                if (ctx->stats_on) ctx->st.send_escalations++;
                ctx->pending_route = route;
            } else {
                ctx->retry_pending = 1;
                return hrc == -ETIMEDOUT ? -EAGAIN : hrc;
            }
        }
    }

    /* Frame: tag byte + payload. */
    unsigned char frame[UDS_MAX_DGRAM];
    int rc;

    if (route == ADAPT_ROUTE_SHM) {
        if (ctx->stats_on) ctx->st.sends_shm++;
        /* Zero-copy framing: assemble [tag][payload] directly in ring
         * slots via scatter push -- no staging malloc, no concat copy.
         * For cost-aware policies the first (non-blocking) attempt is
         * timed for calibration; parking time must not poison the
         * transport-cost estimate, so the blocking retry is unmeasured. */
        const int measure = ctx->policy >= ADAPT_POLICY_COST_AWARE;
        STATS_T0(ctx);
        const uint64_t t_meas = measure ? stats_now_ns() : 0;
        const unsigned char tag = (unsigned char)ADAPT_ROUTE_SHM;
        shm_segment_t segs[2] = {
            { .base = &tag,  .len = 1 },
            { .base = data,  .len = size },
        };
        if (ctx->cfg.nonblocking_send)
            rc = shm_ring_push_scatter(ctx->ring, segs, 2);
        else if (measure) {
            rc = shm_ring_push_scatter(ctx->ring, segs, 2);
            if (rc == -EAGAIN)
                rc = shm_ring_push_scatter_blocking(ctx->ring, segs, 2);
        } else
            rc = shm_ring_push_scatter_blocking(ctx->ring, segs, 2);
        if (t_meas) {
            const double us =
                (double)(stats_now_ns() - t_meas) / 1000.0;
            adapt_learn_observe(&ctx->learn, &ctx->cmcfg,
                                ADAPT_ROUTE_SHM, size, us);
            ctx->rt.ewma_latency_us = ctx->alpha * us +
                (1.0 - ctx->alpha) * ctx->rt.ewma_latency_us;
        }
        if (ctx->stats_on) {
            ctx->st.send_copy_ns += stats_now_ns() - t0_;
            ctx->st.send_copy_bytes += size + 1;
            ctx->st.backpressure_parks = shm_ring_park_count(ctx->ring);
        }
        if (rc == -EAGAIN) {
            ctx->retry_pending = 1; /* resume with stored route on retry */
            return rc;
        }
        ctx->retry_pending = 0;
    } else {
        if (ctx->stats_on) ctx->st.sends_uds++;
        const int measure = ctx->policy >= ADAPT_POLICY_COST_AWARE;
        STATS_T0(ctx);
        const uint64_t t_meas = measure ? stats_now_ns() : 0;
        frame[0] = (unsigned char)ADAPT_ROUTE_UDS;
        memcpy(frame + 1, data, size);
        if (ctx->stats_on) {
            ctx->st.send_copy_ns += stats_now_ns() - t0_;
            ctx->st.send_copy_bytes += size + 1;
        }
        rc = uds_send(ctx->uds, ctx->cfg.peer_sock, frame, size + 1);
        if (t_meas) {
            /* measured only on success: failures are health, not cost */
            if (rc == 0) {
                const double us =
                    (double)(stats_now_ns() - t_meas) / 1000.0;
                adapt_learn_observe(&ctx->learn, &ctx->cmcfg,
                                    ADAPT_ROUTE_UDS, size, us);
                ctx->rt.ewma_latency_us = ctx->alpha * us +
                    (1.0 - ctx->alpha) * ctx->rt.ewma_latency_us;
            }
        }
        if (rc == -ECONNREFUSED) return rc;
        ctx->retry_pending = 0;
    }

    return rc;
}

int adapt_recv(adapt_ctx_t *ctx, void *buf, size_t max_size)
{
    if (!ctx || !buf || max_size == 0) return -EINVAL;
    if (ctx->role != ADAPT_ROLE_CONSUMER) return -EPERM;
    if (max_size < 2) return -EMSGSIZE;

    int tmo_in_msg = 0; /* poll timeouts waited for THIS message */

    /*
     * Multiplex both transports: messages may arrive on either path while
     * we wait (the sender's EWMA route can differ from our expectation),
     * so never block indefinitely on UDS without re-checking the ring.
     * poll() with a small timeout bounds the added latency; SHM polling
     * itself is a couple of atomic loads. Control frames (lazy SHM
     * negotiation) are serviced inline and never delivered to the caller.
     * tmp/cap are re-read every iteration: handle_control() may map the
     * ring and grow the scratch buffer while we are in this loop.
     */
    for (;;) {
        if (ctx->stats_on) ctx->st.recv_loop_iters++;
        unsigned char *tmp = ctx->rxbuf;
        const size_t cap = ctx->rxbuf_cap;
        /* Cheap non-blocking SHM poll first (absent until negotiated). */
        int n = -EAGAIN;
        if (ctx->ring) {
            n = shm_ring_pop(ctx->ring, tmp, cap);
        }
        if (n > 0) {
            if (max_size < (size_t)n - 1) return -EMSGSIZE;
            memcpy(buf, tmp + 1, (size_t)n - 1);
            if (ctx->stats_on) {
                ctx->st.recv_msgs++;
                if (tmo_in_msg > 0) ctx->st.rx_shm_later++;
                else ctx->st.rx_shm_first++;
            }
            return n - 1;
        }
        if (n != -EAGAIN) return n;

        /* Bounded UDS wait; on timeout, re-poll the ring. */
        uint64_t tp = ctx->stats_on ? stats_now_ns() : 0;
        n = uds_recv_timeout(ctx->uds, tmp, cap, ADAPT_UDS_POLL_MS);
        if (ctx->stats_on) {
            ctx->st.poll_calls++;
            ctx->st.poll_wait_ns += stats_now_ns() - tp;
        }
        if (n > 1) {
            if (tmp[0] == ADAPT_WIRE_TAG_CTRL) {
                handle_control(ctx, tmp, n);
                tmo_in_msg = 0;
                continue;
            }
            if (max_size < (size_t)n - 1) return -EMSGSIZE;
            memcpy(buf, tmp + 1, (size_t)n - 1);
            if (ctx->stats_on) {
                ctx->st.recv_msgs++;
                if (tmo_in_msg > 0) ctx->st.rx_uds_later++;
                else ctx->st.rx_uds_prompt++;
            }
            return n - 1;
        }
        if (n <= 0 && n != -EAGAIN) return n;
        if (ctx->stats_on) {
            ctx->st.poll_timeouts++;
            tmo_in_msg++;
        }
    }
}

void adapt_get_stats(adapt_ctx_t *ctx, adapt_stats_t *out)
{
    if (out) *out = ctx ? ctx->st : (adapt_stats_t){0};
}

adapt_route_t adapt_last_route(const adapt_ctx_t *ctx)
{
    return ctx ? ctx->last_route : ADAPT_ROUTE_NONE;
}

double adapt_ewma(const adapt_ctx_t *ctx)
{
    return ctx ? ctx->ewma_size : -1.0;
}

size_t adapt_shm_used_bytes(const adapt_ctx_t *ctx)
{
    return ctx ? shm_ring_used_bytes(ctx->ring) : 0;
}

size_t adapt_shm_capacity(const adapt_ctx_t *ctx)
{
    if (!ctx || !ctx->ring) return 0;
    return shm_ring_used_bytes(ctx->ring) + shm_ring_free_bytes(ctx->ring);
}

adapt_policy_mode_t adapt_policy(const adapt_ctx_t *ctx)
{
    return ctx ? ctx->policy : ADAPT_POLICY_DEFAULT;
}

double adapt_crossover(const adapt_ctx_t *ctx)
{
    return ctx ? ctx->learn.crossover_b : -1.0;
}

adapt_health_state_t adapt_shm_health(const adapt_ctx_t *ctx)
{
    return ctx ? ctx->health.state : ADAPT_HEALTH_UNAVAILABLE;
}

void adapt_shutdown(adapt_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->declog.enabled && ctx->declog_path)
        adapt_declog_dump(&ctx->declog, ctx->declog_path);
    if (ctx->ring)
        shm_ring_close(ctx->ring, ctx->role == ADAPT_ROLE_PRODUCER);
    uds_close(ctx->uds);
    free(ctx->rxbuf);
    free(ctx);
}

const char *adapt_route_name(adapt_route_t r)
{
    switch (r) {
    case ADAPT_ROUTE_SHM: return "SHM";
    case ADAPT_ROUTE_UDS: return "UDS";
    default:              return "NONE";
    }
}

const char *adapt_strerror(int rc)
{
    switch (rc) {
    case 0:        return "ok";
    case -EAGAIN:  return "resource temporarily unavailable";
    case -EPERM:   return "wrong role for this operation";
    case -EMSGSIZE:return "message exceeds transport limit";
    default:       return strerror(-rc);
    }
}
