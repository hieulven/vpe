#include "v_port_pfcp.h"
#include "v_stub_pfcp_test.h"
#include "v_common.h"
#include "v_log.h"
#include "v_stub_common.h"

#include <rte_mempool.h>
#include <rte_errno.h>
#include <string.h>

#define V_STUB_PFCP_MSG_POOL_CAP 4096
#define V_STUB_TX_BUF_CAP 512

/* The stub's own decoded-message representation — bears no relation to
 * a real PFCP message, just enough fields for every port accessor
 * below and for src/v_flow.c to exercise every branch it needs to. */
struct pfcp_msg {
    uint32_t magic;
    uint8_t  type;
    uint8_t  has_ue_ip;
    uint32_t seq;
    uint64_t seid;
    uint64_t smf_fseid;
    uint32_t v4_ue_ip;
    uint8_t  v6_ue_ip[16];
};

#define V_STUB_MSG_MAGIC 0x4d534731u /* "MSG1" */

static struct rte_mempool *g_msg_pool;
static v_pfcp_rx_cb_t g_rx_cb;
static void *g_rx_arg;

static uint8_t g_last_tx[V_STUB_TX_BUF_CAP];
static size_t g_last_tx_len;

/* Lazily creates the pfcp_msg object pool on first use. */
static int ensure_msg_pool(void)
{
    if (g_msg_pool)
        return RET_CODE_OK;
    g_msg_pool = rte_mempool_create("v_pfcp_msg_pool", V_STUB_PFCP_MSG_POOL_CAP,
                                     sizeof(struct pfcp_msg), 0, 0,
                                     NULL, NULL, NULL, NULL, SOCKET_ID_ANY, 0);
    if (!g_msg_pool) {
        V_LOG(ERR, "PFCP", "pfcp_msg pool create failed: %s",
              rte_strerror(rte_errno));
        return RET_CODE_ERR;
    }
    return RET_CODE_OK;
}

/* Port impl: stores the rx callback for v_stub_pfcp_io_inject_rx() to
 * invoke later — no real socket exists in this stub. */
int v_port_pfcp_io_init(v_pfcp_rx_cb_t cb, void *arg)
{
    if (ensure_msg_pool() != RET_CODE_OK)
        return RET_CODE_ERR;
    g_rx_cb = cb;
    g_rx_arg = arg;
    V_LOG(INFO, "PFCP", "stub pfcp io initialized");
    return RET_CODE_OK;
}

/* Port impl: captures the bytes into g_last_tx instead of sending
 * anywhere — v_stub_pfcp_io_last_tx()/_reset() are how tests observe
 * what would have been sent. */
int v_port_pfcp_io_send(const struct sockaddr *peer, const uint8_t *buf, size_t len)
{
    (void)peer;
    if (len > sizeof(g_last_tx)) {
        V_LOG(ERR, "PFCP", "stub tx buffer too small (%zu > %zu)", len, sizeof(g_last_tx));
        return RET_CODE_ERR;
    }
    memcpy(g_last_tx, buf, len);
    g_last_tx_len = len;
    return RET_CODE_OK;
}

/* Port impl: parses the stub's synthetic struct v_stub_wire_req (see
 * v_stub_common.h) into a pool-allocated pfcp_msg. */
int v_port_pfcp_decode(const uint8_t *buf, size_t len, struct pfcp_msg **out)
{
    if (len < sizeof(struct v_stub_wire_req)) {
        V_LOG(WARNING, "PFCP", "decode: short buffer (%zu)", len);
        return RET_CODE_ERR;
    }
    if (ensure_msg_pool() != RET_CODE_OK)
        return RET_CODE_ERR;

    struct v_stub_wire_req wire;
    memcpy(&wire, buf, sizeof(wire));

    struct pfcp_msg *m;
    if (rte_mempool_get(g_msg_pool, (void **)&m) != 0) {
        V_LOG(WARNING, "PFCP", "pfcp_msg pool exhausted");
        return RET_CODE_ERR;
    }
    m->magic = V_STUB_MSG_MAGIC;
    m->type = wire.type;
    m->has_ue_ip = wire.has_ue_ip;
    m->seq = wire.seq;
    m->seid = wire.seid;
    m->smf_fseid = wire.smf_fseid;
    m->v4_ue_ip = wire.v4_ue_ip;
    memcpy(m->v6_ue_ip, wire.v6_ue_ip, sizeof(m->v6_ue_ip));

    *out = m;
    return RET_CODE_OK;
}

