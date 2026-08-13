#include "v_port_db.h"
#include "v_common.h"
#include "v_log.h"

#include <hiredis/hiredis.h>
#include <hiredis/async.h>
#include <hiredis/adapters/libevent.h>
#include <event2/event.h>

#include <rte_mempool.h>
#include <rte_errno.h>

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/*
 * DB port stub. Talks to a REAL local redis-server via hiredis, per
 * plan.md §3.6 ("DB stub ... preferred, so the Lua is genuinely
 * exercised"). Five logical shards are five separate hiredis async
 * connections; this environment has exactly one physical redis-server
 * (no 5-master Sentinel topology available in Stage 1), so all five
 * connections target the same instance. Shard *routing* (v_db_shard_of,
 * key co-location) is still exercised for real; true cross-shard
 * independence under Sentinel failover is a Stage 2 concern (plan.md
 * §9.3 "Sentinel failover under load").
 */

#define V_STUB_DB_SHARDS 5
#define V_STUB_DB_PENDING_CAP 4096
#define V_STUB_DB_REPLY_SCRATCH_CAP 512

/* One in-flight command — carries the caller's callback through to
 * on_reply(), which hiredis invokes via privdata. */
struct v_db_pending {
    v_db_cb_t cb;
    void *arg;
};

static struct event_base *g_base;
static redisAsyncContext *g_ctx[V_STUB_DB_SHARDS];
static struct rte_mempool *g_pending_pool;

static v_db_reconnect_cb_t g_reconnect_cb;
static void *g_reconnect_arg;

static v_db_reply_t g_scratch[V_STUB_DB_REPLY_SCRATCH_CAP];
static size_t g_scratch_used;

/* Redis host, overridable via VPE_TEST_REDIS_HOST for the test suite. */
static const char *db_host(void)
{
    const char *h = getenv("VPE_TEST_REDIS_HOST");
    return h ? h : "127.0.0.1";
}

/* Redis port, overridable via VPE_TEST_REDIS_PORT. */
static int db_port(void)
{
    const char *p = getenv("VPE_TEST_REDIS_PORT");
    return p ? atoi(p) : 6379;
}

static void connect_cb(const redisAsyncContext *c, int status);
static void disconnect_cb(const redisAsyncContext *c, int status);

/* (Re)connects one shard: opens a new hiredis async context, attaches
 * it to the shared libevent base, and wires the connect/disconnect
 * callbacks. Used both at startup (all 5 shards) and by
 * v_stub_db_test_force_reconnect() (one shard, on demand). */
static int shard_connect(int shard)
{
    redisAsyncContext *ctx = redisAsyncConnect(db_host(), db_port());
    if (!ctx || ctx->err) {
        V_LOG(ERR, "DPDB", "shard %d connect failed: %s", shard,
              ctx ? ctx->errstr : "alloc failure");
        if (ctx)
            redisAsyncFree(ctx);
        return RET_CODE_ERR;
    }
    ctx->data = (void *)(intptr_t)shard;
    redisLibeventAttach(ctx, g_base);
    redisAsyncSetConnectCallback(ctx, connect_cb);
    redisAsyncSetDisconnectCallback(ctx, disconnect_cb);
    g_ctx[shard] = ctx;
    return RET_CODE_OK;
}

/* hiredis connect callback: on success, fires the registered
 * v_db_reconnect_cb_t — this fires for the VERY FIRST connect too, not
 * just later reconnects, which is what lets v_db_script's SCRIPT LOAD-
 * on-reconnect hook double as the initial script load. */
static void connect_cb(const redisAsyncContext *c, int status)
{
    int shard = (int)(intptr_t)c->data;
    if (status != REDIS_OK) {
        V_LOG(ERR, "DPDB", "shard %d connect error: %s", shard, c->errstr);
        return;
    }
    V_LOG(INFO, "DPDB", "shard %d connected", shard);
    if (g_reconnect_cb)
        g_reconnect_cb((uint8_t)shard, g_reconnect_arg);
}

/* hiredis disconnect callback: just clears the shard's context pointer
 * — this stub doesn't auto-reconnect on an unexpected disconnect
 * (that's what v_stub_db_test_force_reconnect() is for, on demand). */
static void disconnect_cb(const redisAsyncContext *c, int status)
{
    int shard = (int)(intptr_t)c->data;
    g_ctx[shard] = NULL;
    if (status != REDIS_OK)
        V_LOG(WARNING, "DPDB", "shard %d disconnected unexpectedly: %s", shard, c->errstr);
    else
        V_LOG(DEBUG, "DPDB", "shard %d disconnected", shard);
}

/* Port impl (Stage 1 addition — see include/v_port_db.h): creates the
 * libevent base and pending-object pool, then connects all 5 shards. */
