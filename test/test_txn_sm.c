#include "test_util.h"
#include "test_common.h"
#include "v_txn.h"
#include "v_port_txn.h"
#include "v_port_mem.h"
#include "v_port_pfcp.h"
#include "v_sess_store.h"
#include "v_seid.h"
#include "v_stub_pfcp_test.h"
#include "v_common.h"

#include <stdio.h>
#include <time.h>

static void pump(int ms)
{
    for (int i = 0; i < ms; i++) {
        v_port_db_poll(1);
        struct timespec ts = { 0, 1000000L };
        nanosleep(&ts, NULL);
    }
}

static struct v_txn *make_txn(uint16_t part, uint64_t seid, uint64_t db_ver, v_txn_state_t state)
{
    struct v_txn *t = v_port_txn_create();
    CHECK(t != NULL);
    if (!t)
        return NULL;

    t->part_id = part;
    t->seid = seid;
    t->db_ver = db_ver;
    t->state = state;
    t->ctx = v_port_ctx_alloc();
    CHECK(t->ctx != NULL);

    v_stub_pfcp_desc_t desc = {
        .type = V_PFCP_MSG_SESSION_EST_REQ,
        .seq = (uint32_t)seid,
        .seid = 0,
        .smf_fseid = seid,
    };
    uint8_t buf[256];
    size_t len = v_stub_pfcp_encode(&desc, buf, sizeof(buf));
    struct pfcp_msg *m = NULL;
    CHECK(v_port_pfcp_decode(buf, len, &m) == RET_CODE_OK);
    t->req = m;

    return t;
}

static int g_read_seen2, g_read_found2;
static void read_cb2(int status, v_cas_result_t found, struct pdu_ses_ctx *ctx, uint64_t ver, void *arg)
{
    (void)status; (void)ctx; (void)ver; (void)arg;
    g_read_found2 = found;
    g_read_seen2 = 1;
}
static int read_seen2_pred(void) { return g_read_seen2; }

static int g_cas_seen2;
static void cas_cb2(int status, v_cas_result_t result, uint64_t new_ver, void *arg)
{
    (void)status; (void)result; (void)new_ver; (void)arg;
    g_cas_seen2 = 1;
}
static int cas_seen2_pred(void) { return g_cas_seen2; }

void test_txn_sm_timeout_paths(void)
{
    printf("-- test_txn_sm_timeout_paths --\n");

    uint64_t hard_ms = v_port_txn_test_hard_timeout_ms();
    size_t base_ctx = v_port_ctx_pool_in_use();

    /* Simple states: no Redis compensation expected, just resource
     * release. */
    v_txn_state_t simple_states[] = { TXN_ST_RECV, TXN_ST_DB_READ_WAIT, TXN_ST_DB_WRITE_WAIT,
                                       TXN_ST_DB_CONFIRM_WAIT, TXN_ST_REPLIED };
    for (size_t i = 0; i < sizeof(simple_states) / sizeof(simple_states[0]); i++) {
        uint64_t seid;
        CHECK(v_seid_encode(31, 100 + i, &seid) == RET_CODE_OK);
        struct v_txn *t = make_txn(31, seid, 1, simple_states[i]);
        CHECK(t != NULL);

        v_port_txn_test_advance_ms(hard_ms + 10);

        CHECK(v_port_ctx_pool_in_use() == base_ctx);
        CHECK(v_port_txn_find_by_seid(seid) == NULL);
    }

    /* VDP_WAIT, establishment (db_ver==0): the sweeper must delete the
     * pending record it left behind, not just wait out the TTL. */
    {
        uint64_t seid;
        CHECK(v_seid_encode(31, 200, &seid) == RET_CODE_OK);

        v_stub_pfcp_desc_t desc = { .type = V_PFCP_MSG_SESSION_EST_REQ, .seq = 5001, .smf_fseid = seid, .has_ue_ip = 1 };
        uint8_t buf[256];
        size_t len = v_stub_pfcp_encode(&desc, buf, sizeof(buf));
        struct pfcp_msg *m = NULL;
        CHECK(v_port_pfcp_decode(buf, len, &m) == RET_CODE_OK);
        struct pdu_ses_ctx *ctx = v_port_ctx_alloc();
        CHECK(v_port_pfcp_build_session(m, seid, 77, ctx) == RET_CODE_OK);
        v_port_pfcp_msg_free(m);

        /* Simulate the CAS write step 7 of plan.md §5.1 that would have
         * happened before entering VDP_WAIT. */
        g_cas_seen2 = 0;
        CHECK(v_sess_cas_write(31, seid, 0, ctx, V_SESS_PENDING, cas_cb2, NULL) == RET_CODE_OK);
        CHECK(test_wait_until(cas_seen2_pred, 2000));
        v_port_ctx_free(ctx);

        struct v_txn *t = make_txn(31, seid, 0 /* establishment */, TXN_ST_VDP_WAIT);
        CHECK(t != NULL);

        v_port_txn_test_advance_ms(hard_ms + 10);
        pump(50); /* let the sweeper's delete round-trip complete */

        CHECK(v_port_ctx_pool_in_use() == base_ctx);
        CHECK(v_port_txn_find_by_seid(seid) == NULL);

        struct pdu_ses_ctx *ctx2 = v_port_ctx_alloc();
        g_read_seen2 = 0;
        CHECK(v_sess_read(31, seid, ctx2, read_cb2, NULL) == RET_CODE_OK);
        CHECK(test_wait_until(read_seen2_pred, 2000));
        CHECK(g_read_found2 == V_CAS_GONE); /* sweeper deleted the orphaned pending record */
        v_port_ctx_free(ctx2);
    }

    /* VDP_WAIT, modification (db_ver != 0): known gap (see v_txn.h) —
     * the sweeper cannot compensate the write-back, but it must still
     * release its own resources without leaking. */
    {
        uint64_t seid;
        CHECK(v_seid_encode(31, 201, &seid) == RET_CODE_OK);
        struct v_txn *t = make_txn(31, seid, 3 /* pretend a prior read got ver=3 */, TXN_ST_VDP_WAIT);
        CHECK(t != NULL);

        v_port_txn_test_advance_ms(hard_ms + 10);
        pump(20);

        CHECK(v_port_ctx_pool_in_use() == base_ctx);
        CHECK(v_port_txn_find_by_seid(seid) == NULL);
    }

    printf("test_txn_sm_timeout_paths: done\n");
}