/* Port impl: magic-checked free (catches a double-free or a foreign
 * pointer) then returns the block to the pool. */
void v_port_pfcp_msg_free(struct pfcp_msg *msg)
{
    if (!msg)
        return;
    if (msg->magic != V_STUB_MSG_MAGIC) {
        V_LOG(CRIT, "PFCP", "double-free or foreign pfcp_msg %p", (void *)msg);
        return;
    }
    memset(msg, 0, sizeof(*msg));
    rte_mempool_put(g_msg_pool, msg);
}

/* Port impls: trivial field accessors on the decoded message. */
uint8_t v_port_pfcp_msg_type(const struct pfcp_msg *m) { return m->type; }
uint64_t v_port_pfcp_hdr_seid(const struct pfcp_msg *m) { return m->seid; }
uint32_t v_port_pfcp_seq(const struct pfcp_msg *m) { return m->seq; }
uint64_t v_port_pfcp_smf_fseid(const struct pfcp_msg *m) { return m->smf_fseid; }

/* Port impl: RET_CODE_ERR if the message carries no UE IP — callers
 * (v_flow's part_id selection) treat that as "use round-robin instead". */
int v_port_pfcp_ue_ip(const struct pfcp_msg *m, uint32_t *v4, uint8_t v6[16])
{
    if (!m->has_ue_ip)
        return RET_CODE_ERR;
    if (v4)
        *v4 = m->v4_ue_ip;
    if (v6)
        memcpy(v6, m->v6_ue_ip, 16);
    return RET_CODE_OK;
}

/* Deterministic payload derived from seed — this is what
 * test_sess_store.c/test_flow_mod_del.c compare against to prove data
 * actually changed (or didn't), not just that a call returned OK. */
static void fill_pattern(struct v_stub_ses_ctx *c, uint32_t seed)
{
    for (size_t i = 0; i < sizeof(c->payload); i++)
        c->payload[i] = (uint8_t)(seed + i);
}

/* Port impl: fills ctx with the session's identifying fields plus a
 * seq-derived pattern. */
int v_port_pfcp_build_session(const struct pfcp_msg *req,
                               uint64_t seid, uint32_t teid,
                               struct pdu_ses_ctx *ctx)
{
    struct v_stub_ses_ctx *c = (struct v_stub_ses_ctx *)ctx;
    c->seid = seid;
    c->teid = teid;
    c->has_ue_ip = req->has_ue_ip;
    c->v4_ue_ip = req->v4_ue_ip;
    memcpy(c->v6_ue_ip, req->v6_ue_ip, sizeof(c->v6_ue_ip));
    fill_pattern(c, req->seq);
    return RET_CODE_OK;
}

/* Port impl: XORs the pattern with a different constant so tests can
 * tell a modified session's payload apart from a freshly-built one. */
int v_port_pfcp_modify_session(const struct pfcp_msg *req, struct pdu_ses_ctx *ctx)
{
    struct v_stub_ses_ctx *c = (struct v_stub_ses_ctx *)ctx;
    fill_pattern(c, req->seq ^ 0xA5A5A5A5u);
    return RET_CODE_OK;
}

/* Port impl: no-op — deletion doesn't mutate ctx in this stub. */
int v_port_pfcp_delete_session(const struct pfcp_msg *req, struct pdu_ses_ctx *ctx)
{
    (void)req;
    (void)ctx;
    return RET_CODE_OK;
}

/* Port impl (ADAPTATION, see include/v_port_pfcp.h): pulls the TEID
 * back out of a previously-built ctx, for v_flow's deletion path. */