int v_port_db_init(void)
{
    if (g_base) {
        V_LOG(WARNING, "DPDB", "v_port_db_init called twice");
        return RET_CODE_ERR;
    }
    g_base = event_base_new();
    if (!g_base) {
        V_LOG(ERR, "DPDB", "event_base_new failed");
        return RET_CODE_ERR;
    }
    g_pending_pool = rte_mempool_create("v_db_pending_pool", V_STUB_DB_PENDING_CAP,
                                         sizeof(struct v_db_pending), 0, 0,
                                         NULL, NULL, NULL, NULL, SOCKET_ID_ANY, 0);
    if (!g_pending_pool) {
        V_LOG(ERR, "DPDB", "pending pool create failed: %s", rte_strerror(rte_errno));
        return RET_CODE_ERR;
    }

    for (int i = 0; i < V_STUB_DB_SHARDS; i++) {
        if (shard_connect(i) != RET_CODE_OK)
            return RET_CODE_ERR;
    }
    return RET_CODE_OK;
}

/* Port impl: tears down every shard connection, the pending pool, and
 * the event base. */
void v_port_db_fini(void)
{
    for (int i = 0; i < V_STUB_DB_SHARDS; i++) {
        if (g_ctx[i]) {
            redisAsyncFree(g_ctx[i]);
            g_ctx[i] = NULL;
        }
    }
    if (g_pending_pool) {
        rte_mempool_free(g_pending_pool);
        g_pending_pool = NULL;
    }
    if (g_base) {
        event_base_free(g_base);
        g_base = NULL;
    }
}

/* Port impl: pumps the libevent loop once. Production binds the real
 * DB layer's event loop to a worker's own libevent loop instead (the
 * v_pdu_cacher pattern); this exists because Stage 1 has no such loop
 * to bind to. */
void v_port_db_poll(int nonblock)
{
    if (!g_base)
        return;
    event_base_loop(g_base, nonblock ? EVLOOP_NONBLOCK : 0);
}

/* Reconnect a shard on demand — used by the standalone test suite to
 * simulate a Sentinel promotion (plan.md §4 "Sentinel failover produces
 * NOSCRIPT"). Not part of the production port surface. */
int v_stub_db_test_force_reconnect(uint8_t shard)
{
    if (shard >= V_STUB_DB_SHARDS)
        return RET_CODE_ERR;
    if (g_ctx[shard])
        redisAsyncFree(g_ctx[shard]);
    return shard_connect(shard);
}

/* Recursively converts a hiredis redisReply tree into the port's
 * v_db_reply_t tree, bump-allocating nodes from the static g_scratch
 * arena (reset at the top of every on_reply() call — see the lifetime
 * note in include/v_port_db.h and CLAUDE.md: the result is only valid
 * for the duration of the callback it's handed to). */
static v_db_reply_t *convert_reply(redisReply *r)
{
    if (g_scratch_used >= V_STUB_DB_REPLY_SCRATCH_CAP) {
        V_LOG(ERR, "DPDB", "reply scratch exhausted");
        return NULL;
    }
    v_db_reply_t *out = &g_scratch[g_scratch_used++];
    memset(out, 0, sizeof(*out));

    if (!r) {
        out->type = V_DB_REPLY_NIL;
        return out;
    }

    switch (r->type) {
    case REDIS_REPLY_NIL:
        out->type = V_DB_REPLY_NIL;
        break;
    case REDIS_REPLY_STATUS:
        out->type = V_DB_REPLY_STATUS;
        out->str = r->str;
        out->len = r->len;
        break;
    case REDIS_REPLY_ERROR:
        out->type = V_DB_REPLY_ERROR;
        out->str = r->str;
        out->len = r->len;
        break;
    case REDIS_REPLY_INTEGER:
        out->type = V_DB_REPLY_INTEGER;
        out->integer = r->integer;
        break;
    case REDIS_REPLY_STRING:
        out->type = V_DB_REPLY_STRING;
        out->str = r->str;
        out->len = r->len;
        break;
    case REDIS_REPLY_ARRAY:
#if defined(REDIS_REPLY_PUSH)
    case REDIS_REPLY_PUSH:
#endif
        out->type = V_DB_REPLY_ARRAY;
        out->nelements = r->elements;
        if (r->elements > 0) {
            out->element = &g_scratch[g_scratch_used];
            for (size_t i = 0; i < r->elements; i++) {
                v_db_reply_t *child = convert_reply(r->element[i]);
                if (!child)
                    return out;
                /* re-fetch out->element in case realloc-like growth ever
                 * happens; scratch is static so pointer stays valid. */
            }
        }
        break;
    default:
        out->type = V_DB_REPLY_ERROR;
        out->str = "unsupported reply type";
        out->len = strlen(out->str);
        break;
    }
    return out;
}

