/*
 * uds_fallback.c -- AF_UNIX SOCK_DGRAM transport for small control payloads.
 */
#define _POSIX_C_SOURCE 200809L

#include "uds_fallback.h"

#include <errno.h>
#include <poll.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct uds_endpoint {
    int  fd;
    char local_path[108]; /* sockaddr_un sun_path limit */
};

int uds_open(const char *local_path, uds_endpoint_t **out)
{
    if (!local_path || !out) return -EINVAL;
    if (strlen(local_path) >= 108) /* sizeof(struct sockaddr_un.sun_path) */
        return -ENAMETOOLONG;

    uds_endpoint_t *ep = calloc(1, sizeof(*ep));
    if (!ep) return -ENOMEM;

    ep->fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (ep->fd < 0) { int e = errno; free(ep); return -e; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, local_path, sizeof(addr.sun_path) - 1);

    /* Stale socket files would break bind(). */
    unlink(local_path);

    if (bind(ep->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        int e = errno;
        close(ep->fd);
        free(ep);
        return -e;
    }

    /* Platform defaults for AF_UNIX buffers are small (macOS ~2 KB), which
     * both throttles throughput and makes >=4 KB datagrams unreliable.
     * Explicitly size send/receive buffers for bulk transfer. */
    int bufsz = 262144; /* 256 KiB */
    setsockopt(ep->fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
    setsockopt(ep->fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));

    snprintf(ep->local_path, sizeof(ep->local_path), "%s", local_path);
    *out = ep;
    return 0;
}

int uds_send(uds_endpoint_t *ep, const char *peer_path,
             const void *data, size_t size)
{
    if (!ep || !peer_path || !data || size == 0) return -EINVAL;
    if (size > UDS_MAX_DGRAM) return -EMSGSIZE;

    struct sockaddr_un peer;
    memset(&peer, 0, sizeof(peer));
    peer.sun_family = AF_UNIX;
    strncpy(peer.sun_path, peer_path, sizeof(peer.sun_path) - 1);

    ssize_t n;
    do {
        n = sendto(ep->fd, data, size, 0,
                   (struct sockaddr *)&peer, sizeof(peer));
        /* macOS returns ENOBUFS (instead of blocking) when the peer's
         * receive queue is full; occasionally ETIMEDOUT under sustained
         * load. Back off briefly and retry in both cases. */
        if (n < 0 && (errno == ENOBUFS || errno == ETIMEDOUT)) {
            struct timespec ts = { 0, 200000 }; /* 200 us */
            nanosleep(&ts, NULL);
        }
    } while (n < 0 &&
             (errno == EINTR || errno == ENOBUFS || errno == ETIMEDOUT));

    if (n < 0) return -errno;
    if ((size_t)n != size) return -EIO; /* datagrams are atomic; partial impossible */
    return 0;
}

int uds_recv(uds_endpoint_t *ep, void *buf, size_t max_size)
{
    if (!ep || !buf || max_size == 0) return -EINVAL;

    ssize_t n;
    do {
        n = recvfrom(ep->fd, buf, max_size, 0, NULL, NULL);
    } while (n < 0 && errno == EINTR);

    if (n < 0) return -errno;
    if (n == 0) return -ECONNRESET;
    return (int)n;
}

int uds_recv_timeout(uds_endpoint_t *ep, void *buf, size_t max_size,
                     int timeout_ms)
{
    if (!ep || !buf || max_size == 0) return -EINVAL;

    if (timeout_ms > 0) {
        struct pollfd pfd = { .fd = ep->fd, .events = POLLIN, .revents = 0 };
        int pr;
        do { pr = poll(&pfd, 1, timeout_ms); } while (pr < 0 && errno == EINTR);
        if (pr < 0) return -errno;
        if (pr == 0) return -EAGAIN; /* timed out; caller may poll other src */
    }

    ssize_t n;
    do {
        n = recvfrom(ep->fd, buf, max_size, 0, NULL, NULL);
    } while (n < 0 && errno == EINTR);

    if (n < 0) return -errno;
    if (n == 0) return -ECONNRESET;
    return (int)n;
}

void uds_close(uds_endpoint_t *ep)
{
    if (!ep) return;
    if (ep->fd >= 0) close(ep->fd);
    if (ep->local_path[0]) unlink(ep->local_path);
    free(ep);
}

const char *uds_strerror(int rc)
{
    switch (rc) {
    case 0:         return "ok";
    case -EMSGSIZE: return "datagram exceeds UDS_MAX_DGRAM";
    case -ECONNREFUSED: return "peer socket not bound";
    default:        return strerror(-rc);
    }
}