uint32_t v_port_pfcp_ctx_teid(const struct pdu_ses_ctx *ctx)
{
    return ((const struct v_stub_ses_ctx *)ctx)->teid;
}

/* Port impl: builds the synthetic response header. ctx==NULL forces
 * seid=0 in the response — this is how v_flow signals "Session context
 * not found" per plan.md §7. */
int v_port_pfcp_encode_rsp(const struct pfcp_msg *req, const struct pdu_ses_ctx *ctx,
                            uint8_t cause, uint8_t *buf, size_t *len)
{
    if (*len < sizeof(struct v_stub_wire_rsp))
        return RET_CODE_ERR;

    struct v_stub_wire_rsp rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.type = req->type + 1;
    rsp.cause = cause;
    rsp.seq = req->seq;
    rsp.seid = ctx ? ((const struct v_stub_ses_ctx *)ctx)->seid : 0;

    memcpy(buf, &rsp, sizeof(rsp));
    *len = sizeof(rsp);
    return RET_CODE_OK;
}

/* Port impl: the "serializer" is just a raw memcpy of the internal
 * struct — real production serialization is presumably a real wire
 * encoding, but the round-trip contract (serialize then deserialize
 * reproduces the original bytes) is what v_sess_store depends on, and
 * this satisfies it. */
int v_port_pdu_serialize(const struct pdu_ses_ctx *ctx, uint8_t *buf, size_t *len)
{
    if (*len < sizeof(struct v_stub_ses_ctx))
        return RET_CODE_ERR;
    memcpy(buf, ctx, sizeof(struct v_stub_ses_ctx));
    *len = sizeof(struct v_stub_ses_ctx);
    return RET_CODE_OK;
}

/* Port impl: inverse of v_port_pdu_serialize(). */
int v_port_pdu_deserialize(const uint8_t *buf, size_t len, struct pdu_ses_ctx *ctx)
{
    if (len < sizeof(struct v_stub_ses_ctx))
        return RET_CODE_ERR;
    memcpy(ctx, buf, sizeof(struct v_stub_ses_ctx));
    return RET_CODE_OK;
}

/* --- test-only surface --- */

/* Encodes desc into the stub wire format — the standalone test suite's
 * only way to construct a "received datagram". */
size_t v_stub_pfcp_encode(const v_stub_pfcp_desc_t *desc, uint8_t *buf, size_t buf_cap)
{
    if (buf_cap < sizeof(struct v_stub_wire_req))
        return 0;
    struct v_stub_wire_req wire;
    memset(&wire, 0, sizeof(wire));
    wire.type = desc->type;
    wire.has_ue_ip = desc->has_ue_ip ? 1 : 0;
    wire.seq = desc->seq;
    wire.seid = desc->seid;
    wire.smf_fseid = desc->smf_fseid;
    wire.v4_ue_ip = desc->v4_ue_ip;
    memcpy(wire.v6_ue_ip, desc->v6_ue_ip, sizeof(wire.v6_ue_ip));
    memcpy(buf, &wire, sizeof(wire));
    return sizeof(wire);
}

/* Synchronously invokes the rx callback registered via
 * v_port_pfcp_io_init(), as if a UDP datagram had just arrived — this
 * is how every test_flow_*.c test injects inbound traffic. */
void v_stub_pfcp_io_inject_rx(const uint8_t *buf, size_t len, const struct sockaddr *peer)
{
    if (g_rx_cb)
        g_rx_cb(buf, len, peer, g_rx_arg);
}

/* Returns whatever the most recent v_port_pfcp_io_send() call sent. */
size_t v_stub_pfcp_io_last_tx(uint8_t *out, size_t out_cap)
{
    size_t n = g_last_tx_len < out_cap ? g_last_tx_len : out_cap;
    if (n)
        memcpy(out, g_last_tx, n);
    return g_last_tx_len;
}

/* Clears the captured last-sent buffer — call between test cases so a
 * stale send from a previous test can't be mistaken for a fresh one. */
void v_stub_pfcp_io_reset(void)
{
    g_last_tx_len = 0;
    memset(g_last_tx, 0, sizeof(g_last_tx));
}
