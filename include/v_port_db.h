#ifndef VPE_PORT_DB_H
#define VPE_PORT_DB_H

#include <stdint.h>
#include <stddef.h>

/*
 * Adapter contract — DB connection handling + command layer (plan.md §3.2).
 *
 * ADAPTATION vs plan.md §3.2 (documented in INTEGRATION.md):
 *  1. v_db_cb_t now carries a parsed v_db_reply_t instead of a flat
 *     (const char *, len) buffer. A flat buffer cannot represent the
 *     array reply v_teid_refill's Lua script returns (plan.md §6.3), so
 *     the richer shape is required for v_db_script to work at all.
 *     Stage 2: adapt in the port body only — wrap the reused layer's
 *     redisReply (or equivalent) into a v_db_reply_t.
 *  2. v_port_db_cmd_argv() is added because EVALSHA must pass a
 *     serialized session blob as one binary-safe argument; blobs may
 *     contain embedded NULs/spaces, which a printf-style v_port_db_cmd()
 *     format string cannot carry safely. Stage 2: confirm the reused
 *     command layer exposes an argv-style call (e.g. a wrapper over
 *     redisAsyncCommandArgv); if not, build the RESP array manually in
 *     the port body.
 */

typedef enum {
    V_DB_REPLY_NIL = 0,
    V_DB_REPLY_STATUS,
    V_DB_REPLY_INTEGER,
    V_DB_REPLY_STRING,
    V_DB_REPLY_ARRAY,
    V_DB_REPLY_ERROR,
} v_db_reply_type_t;

typedef struct v_db_reply {
    v_db_reply_type_t   type;
    long long            integer;
    const char          *str;      /* STATUS / STRING / ERROR, not NUL-safe len below governs */
    size_t               len;
    struct v_db_reply   *element;  /* ARRAY: nelements entries */
    size_t               nelements;
} v_db_reply_t;

/* status: RET_CODE_OK on a completed round trip (even if the server
 * returned an application-level error — check reply->type ==
 * V_DB_REPLY_ERROR for that); RET_CODE_ERR on transport/timeout failure,
 * in which case reply may be NULL. */
typedef void (*v_db_cb_t)(int status, const v_db_reply_t *reply, void *arg);

/* reused command layer, printf-style — fine for commands with no
 * binary/variable-arity arguments (GET, EXPIRE, PERSIST, HGET, ...). */
int v_port_db_cmd(uint8_t shard, v_db_cb_t cb, void *arg,
                   const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

/* binary-safe, variable-arity command — see ADAPTATION note 2 above. */
int v_port_db_cmd_argv(uint8_t shard, v_db_cb_t cb, void *arg,
                        int argc, const char **argv, const size_t *argvlen);

uint8_t v_port_db_shard_of(const char *key);
int v_port_db_shard_count(void);          /* expected: 5 */

/* CONFIRMED PRESENT in the reused DB layer. Fired on (re)connect and on
 * Sentinel promotion. v_db_script binds to it to reload the Lua cache —
 * plan.md §4. Stage 2 wires this to the existing hook; no change to the
 * DB layer needed. */
typedef void (*v_db_reconnect_cb_t)(uint8_t shard, void *arg);
int v_port_db_on_reconnect(v_db_reconnect_cb_t cb, void *arg);

/* Stage 1 only: drive the DB layer's event loop. Production binds this
 * to the worker's libevent loop instead (v_pdu_cacher pattern per
 * CLAUDE.md precedent); exposed here so the standalone test suite and
 * startup-time synchronous-looking sequences (e.g. initial SCRIPT LOAD)
 * can pump completions without a second thread. */
int v_port_db_init(void);
void v_port_db_fini(void);
void v_port_db_poll(int nonblock);

#endif /* VPE_PORT_DB_H */
