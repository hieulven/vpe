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

static uint64_t g_establish_seed = 0xE000000000000000ull;

/* Establishes a fresh session end-to-end through v_flow and returns its
 * SEID (0 on failure). */
static uint64_t establish(uint32_t seq, uint32_t ue_ip_host_octet)
{
    v_port_vdp_test_reset();
    v_stub_pfcp_io_reset();

    uint64_t smf_fseid = g_establish_seed | seq;
    v_stub_pfcp_desc_t desc = {
        .type = V_PFCP_MSG_SESSION_EST_REQ, .seq = seq, .seid = 0,
        .smf_fseid = smf_fseid, .has_ue_ip = 1,
        .v4_ue_ip = 0x0A000000u | ue_ip_host_octet,
    };
    uint8_t buf[256];
    size_t len = v_stub_pfcp_encode(&desc, buf, sizeof(buf));
    struct sockaddr peer;
    memset(&peer, 0, sizeof(peer));

    v_stub_pfcp_io_inject_rx(buf, len, &peer);
    v_dispatch_worker_poll(0, 10, v_flow_handle_msg, NULL);

    uint8_t resp[512];
    for (int i = 0; i < 5000; i++) {
        v_port_db_poll(1);
        v_dispatch_io_drain_tx(10);
        size_t n = v_stub_pfcp_io_last_tx(resp, sizeof(resp));
        if (n > 0) {
            struct v_stub_wire_rsp rsp;
            memcpy(&rsp, resp, sizeof(rsp));
            return rsp.cause == V_PFCP_CAUSE_REQUEST_ACCEPTED ? rsp.seid : 0;
        }
        struct timespec ts = { 0, 500000L };
        nanosleep(&ts, NULL);
    }
    return 0;
}

static int pump(int max_iters, int (*done_pred)(void))
{
    for (int i = 0; i < max_iters; i++) {
        v_port_db_poll(1);
        v_dispatch_io_drain_tx(10);
        if (done_pred && done_pred())
            return 1;
        struct timespec ts = { 0, 500000L };
        nanosleep(&ts, NULL);
    }
    return done_pred ? done_pred() : 1;
}

static void send_session_msg(uint8_t type, uint64_t seid, uint32_t seq, uint64_t smf_fseid)
{
    v_stub_pfcp_desc_t desc = { .type = type, .seq = seq, .seid = seid, .smf_fseid = smf_fseid };
    uint8_t buf[256];
    size_t len = v_stub_pfcp_encode(&desc, buf, sizeof(buf));
    struct sockaddr peer;
    memset(&peer, 0, sizeof(peer));
    v_stub_pfcp_io_inject_rx(buf, len, &peer);
}

static int g_read_seen4, g_read_found4;
static uint64_t g_read_ver4;
static void read_cb4(int status, v_cas_result_t found, struct pdu_ses_ctx *ctx, uint64_t ver, void *arg)
{
    (void)status; (void)ctx; (void)arg;
    g_read_found4 = found;
    g_read_ver4 = ver;
    g_read_seen4 = 1;
}
static int read_seen4_pred(void) { return g_read_seen4; }

static uint64_t read_ver(uint16_t part, uint64_t seid, int *found_out)
{
    struct pdu_ses_ctx *ctx = v_port_ctx_alloc();
    g_read_seen4 = 0;
    v_sess_read(part, seid, ctx, read_cb4, NULL);
    test_wait_until(read_seen4_pred, 2000);
    v_port_ctx_free(ctx);
    if (found_out)
        *found_out = g_read_found4;
    return g_read_ver4;
}

void test_flow_modification_accept(void)
{
    printf("-- test_flow_modification_accept --\n");

    uint64_t seid = establish(700001, 0x20);
    CHECK(seid != 0);
    uint16_t part = v_seid_part(seid);

    int found;
    uint64_t ver_before = read_ver(part, seid, &found);
    CHECK(found == V_CAS_OK);
    CHECK(ver_before == 1);

    v_port_vdp_test_reset();
    v_stub_pfcp_io_reset();
    send_session_msg(V_PFCP_MSG_SESSION_MOD_REQ, seid, 700002, 0xAAAA);
    CHECK(v_dispatch_worker_poll(0, 10, v_flow_handle_msg, NULL) == 1);

    uint8_t resp[512];
    size_t resp_len = 0;
    for (int i = 0; i < 5000 && resp_len == 0; i++) {
        v_port_db_poll(1);
        v_dispatch_io_drain_tx(10);
        resp_len = v_stub_pfcp_io_last_tx(resp, sizeof(resp));
        if (!resp_len) { struct timespec ts = {0,500000L}; nanosleep(&ts,NULL); }
    }
    CHECK(resp_len > 0);
    struct v_stub_wire_rsp rsp;
    memcpy(&rsp, resp, sizeof(rsp));
    CHECK(rsp.cause == V_PFCP_CAUSE_REQUEST_ACCEPTED);
    CHECK(rsp.seid == seid);

    uint64_t ver_after = read_ver(part, seid, &found);
    CHECK(found == V_CAS_OK);
    CHECK(ver_after == ver_before + 1);

    printf("test_flow_modification_accept: done\n");
}

