#ifndef UDS_FALLBACK_H
#define UDS_FALLBACK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Thin AF_UNIX + SOCK_DGRAM wrapper used as the small-payload fallback
 * transport of AdaptIPC. Each endpoint binds its own datagram socket and
 * sends to the peer's address.
 */

#define UDS_MAX_DGRAM 4096u   /* conservative SOCK_DGRAM payload cap */

typedef struct uds_endpoint uds_endpoint_t;

/*
 * Create and bind a datagram endpoint.
 *
 * local_path : filesystem path for this endpoint's socket, e.g.
 *              "/tmp/adaptipc_a.sock". The file is unlinked first if present.
 * Returns 0 on success, negative errno otherwise.
 */
int uds_open(const char *local_path, uds_endpoint_t **out);

/* Send one datagram to the peer. Blocking send with retry on EINTR.
 * Returns 0 on success, negative errno otherwise. */
int uds_send(uds_endpoint_t *ep, const char *peer_path,
             const void *data, size_t size);

/* Receive one datagram. Blocks until a message arrives (retry on EINTR).
 * Returns received byte count (>0) on success, negative errno otherwise. */
int uds_recv(uds_endpoint_t *ep, void *buf, size_t max_size);

/* Receive one datagram with a bounded wait. Returns byte count (>0),
 * -EAGAIN on timeout, negative errno otherwise. */
int uds_recv_timeout(uds_endpoint_t *ep, void *buf, size_t max_size,
                     int timeout_ms);

/* Close socket and unlink local_path. Safe to call on partially-open ep. */
void uds_close(uds_endpoint_t *ep);

const char *uds_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif /* UDS_FALLBACK_H */
