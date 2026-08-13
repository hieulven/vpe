#include "test_util.h"
#include "test_common.h"
#include "v_sess_store.h"
#include "v_port_mem.h"
#include "v_port_pfcp.h"
#include "v_port_db.h"
#include "v_stub_pfcp_test.h"
#include "v_seid.h"
#include "v_common.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int g_read_seen, g_read_status, g_read_found;
static uint64_t g_read_ver;

static void read_cb(int status, v_cas_result_t found, struct pdu_ses_ctx *ctx, uint64_t ver, void *arg)
{
    (void)ctx; (void)arg;
    g_read_status = status;
    g_read_found = found;
    g_read_ver = ver;
    g_read_seen = 1;
}
static int read_seen_pred(void) { return g_read_seen; }

static int g_cas_seen, g_cas_status, g_cas_result;
static uint64_t g_cas_new_ver;

static void cas_cb(int status, v_cas_result_t result, uint64_t new_ver, void *arg)
{
    (void)arg;
    g_cas_status = status;
    g_cas_result = result;
    g_cas_new_ver = new_ver;
    g_cas_seen = 1;
}
static int cas_seen_pred(void) { return g_cas_seen; }

static int g_simple_seen, g_simple_status;
static void simple_cb(int status, void *arg)
{
    (void)arg;
    g_simple_status = status;
    g_simple_seen = 1;
}
static int simple_seen_pred(void) { return g_simple_seen; }

static int g_ack_seen;
static void db_ack_cb(int status, const v_db_reply_t *reply, void *arg)
{
    (void)status; (void)reply; (void)arg;
    g_ack_seen = 1;
}
static int ack_seen_pred(void) { return g_ack_seen; }

static struct pdu_ses_ctx *make_established_ctx(uint32_t seq, uint64_t seid, uint32_t teid)
{
    v_stub_pfcp_desc_t desc = {
        .type = V_PFCP_MSG_SESSION_EST_REQ,
        .seq = seq,
        .seid = 0,
        .smf_fseid = 0xABCD000000000000ull | seq,
        .has_ue_ip = 1,
        .v4_ue_ip = 0x0A000001u,
    };
    uint8_t buf[256];
    size_t len = v_stub_pfcp_encode(&desc, buf, sizeof(buf));
    CHECK(len > 0);

    struct pfcp_msg *m = NULL;
    CHECK(v_port_pfcp_decode(buf, len, &m) == RET_CODE_OK);

    struct pdu_ses_ctx *ctx = v_port_ctx_alloc();
    CHECK(ctx != NULL);
    CHECK(v_port_pfcp_build_session(m, seid, teid, ctx) == RET_CODE_OK);
    v_port_pfcp_msg_free(m);
    return ctx;
}

