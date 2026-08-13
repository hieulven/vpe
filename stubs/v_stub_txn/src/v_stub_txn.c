#include "v_port_txn.h"
#include "v_port_pfcp.h"
#include "v_common.h"
#include "v_log.h"

#include <rte_mempool.h>
#include <rte_errno.h>
#include <string.h>

#define V_STUB_TXN_CAP 8192
#define V_STUB_TXN_HARD_TIMEOUT_MS 2000

static struct rte_mempool *g_pool;
static struct v_txn *g_slots[V_STUB_TXN_CAP];
static int g_fired[V_STUB_TXN_CAP];
static uint64_t g_now_ms;

static v_txn_timeout_cb_t g_timeout_cb;
static void *g_timeout_arg;

int v_port_txn_init(void)
{
    if (g_pool) {
        V_LOG(WARNING, "PFCP", "v_port_txn_init called twice");
        return RET_CODE_ERR;
    }
    g_pool = rte_mempool_create("v_txn_pool", V_STUB_TXN_CAP, sizeof(struct v_txn),
                                 0, 0, NULL, NULL, NULL, NULL, SOCKET_ID_ANY, 0);
    if (!g_pool) {
        V_LOG(ERR, "PFCP", "txn pool create failed: %s", rte_strerror(rte_errno));
        return RET_CODE_ERR;
    }
    memset(g_slots, 0, sizeof(g_slots));
    memset(g_fired, 0, sizeof(g_fired));
    g_now_ms = 0;
    return RET_CODE_OK;
}

void v_port_txn_fini(void)
{
    if (g_pool) {
        rte_mempool_free(g_pool);
        g_pool = NULL;
    }
    memset(g_slots, 0, sizeof(g_slots));
}

struct v_txn *v_port_txn_create(void)
{
    int slot = -1;
    for (int i = 0; i < V_STUB_TXN_CAP; i++) {
        if (!g_slots[i]) { slot = i; break; }
    }
    if (slot < 0) {
        V_LOG(WARNING, "PFCP", "txn table full (cap=%d)", V_STUB_TXN_CAP);
        return NULL;
    }

    struct v_txn *t;
    if (rte_mempool_get(g_pool, (void **)&t) != 0) {
        V_LOG(WARNING, "PFCP", "txn pool exhausted");
        return NULL;
    }
    memset(t, 0, sizeof(*t));
    t->deadline_ms = g_now_ms + V_STUB_TXN_HARD_TIMEOUT_MS;

    g_slots[slot] = t;
    g_fired[slot] = 0;
    return t;
}

static int slot_of(struct v_txn *t)
{
    for (int i = 0; i < V_STUB_TXN_CAP; i++)
        if (g_slots[i] == t)
            return i;
    return -1;
}

void v_port_txn_destroy(struct v_txn *t)
{
    if (!t)
        return;
    int slot = slot_of(t);
    if (slot < 0) {
        V_LOG(CRIT, "PFCP", "double-free or foreign v_txn %p", (void *)t);
        return;
    }
    g_slots[slot] = NULL;
    g_fired[slot] = 0;
    memset(t, 0, sizeof(*t));
    rte_mempool_put(g_pool, t);
}

struct v_txn *v_port_txn_find_by_seq(uint64_t smf_fseid, uint32_t seq)
{
    for (int i = 0; i < V_STUB_TXN_CAP; i++) {
        struct v_txn *t = g_slots[i];
        if (!t || !t->req)
            continue;
        if (v_port_pfcp_smf_fseid(t->req) == smf_fseid && v_port_pfcp_seq(t->req) == seq)
            return t;
    }
    return NULL;
}

struct v_txn *v_port_txn_find_by_seid(uint64_t seid)
{
    for (int i = 0; i < V_STUB_TXN_CAP; i++) {
        struct v_txn *t = g_slots[i];
        if (t && t->seid == seid)
            return t;
    }
    return NULL;
}

void v_port_txn_set_state(struct v_txn *t, v_txn_state_t st)
{
    t->state = st;
}

int v_port_txn_on_timeout(v_txn_timeout_cb_t cb, void *arg)
{
    g_timeout_cb = cb;
    g_timeout_arg = arg;
    return RET_CODE_OK;
}

void v_port_txn_test_advance_ms(uint64_t ms)
{
    g_now_ms += ms;
    for (int i = 0; i < V_STUB_TXN_CAP; i++) {
        struct v_txn *t = g_slots[i];
        if (!t || g_fired[i])
            continue;
        if (t->deadline_ms <= g_now_ms) {
            g_fired[i] = 1;
            if (g_timeout_cb)
                g_timeout_cb(t, g_timeout_arg);
        }
    }
}

uint64_t v_port_txn_test_hard_timeout_ms(void)
{
    return V_STUB_TXN_HARD_TIMEOUT_MS;
}
