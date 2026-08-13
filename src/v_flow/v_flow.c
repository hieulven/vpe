#include "v_flow.h"
#include "v_port_pfcp.h"
#include "v_port_mem.h"
#include "v_port_txn.h"
#include "v_port_vdp.h"
#include "v_seid.h"
#include "v_id_alloc.h"
#include "v_sess_store.h"
#include "v_retrans_cache.h"
#include "v_common.h"
#include "v_log.h"

#include <unistd.h>
#include <string.h>

/* Round-robin part_id for sessions with no UE IP — plan.md §6.4. Seeded
 * once at startup so different simulated pods (different processes in
 * a real deployment) don't all start at the same partition. */
#define V_PART_RR_SPAN 16
static uint16_t g_rr_base;
static uint16_t g_rr_ctr;

static uint16_t part_rr(void)
{
    return (g_rr_base + (g_rr_ctr++ & (V_PART_RR_SPAN - 1))) & (V_NUM_PARTS - 1);
}

static uint16_t hash_ue_ip(uint32_t v4, const uint8_t v6[16])
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 4; i++) {
        h ^= (uint8_t)(v4 >> (i * 8));
        h *= 16777619u;
    }
    for (int i = 0; i < 16; i++) {
        h ^= v6[i];
        h *= 16777619u;
    }
    return (uint16_t)(h % V_NUM_PARTS);
}

int v_flow_init(void)
{
    g_rr_base = (uint16_t)(((uintptr_t)&g_rr_base) ^ (uintptr_t)getpid()) & (V_NUM_PARTS - 1);
    g_rr_ctr = 0;
    return RET_CODE_OK;
}

/* --- establishment (plan.md §5.1) --- */

static void delete_pending_noop_cb(int status, v_cas_result_t result, uint64_t new_ver, void *arg)
{
    (void)new_ver; (void)arg;
    if (status != RET_CODE_OK)
        V_LOG(WARNING, "PFCP", "establishment rollback: pending-record delete dispatch failed");
    else if (result != V_CAS_OK)
        V_LOG(DEBUG, "PFCP", "establishment rollback: pending record already gone/changed (result=%d)", result);
}

/* Builds the response (real reply on ACCEPTED, cause-coded failure
 * otherwise), stores it in the retransmit cache so a same-seq
 * retransmit gets the identical answer instead of redoing the whole
 * flow, sends it, and releases everything the txn is holding. Every
 * terminal path of the establishment flow funnels through here. */
