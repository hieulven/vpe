#include "v_sess_store.h"
#include "v_db_script.h"
#include "v_port_db.h"
#include "v_port_pfcp.h"
#include "v_common.h"
#include "v_log.h"

#include <rte_mempool.h>
#include <rte_errno.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ADAPTATION: plan.md doesn't bound the serialized blob size. Stage 2
 * must confirm the real serializer's worst case and resize; this is
 * generous for the Stage 1 stub (~165 bytes). */
#define V_SESS_BLOB_MAX_BYTES 2048
#define V_SESS_PENDING_POOL_CAP 4096

typedef enum { V_SESS_OP_READ, V_SESS_OP_CAS, V_SESS_OP_SIMPLE } v_sess_op_t;

/* One in-flight session-store call. `op` isn't read anywhere today
 * (the union member accessed is always known from which function
 * created the pending object) — kept for clarity/future assertions. */
struct v_sess_pending {
    v_sess_op_t op;
    void *arg;
    union {
        struct { v_sess_read_cb_t cb; struct pdu_ses_ctx *ctx; } read;
        struct { v_sess_cas_cb_t cb; uint64_t exp_ver; } cas;
        struct { v_sess_cb_t cb; } simple;
    } u;
};

static struct rte_mempool *g_pending_pool;

/* Lazily creates the pending-object pool on first use. */
static int ensure_pool(void)
{
    if (g_pending_pool)
        return RET_CODE_OK;
    g_pending_pool = rte_mempool_create("v_sess_pending_pool", V_SESS_PENDING_POOL_CAP,
                                         sizeof(struct v_sess_pending), 0, 0,
                                         NULL, NULL, NULL, NULL, SOCKET_ID_ANY, 0);
    if (!g_pending_pool) {
        V_LOG(ERR, "DPDB", "sess pending pool create failed: %s", rte_strerror(rte_errno));
        return RET_CODE_ERR;
    }
    return RET_CODE_OK;
}

/* Formats the fixed pdu_<partid>:<seid> key — see inc/v_sess_store.h,
 * this format string MUST NOT CHANGE (plan.md environment facts). */
static void make_key(char *buf, size_t buf_cap, uint16_t part_id, uint64_t seid)
{
    snprintf(buf, buf_cap, V_SESS_KEY_FMT, part_id, seid);
}

/* --- read --- */

/* HMGET reply handler. A NIL data or ver field means the key doesn't
 * exist — reported as V_CAS_GONE with RET_CODE_OK (the read itself
 * succeeded; "not found" is a normal outcome, not a transport error). */
static void read_reply_cb(int status, const v_db_reply_t *reply, void *arg)
{
    struct v_sess_pending *p = (struct v_sess_pending *)arg;
    v_sess_read_cb_t cb = p->u.read.cb;
    struct pdu_ses_ctx *ctx = p->u.read.ctx;
    void *user_arg = p->arg;

    if (status != RET_CODE_OK || !reply || reply->type != V_DB_REPLY_ARRAY || reply->nelements != 2) {
        V_LOG(ERR, "DPDB", "sess read failed");
        cb(RET_CODE_ERR, V_CAS_GONE, ctx, 0, user_arg);
        rte_mempool_put(g_pending_pool, p);
        return;
    }

    const v_db_reply_t *data = &reply->element[0];
    const v_db_reply_t *ver = &reply->element[1];

    if (data->type == V_DB_REPLY_NIL || ver->type == V_DB_REPLY_NIL) {
        cb(RET_CODE_OK, V_CAS_GONE, ctx, 0, user_arg);
        rte_mempool_put(g_pending_pool, p);
        return;
    }

    if (v_port_pdu_deserialize((const uint8_t *)data->str, data->len, ctx) != RET_CODE_OK) {
        V_LOG(ERR, "DPDB", "sess read: deserialize failed");
        cb(RET_CODE_ERR, V_CAS_GONE, ctx, 0, user_arg);
        rte_mempool_put(g_pending_pool, p);
        return;
    }

    char verbuf[24];
    size_t n = ver->len < sizeof(verbuf) - 1 ? ver->len : sizeof(verbuf) - 1;
    memcpy(verbuf, ver->str, n);
    verbuf[n] = '\0';
    uint64_t ver_val = strtoull(verbuf, NULL, 10);

    cb(RET_CODE_OK, V_CAS_OK, ctx, ver_val, user_arg);
    rte_mempool_put(g_pending_pool, p);
}

/* Public: HMGET data+ver in one round trip, deserializing directly
 * into caller-owned ctx on a hit. */
