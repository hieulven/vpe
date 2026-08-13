#include "test_util.h"
#include "test_common.h"
#include "v_flow.h"
#include "v_dispatch.h"
#include "v_port_pfcp.h"
#include "v_port_vdp.h"
#include "v_port_mem.h"
#include "v_port_db.h"
#include "v_sess_store.h"
#include "v_seid.h"
#include "v_stub_pfcp_test.h"
#include "v_stub_common.h"
#include "v_common.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int wait_for_tx(uint8_t *out, size_t *out_len, int max_iters)
{
    for (int i = 0; i < max_iters; i++) {
        v_port_db_poll(1);
        v_dispatch_io_drain_tx(10);
        size_t n = v_stub_pfcp_io_last_tx(out, 512);
        if (n > 0) {
            *out_len = n;
            return 1;
        }
        struct timespec ts = { 0, 500000L };
        nanosleep(&ts, NULL);
    }
    return 0;
}

static int g_read_seen3, g_read_found3;
static uint64_t g_read_ver3;
static void read_cb3(int status, v_cas_result_t found, struct pdu_ses_ctx *ctx, uint64_t ver, void *arg)
{
    (void)status; (void)ctx; (void)arg;
    g_read_found3 = found;
    g_read_ver3 = ver;
    g_read_seen3 = 1;
}
static int read_seen3_pred(void) { return g_read_seen3; }

