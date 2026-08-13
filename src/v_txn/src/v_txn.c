#include "v_txn.h"
#include "v_port_txn.h"
#include "v_port_mem.h"
#include "v_port_pfcp.h"
#include "v_sess_store.h"
#include "v_common.h"
#include "v_log.h"

/* Ack-only callback for the best-effort PERSIST retry issued from the
 * DB_CONFIRM_WAIT timeout branch below. */
static void confirm_retry_cb(int status, void *arg)
{
    (void)arg;
    if (status != RET_CODE_OK)
        V_LOG(WARNING, "PFCP", "timeout sweeper: PERSIST retry dispatch failed");
}

/* Ack-only callback for the pending-record cleanup issued from the
 * VDP_WAIT/establishment timeout branch below. */
static void delete_pending_cb(int status, v_cas_result_t result, uint64_t new_ver, void *arg)
{
    (void)new_ver; (void)arg;
    if (status != RET_CODE_OK)
        V_LOG(WARNING, "PFCP", "timeout sweeper: pending-record delete dispatch failed");
    else if (result != V_CAS_OK)
        V_LOG(DEBUG, "PFCP", "timeout sweeper: pending record already gone/changed (result=%d)", result);
}

/* The v_txn_timeout_cb_t registered with v_port_txn_on_timeout(). This
 * is the entire module: decide per-state what "timed out" means, take
 * whatever recovery action that state allows, then always free t->ctx
 * and t->req and destroy the txn — see inc/v_txn.h and CLAUDE.md for
 * the full per-state rationale. */
static void timeout_handler(struct v_txn *t, void *arg)
{
    (void)arg;

    switch (t->state) {
    case TXN_ST_RECV:
    case TXN_ST_DB_READ_WAIT:
        /* No write was ever issued — nothing to compensate. */
        V_LOG(WARNING, "PFCP", "txn hard timeout state=%d seid=%lu (no db action needed)",
              t->state, t->seid);
        break;

    case TXN_ST_DB_WRITE_WAIT:
        /* The CAS write was issued but never confirmed. Establishment:
         * if it landed anyway, the PENDING TTL (plan.md §5.1) self-
         * cleans it. Modification/deletion: no state was reported back
         * as applied, so the SMF sees a failure and may retransmit
         * against fresh state — safe either way without an explicit
         * compensating action here. */
        V_LOG(WARNING, "PFCP", "txn hard timeout in DB_WRITE_WAIT seid=%lu (relying on pending TTL / retransmit)",
              t->seid);
        break;

    case TXN_ST_VDP_WAIT:
        if (t->db_ver == 0) {
            /* Establishment: the CAS write that got us here always
             * created the record at ver=1. VDP never confirmed, so
             * delete it now instead of waiting out the full pending
             * TTL. */
            V_LOG(WARNING, "PFCP",
                  "txn hard timeout in VDP_WAIT (establishment) seid=%lu — deleting pending record",
                  t->seid);
            v_sess_delete(t->part_id, t->seid, 1, delete_pending_cb, NULL);
        } else {
            /* Modification/deletion: the record was already CAS-written
             * to its new value before the VDP call. See the KNOWN GAP
             * note in v_txn.h — compensating this requires the prior
             * ctx, which this generic sweeper does not have. */
            V_LOG(CRIT, "PFCP",
                  "txn hard timeout in VDP_WAIT (modification/deletion) seid=%lu ver=%lu — "
                  "cannot compensate write-back here (see v_txn.h KNOWN GAP)",
                  t->seid, t->db_ver);
        }
        break;

    case TXN_ST_DB_CONFIRM_WAIT:
        /* VDP already accepted and the session data is already durable
         * (written before the VDP call); PERSIST only clears the
         * pending TTL. Retry it once, best-effort, rather than risk the
         * TTL expiring under a live session. */
        V_LOG(WARNING, "PFCP", "txn hard timeout in DB_CONFIRM_WAIT seid=%lu — retrying PERSIST once",
              t->seid);
        v_sess_confirm(t->part_id, t->seid, confirm_retry_cb, NULL);
        break;

    case TXN_ST_REPLIED:
        /* Should never happen — the timer should have been implicitly
         * retired the moment the txn was destroyed after replying. */
        V_LOG(CRIT, "PFCP", "txn hard timeout fired in REPLIED state seid=%lu (timer not retired?)",
              t->seid);
        break;
    }

    if (t->ctx) {
        v_port_ctx_free(t->ctx);
        t->ctx = NULL;
    }
    if (t->req) {
        v_port_pfcp_msg_free(t->req);
        t->req = NULL;
    }
    v_port_txn_destroy(t);
}

/* Public: registers timeout_handler as the port's single hard-timeout
 * callback. Call once at startup, after v_port_txn_init(). */
int v_txn_sm_init(void)
{
    return v_port_txn_on_timeout(timeout_handler, NULL);
}

/* Public: no-op — see the comment below. */
void v_txn_sm_fini(void)
{
    /* Nothing to release: v_port_txn owns the callback registration and
     * is torn down independently via v_port_txn_fini(). */
}
