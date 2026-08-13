#include "v_retrans_cache.h"
#include "v_seid.h"
#include "v_port_db.h"
#include "v_common.h"
#include "v_log.h"

#include <rte_mempool.h>
#include <rte_errno.h>

#include <stdio.h>
#include <string.h>

#define V_RETRANS_PENDING_POOL_CAP 4096

struct v_retrans_pending {
    v_retrans_cb_t cb;
    void *arg;
};

static struct rte_mempool *g_pool;

static int ensure_pool(void)
{
    if (g_pool)
        return RET_CODE_OK;
    g_pool = rte_mempool_create("v_retrans_pending_pool", V_RETRANS_PENDING_POOL_CAP,
                                 sizeof(struct v_retrans_pending), 0, 0,
                                 NULL, NULL, NULL, NULL, SOCKET_ID_ANY, 0);
    if (!g_pool) {
        V_LOG(ERR, "DPDB", "retrans pending pool create failed: %s", rte_strerror(rte_errno));
        return RET_CODE_ERR;
    }
    return RET_CODE_OK;
}

uint16_t v_retrans_part(uint64_t smf_fseid)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 8; i++) {
        uint8_t byte = (uint8_t)(smf_fseid >> (i * 8));
        h ^= byte;
        h *= 16777619u;
    }
    return (uint16_t)(h % V_NUM_PARTS);
}

static void lookup_reply_cb(int status, const v_db_reply_t *reply, void *arg)
{
    struct v_retrans_pending *p = (struct v_retrans_pending *)arg;

    if (status != RET_CODE_OK) {
        V_LOG(ERR, "DPDB", "retrans lookup failed");
        p->cb(RET_CODE_ERR, 0, NULL, 0, p->arg);
    } else if (!reply || reply->type == V_DB_REPLY_NIL) {
        p->cb(RET_CODE_OK, 0, NULL, 0, p->arg);
    } else if (reply->type == V_DB_REPLY_STRING) {
        /* resp is only valid for the duration of this callback — same
         * lifetime rule as every other v_db_reply_t (see v_stub_db.c). */
        p->cb(RET_CODE_OK, 1, (const uint8_t *)reply->str, reply->len, p->arg);
    } else {
        V_LOG(ERR, "DPDB", "retrans lookup: unexpected reply type %d", reply->type);
        p->cb(RET_CODE_ERR, 0, NULL, 0, p->arg);
    }
    rte_mempool_put(g_pool, p);
}

int v_retrans_lookup(uint16_t part_id, uint64_t smf_fseid, uint32_t seq,
                      v_retrans_cb_t cb, void *arg)
{
    if (ensure_pool() != RET_CODE_OK)
        return RET_CODE_ERR;

    char key[64];
    snprintf(key, sizeof(key), V_RETRANS_KEY_FMT, part_id, smf_fseid, seq);

    struct v_retrans_pending *p;
    if (rte_mempool_get(g_pool, (void **)&p) != 0) {
        V_LOG(WARNING, "DPDB", "retrans pending pool exhausted");
        return RET_CODE_ERR;
    }
    p->cb = cb;
    p->arg = arg;

    uint8_t shard = v_port_db_shard_of(key);
    if (v_port_db_cmd(shard, lookup_reply_cb, p, "GET %s", key) != RET_CODE_OK) {
        rte_mempool_put(g_pool, p);
        return RET_CODE_ERR;
    }
    return RET_CODE_OK;
}

static void store_ack_cb(int status, const v_db_reply_t *reply, void *arg)
{
    (void)reply; (void)arg;
    if (status != RET_CODE_OK)
        V_LOG(WARNING, "DPDB", "retrans store failed");
}

int v_retrans_store(uint16_t part_id, uint64_t smf_fseid, uint32_t seq,
                     const uint8_t *resp, size_t len)
{
    char key[64];
    snprintf(key, sizeof(key), V_RETRANS_KEY_FMT, part_id, smf_fseid, seq);
    char ttl_str[8];
    snprintf(ttl_str, sizeof(ttl_str), "%d", V_RETRANS_TTL);

    /* Binary-safe argv form (see v_port_db.h ADAPTATION note 2) — resp
     * is a raw PFCP response and may contain arbitrary bytes. */
    const char *argv[4] = { "SETEX", key, ttl_str, (const char *)resp };
    size_t argvlen[4] = { 5, strlen(key), strlen(ttl_str), len };

    uint8_t shard = v_port_db_shard_of(key);
    return v_port_db_cmd_argv(shard, store_ack_cb, NULL, 4, argv, argvlen);
}