/* hiredis async-command reply trampoline: converts the raw reply,
 * invokes the caller's v_db_cb_t, then releases the pending object.
 * Shared by both v_port_db_cmd() and v_port_db_cmd_argv(). */
static void on_reply(struct redisAsyncContext *c, void *r, void *privdata)
{
    (void)c;
    struct v_db_pending *p = (struct v_db_pending *)privdata;
    redisReply *reply = (redisReply *)r;

    g_scratch_used = 0;
    v_db_reply_t *converted = reply ? convert_reply(reply) : NULL;

    int status = reply ? RET_CODE_OK : RET_CODE_ERR;
    if (p->cb)
        p->cb(status, converted, p->arg);

    rte_mempool_put(g_pending_pool, p);
}

/* Grabs one pending-object slot and fills it in — shared setup for
 * both command-dispatch functions below. */
static struct v_db_pending *pending_get(v_db_cb_t cb, void *arg)
{
    struct v_db_pending *p;
    if (rte_mempool_get(g_pending_pool, (void **)&p) != 0) {
        V_LOG(WARNING, "DPDB", "pending pool exhausted");
        return NULL;
    }
    p->cb = cb;
    p->arg = arg;
    return p;
}

/* Port impl: printf-style command dispatch (redisvAsyncCommand). Fine
 * for commands with no binary/variable-arity arguments — see the
 * lifetime/tokenization notes in include/v_port_db.h before using this
 * for anything that isn't a simple fixed-arity command. */
int v_port_db_cmd(uint8_t shard, v_db_cb_t cb, void *arg, const char *fmt, ...)
{
    if (shard >= V_STUB_DB_SHARDS || !g_ctx[shard]) {
        V_LOG(ERR, "DPDB", "shard %u not connected", shard);
        return RET_CODE_ERR;
    }
    struct v_db_pending *p = pending_get(cb, arg);
    if (!p)
        return RET_CODE_ERR;

    va_list ap;
    va_start(ap, fmt);
    int rc = redisvAsyncCommand(g_ctx[shard], on_reply, p, fmt, ap);
    va_end(ap);

    if (rc != REDIS_OK) {
        V_LOG(ERR, "DPDB", "shard %u command dispatch failed", shard);
        rte_mempool_put(g_pending_pool, p);
        return RET_CODE_ERR;
    }
    return RET_CODE_OK;
}

/* Port impl (ADAPTATION, see include/v_port_db.h): binary-safe,
 * explicit-argc/argv/argvlen command dispatch (redisAsyncCommandArgv).
 * Required for any argument that may contain embedded NULs/spaces —
 * v_db_script's EVALSHA and v_retrans_cache's store both use this. */
int v_port_db_cmd_argv(uint8_t shard, v_db_cb_t cb, void *arg,
                        int argc, const char **argv, const size_t *argvlen)
{
    if (shard >= V_STUB_DB_SHARDS || !g_ctx[shard]) {
        V_LOG(ERR, "DPDB", "shard %u not connected", shard);
        return RET_CODE_ERR;
    }
    struct v_db_pending *p = pending_get(cb, arg);
    if (!p)
        return RET_CODE_ERR;

    int rc = redisAsyncCommandArgv(g_ctx[shard], on_reply, p, argc, argv, argvlen);
    if (rc != REDIS_OK) {
        V_LOG(ERR, "DPDB", "shard %u argv command dispatch failed", shard);
        rte_mempool_put(g_pending_pool, p);
        return RET_CODE_ERR;
    }
    return RET_CODE_OK;
}

/* Port impl: FNV-1a over the whole key string. NOT guaranteed
 * <partid>-derived — see v_id_alloc's shard_for_part() workaround and
 * INTEGRATION.md §2 for why callers needing co-location of two
 * different keys within one partition can't just call this directly
 * on the full key. */
uint8_t v_port_db_shard_of(const char *key)
{
    /* FNV-1a, mod shard count. Deterministic and good enough for
     * routing/co-location purposes (plan.md §4 "must be on one shard"). */
    uint32_t h = 2166136261u;
    for (const char *p = key; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 16777619u;
    }
    return (uint8_t)(h % V_STUB_DB_SHARDS);
}

/* Port impl: fixed at 5, matching plan.md's "5 master/slave pairs". */
int v_port_db_shard_count(void)
{
    return V_STUB_DB_SHARDS;
}

/* Port impl: stores the callback — fired by connect_cb() on every
 * (re)connect, including the initial connect. */
int v_port_db_on_reconnect(v_db_reconnect_cb_t cb, void *arg)
{
    g_reconnect_cb = cb;
    g_reconnect_arg = arg;
    return RET_CODE_OK;
}