int v_sess_read(uint16_t part_id, uint64_t seid, struct pdu_ses_ctx *ctx,
                 v_sess_read_cb_t cb, void *arg)
{
    if (ensure_pool() != RET_CODE_OK)
        return RET_CODE_ERR;

    char key[64];
    make_key(key, sizeof(key), part_id, seid);

    struct v_sess_pending *p;
    if (rte_mempool_get(g_pending_pool, (void **)&p) != 0) {
        V_LOG(WARNING, "DPDB", "sess pending pool exhausted");
        return RET_CODE_ERR;
    }
    p->op = V_SESS_OP_READ;
    p->arg = arg;
    p->u.read.cb = cb;
    p->u.read.ctx = ctx;

    uint8_t shard = v_port_db_shard_of(key);
    if (v_port_db_cmd(shard, read_reply_cb, p, "HMGET %s %s %s",
                       key, V_SESS_FIELD_DATA, V_SESS_FIELD_VER) != RET_CODE_OK) {
        rte_mempool_put(g_pending_pool, p);
        return RET_CODE_ERR;
    }
    return RET_CODE_OK;
}

/* --- cas write --- */

/* v_sess_cas_write Lua script reply handler — maps the script's
 * integer return (1/0/-1, see inc/v_db_lua_scripts.h) to a
 * v_cas_result_t. new_ver is computed client-side from exp_ver rather
 * than re-read from Redis, since the script's own increment logic is
 * exactly "exp_ver==0 ? 1 : exp_ver+1". */
static void cas_write_reply_cb(int status, const v_db_reply_t *reply, void *arg)
{
    struct v_sess_pending *p = (struct v_sess_pending *)arg;
    v_sess_cas_cb_t cb = p->u.cas.cb;
    uint64_t exp_ver = p->u.cas.exp_ver;
    void *user_arg = p->arg;

    if (status != RET_CODE_OK || !reply || reply->type != V_DB_REPLY_INTEGER) {
        V_LOG(ERR, "DPDB", "sess cas_write failed");
        cb(RET_CODE_ERR, V_CAS_GONE, 0, user_arg);
        rte_mempool_put(g_pending_pool, p);
        return;
    }

    switch (reply->integer) {
    case 1: {
        uint64_t new_ver = (exp_ver == 0) ? 1 : (exp_ver + 1);
        cb(RET_CODE_OK, V_CAS_OK, new_ver, user_arg);
        break;
    }
    case 0:
        cb(RET_CODE_OK, V_CAS_CONFLICT, 0, user_arg);
        break;
    case -1:
        cb(RET_CODE_OK, V_CAS_GONE, 0, user_arg);
        break;
    default:
        V_LOG(ERR, "DPDB", "sess cas_write: unexpected script return %lld", reply->integer);
        cb(RET_CODE_ERR, V_CAS_GONE, 0, user_arg);
        break;
    }
    rte_mempool_put(g_pending_pool, p);
}

/* Public: serializes ctx synchronously (into a stack buffer, before
 * this function returns) then dispatches the CAS-write Lua script.
 * Because the serialize happens synchronously here, a caller is free
 * to free/reuse ctx immediately after this call returns — it does not
 * need to stay alive until the callback fires. */
int v_sess_cas_write(uint16_t part_id, uint64_t seid, uint64_t exp_ver,
                      const struct pdu_ses_ctx *ctx, v_sess_state_t state,
                      v_sess_cas_cb_t cb, void *arg)
{
    if (ensure_pool() != RET_CODE_OK)
        return RET_CODE_ERR;

    char key[64];
    make_key(key, sizeof(key), part_id, seid);

    uint8_t blob[V_SESS_BLOB_MAX_BYTES];
    size_t blob_len = sizeof(blob);
    if (v_port_pdu_serialize(ctx, blob, &blob_len) != RET_CODE_OK) {
        V_LOG(ERR, "DPDB", "sess cas_write: serialize failed");
        return RET_CODE_ERR;
    }

    char ver_str[24];
    snprintf(ver_str, sizeof(ver_str), "%lu", exp_ver);
    char ttl_str[8];
    snprintf(ttl_str, sizeof(ttl_str), "%d", state == V_SESS_PENDING ? V_SESS_PENDING_TTL : 0);

    struct v_sess_pending *p;
    if (rte_mempool_get(g_pending_pool, (void **)&p) != 0) {
        V_LOG(WARNING, "DPDB", "sess pending pool exhausted");
        return RET_CODE_ERR;
    }
    p->op = V_SESS_OP_CAS;
    p->arg = arg;
    p->u.cas.cb = cb;
    p->u.cas.exp_ver = exp_ver;

    const char *keys[1] = { key };
    const char *argv[3] = { ver_str, (const char *)blob, ttl_str };
    size_t argvlen[3] = { strlen(ver_str), blob_len, strlen(ttl_str) };
    uint8_t shard = v_port_db_shard_of(key);

    if (v_db_evalsha(shard, V_SCRIPT_SESS_CAS_WRITE, 1, keys, argv, argvlen, 3,
                      cas_write_reply_cb, p) != RET_CODE_OK) {
        rte_mempool_put(g_pending_pool, p);
        return RET_CODE_ERR;
    }
    return RET_CODE_OK;
}