void test_sess_store_cas(void)
{
    printf("-- test_sess_store_cas --\n");

    size_t baseline = v_sess_store_test_pending_in_use();

    uint16_t part = 21;
    uint64_t seid;
    CHECK(v_seid_encode(part, 555, &seid) == RET_CODE_OK);

    /* Clean slate: previous runs may have left this key behind. */
    struct pdu_ses_ctx *ctx = make_established_ctx(9001, seid, 42);
    g_cas_seen = 0;
    v_sess_delete(part, seid, 1, cas_cb, NULL);
    test_wait_until(cas_seen_pred, 2000);
    v_sess_delete(part, seid, 2, cas_cb, NULL); /* belt and suspenders vs. earlier test runs */

    /* Fresh write, exp_ver=0 -> OK, new_ver=1. */
    g_cas_seen = 0;
    CHECK(v_sess_cas_write(part, seid, 0, ctx, V_SESS_PENDING, cas_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(cas_seen_pred, 2000));
    CHECK(g_cas_status == RET_CODE_OK);
    CHECK(g_cas_result == V_CAS_OK);
    CHECK(g_cas_new_ver == 1);

    /* Same exp_ver=0 again -> CONFLICT (already exists). */
    g_cas_seen = 0;
    CHECK(v_sess_cas_write(part, seid, 0, ctx, V_SESS_PENDING, cas_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(cas_seen_pred, 2000));
    CHECK(g_cas_result == V_CAS_CONFLICT);

    /* Read it back: found, ver=1, and the round-tripped blob matches
     * what was written (compared via the opaque serializer, never by
     * dereferencing ctx fields — plan.md §3.0). */
    struct pdu_ses_ctx *ctx_read = v_port_ctx_alloc();
    CHECK(ctx_read != NULL);
    g_read_seen = 0;
    CHECK(v_sess_read(part, seid, ctx_read, read_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(read_seen_pred, 2000));
    CHECK(g_read_status == RET_CODE_OK);
    CHECK(g_read_found == V_CAS_OK);
    CHECK(g_read_ver == 1);

    uint8_t blob_a[2048], blob_b[2048];
    size_t len_a = sizeof(blob_a), len_b = sizeof(blob_b);
    CHECK(v_port_pdu_serialize(ctx, blob_a, &len_a) == RET_CODE_OK);
    CHECK(v_port_pdu_serialize(ctx_read, blob_b, &len_b) == RET_CODE_OK);
    CHECK(len_a == len_b);
    CHECK(len_a > 0 && memcmp(blob_a, blob_b, len_a) == 0);

    /* Correct ver (1) succeeds, bumps to 2. */
    g_cas_seen = 0;
    CHECK(v_sess_cas_write(part, seid, 1, ctx, V_SESS_CONFIRMED, cas_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(cas_seen_pred, 2000));
    CHECK(g_cas_result == V_CAS_OK);
    CHECK(g_cas_new_ver == 2);

    /* Stale ver (1) now conflicts. */
    g_cas_seen = 0;
    CHECK(v_sess_cas_write(part, seid, 1, ctx, V_SESS_CONFIRMED, cas_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(cas_seen_pred, 2000));
    CHECK(g_cas_result == V_CAS_CONFLICT);

    /* confirm (PERSIST) succeeds regardless of prior TTL state. */
    g_simple_seen = 0;
    CHECK(v_sess_confirm(part, seid, simple_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(simple_seen_pred, 2000));
    CHECK(g_simple_status == RET_CODE_OK);

    /* Delete with wrong ver -> CONFLICT. */
    g_cas_seen = 0;
    CHECK(v_sess_delete(part, seid, 1, cas_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(cas_seen_pred, 2000));
    CHECK(g_cas_result == V_CAS_CONFLICT);

    /* Delete with correct ver (2) -> OK. */
    g_cas_seen = 0;
    CHECK(v_sess_delete(part, seid, 2, cas_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(cas_seen_pred, 2000));
    CHECK(g_cas_result == V_CAS_OK);

    /* Delete again -> GONE. */
    g_cas_seen = 0;
    CHECK(v_sess_delete(part, seid, 2, cas_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(cas_seen_pred, 2000));
    CHECK(g_cas_result == V_CAS_GONE);

    /* Read after delete -> not found. */
    g_read_seen = 0;
    CHECK(v_sess_read(part, seid, ctx_read, read_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(read_seen_pred, 2000));
    CHECK(g_read_found == V_CAS_GONE);

    v_port_ctx_free(ctx);
    v_port_ctx_free(ctx_read);

    CHECK(v_sess_store_test_pending_in_use() == baseline);

    printf("test_sess_store_cas: done\n");
}

void test_sess_store_pending_ttl(void)
{
    printf("-- test_sess_store_pending_ttl --\n");

    uint16_t part = 22;
    uint64_t seid;
    CHECK(v_seid_encode(part, 777, &seid) == RET_CODE_OK);

    struct pdu_ses_ctx *ctx = make_established_ctx(9002, seid, 43);

    g_cas_seen = 0;
    v_sess_delete(part, seid, 1, cas_cb, NULL); /* clean slate */
    test_wait_until(cas_seen_pred, 2000);

    g_cas_seen = 0;
    CHECK(v_sess_cas_write(part, seid, 0, ctx, V_SESS_PENDING, cas_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(cas_seen_pred, 2000));
    CHECK(g_cas_result == V_CAS_OK);

    /* v_sess_cas_write set a real 30s EXPIRE (V_SESS_PENDING_TTL). Don't
     * wait 30s in a build/test loop — shrink it via a raw PEXPIRE (same
     * key, same Redis expiry mechanism) to prove the pending record is
     * genuinely self-cleaning, without inventing a test-only constant
     * that the production code path never exercises. */
    char key[64];
    snprintf(key, sizeof(key), V_SESS_KEY_FMT, part, seid);
    g_ack_seen = 0;
    v_port_db_cmd(v_port_db_shard_of(key), db_ack_cb, NULL, "PEXPIRE %s 100", key);
    CHECK(test_wait_until(ack_seen_pred, 2000));

    struct timespec ts = { 0, 300000000L }; /* 300ms, comfortably > 100ms TTL */
    nanosleep(&ts, NULL);

    g_read_seen = 0;
    CHECK(v_sess_read(part, seid, ctx, read_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(read_seen_pred, 2000));
    CHECK(g_read_found == V_CAS_GONE);

    v_port_ctx_free(ctx);

    printf("test_sess_store_pending_ttl: done\n");
}
