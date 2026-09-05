/*
 * test_policy_modes.c -- end-to-end policy behavior through adapt_ipc.
 *
 *  1. Adversarial oscillation: payloads alternating either side of the
 *     EWMA threshold. Verifies the stability ladder: SIZE_ONLY switches
 *     on every message, SIZE_HYSTERESIS dampens, FULL_ADAPTIVE is at
 *     least as stable as SIZE_HYSTERESIS while all messages deliver.
 *  2. Degradation: a stalled consumer drives SHM health to DEGRADED/
 *     BLOCKED and the queue-aware policies escape to UDS (for sizes
 *     that fit), then recover when the consumer catches up.
 *  3. Decision log: ADAPTIPC_DECISION_LOG produces a parseable CSV.
 */
#define _POSIX_C_SOURCE 200809L

#include "adapt_ipc.h"
#include "cost_model.h"

#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static adapt_ctx_t *g_prod, *g_cons;

static void init_pair_nb(adapt_policy_mode_t policy, const char *tag,
                         int nonblocking);

static void init_pair(adapt_policy_mode_t policy, const char *tag)
{
    init_pair_nb(policy, tag, 0);
}

static void init_pair_nb(adapt_policy_mode_t policy, const char *tag,
                         int nonblocking)
{
    /* Policy tests use eager SHM: they are sequential (send then recv
     * in one thread), so the asynchronous lazy handshake cannot
     * complete mid-test (it is covered by test_lazy_negotiation). */
    setenv("ADAPTIPC_EAGER_SHM", "1", 1);
    /* static: adapt_ctx keeps the pointers; pairs are used strictly
     * sequentially (shutdown_pair before the next init_pair). */
    static char pp[64], cp[64];
    snprintf(pp, sizeof(pp), "/tmp/adaptipc_pm_p_%s_%ld", tag,
             (long)getpid());
    snprintf(cp, sizeof(cp), "/tmp/adaptipc_pm_c_%s_%ld", tag,
             (long)getpid());
    adapt_config_t pcfg = {
        .local_sock = pp, .peer_sock = cp, .shm_capacity = 65536,
        .policy = policy, .nonblocking_send = nonblocking,
    };
    adapt_config_t ccfg = {
        .local_sock = cp, .peer_sock = pp, .shm_capacity = 65536,
    };
    int rc = adapt_init(ADAPT_ROLE_PRODUCER, &pcfg, &g_prod);
    if (rc != 0)
        fprintf(stderr, "producer init rc=%d (%s)\n", rc,
                adapt_strerror(rc));
    assert(rc == 0);
    rc = adapt_init(ADAPT_ROLE_CONSUMER, &ccfg, &g_cons);
    assert(rc == 0);
}

static void shutdown_pair(void)
{
    adapt_shutdown(g_prod);
    adapt_shutdown(g_cons);
}

/* Deliver one message and return the route used (from the receiver's
 * demultiplexing perspective, the sender's last_route is the truth). */
static void deliver(size_t size)
{
    static unsigned char buf[70000];
    memset(buf, 0x5a, size > 70000 ? 70000 : size);
    int rc;
    do { rc = adapt_send(g_prod, buf, size); } while (rc == -EAGAIN);
    if (getenv("PM_DEBUG"))
        fprintf(stderr, "send %zu -> %s (ewma=%.0f) cons_ring=%zu\n",
                size, adapt_route_name(adapt_last_route(g_prod)),
                adapt_ewma(g_prod), adapt_shm_used_bytes(g_cons));
    if (rc != 0)
        fprintf(stderr, "deliver(%zu) send rc=%d (%s)\n", size, rc,
                adapt_strerror(rc));
    assert(rc == 0);
    int rn = adapt_recv(g_cons, buf, sizeof(buf));
    if (rn != (int)size)
        fprintf(stderr, "deliver(%zu) recv rc=%d (%s)\n", size, rn,
                adapt_strerror(rn));
    assert(rn == (int)size);
}

static unsigned long count_switches(adapt_policy_mode_t policy,
                                    const size_t *sizes, unsigned n,
                                    const char *tag)
{
    init_pair(policy, tag);
    /* warm the EWMA to the middle of the deadband */
    for (int i = 0; i < 32; i++) deliver(2048);
    adapt_stats_t st;
    adapt_get_stats(g_prod, &st);
    const uint64_t before = st.route_switches;
    for (unsigned i = 0; i < n; i++) deliver(sizes[i]);
    adapt_get_stats(g_prod, &st);
    const unsigned long switches =
        (unsigned long)(st.route_switches - before);
    shutdown_pair();
    return switches;
}

static void test_adversarial_oscillation(void)
{
    /* Payloads alternate just across TAU_HIGH's EWMA pull: 1024 and
     * 16384 alternate; the EWMA sweeps the deadband repeatedly. */
    enum { N = 64 };
    size_t sizes[N];
    for (int i = 0; i < N; i++) sizes[i] = (i % 2) ? 16384 : 1024;

    fprintf(stderr, "[stage size_only]\n");
    const unsigned long sw_only = count_switches(
        ADAPT_POLICY_SIZE_ONLY, sizes, N, "so");
    fprintf(stderr, "[stage size_only done: %lu]\n", sw_only);
    fprintf(stderr, "[stage hysteresis]\n");
    const unsigned long sw_hyst = count_switches(
        ADAPT_POLICY_SIZE_HYSTERESIS, sizes, N, "sh");
    fprintf(stderr, "[stage hysteresis done: %lu]\n", sw_hyst);
    const unsigned long sw_full = count_switches(
        ADAPT_POLICY_FULL_ADAPTIVE, sizes, N, "fa");

    printf("  adversarial switches over %d msgs: size_only=%lu "
           "hysteresis=%lu full=%lu\n", N, sw_only, sw_hyst, sw_full);
    assert(sw_only > sw_hyst);  /* deadband must dampen oscillation */
    assert(sw_full <= sw_hyst); /* cost model never less stable     */
}

