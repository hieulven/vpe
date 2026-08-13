#ifndef VPE_V_DB_SCRIPT_H
#define VPE_V_DB_SCRIPT_H

#include "v_port_db.h"

/*
 * v_db_script — EVAL/EVALSHA layered on the reused DB command layer,
 * because that layer lacks Lua support (plan.md §4). First module
 * built: v_sess_store and v_id_alloc both depend on it.
 */

typedef enum {
    V_SCRIPT_SESS_CAS_WRITE = 0,
    V_SCRIPT_SESS_DELETE,
    V_SCRIPT_TEID_REFILL,
    V_SCRIPT_COUNT,
} v_script_id_t;

/* Registers the SCRIPT LOAD-on-(re)connect hook (plan.md §4: "SCRIPT
 * LOAD to all 5 masters at startup, and again on every reconnect via
 * v_port_db_on_reconnect"). Call BEFORE v_port_db_init(), so the
 * initial connect events are caught by the same hook. */
int v_db_script_init(void);

/* True once every shard has a SHA cached for every script — i.e. the
 * initial SCRIPT LOAD wave has completed on all shards. Callers that
 * must not issue EVALSHA before this (see v_id_alloc_ready() analog,
 * plan.md §6.2 "do not accept PFCP traffic until ...") poll this. */
int v_db_script_ready(void);

/* EVALSHA with automatic NOSCRIPT recovery: on NOSCRIPT, SCRIPT LOAD to
 * the failing shard and retry the EVALSHA exactly once, then fail
 * (plan.md §4). Returns RET_CODE_OK if the request was ISSUED;
 * RET_CODE_ERR if it could not even be queued (pool exhaustion, bad
 * args, shard down). The eventual outcome — including a permanent
 * NOSCRIPT failure after the one retry — arrives in cb as an ordinary
 * v_db_reply_t (V_DB_REPLY_ERROR on failure). Do not conflate the two,
 * same rule as v_sess_cas_write (plan.md §6.1).
 *
 * ADAPTATION vs plan.md §4: argv here is paired with an explicit
 * argvlen[] array. plan.md's signature has no lengths, but
 * v_sess_cas_write's ARGV[2] carries the serialized session blob, which
 * may contain embedded NULs — a strlen()-based copy would silently
 * truncate it. keys[] stays plain NUL-terminated text (session/id-alloc
 * keys are always printable). */
int v_db_evalsha(uint8_t shard, v_script_id_t script_id,
                  int nkeys, const char **keys,
                  const char **argv, const size_t *argvlen, int nargv,
                  v_db_cb_t cb, void *arg);

#endif /* VPE_V_DB_SCRIPT_H */
