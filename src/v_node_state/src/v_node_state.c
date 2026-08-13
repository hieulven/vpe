#include "v_node_state.h"
#include "v_port_db.h"
#include "v_common.h"
#include "v_log.h"

#include <rte_mempool.h>
#include <rte_errno.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define V_NODE_STATE_PENDING_POOL_CAP 64

static char g_node_id_hex[V_NODE_ID_MAX_BYTES * 2 + 1];
static char g_recovery_key[V_NODE_ID_MAX_BYTES * 2 + 32];
static char g_assoc_key[V_NODE_ID_MAX_BYTES * 2 + 32];

static uint32_t g_recovery_ts;
static int g_recovery_ts_ready;

static struct rte_mempool *g_pool;

typedef enum { OP_RECOVERY, OP_SIMPLE, OP_ASSOC_QUERY } op_t;

/* One in-flight node_state call. */
struct pending {
    op_t op;
    void *arg;
    union {
        v_node_state_recovery_cb_t recovery_cb;
        v_node_state_cb_t simple_cb;
        v_node_state_assoc_cb_t assoc_cb;
    } u;
};

/* Lazily creates the pending-object pool on first use. */
static int ensure_pool(void)
{
    if (g_pool)
        return RET_CODE_OK;
    g_pool = rte_mempool_create("v_node_state_pending_pool", V_NODE_STATE_PENDING_POOL_CAP,
                                 sizeof(struct pending), 0, 0,
                                 NULL, NULL, NULL, NULL, SOCKET_ID_ANY, 0);
    if (!g_pool) {
        V_LOG(ERR, "DPDB", "node_state pending pool create failed: %s", rte_strerror(rte_errno));
        return RET_CODE_ERR;
    }
    return RET_CODE_OK;
}

/* Public: hex-encodes node_id into the module's Redis key namespace
 * and allocates the pending pool. Call once at startup. */
int v_node_state_init(const uint8_t *node_id, size_t node_id_len)
{
    if (node_id_len == 0 || node_id_len > V_NODE_ID_MAX_BYTES) {
        V_LOG(ERR, "PFCP", "v_node_state_init: bad node_id_len %zu", node_id_len);
        return RET_CODE_ERR;
    }
    if (ensure_pool() != RET_CODE_OK)
        return RET_CODE_ERR;

    for (size_t i = 0; i < node_id_len; i++)
        snprintf(&g_node_id_hex[i * 2], 3, "%02x", node_id[i]);
    g_node_id_hex[node_id_len * 2] = '\0';

    snprintf(g_recovery_key, sizeof(g_recovery_key), "upf:%s:recovery", g_node_id_hex);
    snprintf(g_assoc_key, sizeof(g_assoc_key), "upf:%s:assoc", g_node_id_hex);

    g_recovery_ts_ready = 0;
    return RET_CODE_OK;
}

/* Public: frees the pending pool and resets the cached recovery_ts
 * readiness flag. */
void v_node_state_fini(void)
{
    if (g_pool) {
        rte_mempool_free(g_pool);
        g_pool = NULL;
    }
    g_recovery_ts_ready = 0;
}

/* Grabs one pending-object slot, logging (not silently dropping) on
 * exhaustion. */
static struct pending *pending_get(void)
{
    struct pending *p;
    if (rte_mempool_get(g_pool, (void **)&p) != 0) {
        V_LOG(WARNING, "DPDB", "node_state pending pool exhausted");
        return NULL;
    }
    return p;
}

/* SET...NX GET reply handler: a STRING reply is the pre-existing value
 * (an earlier cold start won, that value is authoritative); NIL means
 * this call's SET actually landed (this boot won). Either way the
 * result is cached for v_node_state_recovery_ts(). */
static void recovery_reply_cb(int status, const v_db_reply_t *reply, void *arg)
{
    struct pending *p = (struct pending *)arg;
    v_node_state_recovery_cb_t cb = p->u.recovery_cb;
    void *user_arg = p->arg;
    rte_mempool_put(g_pool, p);

    if (status != RET_CODE_OK) {
        V_LOG(ERR, "DPDB", "node_state: recovery ts load failed");
        cb(RET_CODE_ERR, 0, user_arg);
        return;
    }

    if (reply && reply->type == V_DB_REPLY_STRING) {
        /* Someone else's cold start already set it — that value wins,
         * unconditionally, regardless of what we tried to set. */
        char buf[24];
        size_t n = reply->len < sizeof(buf) - 1 ? reply->len : sizeof(buf) - 1;
        memcpy(buf, reply->str, n);
        buf[n] = '\0';
        g_recovery_ts = (uint32_t)strtoul(buf, NULL, 10);
    } else {
        /* NIL: we won the race, our SET actually landed. */
        g_recovery_ts = (uint32_t)time(NULL);
    }
    g_recovery_ts_ready = 1;
    V_LOG(INFO, "PFCP", "node_state: recovery_ts=%u (node=%s)", g_recovery_ts, g_node_id_hex);
    cb(RET_CODE_OK, g_recovery_ts, user_arg);
}

/* Public: one round trip (SET...NX GET) to establish or read back the
 * node's Recovery Time Stamp. See recovery_reply_cb() for how the
 * result is decided. Call once at startup; result is cached. */
int v_node_state_load_recovery_ts(v_node_state_recovery_cb_t cb, void *arg)
{
    struct pending *p = pending_get();
    if (!p)
        return RET_CODE_ERR;
    p->op = OP_RECOVERY;
    p->arg = arg;
    p->u.recovery_cb = cb;

    uint32_t now = (uint32_t)time(NULL);
    uint8_t shard = v_port_db_shard_of(g_recovery_key);
    if (v_port_db_cmd(shard, recovery_reply_cb, p, "SET %s %u NX GET", g_recovery_key, now) != RET_CODE_OK) {
        rte_mempool_put(g_pool, p);
        return RET_CODE_ERR;
    }
    return RET_CODE_OK;
}