static void test_degradation_and_recovery(void)
{
    /* Nonblocking sends: the backlog phase fills the ring with the
     * consumer deliberately stopped, which must not park the producer
     * forever (blocking sends would legitimately wait for drain). */
    init_pair_nb(ADAPT_POLICY_FULL_ADAPTIVE, "deg", 1);
    /* Warm up: bulk traffic establishes the SHM route and mapping. */
    for (int i = 0; i < 32; i++) deliver(8192);
    assert(adapt_last_route(g_prod) == ADAPT_ROUTE_SHM);
    assert(adapt_shm_health(g_prod) != ADAPT_HEALTH_UNAVAILABLE);

    /* Backlog: fill the ring far beyond HW with the consumer stopped.
     * Continued send attempts at high occupancy (each rejected with
     * -EAGAIN) still feed the health monitor: after the debounce
     * streak the SHM transport must leave HEALTHY. */
    static unsigned char big[8192];
    memset(big, 0x33, sizeof(big));
    int pushed = 0;
    for (int i = 0; i < 64; i++) {
        if (adapt_send(g_prod, big, 8192) == 0) pushed++;
    }
    assert(pushed > 0);
    assert(adapt_shm_used_bytes(g_prod) > 0);
    for (int i = 0; i < 20; i++)
        (void)adapt_send(g_prod, big, 8192); /* rejected, but sampled */
    assert(adapt_shm_health(g_prod) == ADAPT_HEALTH_DEGRADED ||
           adapt_shm_health(g_prod) == ADAPT_HEALTH_BLOCKED);

    /* A small message now: the queue-aware cost must prefer UDS (the
     * ring is backlogged, health is degraded). The send is rejected
     * with -EAGAIN (ring full) -- the decision, not delivery, is what
     * we verify here. */
    static unsigned char small[1024];
    memset(small, 0x44, sizeof(small));
    (void)adapt_send(g_prod, small, sizeof(small));
    assert(adapt_last_route(g_prod) == ADAPT_ROUTE_UDS);

    /* Drain everything (adapt_recv blocks until a message arrives, so
     * drain the exact count: the 7 ring records plus the one small
     * message that escaped to UDS). */
    static unsigned char buf[8192];
    int drained = 0;
    while (drained < pushed + 1) {
        int n = adapt_recv(g_cons, buf, sizeof(buf));
        assert(n > 0);
        drained++;
    }
    assert(drained == pushed + 1); /* zero loss */

    for (int i = 0; i < 40; i++) {
        int rc = adapt_send(g_prod, small, sizeof(small));
        if (rc == 0) {
            int n = adapt_recv(g_cons, buf, sizeof(buf));
            assert(n == (int)sizeof(small));
        }
    }
    assert(adapt_shm_health(g_prod) == ADAPT_HEALTH_HEALTHY);

    adapt_stats_t st;
    adapt_get_stats(g_prod, &st);
    printf("  degradation: UDS escape under backlog, %d msgs drained "
           "with zero loss, health=%s after %llu transitions\n",
           drained, adapt_health_name(adapt_shm_health(g_prod)),
           (unsigned long long)st.health_transitions);
    assert(st.health_transitions >= 2); /* degrade + recover */
    shutdown_pair();
}

static void test_decision_log_csv(void)
{
    char path[128];
    snprintf(path, sizeof(path), "/tmp/adaptipc_declog_%ld.csv",
             (long)getpid());
    setenv("ADAPTIPC_DECISION_LOG", path, 1);
    init_pair(ADAPT_POLICY_FULL_ADAPTIVE, "log");
    for (int i = 0; i < 40; i++) deliver((i % 2) ? 8192 : 512);
    adapt_shutdown(g_prod);   /* dump happens here */
    adapt_shutdown(g_cons);
    unsetenv("ADAPTIPC_DECISION_LOG");

    FILE *f = fopen(path, "r");
    assert(f);
    char line[512];
    assert(fgets(line, sizeof(line), f)); /* header */
    assert(strstr(line, "score_uds_us") && strstr(line, "reason"));
    unsigned rows = 0;
    while (fgets(line, sizeof(line), f)) {
        if (getenv("PM_DEBUG")) fprintf(stderr, "row: %s", line);
        assert(strstr(line, "COST_") || strstr(line, "SIZE_") ||
               strstr(line, "STICKY_") || strstr(line, "QUEUE_") ||
               strstr(line, "COLD_") || strstr(line, "SETUP_"));
        rows++;
    }
    fclose(f);
    assert(rows >= 40);
    unlink(path);
    printf("  decision log: %u rows with scores + reasons\n", rows);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("ADAPTIPC_STATS", "1", 1);
    printf("policy mode integration tests:\n");
    test_adversarial_oscillation();
    test_degradation_and_recovery();
    test_decision_log_csv();
    printf("test_policy_modes: PASS\n");
    return 0;
}
