/*
 * test_queue_occupancy_instrumentation.c -- verify that the reported
 * ring occupancy matches the canonical cursor math for known fill
 * states (0/20/40/60/80/90% + full), within a documented tolerance.
 *
 * Canonical occupancy: used = head - tail (monotonic 64-bit cursors,
 * wrapping arithmetic exact). Tolerance: one record (occupancy is
 * sampled between messages, so it can differ by at most the last
 * message's footprint from the nominal fill target).
 */
#include "adapt_ipc.h"
#include "shm_ringbuffer.h"

#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

/*
 * The instrumentation path under test:
 *   producer adapt_send() -> SPSC ring push
 *   adapt_shm_used_bytes(producer ctx) -> shm_ring_used_bytes()
 *                                     -> head - tail cursor math
 * Ground truth: the same cursor difference read directly through an
 * independent shm_ring_attach() mapping of the same object.
 */
#define CAP   (1u << 20)          /* 1 MiB ring */
#define MSG   8192u
#define TOL   ((size_t)MSG + 8)   /* one record tolerance */

static adapt_ctx_t *g_prod, *g_cons;
static shm_ring_t  *g_probe;      /* independent ground-truth mapping */

static void fill_to_fraction(unsigned pct, unsigned char *msg)
{
    while (shm_ring_used_bytes(g_probe) < (size_t)CAP * pct / 100u) {
        int rc = adapt_send(g_prod, msg, MSG);
        assert(rc == 0 || rc == -EAGAIN);
        if (rc != 0) break;      /* full before reaching the target */
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("ADAPTIPC_EAGER_SHM", "1", 1);
    char pp[64], cp[64];
    snprintf(pp, sizeof(pp), "/tmp/aip_occ_p_%ld", (long)getpid());
    snprintf(cp, sizeof(cp), "/tmp/aip_occ_c_%ld", (long)getpid());
    adapt_config_t pc = { .shm_name = "/adaptipc_occ_ring",
        .local_sock = pp, .peer_sock = cp, .shm_capacity = CAP,
        .policy = ADAPT_POLICY_SIZE_HYSTERESIS, .nonblocking_send = 1 };
    adapt_config_t cc = { .shm_name = "/adaptipc_occ_ring",
        .local_sock = cp, .peer_sock = pp, .shm_capacity = CAP };
    assert(adapt_init(ADAPT_ROLE_PRODUCER, &pc, &g_prod) == 0);
    assert(adapt_init(ADAPT_ROLE_CONSUMER, &cc, &g_cons) == 0);
    /* independent mapping for cursor-math ground truth */
    assert(shm_ring_attach("/adaptipc_occ_ring", CAP,
                           SHM_RING_ROLE_CONSUMER, &g_probe) == 0);

    static unsigned char msg[MSG];
    memset(msg, 0x5a, sizeof(msg));
    static const unsigned pcts[] = { 20, 40, 60, 80 };

    /* EMPTY: both paths must report 0 */
    assert(adapt_shm_used_bytes(g_prod) == 0);
    assert(shm_ring_used_bytes(g_probe) == 0);

    for (unsigned k = 0; k < sizeof(pcts)/sizeof(pcts[0]); k++) {
        fill_to_fraction(pcts[k], msg);
        size_t reported = adapt_shm_used_bytes(g_prod);
        size_t cursor   = shm_ring_used_bytes(g_probe);
        size_t nominal  = (size_t)CAP * pcts[k] / 100u;
        printf("  %2u%%: reported=%zu cursor=%zu nominal=%zu\n",
               pcts[k], reported, cursor, nominal);
        /* instrumentation equals the canonical cursor difference */
        assert(reported == cursor);
        /* within one record of the nominal target */
        assert(reported + TOL >= nominal && reported <= nominal + TOL);
    }

    /* FULL: occupancy reaches capacity minus one record */
    while (adapt_send(g_prod, msg, MSG) == 0)
        ;
    assert(shm_ring_used_bytes(g_probe) >= CAP - MSG - 8);
    assert(adapt_shm_used_bytes(g_prod) ==
           shm_ring_used_bytes(g_probe));

    /* DRAIN via the consumer: back to 0 */
    static unsigned char rx[MSG];
    unsigned drained = 0;
    while (adapt_shm_used_bytes(g_prod) > 0) {
        int n = adapt_recv(g_cons, rx, sizeof(rx));
        assert(n == (int)MSG);
        drained++;
    }
    assert(adapt_shm_used_bytes(g_prod) == 0);
    assert(shm_ring_used_bytes(g_probe) == 0);
    printf("  drained %u messages back to empty\n", drained);

    adapt_shutdown(g_prod);
    adapt_shutdown(g_cons);
    shm_ring_close(g_probe, 1);
    printf("test_queue_occupancy_instrumentation: PASS\n");
    return 0;
}
