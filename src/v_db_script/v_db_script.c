#include "v_db_script.h"
#include "v_db_lua_scripts.h"
#include "v_common.h"
#include "v_log.h"

#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_errno.h>

#include <string.h>
#include <stdio.h>

#define V_DB_SCRIPT_SHA_LEN 40
#define V_DB_EVALSHA_MAX_PARTS 12
#define V_DB_EVALSHA_SCRATCH_BYTES 1024
#define V_DB_EVALSHA_POOL_CAP 2048
#define V_DB_SCRIPT_LOAD_POOL_CAP 256

static const char *g_script_src[V_SCRIPT_COUNT] = {
    [V_SCRIPT_SESS_CAS_WRITE] = NULL, /* filled in v_db_script_init */
    [V_SCRIPT_SESS_DELETE]    = NULL,
    [V_SCRIPT_TEID_REFILL]    = NULL,
};

/* sha[shard][script_id] -> 40 hex chars + NUL. rte_zmalloc'd once at
 * init, sized to the actual shard count — startup allocation, not a
 * hot-path one, so plain rte_zmalloc is correct per plan.md's
 * "Startup only: use rte_zmalloc" convention. */
static char (*g_sha)[V_SCRIPT_COUNT][V_DB_SCRIPT_SHA_LEN + 1];
static int g_shard_count;

struct v_evalsha_pending {
    uint8_t shard;
    v_script_id_t script_id;
    v_db_cb_t user_cb;
    void *user_arg;
    int retried;

    int argc;
    const char *part_ptr[V_DB_EVALSHA_MAX_PARTS];
    size_t part_len[V_DB_EVALSHA_MAX_PARTS];
    char scratch[V_DB_EVALSHA_SCRATCH_BYTES];
    size_t scratch_used;
};

struct v_script_load_ctx {
    uint8_t shard;
    v_script_id_t script_id;
    struct v_evalsha_pending *resume; /* NULL for a plain startup load */
};

static struct rte_mempool *g_evalsha_pool;
static struct rte_mempool *g_load_pool;

static void issue_evalsha(struct v_evalsha_pending *p);

static char *scratch_copy(struct v_evalsha_pending *p, const char *src, size_t len)
{
    if (p->scratch_used + len > sizeof(p->scratch)) {
        V_LOG(ERR, "DPDB", "evalsha scratch exhausted (%zu + %zu > %zu)",
              p->scratch_used, len, sizeof(p->scratch));
        return NULL;
    }
    char *dst = &p->scratch[p->scratch_used];
    memcpy(dst, src, len);
    p->scratch_used += len;
    return dst;
}

static void load_reply_cb(int status, const v_db_reply_t *reply, void *arg)
{
    struct v_script_load_ctx *lc = (struct v_script_load_ctx *)arg;

    if (status != RET_CODE_OK || !reply || reply->type != V_DB_REPLY_STRING ||
        reply->len != V_DB_SCRIPT_SHA_LEN) {
        V_LOG(ERR, "DPDB", "SCRIPT LOAD failed shard=%u script=%d",
              lc->shard, lc->script_id);
        if (lc->resume) {
            struct v_evalsha_pending *p = lc->resume;
            if (p->user_cb)
                p->user_cb(RET_CODE_ERR, reply, p->user_arg);
            rte_mempool_put(g_evalsha_pool, p);
        }
        rte_mempool_put(g_load_pool, lc);
        return;
    }

    memcpy(g_sha[lc->shard][lc->script_id], reply->str, V_DB_SCRIPT_SHA_LEN);
    g_sha[lc->shard][lc->script_id][V_DB_SCRIPT_SHA_LEN] = '\0';
    V_LOG(DEBUG, "DPDB", "SCRIPT LOAD ok shard=%u script=%d sha=%s",
          lc->shard, lc->script_id, g_sha[lc->shard][lc->script_id]);

    if (lc->resume) {
        struct v_evalsha_pending *p = lc->resume;
        p->part_ptr[1] = g_sha[lc->shard][lc->script_id];
        p->part_len[1] = V_DB_SCRIPT_SHA_LEN;
        issue_evalsha(p);
    }

    rte_mempool_put(g_load_pool, lc);
}

static int request_script_load(uint8_t shard, v_script_id_t id, struct v_evalsha_pending *resume)
{
    struct v_script_load_ctx *lc;
    if (rte_mempool_get(g_load_pool, (void **)&lc) != 0) {
        V_LOG(ERR, "DPDB", "script-load ctx pool exhausted");
        return RET_CODE_ERR;
    }
    lc->shard = shard;
    lc->script_id = id;
    lc->resume = resume;

    if (v_port_db_cmd(shard, load_reply_cb, lc, "SCRIPT LOAD %s", g_script_src[id]) != RET_CODE_OK) {
        rte_mempool_put(g_load_pool, lc);
        return RET_CODE_ERR;
    }
    return RET_CODE_OK;
}

static void script_reconnect_cb(uint8_t shard, void *arg)
{
    (void)arg;
    for (int id = 0; id < V_SCRIPT_COUNT; id++)
        request_script_load(shard, (v_script_id_t)id, NULL);
}