/* --- confirm (PERSIST) --- */

/* PERSIST reply handler — the integer result (0 or 1, whether a TTL
 * existed to remove) doesn't matter to the caller, only whether the
 * round trip itself succeeded. */
static void confirm_reply_cb(int status, const v_db_reply_t *reply, void *arg)
{
    (void)reply;
    struct v_sess_pending *p = (struct v_sess_pending *)arg;
    v_sess_cb_t cb = p->u.simple.cb;
    void *user_arg = p->arg;

    cb(status, user_arg);
    rte_mempool_put(g_pending_pool, p);
}

/* Public: PERSIST — clears the pending TTL without touching ver. */
int v_sess_confirm(uint16_t part_id, uint64_t seid, v_sess_cb_t cb, void *arg)
{
    if (ensure_pool() != RET_CODE_OK)
        return RET_CODE_ERR;

    char key[64];
    make_key(key, sizeof(key), part_id, seid);

    struct v_sess_pending *p;
    if (rte_mempool_get(g_pending_pool, (void **)&p) != 0) {
        V_LOG(WARNING, "DPDB", "sess pending pool exhausted");
        return RET_CODE_ERR;
    }
    p->op = V_SESS_OP_SIMPLE;
    p->arg = arg;
    p->u.simple.cb = cb;

    uint8_t shard = v_port_db_shard_of(key);
    if (v_port_db_cmd(shard, confirm_reply_cb, p, "PERSIST %s", key) != RET_CODE_OK) {
        rte_mempool_put(g_pending_pool, p);
        return RET_CODE_ERR;
    }
    return RET_CODE_OK;
}

/* --- delete --- */

/* v_sess_delete Lua script reply handler — same 1/0/-1 -> CAS-result
 * mapping as cas_write_reply_cb(), but delete has no "new_ver" to
 * compute (always passes 0). */
static void delete_reply_cb(int status, const v_db_reply_t *reply, void *arg)
{
    struct v_sess_pending *p = (struct v_sess_pending *)arg;
    v_sess_cas_cb_t cb = p->u.cas.cb;
    void *user_arg = p->arg;

    if (status != RET_CODE_OK || !reply || reply->type != V_DB_REPLY_INTEGER) {
        V_LOG(ERR, "DPDB", "sess delete failed");
        cb(RET_CODE_ERR, V_CAS_GONE, 0, user_arg);
        rte_mempool_put(g_pending_pool, p);
        return;
    }

    switch (reply->integer) {
    case 1:  cb(RET_CODE_OK, V_CAS_OK, 0, user_arg); break;
    case 0:  cb(RET_CODE_OK, V_CAS_CONFLICT, 0, user_arg); break;
    case -1: cb(RET_CODE_OK, V_CAS_GONE, 0, user_arg); break;
    default:
        V_LOG(ERR, "DPDB", "sess delete: unexpected script return %lld", reply->integer);
        cb(RET_CODE_ERR, V_CAS_GONE, 0, user_arg);
        break;
    }
    rte_mempool_put(g_pending_pool, p);
}

/* Public: CAS-guarded DEL. */
int v_sess_delete(uint16_t part_id, uint64_t seid, uint64_t exp_ver,
                   v_sess_cas_cb_t cb, void *arg)
{
    if (ensure_pool() != RET_CODE_OK)
        return RET_CODE_ERR;

    char key[64];
    make_key(key, sizeof(key), part_id, seid);

    char ver_str[24];
    snprintf(ver_str, sizeof(ver_str), "%lu", exp_ver);

    struct v_sess_pending *p;
    if (rte_mempool_get(g_pending_pool, (void **)&p) != 0) {
        V_LOG(WARNING, "DPDB", "sess pending pool exhausted");
        return RET_CODE_ERR;
    }
    p->op = V_SESS_OP_CAS;
    p->arg = arg;
    p->u.cas.cb = cb;
    p->u.cas.exp_ver = exp_ver;

    const char *keys[1] = { key };
    const char *argv[1] = { ver_str };
    size_t argvlen[1] = { strlen(ver_str) };
    uint8_t shard = v_port_db_shard_of(key);

    if (v_db_evalsha(shard, V_SCRIPT_SESS_DELETE, 1, keys, argv, argvlen, 1,
                      delete_reply_cb, p) != RET_CODE_OK) {
        rte_mempool_put(g_pending_pool, p);
        return RET_CODE_ERR;
    }
    return RET_CODE_OK;
}

/* Test-only: pending-pool leak assertion helper (see
 * inc/v_sess_store.h). */
size_t v_sess_store_test_pending_in_use(void)
{
    if (!g_pending_pool)
        return 0;
    return V_SESS_PENDING_POOL_CAP - rte_mempool_avail_count(g_pending_pool);
}