static void finish(struct v_txn *t, uint8_t cause)
{
    uint8_t buf[512];
    size_t len = sizeof(buf);
    const struct pdu_ses_ctx *ctx_for_rsp = (cause == V_PFCP_CAUSE_REQUEST_ACCEPTED) ? t->ctx : NULL;

    if (v_port_pfcp_encode_rsp(t->req, ctx_for_rsp, cause, buf, &len) != RET_CODE_OK) {
        V_LOG(ERR, "PFCP", "establishment: encode_rsp failed seid=%lu cause=%u", t->seid, cause);
    } else {
        uint64_t smf_fseid = v_port_pfcp_smf_fseid(t->req);
        uint32_t seq = v_port_pfcp_seq(t->req);
        uint16_t rpart = v_retrans_part(smf_fseid);
        v_retrans_store(rpart, smf_fseid, seq, buf, len);

        if (v_dispatch_tx_enqueue(buf, len, &t->peer) != RET_CODE_OK)
            V_LOG(ERR, "PFCP", "establishment: tx enqueue failed seid=%lu", t->seid);
    }

    v_port_txn_set_state(t, TXN_ST_REPLIED);
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

static void confirm_cb(int status, void *arg)
{
    struct v_txn *t = (struct v_txn *)arg;
    if (status != RET_CODE_OK) {
        /* The session data itself is already durable and VDP already
         * accepted it — only the TTL-clearing PERSIST failed. Do NOT
         * delete here (that would create exactly the "successful VDP
         * push, failed Redis cleanup" hazard plan.md §5.1 warns
         * against in reverse). Fail this SMF reply and rely on the
         * pending TTL / a retransmit to recover. */
        V_LOG(ERR, "DPDB", "establishment: confirm(PERSIST) failed seid=%lu", t->seid);
        finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
        return;
    }
    finish(t, V_PFCP_CAUSE_REQUEST_ACCEPTED);
}

static void vdp_create_cb(v_vdp_result_t res, void *arg)
{
    struct v_txn *t = (struct v_txn *)arg;

    if (res != V_VDP_ACCEPT) {
        V_LOG(WARNING, "PFCP", "establishment: vdp %s seid=%lu",
              res == V_VDP_REJECT ? "reject" : "timeout", t->seid);
        v_sess_delete(t->part_id, t->seid, t->db_ver, delete_pending_noop_cb, NULL);
        v_teid_free(t->part_id, t->teid); /* SEID is never reclaimed — plan.md §6.3 */
        finish(t, V_PFCP_CAUSE_REQUEST_REJECTED);
        return;
    }

    v_port_txn_set_state(t, TXN_ST_DB_CONFIRM_WAIT);
    if (v_sess_confirm(t->part_id, t->seid, confirm_cb, t) != RET_CODE_OK) {
        V_LOG(ERR, "DPDB", "establishment: confirm dispatch failed seid=%lu", t->seid);
        finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
    }
}

static void cas_write_cb(int status, v_cas_result_t result, uint64_t new_ver, void *arg)
{
    struct v_txn *t = (struct v_txn *)arg;

    if (status != RET_CODE_OK || result != V_CAS_OK) {
        /* exp_ver=0 on a freshly allocated SEID should never conflict
         * or find the key gone — a fresh SEID is never reused (plan.md
         * §6.3), so this branch means a transport error or a genuine
         * bug elsewhere. Either way, fail safe. */
        V_LOG(ERR, "DPDB", "establishment: cas_write failed seid=%lu status=%d result=%d",
              t->seid, status, result);
        v_teid_free(t->part_id, t->teid);
        finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
        return;
    }

    t->db_ver = new_ver; /* == 1 */
    v_port_txn_set_state(t, TXN_ST_VDP_WAIT);
    if (v_port_vdp_session_create(t->part_id, t->ctx, vdp_create_cb, t) != RET_CODE_OK) {
        V_LOG(ERR, "PFCP", "establishment: vdp session_create dispatch failed seid=%lu", t->seid);
        v_sess_delete(t->part_id, t->seid, t->db_ver, delete_pending_noop_cb, NULL);
        v_teid_free(t->part_id, t->teid);
        finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
    }
}

static void retrans_lookup_cb(int status, int hit, const uint8_t *resp, size_t len, void *arg)
{
    struct v_txn *t = (struct v_txn *)arg;

    if (status == RET_CODE_OK && hit) {
        V_LOG(DEBUG, "PFCP", "establishment: retransmit dedup hit seq=%u", v_port_pfcp_seq(t->req));
        v_dispatch_tx_enqueue(resp, len, &t->peer);
        v_port_pfcp_msg_free(t->req);
        t->req = NULL;
        v_port_txn_destroy(t);
        return;
    }
    /* status != OK (cache unreachable) fails OPEN — proceed as a miss.
     * Losing dedup for one request only risks a duplicate SEID/TEID on
     * a subsequent cross-pod retransmit, which is strictly better than
     * refusing to serve the request at all (plan.md §6.6 frames the
     * cache as load-bearing for dedup, not as a gate on availability). */

    uint32_t v4;
    uint8_t v6[16];
    uint16_t part = (v_port_pfcp_ue_ip(t->req, &v4, v6) == RET_CODE_OK)
                         ? hash_ue_ip(v4, v6)
                         : part_rr();
    t->part_id = part;

    uint64_t seid = v_seid_alloc(part);
    uint32_t teid = v_teid_alloc(part);
    if (seid == 0 || teid == 0) {
        V_LOG(WARNING, "PFCP", "establishment: id alloc exhausted part=%u seq=%u",
              part, v_port_pfcp_seq(t->req));
        if (teid != 0)
            v_teid_free(part, teid);
        finish(t, V_PFCP_CAUSE_NO_RESOURCES_AVAILABLE);
        return;
    }
    t->seid = seid;
    t->teid = teid;

    struct pdu_ses_ctx *ctx = v_port_ctx_alloc();
    if (!ctx) {
        V_LOG(ERR, "MEM", "establishment: ctx pool exhausted seid=%lu", seid);
        v_teid_free(part, teid);
        finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
        return;
    }
    t->ctx = ctx;

    if (v_port_pfcp_build_session(t->req, seid, teid, ctx) != RET_CODE_OK) {
        V_LOG(ERR, "PFCP", "establishment: build_session failed seid=%lu", seid);
        v_teid_free(part, teid);
        finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
        return;
    }

    v_port_txn_set_state(t, TXN_ST_DB_WRITE_WAIT);
    if (v_sess_cas_write(part, seid, 0, ctx, V_SESS_PENDING, cas_write_cb, t) != RET_CODE_OK) {
        V_LOG(ERR, "DPDB", "establishment: cas_write dispatch failed seid=%lu", seid);
        v_teid_free(part, teid);
        finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
    }
}

static void handle_establishment(struct pfcp_msg *req, const struct sockaddr *peer)
{
    uint64_t smf_fseid = v_port_pfcp_smf_fseid(req);
    uint32_t seq = v_port_pfcp_seq(req);
    uint16_t rpart = v_retrans_part(smf_fseid);

    struct v_txn *t = v_port_txn_create();
    if (!t) {
        V_LOG(ERR, "PFCP", "establishment: txn pool exhausted, dropping seq=%u", seq);
        v_port_pfcp_msg_free(req);
        return;
    }
    t->req = req;
    if (peer)
        t->peer = *peer;
    v_port_txn_set_state(t, TXN_ST_RECV);

    if (v_retrans_lookup(rpart, smf_fseid, seq, retrans_lookup_cb, t) != RET_CODE_OK) {
        V_LOG(ERR, "DPDB", "establishment: retrans lookup dispatch failed seq=%u", seq);
        v_port_pfcp_msg_free(t->req);
        t->req = NULL;
        v_port_txn_destroy(t);
    }
}

/* --- modification (plan.md §5.2) --- */

static void mod_start_read(struct v_txn *t);
static void del_start_read(struct v_txn *t);

static void mod_compensate_write_cb(int status, v_cas_result_t result, uint64_t new_ver, void *arg)
{
    struct v_txn *t = (struct v_txn *)arg;
    (void)new_ver;
    if (status != RET_CODE_OK || result != V_CAS_OK)
        V_LOG(ERR, "DPDB", "modification: compensating write-back failed seid=%lu status=%d result=%d",
              t->seid, status, result);
    finish(t, V_PFCP_CAUSE_REQUEST_REJECTED);
}

static void mod_vdp_cb(v_vdp_result_t res, void *arg)
{
    struct v_txn *t = (struct v_txn *)arg;
    struct pdu_ses_ctx *prev = (struct pdu_ses_ctx *)t->impl;
    t->impl = NULL;

    if (res != V_VDP_ACCEPT) {
        V_LOG(WARNING, "PFCP", "modification: vdp %s seid=%lu, compensating write-back",
              res == V_VDP_REJECT ? "reject" : "timeout", t->seid);
        /* v_sess_cas_write() serializes prev synchronously before it
         * returns, so it's safe to free prev right after — no need to
         * wait for the async reply. */
        int rc = v_sess_cas_write(t->part_id, t->seid, t->db_ver, prev, V_SESS_CONFIRMED,
                                   mod_compensate_write_cb, t);
        v_port_ctx_free(prev);
        if (rc != RET_CODE_OK) {
            V_LOG(ERR, "DPDB", "modification: compensating write dispatch failed seid=%lu", t->seid);
            finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
        }
        return;
    }

    if (prev)
        v_port_ctx_free(prev);
    finish(t, V_PFCP_CAUSE_REQUEST_ACCEPTED);
}

static void mod_cas_cb(int status, v_cas_result_t result, uint64_t new_ver, void *arg)
{
    struct v_txn *t = (struct v_txn *)arg;

    if (status != RET_CODE_OK) {
        V_LOG(ERR, "DPDB", "modification: cas_write transport failure seid=%lu", t->seid);
        if (t->impl) { v_port_ctx_free((struct pdu_ses_ctx *)t->impl); t->impl = NULL; }
        finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
        return;
    }

    switch (result) {
    case V_CAS_OK:
        t->db_ver = new_ver;
        v_port_txn_set_state(t, TXN_ST_VDP_WAIT);
        if (v_port_vdp_session_modify(t->part_id, t->ctx, mod_vdp_cb, t) != RET_CODE_OK) {
            V_LOG(ERR, "PFCP", "modification: vdp session_modify dispatch failed seid=%lu", t->seid);
            if (t->impl) { v_port_ctx_free((struct pdu_ses_ctx *)t->impl); t->impl = NULL; }
            finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
        }
        break;

    case V_CAS_CONFLICT:
        if (t->impl) { v_port_ctx_free((struct pdu_ses_ctx *)t->impl); t->impl = NULL; }
        t->cas_retry++;
        if (t->cas_retry >= V_CAS_MAX_RETRY) {
            V_LOG(WARNING, "PFCP", "modification: CAS retry exhausted seid=%lu", t->seid);
            finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
            return;
        }
        v_port_ctx_clear(t->ctx);
        mod_start_read(t); /* plan.md §5.2: "goto 5" */
        break;

    case V_CAS_GONE:
        if (t->impl) { v_port_ctx_free((struct pdu_ses_ctx *)t->impl); t->impl = NULL; }
        /* Modification racing Deletion: deletion won — correct per
         * plan.md §5.2 "Race outcomes". */
        finish(t, V_PFCP_CAUSE_SESSION_CTX_NOT_FOUND);
        break;
    }
}

static void mod_read_cb(int status, v_cas_result_t found, struct pdu_ses_ctx *ctx, uint64_t ver, void *arg)
{
    struct v_txn *t = (struct v_txn *)arg;

    if (status != RET_CODE_OK) {
        V_LOG(ERR, "DPDB", "modification: read failed seid=%lu", t->seid);
        finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
        return;
    }
    if (found != V_CAS_OK) {
        finish(t, V_PFCP_CAUSE_SESSION_CTX_NOT_FOUND);
        return;
    }
    t->db_ver = ver;

    /* Snapshot the pre-modification state for a possible compensating
     * write-back on VDP reject (plan.md §5.2 step 8). struct v_txn has
     * no dedicated field for this: t->impl ("reused table's own
     * handle" per v_port_txn.h) is repurposed here as scratch storage,
     * Stage 1 only. Stage 2 needs a real slot once impl is claimed by
     * the real table — extend it, or key a small side-table by seid. */
    struct pdu_ses_ctx *prev = v_port_ctx_alloc();
    if (!prev) {
        V_LOG(ERR, "MEM", "modification: ctx pool exhausted (snapshot) seid=%lu", t->seid);
        finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
        return;
    }
    uint8_t blob[2048];
    size_t blob_len = sizeof(blob);
    if (v_port_pdu_serialize(ctx, blob, &blob_len) != RET_CODE_OK ||
        v_port_pdu_deserialize(blob, blob_len, prev) != RET_CODE_OK) {
        V_LOG(ERR, "PFCP", "modification: ctx snapshot failed seid=%lu", t->seid);
        v_port_ctx_free(prev);
        finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
        return;
    }
    t->impl = prev;

    if (v_port_pfcp_modify_session(t->req, ctx) != RET_CODE_OK) {
        V_LOG(ERR, "PFCP", "modification: modify_session failed seid=%lu", t->seid);
        v_port_ctx_free(prev);
        t->impl = NULL;
        finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
        return;
    }

    v_port_txn_set_state(t, TXN_ST_DB_WRITE_WAIT);
    if (v_sess_cas_write(t->part_id, t->seid, t->db_ver, ctx, V_SESS_CONFIRMED, mod_cas_cb, t) != RET_CODE_OK) {
        V_LOG(ERR, "DPDB", "modification: cas_write dispatch failed seid=%lu", t->seid);
        v_port_ctx_free(prev);
        t->impl = NULL;
        finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
    }
}

static void mod_start_read(struct v_txn *t)
{
    v_port_txn_set_state(t, TXN_ST_DB_READ_WAIT);
    if (v_sess_read(t->part_id, t->seid, t->ctx, mod_read_cb, t) != RET_CODE_OK) {
        V_LOG(ERR, "DPDB", "modification: read dispatch failed seid=%lu", t->seid);
        finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
    }
}

static void mod_retrans_lookup_cb(int status, int hit, const uint8_t *resp, size_t len, void *arg)
{
    struct v_txn *t = (struct v_txn *)arg;

    if (status == RET_CODE_OK && hit) {
        v_dispatch_tx_enqueue(resp, len, &t->peer);
        v_port_pfcp_msg_free(t->req);
        t->req = NULL;
        v_port_txn_destroy(t);
        return;
    }

    t->ctx = v_port_ctx_alloc();
    if (!t->ctx) {
        V_LOG(ERR, "MEM", "modification: ctx pool exhausted seid=%lu", t->seid);
        finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
        return;
    }
    mod_start_read(t);
}

static void reply_ctx_not_found(struct pfcp_msg *req, const struct sockaddr *peer)
{
    uint8_t buf[512];
    size_t len = sizeof(buf);
    if (v_port_pfcp_encode_rsp(req, NULL, V_PFCP_CAUSE_SESSION_CTX_NOT_FOUND, buf, &len) == RET_CODE_OK)
        v_dispatch_tx_enqueue(buf, len, peer);
    else
        V_LOG(ERR, "PFCP", "encode_rsp (ctx-not-found) failed");
    v_port_pfcp_msg_free(req);
}

static void handle_modification(struct pfcp_msg *req, const struct sockaddr *peer)
{
    uint64_t seid = v_port_pfcp_hdr_seid(req);
    uint16_t part;

    /* free5gc #730/#731-class defense — plan.md §7: validate every
     * session-referencing SEID, not just the shared dispatcher. */
    if (v_seid_validate(seid, &part) != RET_CODE_OK) {
        reply_ctx_not_found(req, peer);
        return;
    }

    uint64_t smf_fseid = v_port_pfcp_smf_fseid(req);
    uint32_t seq = v_port_pfcp_seq(req);
    uint16_t rpart = v_retrans_part(smf_fseid);

    struct v_txn *t = v_port_txn_create();
    if (!t) {
        V_LOG(ERR, "PFCP", "modification: txn pool exhausted, dropping seq=%u", seq);
        v_port_pfcp_msg_free(req);
        return;
    }
    t->req = req;
    if (peer)
        t->peer = *peer;
    t->part_id = part;
    t->seid = seid;
    v_port_txn_set_state(t, TXN_ST_RECV);

    if (v_retrans_lookup(rpart, smf_fseid, seq, mod_retrans_lookup_cb, t) != RET_CODE_OK) {
        V_LOG(ERR, "DPDB", "modification: retrans lookup dispatch failed seq=%u", seq);
        v_port_pfcp_msg_free(t->req);
        t->req = NULL;
        v_port_txn_destroy(t);
    }
}

/* --- deletion (plan.md §5.3) --- */

static void del_delete_cb(int status, v_cas_result_t result, uint64_t new_ver, void *arg)
{
    struct v_txn *t = (struct v_txn *)arg;
    (void)new_ver;

    if (status != RET_CODE_OK) {
        V_LOG(ERR, "DPDB", "deletion: sess_delete transport failure seid=%lu", t->seid);
        finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
        return;
    }

    switch (result) {
    case V_CAS_OK: {
        uint32_t teid = v_port_pfcp_ctx_teid(t->ctx);
        v_teid_free(t->part_id, teid); /* SEID never reclaimed — plan.md §6.3 */
        finish(t, V_PFCP_CAUSE_REQUEST_ACCEPTED);
        break;
    }
    case V_CAS_CONFLICT:
        t->cas_retry++;
        if (t->cas_retry >= V_CAS_MAX_RETRY) {
            V_LOG(WARNING, "PFCP", "deletion: CAS retry exhausted seid=%lu", t->seid);
            finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
            return;
        }
        v_port_ctx_clear(t->ctx);
        del_start_read(t);
        break;
    case V_CAS_GONE:
        finish(t, V_PFCP_CAUSE_SESSION_CTX_NOT_FOUND);
        break;
    }
}

static void del_vdp_cb(v_vdp_result_t res, void *arg)
{
    struct v_txn *t = (struct v_txn *)arg;

    if (res != V_VDP_ACCEPT) {
        /* Nothing in Redis has been touched yet at this point — no
         * compensation needed, unlike modification. */
        V_LOG(WARNING, "PFCP", "deletion: vdp %s seid=%lu",
              res == V_VDP_REJECT ? "reject" : "timeout", t->seid);
        finish(t, V_PFCP_CAUSE_REQUEST_REJECTED);
        return;
    }

    v_port_txn_set_state(t, TXN_ST_DB_WRITE_WAIT);
    if (v_sess_delete(t->part_id, t->seid, t->db_ver, del_delete_cb, t) != RET_CODE_OK) {
        V_LOG(ERR, "DPDB", "deletion: sess_delete dispatch failed seid=%lu", t->seid);
        finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
    }
}

static void del_read_cb(int status, v_cas_result_t found, struct pdu_ses_ctx *ctx, uint64_t ver, void *arg)
{
    struct v_txn *t = (struct v_txn *)arg;
    (void)ctx;

    if (status != RET_CODE_OK) {
        V_LOG(ERR, "DPDB", "deletion: read failed seid=%lu", t->seid);
        finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
        return;
    }
    if (found != V_CAS_OK) {
        finish(t, V_PFCP_CAUSE_SESSION_CTX_NOT_FOUND);
        return;
    }
    t->db_ver = ver;

    v_port_txn_set_state(t, TXN_ST_VDP_WAIT);
    if (v_port_vdp_session_delete(t->part_id, t->seid, del_vdp_cb, t) != RET_CODE_OK) {
        V_LOG(ERR, "PFCP", "deletion: vdp session_delete dispatch failed seid=%lu", t->seid);
        finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
    }
}

static void del_start_read(struct v_txn *t)
{
    v_port_txn_set_state(t, TXN_ST_DB_READ_WAIT);
    if (v_sess_read(t->part_id, t->seid, t->ctx, del_read_cb, t) != RET_CODE_OK) {
        V_LOG(ERR, "DPDB", "deletion: read dispatch failed seid=%lu", t->seid);
        finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
    }
}

static void del_retrans_lookup_cb(int status, int hit, const uint8_t *resp, size_t len, void *arg)
{
    struct v_txn *t = (struct v_txn *)arg;

    if (status == RET_CODE_OK && hit) {
        v_dispatch_tx_enqueue(resp, len, &t->peer);
        v_port_pfcp_msg_free(t->req);
        t->req = NULL;
        v_port_txn_destroy(t);
        return;
    }

    t->ctx = v_port_ctx_alloc();
    if (!t->ctx) {
        V_LOG(ERR, "MEM", "deletion: ctx pool exhausted seid=%lu", t->seid);
        finish(t, V_PFCP_CAUSE_SYSTEM_FAILURE);
        return;
    }
    del_start_read(t);
}

static void handle_deletion(struct pfcp_msg *req, const struct sockaddr *peer)
{
    uint64_t seid = v_port_pfcp_hdr_seid(req);
    uint16_t part;

    if (v_seid_validate(seid, &part) != RET_CODE_OK) {
        reply_ctx_not_found(req, peer);
        return;
    }

    uint64_t smf_fseid = v_port_pfcp_smf_fseid(req);
    uint32_t seq = v_port_pfcp_seq(req);
    uint16_t rpart = v_retrans_part(smf_fseid);

    struct v_txn *t = v_port_txn_create();
    if (!t) {
        V_LOG(ERR, "PFCP", "deletion: txn pool exhausted, dropping seq=%u", seq);
        v_port_pfcp_msg_free(req);
        return;
    }
    t->req = req;
    if (peer)
        t->peer = *peer;
    t->part_id = part;
    t->seid = seid;
    v_port_txn_set_state(t, TXN_ST_RECV);

    if (v_retrans_lookup(rpart, smf_fseid, seq, del_retrans_lookup_cb, t) != RET_CODE_OK) {
        V_LOG(ERR, "DPDB", "deletion: retrans lookup dispatch failed seq=%u", seq);
        v_port_pfcp_msg_free(t->req);
        t->req = NULL;
        v_port_txn_destroy(t);
    }
}

void v_flow_handle_msg(const v_dispatch_msg_t *msg, void *arg)
{
    (void)arg;

    struct pfcp_msg *req = NULL;
    if (v_port_pfcp_decode(msg->buf, msg->len, &req) != RET_CODE_OK) {
        V_LOG(WARNING, "PFCP", "v_flow: decode failed, dropping datagram (%zu bytes)", msg->len);
        return;
    }

    uint8_t type = v_port_pfcp_msg_type(req);
    switch (type) {
    case V_PFCP_MSG_SESSION_EST_REQ:
        handle_establishment(req, &msg->peer);
        break;
    case V_PFCP_MSG_SESSION_MOD_REQ:
        handle_modification(req, &msg->peer);
        break;
    case V_PFCP_MSG_SESSION_DEL_REQ:
        handle_deletion(req, &msg->peer);
        break;
    default:
        /* Node-level messages (heartbeat, association) land in S11
         * (v_node_state) — forward declaration point per the project's
         * iterative-build convention. */
        V_LOG(WARNING, "PFCP", "v_flow: msg type %u not yet implemented, dropping", type);
        v_port_pfcp_msg_free(req);
        break;
    }
}