/* Public: deliberate-cold-start-only unconditional SET — see
 * inc/v_node_state.h for why this must never be called from a rolling-
 * update path. */
int v_node_state_force_new_recovery_ts(v_node_state_recovery_cb_t cb, void *arg)
{
    struct pending *p = pending_get();
    if (!p)
        return RET_CODE_ERR;
    p->op = OP_RECOVERY;
    p->arg = arg;
    p->u.recovery_cb = cb;

    uint32_t now = (uint32_t)time(NULL);
    uint8_t shard = v_port_db_shard_of(g_recovery_key);
    /* Unconditional SET (no NX): always overwrites, always "we won". */
    if (v_port_db_cmd(shard, recovery_reply_cb, p, "SET %s %u", g_recovery_key, now) != RET_CODE_OK) {
        rte_mempool_put(g_pool, p);
        return RET_CODE_ERR;
    }
    return RET_CODE_OK;
}

/* Public: cached accessor for the heartbeat hot path — never touches
 * Redis. Only meaningful once v_node_state_recovery_ts_ready() is true. */
uint32_t v_node_state_recovery_ts(void)
{
    return g_recovery_ts;
}

/* Public: true once v_node_state_load_recovery_ts()'s round trip has
 * completed. */
int v_node_state_recovery_ts_ready(void)
{
    return g_recovery_ts_ready;
}

/* Ack-only callback shared by set_associated()/clear_associated(). */
static void simple_ack_cb(int status, const v_db_reply_t *reply, void *arg)
{
    (void)reply;
    struct pending *p = (struct pending *)arg;
    v_node_state_cb_t cb = p->u.simple_cb;
    void *user_arg = p->arg;
    rte_mempool_put(g_pool, p);
    cb(status, user_arg);
}

/* Public: HSET the association hash to "associated, with this SMF" —
 * visible to every pod, not just this one. */
int v_node_state_set_associated(uint64_t smf_node_id_hash, v_node_state_cb_t cb, void *arg)
{
    struct pending *p = pending_get();
    if (!p)
        return RET_CODE_ERR;
    p->op = OP_SIMPLE;
    p->arg = arg;
    p->u.simple_cb = cb;

    uint8_t shard = v_port_db_shard_of(g_assoc_key);
    if (v_port_db_cmd(shard, simple_ack_cb, p, "HSET %s associated 1 smf %lu",
                       g_assoc_key, smf_node_id_hash) != RET_CODE_OK) {
        rte_mempool_put(g_pool, p);
        return RET_CODE_ERR;
    }
    return RET_CODE_OK;
}

/* Public: HSET the association hash to "not associated". */
int v_node_state_clear_associated(v_node_state_cb_t cb, void *arg)
{
    struct pending *p = pending_get();
    if (!p)
        return RET_CODE_ERR;
    p->op = OP_SIMPLE;
    p->arg = arg;
    p->u.simple_cb = cb;

    uint8_t shard = v_port_db_shard_of(g_assoc_key);
    if (v_port_db_cmd(shard, simple_ack_cb, p, "HSET %s associated 0", g_assoc_key) != RET_CODE_OK) {
        rte_mempool_put(g_pool, p);
        return RET_CODE_ERR;
    }
    return RET_CODE_OK;
}

/* HMGET reply handler: NIL "associated" field means never associated;
 * otherwise parses the "1"/"0" flag and, if set, the SMF identifier. */
static void assoc_query_reply_cb(int status, const v_db_reply_t *reply, void *arg)
{
    struct pending *p = (struct pending *)arg;
    v_node_state_assoc_cb_t cb = p->u.assoc_cb;
    void *user_arg = p->arg;
    rte_mempool_put(g_pool, p);

    if (status != RET_CODE_OK || !reply || reply->type != V_DB_REPLY_ARRAY || reply->nelements != 2) {
        V_LOG(ERR, "DPDB", "node_state: association query failed");
        cb(RET_CODE_ERR, 0, 0, user_arg);
        return;
    }

    const v_db_reply_t *associated_r = &reply->element[0];
    const v_db_reply_t *smf_r = &reply->element[1];

    if (associated_r->type == V_DB_REPLY_NIL) {
        cb(RET_CODE_OK, 0, 0, user_arg); /* never associated */
        return;
    }

    int associated = associated_r->len >= 1 && associated_r->str[0] == '1';
    uint64_t smf = 0;
    if (associated && smf_r->type == V_DB_REPLY_STRING) {
        char buf[24];
        size_t n = smf_r->len < sizeof(buf) - 1 ? smf_r->len : sizeof(buf) - 1;
        memcpy(buf, smf_r->str, n);
        buf[n] = '\0';
        smf = strtoull(buf, NULL, 10);
    }
    cb(RET_CODE_OK, associated, smf, user_arg);
}

/* Public: HMGET the association hash — lets a pod that boots mid-
 * association (or any pod, on every heartbeat) learn the current state
 * from Redis rather than local memory. */
int v_node_state_query_associated(v_node_state_assoc_cb_t cb, void *arg)
{
    struct pending *p = pending_get();
    if (!p)
        return RET_CODE_ERR;
    p->op = OP_ASSOC_QUERY;
    p->arg = arg;
    p->u.assoc_cb = cb;

    uint8_t shard = v_port_db_shard_of(g_assoc_key);
    if (v_port_db_cmd(shard, assoc_query_reply_cb, p, "HMGET %s associated smf", g_assoc_key) != RET_CODE_OK) {
        rte_mempool_put(g_pool, p);
        return RET_CODE_ERR;
    }
    return RET_CODE_OK;
}