void test_flow_establishment_accept(void)
{
    printf("-- test_flow_establishment_accept --\n");

    v_port_vdp_test_reset(); /* default ACCEPT */
    v_stub_pfcp_io_reset();

    uint32_t seq = 111222;
    uint64_t smf_fseid = 0xF00DCAFE00000000ull | seq;
    v_stub_pfcp_desc_t desc = {
        .type = V_PFCP_MSG_SESSION_EST_REQ, .seq = seq, .seid = 0,
        .smf_fseid = smf_fseid, .has_ue_ip = 1, .v4_ue_ip = 0x0A000010u,
    };
    uint8_t buf[256];
    size_t len = v_stub_pfcp_encode(&desc, buf, sizeof(buf));
    struct sockaddr peer;
    memset(&peer, 0, sizeof(peer));

    v_stub_pfcp_io_inject_rx(buf, len, &peer);
    CHECK(v_dispatch_worker_poll(0, 10, v_flow_handle_msg, NULL) == 1);

    uint8_t resp[512];
    size_t resp_len = 0;
    CHECK(wait_for_tx(resp, &resp_len, 5000));
    CHECK(resp_len == sizeof(struct v_stub_wire_rsp));

    struct v_stub_wire_rsp rsp;
    memcpy(&rsp, resp, sizeof(rsp));
    CHECK(rsp.cause == V_PFCP_CAUSE_REQUEST_ACCEPTED);
    CHECK(rsp.seid != 0);
    CHECK(rsp.seq == seq);

    uint16_t part = v_seid_part(rsp.seid);
    struct pdu_ses_ctx *ctx = v_port_ctx_alloc();
    g_read_seen3 = 0;
    CHECK(v_sess_read(part, rsp.seid, ctx, read_cb3, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(read_seen3_pred, 2000));
    CHECK(g_read_found3 == V_CAS_OK);
    CHECK(g_read_ver3 == 1); /* CAS write created it at ver=1; confirm (PERSIST) doesn't bump it */
    v_port_ctx_free(ctx);

    printf("test_flow_establishment_accept: done (seid=0x%016lx)\n", rsp.seid);
}

void test_flow_establishment_reject(void)
{
    printf("-- test_flow_establishment_reject --\n");

    v_port_vdp_test_force_result(V_VDP_REJECT);
    v_stub_pfcp_io_reset();

    uint32_t seq = 111333;
    uint64_t smf_fseid = 0xF00DCAFE00000000ull | seq;
    v_stub_pfcp_desc_t desc = {
        .type = V_PFCP_MSG_SESSION_EST_REQ, .seq = seq, .seid = 0,
        .smf_fseid = smf_fseid, .has_ue_ip = 1, .v4_ue_ip = 0x0A000011u,
    };
    uint8_t buf[256];
    size_t len = v_stub_pfcp_encode(&desc, buf, sizeof(buf));
    struct sockaddr peer;
    memset(&peer, 0, sizeof(peer));

    v_stub_pfcp_io_inject_rx(buf, len, &peer);
    CHECK(v_dispatch_worker_poll(0, 10, v_flow_handle_msg, NULL) == 1);

    uint8_t resp[512];
    size_t resp_len = 0;
    CHECK(wait_for_tx(resp, &resp_len, 5000));

    struct v_stub_wire_rsp rsp;
    memcpy(&rsp, resp, sizeof(rsp));
    CHECK(rsp.cause == V_PFCP_CAUSE_REQUEST_REJECTED);
    CHECK(rsp.seid == 0); /* no live session — response header SEID is 0 per TS 29.244 (plan.md §7) */

    v_port_vdp_test_reset();
    printf("test_flow_establishment_reject: done\n");
}

void test_flow_establishment_timeout(void)
{
    printf("-- test_flow_establishment_timeout --\n");

    v_port_vdp_test_force_result(V_VDP_TIMEOUT);
    v_stub_pfcp_io_reset();

    uint32_t seq = 111444;
    uint64_t smf_fseid = 0xF00DCAFE00000000ull | seq;
    v_stub_pfcp_desc_t desc = {
        .type = V_PFCP_MSG_SESSION_EST_REQ, .seq = seq, .seid = 0,
        .smf_fseid = smf_fseid, .has_ue_ip = 1, .v4_ue_ip = 0x0A000012u,
    };
    uint8_t buf[256];
    size_t len = v_stub_pfcp_encode(&desc, buf, sizeof(buf));
    struct sockaddr peer;
    memset(&peer, 0, sizeof(peer));

    v_stub_pfcp_io_inject_rx(buf, len, &peer);
    CHECK(v_dispatch_worker_poll(0, 10, v_flow_handle_msg, NULL) == 1);

    uint8_t resp[512];
    size_t resp_len = 0;
    CHECK(wait_for_tx(resp, &resp_len, 5000));

    struct v_stub_wire_rsp rsp;
    memcpy(&rsp, resp, sizeof(rsp));
    CHECK(rsp.cause == V_PFCP_CAUSE_REQUEST_REJECTED);
    CHECK(rsp.seid == 0);

    v_port_vdp_test_reset();
    printf("test_flow_establishment_timeout: done\n");
}

/* plan.md §8.1 / §9.3: a retransmit landing on "a different pod" must
 * get the cached response, with no second allocation. One process can't
 * literally run two pods, but the dedup path goes entirely through
 * Redis (v_retrans_cache), so replaying the identical datagram exercises
 * the same mechanism a genuinely different pod would hit. */
void test_flow_establishment_retransmit_dedup(void)
{
    printf("-- test_flow_establishment_retransmit_dedup --\n");

    v_port_vdp_test_reset();
    v_stub_pfcp_io_reset();

    uint32_t seq = 555666;
    uint64_t smf_fseid = 0xABCDEF0000000000ull | seq;
    v_stub_pfcp_desc_t desc = {
        .type = V_PFCP_MSG_SESSION_EST_REQ, .seq = seq, .seid = 0,
        .smf_fseid = smf_fseid, .has_ue_ip = 1, .v4_ue_ip = 0x0A0000FEu,
    };
    uint8_t buf[256];
    size_t len = v_stub_pfcp_encode(&desc, buf, sizeof(buf));
    struct sockaddr peer;
    memset(&peer, 0, sizeof(peer));

    v_stub_pfcp_io_inject_rx(buf, len, &peer);
    CHECK(v_dispatch_worker_poll(0, 10, v_flow_handle_msg, NULL) == 1);
    uint8_t resp1[512];
    size_t resp1_len = 0;
    CHECK(wait_for_tx(resp1, &resp1_len, 5000));
    struct v_stub_wire_rsp rsp1;
    memcpy(&rsp1, resp1, sizeof(rsp1));
    CHECK(rsp1.cause == V_PFCP_CAUSE_REQUEST_ACCEPTED);

    /* Give the async v_retrans_store a moment to actually land before
     * replaying — it's fire-and-forget from finish()'s point of view. */
    for (int i = 0; i < 200; i++) {
        v_port_db_poll(1);
        struct timespec ts = { 0, 1000000L };
        nanosleep(&ts, NULL);
    }

    v_stub_pfcp_io_reset();
    v_stub_pfcp_io_inject_rx(buf, len, &peer); /* identical bytes: same seq + smf_fseid */
    CHECK(v_dispatch_worker_poll(0, 10, v_flow_handle_msg, NULL) == 1);
    uint8_t resp2[512];
    size_t resp2_len = 0;
    CHECK(wait_for_tx(resp2, &resp2_len, 5000));
    struct v_stub_wire_rsp rsp2;
    memcpy(&rsp2, resp2, sizeof(rsp2));

    CHECK(resp1_len == resp2_len);
    CHECK(memcmp(resp1, resp2, resp1_len) == 0); /* byte-identical cached reply */
    CHECK(rsp2.seid == rsp1.seid);               /* no second SEID allocated */

    printf("test_flow_establishment_retransmit_dedup: done (seid=0x%016lx reused via cache)\n", rsp1.seid);
}
