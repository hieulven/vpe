#include "test_util.h"
#include "test_common.h"
#include "v_retrans_cache.h"
#include "v_port_db.h"
#include "v_common.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int g_seen, g_hit, g_status;
static uint8_t g_resp_copy[256];
static size_t g_resp_len;

static void lookup_cb(int status, int hit, const uint8_t *resp, size_t len, void *arg)
{
    (void)arg;
    g_status = status;
    g_hit = hit;
    g_resp_len = 0;
    if (hit && resp && len <= sizeof(g_resp_copy)) {
        memcpy(g_resp_copy, resp, len);
        g_resp_len = len;
    }
    g_seen = 1;
}
static int seen_pred(void) { return g_seen; }

void test_retrans_cache_basic(void)
{
    printf("-- test_retrans_cache_basic --\n");

    uint64_t smf_fseid = 0x1122334455667788ull;
    uint32_t seq = 31415;
    uint16_t part = v_retrans_part(smf_fseid);

    /* Miss before anything is stored. */
    g_seen = 0;
    CHECK(v_retrans_lookup(part, smf_fseid, seq, lookup_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(seen_pred, 2000));
    CHECK(g_status == RET_CODE_OK);
    CHECK(g_hit == 0);

    /* Store, including embedded NUL bytes — proving the argv path is
     * genuinely binary-safe, not strlen()-truncated. */
    uint8_t payload[16] = { 'R','S','P', 0x00, 0xFF, 0x00, 'x', 'y', 'z', 0, 0, 1, 2, 3, 4, 5 };
    CHECK(v_retrans_store(part, smf_fseid, seq, payload, sizeof(payload)) == RET_CODE_OK);

    /* Hit, with the exact bytes back. */
    g_seen = 0;
    CHECK(v_retrans_lookup(part, smf_fseid, seq, lookup_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(seen_pred, 2000));
    CHECK(g_hit == 1);
    CHECK(g_resp_len == sizeof(payload));
    CHECK(memcmp(g_resp_copy, payload, sizeof(payload)) == 0);

    /* A different seq for the same smf_fseid is a separate key. */
    g_seen = 0;
    CHECK(v_retrans_lookup(part, smf_fseid, seq + 1, lookup_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(seen_pred, 2000));
    CHECK(g_hit == 0);

    printf("test_retrans_cache_basic: done\n");
}

static int g_ack_seen;
static void ack_cb(int status, const v_db_reply_t *reply, void *arg)
{
    (void)status; (void)reply; (void)arg;
    g_ack_seen = 1;
}
static int ack_seen_pred(void) { return g_ack_seen; }

void test_retrans_cache_ttl(void)
{
    printf("-- test_retrans_cache_ttl --\n");

    uint64_t smf_fseid = 0x99AABBCCDDEEFF00ull;
    uint32_t seq = 2718;
    uint16_t part = v_retrans_part(smf_fseid);

    uint8_t payload[4] = { 1, 2, 3, 4 };
    CHECK(v_retrans_store(part, smf_fseid, seq, payload, sizeof(payload)) == RET_CODE_OK);

    /* Same self-cleaning-TTL verification approach as
     * test_sess_store_pending_ttl: shrink the real Redis TTL via
     * PEXPIRE rather than waiting out V_RETRANS_TTL (10s) or inventing
     * a test-only constant the production path never uses. */
    char key[64];
    snprintf(key, sizeof(key), V_RETRANS_KEY_FMT, part, smf_fseid, seq);
    g_ack_seen = 0;
    v_port_db_cmd(v_port_db_shard_of(key), ack_cb, NULL, "PEXPIRE %s 100", key);
    CHECK(test_wait_until(ack_seen_pred, 2000));

    struct timespec ts = { 0, 300000000L };
    nanosleep(&ts, NULL);

    g_seen = 0;
    CHECK(v_retrans_lookup(part, smf_fseid, seq, lookup_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(seen_pred, 2000));
    CHECK(g_hit == 0);

    printf("test_retrans_cache_ttl: done\n");
}