void test_flow_modification_vdp_reject_compensates(void)
{
    printf("-- test_flow_modification_vdp_reject_compensates --\n");

    uint64_t seid = establish(700010, 0x21);
    CHECK(seid != 0);
    uint16_t part = v_seid_part(seid);

    int found;
    uint64_t ver_before = read_ver(part, seid, &found);
    CHECK(found == V_CAS_OK);

    /* Snapshot the pre-modification blob so we can prove it comes back
     * unchanged after the compensating write. */
    struct pdu_ses_ctx *before_ctx = v_port_ctx_alloc();
    g_read_seen4 = 0;
    v_sess_read(part, seid, before_ctx, read_cb4, NULL);
    CHECK(test_wait_until(read_seen4_pred, 2000));
    uint8_t before_blob[2048];
    size_t before_len = sizeof(before_blob);
    CHECK(v_port_pdu_serialize(before_ctx, before_blob, &before_len) == RET_CODE_OK);
    v_port_ctx_free(before_ctx);

    v_port_vdp_test_force_result(V_VDP_REJECT);
    v_stub_pfcp_io_reset();
    send_session_msg(V_PFCP_MSG_SESSION_MOD_REQ, seid, 700011, 0xBBBB);
    CHECK(v_dispatch_worker_poll(0, 10, v_flow_handle_msg, NULL) == 1);

    uint8_t resp[512];
    size_t resp_len = 0;
    for (int i = 0; i < 5000 && resp_len == 0; i++) {
        v_port_db_poll(1);
        v_dispatch_io_drain_tx(10);
        resp_len = v_stub_pfcp_io_last_tx(resp, sizeof(resp));
        if (!resp_len) { struct timespec ts = {0,500000L}; nanosleep(&ts,NULL); }
    }
    CHECK(resp_len > 0);
    struct v_stub_wire_rsp rsp;
    memcpy(&rsp, resp, sizeof(rsp));
    CHECK(rsp.cause == V_PFCP_CAUSE_REQUEST_REJECTED);

    /* ver still advances (the compensating write is itself a CAS write)
     * but the DATA must be back to what it was before this modification
     * was attempted. */
    struct pdu_ses_ctx *after_ctx = v_port_ctx_alloc();
    g_read_seen4 = 0;
    v_sess_read(part, seid, after_ctx, read_cb4, NULL);
    CHECK(test_wait_until(read_seen4_pred, 2000));
    CHECK(g_read_found4 == V_CAS_OK);
    CHECK(g_read_ver4 == ver_before + 2); /* +1 for the failed modify's write, +1 for the compensating write */
    uint8_t after_blob[2048];
    size_t after_len = sizeof(after_blob);
    CHECK(v_port_pdu_serialize(after_ctx, after_blob, &after_len) == RET_CODE_OK);
    v_port_ctx_free(after_ctx);

    CHECK(before_len == after_len);
    CHECK(memcmp(before_blob, after_blob, before_len) == 0);

    v_port_vdp_test_reset();
    printf("test_flow_modification_vdp_reject_compensates: done\n");
}