int v_db_script_init(void)
{
    g_script_src[V_SCRIPT_SESS_CAS_WRITE] = V_LUA_SESS_CAS_WRITE;
    g_script_src[V_SCRIPT_SESS_DELETE]    = V_LUA_SESS_DELETE;
    g_script_src[V_SCRIPT_TEID_REFILL]    = V_LUA_TEID_REFILL;

    g_shard_count = v_port_db_shard_count();
    if (g_shard_count <= 0) {
        V_LOG(ERR, "DPDB", "invalid shard count %d", g_shard_count);
        return RET_CODE_ERR;
    }

    g_sha = rte_zmalloc("v_db_script_sha",
                         (size_t)g_shard_count * sizeof(*g_sha), 0);
    if (!g_sha) {
        V_LOG(ERR, "DPDB", "rte_zmalloc sha table failed");
        return RET_CODE_ERR;
    }

    g_evalsha_pool = rte_mempool_create("v_evalsha_pool", V_DB_EVALSHA_POOL_CAP,
                                         sizeof(struct v_evalsha_pending), 0, 0,
                                         NULL, NULL, NULL, NULL, SOCKET_ID_ANY, 0);
    g_load_pool = rte_mempool_create("v_script_load_pool", V_DB_SCRIPT_LOAD_POOL_CAP,
                                      sizeof(struct v_script_load_ctx), 0, 0,
                                      NULL, NULL, NULL, NULL, SOCKET_ID_ANY, 0);
    if (!g_evalsha_pool || !g_load_pool) {
        V_LOG(ERR, "DPDB", "evalsha/load pool create failed: %s", rte_strerror(rte_errno));
        return RET_CODE_ERR;
    }

    return v_port_db_on_reconnect(script_reconnect_cb, NULL);
}

int v_db_script_ready(void)
{
    if (!g_sha)
        return 0;
    for (int s = 0; s < g_shard_count; s++)
        for (int id = 0; id < V_SCRIPT_COUNT; id++)
            if (g_sha[s][id][0] == '\0')
                return 0;
    return 1;
}

static void evalsha_reply_cb(int status, const v_db_reply_t *reply, void *arg)
{
    struct v_evalsha_pending *p = (struct v_evalsha_pending *)arg;

    int is_noscript = (status == RET_CODE_OK && reply && reply->type == V_DB_REPLY_ERROR &&
                        reply->len >= 8 && strncmp(reply->str, "NOSCRIPT", 8) == 0);

    if (is_noscript && !p->retried) {
        p->retried = 1;
        V_LOG(WARNING, "DPDB", "NOSCRIPT shard=%u script=%d, reloading and retrying once",
              p->shard, p->script_id);
        if (request_script_load(p->shard, p->script_id, p) != RET_CODE_OK) {
            if (p->user_cb)
                p->user_cb(RET_CODE_ERR, reply, p->user_arg);
            rte_mempool_put(g_evalsha_pool, p);
        }
        return;
    }

    if (p->user_cb)
        p->user_cb(status, reply, p->user_arg);
    rte_mempool_put(g_evalsha_pool, p);
}

static void issue_evalsha(struct v_evalsha_pending *p)
{
    if (v_port_db_cmd_argv(p->shard, evalsha_reply_cb, p, p->argc,
                            p->part_ptr, p->part_len) != RET_CODE_OK) {
        if (p->user_cb)
            p->user_cb(RET_CODE_ERR, NULL, p->user_arg);
        rte_mempool_put(g_evalsha_pool, p);
    }
}

int v_db_evalsha(uint8_t shard, v_script_id_t script_id,
                  int nkeys, const char **keys,
                  const char **argv, const size_t *argvlen, int nargv,
                  v_db_cb_t cb, void *arg)
{
    if (script_id < 0 || script_id >= V_SCRIPT_COUNT) {
        V_LOG(ERR, "DPDB", "bad script_id %d", script_id);
        return RET_CODE_ERR;
    }
    if (3 + nkeys + nargv > V_DB_EVALSHA_MAX_PARTS) {
        V_LOG(ERR, "DPDB", "too many evalsha parts (%d)", 3 + nkeys + nargv);
        return RET_CODE_ERR;
    }
    if (shard >= g_shard_count || g_sha[shard][script_id][0] == '\0') {
        V_LOG(WARNING, "DPDB", "no cached sha shard=%u script=%d yet", shard, script_id);
        return RET_CODE_ERR;
    }

    struct v_evalsha_pending *p;
    if (rte_mempool_get(g_evalsha_pool, (void **)&p) != 0) {
        V_LOG(WARNING, "DPDB", "evalsha pending pool exhausted");
        return RET_CODE_ERR;
    }
    memset(p, 0, sizeof(*p));
    p->shard = shard;
    p->script_id = script_id;
    p->user_cb = cb;
    p->user_arg = arg;

    char numkeys_str[8];
    int nk_len = snprintf(numkeys_str, sizeof(numkeys_str), "%d", nkeys);

    p->part_ptr[0] = "EVALSHA";
    p->part_len[0] = 7;
    p->part_ptr[1] = g_sha[shard][script_id];
    p->part_len[1] = V_DB_SCRIPT_SHA_LEN;
    char *nk_copy = scratch_copy(p, numkeys_str, (size_t)nk_len);
    if (!nk_copy) {
        rte_mempool_put(g_evalsha_pool, p);
        return RET_CODE_ERR;
    }
    p->part_ptr[2] = nk_copy;
    p->part_len[2] = (size_t)nk_len;

    int idx = 3;
    for (int i = 0; i < nkeys; i++, idx++) {
        size_t len = strlen(keys[i]);
        char *c = scratch_copy(p, keys[i], len);
        if (!c) { rte_mempool_put(g_evalsha_pool, p); return RET_CODE_ERR; }
        p->part_ptr[idx] = c;
        p->part_len[idx] = len;
    }
    for (int i = 0; i < nargv; i++, idx++) {
        size_t len = argvlen[i];
        char *c = scratch_copy(p, argv[i], len);
        if (!c) { rte_mempool_put(g_evalsha_pool, p); return RET_CODE_ERR; }
        p->part_ptr[idx] = c;
        p->part_len[idx] = len;
    }
    p->argc = idx;

    issue_evalsha(p);
    return RET_CODE_OK;
}
