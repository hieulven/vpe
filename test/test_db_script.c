#include "test_util.h"
#include "test_common.h"
#include "v_db_script.h"
#include "v_stub_db_test.h"

#include <string.h>
#include <stdio.h>

static int g_reply_status;
static v_db_reply_t g_reply_copy;
static char g_reply_str_buf[256];
static int g_reply_seen;

static void capture_cb(int status, const v_db_reply_t *reply, void *arg)
{
    (void)arg;
    g_reply_status = status;
    g_reply_seen = 1;
    if (reply) {
        g_reply_copy = *reply;
        if (reply->str && reply->len < sizeof(g_reply_str_buf)) {
            memcpy(g_reply_str_buf, reply->str, reply->len);
            g_reply_str_buf[reply->len] = '\0';
            g_reply_copy.str = g_reply_str_buf;
        }
    } else {
        memset(&g_reply_copy, 0, sizeof(g_reply_copy));
    }
}

static int reply_seen_pred(void) { return g_reply_seen; }

static int scripts_ready_pred(void) { return v_db_script_ready(); }

/* Tests run against a persistent local redis-server and must be
 * idempotent across repeated `make test` invocations — delete any key
 * a test is about to (re)create before asserting on its initial state. */
static void del_key_sync(const char *key)
{
    g_reply_seen = 0;
    v_port_db_cmd(v_port_db_shard_of(key), capture_cb, NULL, "DEL %s", key);
    test_wait_until(reply_seen_pred, 2000);
}

void test_db_script(void)
{
    printf("-- test_db_script --\n");

    CHECK(test_wait_until(scripts_ready_pred, 2000));
    CHECK(v_db_script_ready());

    /* CAS write on a fresh key: exp_ver=0 must succeed (returns 1). */
    const char *key = "pdu_7:test_db_script_1";
    del_key_sync(key);
    const char *keys[1] = { key };
    const char *argv[3] = { "0", "hello-blob", "0" };
    size_t argvlen[3] = { 1, strlen("hello-blob"), 1 };

    g_reply_seen = 0;
    uint8_t shard = v_port_db_shard_of(key);
    CHECK(v_db_evalsha(shard, V_SCRIPT_SESS_CAS_WRITE, 1, keys, argv, argvlen, 3,
                        capture_cb, NULL) == 0);
    CHECK(test_wait_until(reply_seen_pred, 2000));
    CHECK(g_reply_status == 0);
    CHECK(g_reply_copy.type == V_DB_REPLY_INTEGER);
    CHECK(g_reply_copy.integer == 1);

    /* Same key, exp_ver=0 again: must now return 0 (already exists). */
    g_reply_seen = 0;
    CHECK(v_db_evalsha(shard, V_SCRIPT_SESS_CAS_WRITE, 1, keys, argv, argvlen, 3,
                        capture_cb, NULL) == 0);
    CHECK(test_wait_until(reply_seen_pred, 2000));
    CHECK(g_reply_copy.integer == 0);

    /* Correct ver (1) succeeds and bumps to 2. */
    const char *argv2[3] = { "1", "second-blob", "0" };
    size_t argvlen2[3] = { 1, strlen("second-blob"), 1 };
    g_reply_seen = 0;
    CHECK(v_db_evalsha(shard, V_SCRIPT_SESS_CAS_WRITE, 1, keys, argv2, argvlen2, 3,
                        capture_cb, NULL) == 0);
    CHECK(test_wait_until(reply_seen_pred, 2000));
    CHECK(g_reply_copy.integer == 1);

    /* Delete with wrong ver -> 0 (conflict). */
    const char *dargv_wrong[1] = { "1" };
    size_t dargvlen_wrong[1] = { 1 };
    g_reply_seen = 0;
    CHECK(v_db_evalsha(shard, V_SCRIPT_SESS_DELETE, 1, keys, dargv_wrong, dargvlen_wrong, 1,
                        capture_cb, NULL) == 0);
    CHECK(test_wait_until(reply_seen_pred, 2000));
    CHECK(g_reply_copy.integer == 0);

    /* Delete with correct ver (2) -> 1. */
    const char *dargv_ok[1] = { "2" };
    size_t dargvlen_ok[1] = { 1 };
    g_reply_seen = 0;
    CHECK(v_db_evalsha(shard, V_SCRIPT_SESS_DELETE, 1, keys, dargv_ok, dargvlen_ok, 1,
                        capture_cb, NULL) == 0);
    CHECK(test_wait_until(reply_seen_pred, 2000));
    CHECK(g_reply_copy.integer == 1);

    /* Delete again -> -1 (gone). */
    g_reply_seen = 0;
    CHECK(v_db_evalsha(shard, V_SCRIPT_SESS_DELETE, 1, keys, dargv_ok, dargvlen_ok, 1,
                        capture_cb, NULL) == 0);
    CHECK(test_wait_until(reply_seen_pred, 2000));
    CHECK(g_reply_copy.integer == -1);

    /* Simulated Sentinel promotion: force a shard reconnect, which in
     * production would land on a replica with an empty script cache.
     * Exercises the reconnect -> reload path. */
    CHECK(v_stub_db_test_force_reconnect(shard) == 0);
    CHECK(test_wait_until(scripts_ready_pred, 2000));

    printf("test_db_script: done\n");
}

static int g_flush_done;
static void flush_cb(int status, const v_db_reply_t *reply, void *arg)
{
    (void)status; (void)reply; (void)arg;
    g_flush_done = 1;
}
static int flush_done_pred(void) { return g_flush_done; }

void test_db_script_noscript(void)
{
    printf("-- test_db_script_noscript --\n");
    CHECK(test_wait_until(scripts_ready_pred, 2000));

    const char *key = "pdu_9:test_db_script_noscript";
    del_key_sync(key);
    uint8_t shard = v_port_db_shard_of(key);

    /* Real SCRIPT FLUSH on the shard's connection: the server now has
     * no cached scripts at all, but v_db_script's local sha table still
     * thinks it does — exactly the post-failover state plan.md §4
     * describes. */
    g_flush_done = 0;
    CHECK(v_port_db_cmd(shard, flush_cb, NULL, "SCRIPT FLUSH") == 0);
    CHECK(test_wait_until(flush_done_pred, 2000));

    const char *keys[1] = { key };
    const char *argv[3] = { "0", "post-flush-blob", "0" };
    size_t argvlen[3] = { 1, strlen("post-flush-blob"), 1 };

    g_reply_seen = 0;
    CHECK(v_db_evalsha(shard, V_SCRIPT_SESS_CAS_WRITE, 1, keys, argv, argvlen, 3,
                        capture_cb, NULL) == 0);
    /* Give it enough iterations to cover: EVALSHA -> NOSCRIPT -> SCRIPT
     * LOAD -> EVALSHA retry -> reply. */
    CHECK(test_wait_until(reply_seen_pred, 4000));

    /* The retry must have succeeded transparently: caller sees a normal
     * integer reply, not an error. */
    CHECK(g_reply_status == 0);
    CHECK(g_reply_copy.type == V_DB_REPLY_INTEGER);
    CHECK(g_reply_copy.integer == 1);

    printf("test_db_script_noscript: done\n");
}
