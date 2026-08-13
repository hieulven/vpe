#include "v_dispatch.h"
#include "v_port_pfcp.h"
#include "v_common.h"
#include "v_log.h"

#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_errno.h>

#include <string.h>

#define V_DISPATCH_POOL_CAP (V_DISPATCH_RING_SIZE * 2)

static struct rte_ring *g_rx_ring;
static struct rte_ring *g_tx_ring;
static struct rte_mempool *g_msg_pool;

int v_dispatch_init(void)
{
    g_msg_pool = rte_mempool_create("v_dispatch_msg_pool", V_DISPATCH_POOL_CAP,
                                     sizeof(v_dispatch_msg_t), 0, 0,
                                     NULL, NULL, NULL, NULL, SOCKET_ID_ANY, 0);
    if (!g_msg_pool) {
        V_LOG(ERR, "MEM", "dispatch msg pool create failed: %s", rte_strerror(rte_errno));
        return RET_CODE_ERR;
    }

    /* flags=0: MP enqueue / MC dequeue — I/O core is the sole rx
     * producer but any of the 4 workers may dequeue (see v_dispatch.h
     * design note); workers are the tx producers, I/O core the sole tx
     * consumer. */
    g_rx_ring = rte_ring_create("v_dispatch_rx", V_DISPATCH_RING_SIZE, SOCKET_ID_ANY, 0);
    g_tx_ring = rte_ring_create("v_dispatch_tx", V_DISPATCH_RING_SIZE, SOCKET_ID_ANY, 0);
    if (!g_rx_ring || !g_tx_ring) {
        V_LOG(ERR, "MEM", "dispatch ring create failed: %s", rte_strerror(rte_errno));
        return RET_CODE_ERR;
    }

    return RET_CODE_OK;
}

void v_dispatch_fini(void)
{
    if (g_rx_ring) { rte_ring_free(g_rx_ring); g_rx_ring = NULL; }
    if (g_tx_ring) { rte_ring_free(g_tx_ring); g_tx_ring = NULL; }
    if (g_msg_pool) { rte_mempool_free(g_msg_pool); g_msg_pool = NULL; }
}

void v_dispatch_rx(const uint8_t *buf, size_t len, const struct sockaddr *peer, void *arg)
{
    (void)arg;

    if (len > V_DISPATCH_MSG_MAX_BYTES) {
        V_LOG(WARNING, "PFCP", "dispatch rx: datagram too large (%zu > %d), dropped",
              len, V_DISPATCH_MSG_MAX_BYTES);
        return;
    }

    v_dispatch_msg_t *m;
    if (rte_mempool_get(g_msg_pool, (void **)&m) != 0) {
        V_LOG(WARNING, "PFCP", "dispatch rx: msg pool exhausted, dropped");
        return;
    }

    memcpy(m->buf, buf, len);
    m->len = len;
    if (peer)
        memcpy(&m->peer, peer, sizeof(m->peer));
    else
        memset(&m->peer, 0, sizeof(m->peer));

    if (rte_ring_enqueue(g_rx_ring, m) != 0) {
        V_LOG(WARNING, "PFCP", "dispatch rx: rx_ring full, dropped");
        rte_mempool_put(g_msg_pool, m);
    }
}

unsigned v_dispatch_worker_poll(unsigned worker_id, unsigned max,
                                 v_dispatch_handler_t handler, void *arg)
{
    (void)worker_id;
    unsigned n = 0;
    void *obj;

    while (n < max && rte_ring_dequeue(g_rx_ring, &obj) == 0) {
        v_dispatch_msg_t *m = (v_dispatch_msg_t *)obj;
        handler(m, arg);
        rte_mempool_put(g_msg_pool, m);
        n++;
    }
    return n;
}

int v_dispatch_tx_enqueue(const uint8_t *buf, size_t len, const struct sockaddr *peer)
{
    if (len > V_DISPATCH_MSG_MAX_BYTES) {
        V_LOG(ERR, "PFCP", "dispatch tx: message too large (%zu > %d)", len, V_DISPATCH_MSG_MAX_BYTES);
        return RET_CODE_ERR;
    }

    v_dispatch_msg_t *m;
    if (rte_mempool_get(g_msg_pool, (void **)&m) != 0) {
        V_LOG(WARNING, "PFCP", "dispatch tx: msg pool exhausted");
        return RET_CODE_ERR;
    }

    memcpy(m->buf, buf, len);
    m->len = len;
    if (peer)
        memcpy(&m->peer, peer, sizeof(m->peer));
    else
        memset(&m->peer, 0, sizeof(m->peer));

    if (rte_ring_enqueue(g_tx_ring, m) != 0) {
        V_LOG(WARNING, "PFCP", "dispatch tx: tx_ring full, dropped");
        rte_mempool_put(g_msg_pool, m);
        return RET_CODE_ERR;
    }
    return RET_CODE_OK;
}

unsigned v_dispatch_io_drain_tx(unsigned max)
{
    unsigned n = 0;
    void *obj;

    while (n < max && rte_ring_dequeue(g_tx_ring, &obj) == 0) {
        v_dispatch_msg_t *m = (v_dispatch_msg_t *)obj;
        if (v_port_pfcp_io_send(&m->peer, m->buf, m->len) != RET_CODE_OK)
            V_LOG(ERR, "PFCP", "dispatch io drain: send failed");
        rte_mempool_put(g_msg_pool, m);
        n++;
    }
    return n;
}
