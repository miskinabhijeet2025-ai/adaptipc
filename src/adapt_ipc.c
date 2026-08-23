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

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    shm_ring_t     *ring;
    uds_endpoint_t *uds;
    unsigned char  *rxbuf; /* framed-record scratch, sized to shm capacity */
    int             stats_on;
    adapt_stats_t   st;
    /* retry-resume state: classification of the in-flight message */
    int             retry_pending;
    size_t          pending_size;
    adapt_route_t   pending_route;
    double          alpha;   /* EWMA smoothing factor; ADAPTIPC_ALPHA env
                                 overrides ADAPT_EWMA_ALPHA default */
};

static inline double ewma_update(double ewma, double alpha, size_t current_size)
{
    /* ewma_size = (alpha * current) + ((1 - alpha) * ewma) */
    return alpha * (double)current_size + (1.0 - alpha) * ewma;
}

static adapt_route_t classify(double ewma, adapt_route_t last)
{
    if (ewma >= ADAPT_TAU_HIGH) return ADAPT_ROUTE_SHM;
    if (ewma <= ADAPT_TAU_LOW)  return ADAPT_ROUTE_UDS;
    /* Hysteresis band: stay on the last route; default to UDS. */
    return last == ADAPT_ROUTE_SHM ? ADAPT_ROUTE_SHM : ADAPT_ROUTE_UDS;
}

int adapt_init(adapt_role_t role, const adapt_config_t *cfg, adapt_ctx_t **out)
{
    if (!cfg || !cfg->local_sock || !cfg->peer_sock || !out) return -EINVAL;

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
    ctx->stats_on = getenv("ADAPTIPC_STATS") != NULL;
    {
        const char *as = getenv("ADAPTIPC_ALPHA");
        double a = as ? atof(as) : (double)0.0;
        ctx->alpha = (a > 0.0 && a <= 1.0) ? a : (double)0.2;
    }

    shm_ring_role_t rrole = (role == ADAPT_ROLE_PRODUCER)
                                ? SHM_RING_ROLE_PRODUCER
                                : SHM_RING_ROLE_CONSUMER;
    /* Both sides pass create=1 for idempotent setup; shm_open(O_CREAT) is
     * harmless for the second arriver and ftruncate to same size is a no-op.
     * Only the header init races are guarded via the `initialized` flag with
     * release/acquire semantics inside shm_ring_create(). */
    int rc = shm_ring_create(ctx->cfg.shm_name, ctx->cfg.shm_capacity, 1,
                             rrole, &ctx->ring);
    if (rc != 0) goto fail;

    rc = uds_open(cfg->local_sock, &ctx->uds);
    if (rc != 0) goto fail;

    ctx->rxbuf = malloc(ctx->cfg.shm_capacity);
    if (!ctx->rxbuf) { rc = -ENOMEM; goto fail; }

    *out = ctx;
    return 0;

fail:
    if (ctx->ring) shm_ring_close(ctx->ring, role == ADAPT_ROLE_PRODUCER);
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
    adapt_route_t route;
    if (ctx->retry_pending && ctx->pending_size == size &&
        ctx->pending_route != ADAPT_ROUTE_NONE) {
        route = ctx->pending_route;
    } else {
        STATS_T0(ctx);
        ctx->ewma_size = ewma_update(ctx->ewma_size, ctx->alpha, size);
        route = classify(ctx->ewma_size, ctx->last_route);
        STATS_ACC(router_ns);

        /* Remember the CLASSIFIED route (not the per-message override). */
        ctx->last_route = classify(ctx->ewma_size, ctx->last_route);
        ctx->retry_pending = 0;
        ctx->pending_route = ADAPT_ROUTE_NONE;
    }
    if (ctx->stats_on) ctx->st.sends++;

    /* Payload-size guard rails: if the classified route cannot physically
     * carry this message, escalate/fall back to the one that can.
     * This is a per-message transport override ONLY -- it must not move
     * the sticky EWMA state (last_route), otherwise deadband oscillation
     * would masquerade as adaptation. */
    if (route == ADAPT_ROUTE_UDS && size + 1 > UDS_MAX_DGRAM) {
        route = ADAPT_ROUTE_SHM;
        if (ctx->stats_on) ctx->st.send_escalations++;
    }
    if (route == ADAPT_ROUTE_SHM && size + 1 > ctx->cfg.shm_capacity)
        return -EMSGSIZE;
    ctx->pending_size = size;
    ctx->pending_route = route;

    /* Frame: tag byte + payload. */
    unsigned char frame[UDS_MAX_DGRAM];
    int rc;

    if (route == ADAPT_ROUTE_SHM) {
        if (ctx->stats_on) ctx->st.sends_shm++;
        /* Zero-copy framing: assemble [tag][payload] directly in ring
         * slots via scatter push -- no staging malloc, no concat copy,
         * and -EAGAIN retries cost nothing. */
        STATS_T0(ctx);
        const unsigned char tag = (unsigned char)ADAPT_ROUTE_SHM;
        shm_segment_t segs[2] = {
            { .base = &tag,  .len = 1 },
            { .base = data,  .len = size },
        };
        rc = shm_ring_push_scatter(ctx->ring, segs, 2);
        if (ctx->stats_on) {
            ctx->st.send_copy_ns += stats_now_ns() - t0_;
            ctx->st.send_copy_bytes += size + 1;
        }
        if (rc == -EAGAIN) {
            ctx->retry_pending = 1; /* resume with stored route on retry */
            return rc;
        }
        ctx->retry_pending = 0;
    } else {
        if (ctx->stats_on) ctx->st.sends_uds++;
        STATS_T0(ctx);
        frame[0] = (unsigned char)ADAPT_ROUTE_UDS;
        memcpy(frame + 1, data, size);
        if (ctx->stats_on) {
            ctx->st.send_copy_ns += stats_now_ns() - t0_;
            ctx->st.send_copy_bytes += size + 1;
        }
        rc = uds_send(ctx->uds, ctx->cfg.peer_sock, frame, size + 1);
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

    unsigned char *tmp = ctx->rxbuf;
    const size_t cap = ctx->cfg.shm_capacity;
    int tmo_in_msg = 0; /* poll timeouts waited for THIS message */

    /*
     * Multiplex both transports: messages may arrive on either path while
     * we wait (the sender's EWMA route can differ from our expectation),
     * so never block indefinitely on UDS without re-checking the ring.
     * poll() with a small timeout bounds the added latency; SHM polling
     * itself is a couple of atomic loads.
     */
    for (;;) {
        if (ctx->stats_on) ctx->st.recv_loop_iters++;
        /* Cheap non-blocking SHM poll first. */
        int n = shm_ring_pop(ctx->ring, tmp, cap);
        if (n > 0) {
            if (max_size < (size_t)n - 1) return -EMSGSIZE;
            memcpy(buf, tmp + 1, (size_t)n - 1);
            if (ctx->stats_on) {
                ctx->st.recv_msgs++;
                /* tmo_in_msg > 0 => ring had data while we were polling */
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

void adapt_shutdown(adapt_ctx_t *ctx)
{
    if (!ctx) return;
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