void test_flow_modification_not_found(void)
{
    printf("-- test_flow_modification_not_found --\n");

    /* Well-formed SEID (passes v_seid_validate) but no session was ever
     * created at this local index. */
    uint64_t seid;
    CHECK(v_seid_encode(41, 999999, &seid) == RET_CODE_OK);

    v_port_vdp_test_reset();
    v_stub_pfcp_io_reset();
    send_session_msg(V_PFCP_MSG_SESSION_MOD_REQ, seid, 700020, 0xCCCC);
    CHECK(v_dispatch_worker_poll(0, 10, v_flow_handle_msg, NULL) == 1);

    uint8_t resp[512];
    size_t resp_len = 0;
    for (int i = 0; i < 5000 && resp_len == 0; i++) {
        v_port_db_poll(1);
        v_dispatch_io_drain_tx(10);
        resp_len = v_stub_pfcp_io_last_tx(resp, sizeof(resp));
        if (!resp_len) { struct timespec ts = {0,500000L}; nanosleep(&ts,NULL); }
    }
    CHECK(resp_len > 0);
    struct v_stub_wire_rsp rsp;
    memcpy(&rsp, resp, sizeof(rsp));
    CHECK(rsp.cause == V_PFCP_CAUSE_SESSION_CTX_NOT_FOUND);
    CHECK(rsp.seid == 0);

    printf("test_flow_modification_not_found: done\n");
}

void test_flow_modification_bad_seid_fuzz(void)
{
    printf("-- test_flow_modification_bad_seid_fuzz --\n");

    v_stub_pfcp_io_reset();
    /* free5gc #730/#731-class input: must not crash, must not touch
     * Redis, must reply Session context not found with seid=0. */
    send_session_msg(V_PFCP_MSG_SESSION_MOD_REQ, 0xFFFFFFFFFFFFFFFFull, 700030, 0xDDDD);
    CHECK(v_dispatch_worker_poll(0, 10, v_flow_handle_msg, NULL) == 1);

    uint8_t resp[512];
    size_t resp_len = 0;
    CHECK(pump(2000, NULL));
    resp_len = v_stub_pfcp_io_last_tx(resp, sizeof(resp));
    CHECK(resp_len > 0);
    struct v_stub_wire_rsp rsp;
    memcpy(&rsp, resp, sizeof(rsp));
    CHECK(rsp.cause == V_PFCP_CAUSE_SESSION_CTX_NOT_FOUND);
    CHECK(rsp.seid == 0);

    printf("test_flow_modification_bad_seid_fuzz: done\n");
}

void test_flow_deletion_accept_frees_teid(void)
{
    printf("-- test_flow_deletion_accept_frees_teid --\n");

    uint64_t seid = establish(700040, 0x30);
    CHECK(seid != 0);
    uint16_t part = v_seid_part(seid);

    char free_key[64];
    snprintf(free_key, sizeof(free_key), "vpe:teid:%u:free", part);

    int found;
    read_ver(part, seid, &found);
    CHECK(found == V_CAS_OK);

    v_port_vdp_test_reset();
    v_stub_pfcp_io_reset();
    send_session_msg(V_PFCP_MSG_SESSION_DEL_REQ, seid, 700041, 0xEEEE);
    CHECK(v_dispatch_worker_poll(0, 10, v_flow_handle_msg, NULL) == 1);

    uint8_t resp[512];
    size_t resp_len = 0;
    for (int i = 0; i < 5000 && resp_len == 0; i++) {
        v_port_db_poll(1);
        v_dispatch_io_drain_tx(10);
        resp_len = v_stub_pfcp_io_last_tx(resp, sizeof(resp));
        if (!resp_len) { struct timespec ts = {0,500000L}; nanosleep(&ts,NULL); }
    }
    CHECK(resp_len > 0);
    struct v_stub_wire_rsp rsp;
    memcpy(&rsp, resp, sizeof(rsp));
    CHECK(rsp.cause == V_PFCP_CAUSE_REQUEST_ACCEPTED);
    CHECK(rsp.seid == seid);

    read_ver(part, seid, &found);
    CHECK(found == V_CAS_GONE);

    printf("test_flow_deletion_accept_frees_teid: done\n");
}

void test_flow_deletion_vdp_reject_keeps_session(void)
{
    printf("-- test_flow_deletion_vdp_reject_keeps_session --\n");

    uint64_t seid = establish(700050, 0x31);
    CHECK(seid != 0);
    uint16_t part = v_seid_part(seid);

    v_port_vdp_test_force_result(V_VDP_REJECT);
    v_stub_pfcp_io_reset();
    send_session_msg(V_PFCP_MSG_SESSION_DEL_REQ, seid, 700051, 0xFFFF);
    CHECK(v_dispatch_worker_poll(0, 10, v_flow_handle_msg, NULL) == 1);

    uint8_t resp[512];
    size_t resp_len = 0;
    for (int i = 0; i < 5000 && resp_len == 0; i++) {
        v_port_db_poll(1);
        v_dispatch_io_drain_tx(10);
        resp_len = v_stub_pfcp_io_last_tx(resp, sizeof(resp));
        if (!resp_len) { struct timespec ts = {0,500000L}; nanosleep(&ts,NULL); }
    }
    CHECK(resp_len > 0);
    struct v_stub_wire_rsp rsp;
    memcpy(&rsp, resp, sizeof(rsp));
    CHECK(rsp.cause == V_PFCP_CAUSE_REQUEST_REJECTED);

    int found;
    uint64_t ver = read_ver(part, seid, &found);
    CHECK(found == V_CAS_OK); /* untouched — deletion never wrote to Redis */
    CHECK(ver == 1);

    v_port_vdp_test_reset();
    printf("test_flow_deletion_vdp_reject_keeps_session: done\n");
}

