#ifndef VPE_V_SESS_STORE_H
#define VPE_V_SESS_STORE_H

#include <stdint.h>
#include "v_port_db.h"

struct pdu_ses_ctx; /* opaque, plan.md §3.0 */

/*
 * v_sess_store — session persistence with version CAS. plan.md §6.1.
 * The session record is a Redis hash: pdu_<partid>:<seid> with fields
 * "data" (the serialized ctx) and "ver". V_SESS_KEY_FMT is fixed —
 * plan.md environment facts: "MUST NOT CHANGE".
 */

#define V_SESS_KEY_FMT      "pdu_%u:%lu"     /* partid, seid — UNCHANGED */
#define V_SESS_FIELD_DATA   "data"
#define V_SESS_FIELD_VER    "ver"
#define V_SESS_PENDING_TTL  30
#define V_CAS_MAX_RETRY     3

typedef enum {
    V_CAS_OK       =  1,
    V_CAS_CONFLICT =  0,
    V_CAS_GONE     = -1,
} v_cas_result_t;

typedef enum { V_SESS_PENDING, V_SESS_CONFIRMED } v_sess_state_t;

/* Callback shapes per plan.md §6.1: the async ISSUE result
 * (RET_CODE_OK/ERR) and the CAS OUTCOME are different things — do not
 * conflate them. v_sess_read_cb_t additionally hands back the version
 * read, needed for the next CAS write. */
typedef void (*v_sess_read_cb_t)(int status, v_cas_result_t found,
                                  struct pdu_ses_ctx *ctx, uint64_t ver, void *arg);
typedef void (*v_sess_cas_cb_t)(int status, v_cas_result_t result, uint64_t new_ver, void *arg);
typedef void (*v_sess_cb_t)(int status, void *arg);

/* ctx is filled in place (caller-owned, from v_port_ctx_alloc()) on a
 * hit; found is V_CAS_GONE if the key doesn't exist. */
int v_sess_read(uint16_t part_id, uint64_t seid, struct pdu_ses_ctx *ctx,
                 v_sess_read_cb_t cb, void *arg);

/* exp_ver == 0 means "must not exist yet" (establishment). state
 * controls the TTL: V_SESS_PENDING sets V_SESS_PENDING_TTL (self-
 * cleaning two-phase commit, plan.md §5.1), V_SESS_CONFIRMED sets none. */
int v_sess_cas_write(uint16_t part_id, uint64_t seid, uint64_t exp_ver,
                      const struct pdu_ses_ctx *ctx, v_sess_state_t state,
                      v_sess_cas_cb_t cb, void *arg);

/* PERSIST — clears the pending TTL once VDP has accepted. */
int v_sess_confirm(uint16_t part_id, uint64_t seid, v_sess_cb_t cb, void *arg);

int v_sess_delete(uint16_t part_id, uint64_t seid, uint64_t exp_ver,
                   v_sess_cas_cb_t cb, void *arg);

/* test-only: internal pending-op pool usage, for leak assertions
 * (plan.md §6.1 "the retry test must assert v_port_ctx_pool_in_use()
 * returns to baseline" — this is the analogous check for v_sess_store's
 * own bookkeeping pool, not the ctx pool itself). */
size_t v_sess_store_test_pending_in_use(void);

#endif /* VPE_V_SESS_STORE_H */
