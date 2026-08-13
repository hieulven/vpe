#ifndef VPE_V_DISPATCH_H
#define VPE_V_DISPATCH_H

#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>

/*
 * v_dispatch — rings between PFCP IO and the 4 workers, plus the
 * worker-side drain/poll helpers. plan.md §1, §8 S7.
 *
 * DESIGN NOTE (departure from the old per-worker-SEID-pinned model):
 * plan.md §2 Change 2 replaces session ownership with version CAS
 * specifically so that "two pods doing read-modify-write on one session"
 * — or here, two WORKERS in the same pod — no longer needs coordination
 * to be safe: a conflicting write is detected and retried, not
 * prevented by pinning. v_node_state (§6.7) reinforces this — node-level
 * state (Node ID, Recovery Time Stamp, association) lives in Redis too,
 * not in a particular worker's memory. So there is no correctness reason
 * left to route a message to a specific worker by SEID or to special-
 * case node-level messages onto one worker: any worker may process any
 * message. v_dispatch is therefore a single shared work-queue ring, not
 * 4 SEID-routed rings — simpler, and a direct consequence of §2, not an
 * oversight.
 *
 * Architecture rules preserved: the I/O core (v_dispatch_rx, called as
 * the v_pfcp_rx_cb_t) does a pure byte copy onto the ring — no PFCP
 * parsing. Workers never touch the network directly — outbound goes
 * through v_dispatch_tx_enqueue, and only the I/O core's
 * v_dispatch_io_drain_tx() calls v_port_pfcp_io_send().
 */

#define V_DISPATCH_NUM_WORKERS   4
#define V_DISPATCH_MSG_MAX_BYTES 2048
#define V_DISPATCH_RING_SIZE     4096 /* power of 2 */

typedef struct {
    uint8_t buf[V_DISPATCH_MSG_MAX_BYTES];
    size_t len;
    struct sockaddr peer;
} v_dispatch_msg_t;

int v_dispatch_init(void);
void v_dispatch_fini(void);

/* I/O-core side. Signature matches v_pfcp_rx_cb_t so it can be handed
 * directly to v_port_pfcp_io_init(). */
void v_dispatch_rx(const uint8_t *buf, size_t len, const struct sockaddr *peer, void *arg);

/* Worker side: drains up to max messages from the shared rx work
 * queue, invoking handler(msg, arg) for each, then releases the
 * message object. worker_id is accepted for logging/symmetry — with no
 * SEID affinity, any worker may call this and get any message. Returns
 * the number processed (never blocks; 0 means the queue was empty). */
typedef void (*v_dispatch_handler_t)(const v_dispatch_msg_t *msg, void *arg);
unsigned v_dispatch_worker_poll(unsigned worker_id, unsigned max,
                                 v_dispatch_handler_t handler, void *arg);

/* Worker side: queue an outbound datagram. The worker never calls
 * v_port_pfcp_io_send() itself. */
int v_dispatch_tx_enqueue(const uint8_t *buf, size_t len, const struct sockaddr *peer);

/* I/O-core side: drains up to max queued outbound datagrams and
 * actually sends them via v_port_pfcp_io_send(). Returns the number
 * sent. */
unsigned v_dispatch_io_drain_tx(unsigned max);

#endif /* VPE_V_DISPATCH_H */
