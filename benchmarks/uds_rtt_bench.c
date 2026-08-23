/*
 * uds_rtt_bench.c -- isolates per-message UDS transport cost from queueing.
 *
 * Measurements:
 *  1. self-loopback: sendto(self) immediately followed by recvfrom on the
 *     same datagram socket -- measures pure syscall + kernel queue
 *     entry/exit cost with zero process-wakeup component.
 *  2. ping-pong echo between two processes: full RTT including receiver
 *     wakeup/scheduling; RTT/2 approximates the per-message one-way cost
 *     attributed to the transport in eq. (3)'s socket term.
 * No backlog is possible: exactly one datagram is in flight at any time.
 *
 * Build: cc -O2 -std=c11 uds_rtt_bench.c -o uds_rtt_bench
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int main(void)
{
    const char *A = "/tmp/udsrtt_a.sock";
    const char *B = "/tmp/udsrtt_b.sock";
    unlink(A);
    unlink(B);

    /* --- 1. self-loopback: syscall + kernel queue cost ---------------- */
    int s = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un self_;
    memset(&self_, 0, sizeof self_);
    self_.sun_family = AF_UNIX;
    strcpy(self_.sun_path, A);
    if (bind(s, (struct sockaddr *)&self_, sizeof self_) != 0) {
        perror("bind"); return 1;
    }
    char buf[2048];
    memset(buf, 1, sizeof buf);
    enum { WARM = 2000, N = 20000 };
    uint64_t t[N];
    for (int i = 0; i < WARM; ++i) {
        sendto(s, buf, 512, 0, (struct sockaddr *)&self_, sizeof self_);
        recvfrom(s, buf, sizeof buf, 0, NULL, NULL);
    }
    for (int i = 0; i < N; ++i) {
        uint64_t t0 = now_ns();
        sendto(s, buf, 512, 0, (struct sockaddr *)&self_, sizeof self_);
        recvfrom(s, buf, sizeof buf, 0, NULL, NULL);
        t[i] = now_ns() - t0;
    }
    qsort(t, N, sizeof(uint64_t), cmp_u64);
    printf("self-loopback (syscall+queue, no wakeup): "
           "median=%llu ns  p10=%llu ns\n",
           (unsigned long long)t[N / 2], (unsigned long long)t[N / 10]);
    close(s);

    /* --- 2. two-process ping-pong: includes receiver wakeup ----------- */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
        perror("socketpair"); return 1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        char b[512];
        for (;;) {
            ssize_t n = recv(sv[1], b, sizeof b, 0);
            if (n <= 0) _exit(0);
            send(sv[1], b, (size_t)n, 0);
        }
    }
    /* warmup */
    for (int i = 0; i < WARM; ++i) {
        send(sv[0], buf, 512, 0);
        recv(sv[0], buf, sizeof buf, 0);
    }
    for (int i = 0; i < N; ++i) {
        uint64_t t0 = now_ns();
        send(sv[0], buf, 512, 0);
        recv(sv[0], buf, sizeof buf, 0);
        t[i] = now_ns() - t0;
    }
    qsort(t, N, sizeof(uint64_t), cmp_u64);
    printf("ping-pong (incl. receiver wakeup):      "
           "median RTT=%llu ns  one-way=%llu ns\n",
           (unsigned long long)t[N / 2],
           (unsigned long long)t[N / 2] / 2);
    kill(pid, 15);
    waitpid(pid, NULL, 0);
    close(sv[0]);
    close(sv[1]);
    unlink(A);
    unlink(B);
    return 0;
}