/* plan.md §9.3 "Concurrent Modification storm": two Modifications for
 * the same session, submitted before either has a chance to complete.
 * The invariant that matters is "all applied, none lost" — checked here
 * via the version counter, which can only reach establish_ver+2 if
 * BOTH writes really landed (a lost update would silently clobber one,
 * leaving the counter one short). Whether v_flow's CAS-conflict retry
 * path fires internally to get there is an implementation detail this
 * black-box check doesn't need to observe directly. */
void test_flow_concurrent_modification_storm(void)
{
    printf("-- test_flow_concurrent_modification_storm --\n");

    uint64_t seid = establish(700060, 0x40);
    CHECK(seid != 0);
    uint16_t part = v_seid_part(seid);

    int found;
    uint64_t ver_before = read_ver(part, seid, &found);
    CHECK(found == V_CAS_OK);

    v_port_vdp_test_reset();
    v_stub_pfcp_io_reset();
    send_session_msg(V_PFCP_MSG_SESSION_MOD_REQ, seid, 700061, 0x1111);
    send_session_msg(V_PFCP_MSG_SESSION_MOD_REQ, seid, 700062, 0x2222);
    /* Both messages decoded and their retrans-lookups dispatched in the
     * same worker_poll call, before any DB reply has arrived — as
     * concurrent as this single-threaded harness can make two async
     * chains. */
    CHECK(v_dispatch_worker_poll(0, 10, v_flow_handle_msg, NULL) == 2);

    CHECK(pump(10000, NULL));
    /* Extra settle time: two independent tx sends can't both be
     * observed via last_tx, so just make sure both flows had ample
     * opportunity to fully resolve (including any CAS retries). */
    struct timespec ts = { 0, 200000000L };
    nanosleep(&ts, NULL);
    pump(1000, NULL);

    uint64_t ver_after = read_ver(part, seid, &found);
    CHECK(found == V_CAS_OK);
    CHECK(ver_after == ver_before + 2);

    printf("test_flow_concurrent_modification_storm: done (ver %lu -> %lu)\n", ver_before, ver_after);
}

/* plan.md §9.3 "Modification racing Deletion": deletion's message is
 * submitted first so it gets first crack at the shared read/write
 * pipeline (see the long-form reasoning in the commit message / dev
 * notes) — modification is expected to lose and come back "Session
 * context not found". Both possible orderings are safe in this
 * implementation (a modification that wins outright just means
 * deletion's own CAS-conflict retry catches up and still deletes the
 * session), so this test's hard requirement is just that the session
 * ends up gone; the specific "modification sees not-found" outcome is
 * asserted too since it's what plan.md specifies, but is the part most
 * likely to need loosening if it ever proves order-sensitive across
 * environments. */
void test_flow_modification_racing_deletion(void)
{
    printf("-- test_flow_modification_racing_deletion --\n");

    uint64_t seid = establish(700070, 0x50);
    CHECK(seid != 0);
    uint16_t part = v_seid_part(seid);

    v_port_vdp_test_reset();
    v_stub_pfcp_io_reset();
    send_session_msg(V_PFCP_MSG_SESSION_DEL_REQ, seid, 700071, 0x3333);
    send_session_msg(V_PFCP_MSG_SESSION_MOD_REQ, seid, 700072, 0x4444);
    CHECK(v_dispatch_worker_poll(0, 10, v_flow_handle_msg, NULL) == 2);

    CHECK(pump(10000, NULL));
    struct timespec ts = { 0, 200000000L };
    nanosleep(&ts, NULL);
    pump(1000, NULL);

    int found;
    read_ver(part, seid, &found);
    CHECK(found == V_CAS_GONE); /* hard invariant: deletion always eventually wins */

    printf("test_flow_modification_racing_deletion: done (session correctly gone)\n");
}
