#include "test_util.h"
#include "test_common.h"
#include "v_node_state.h"
#include "v_port_db.h"
#include "v_common.h"

#include <stdio.h>
#include <string.h>

static int g_seen5;
static int g_status5;
static uint32_t g_ts5;
static void recovery_cb(int status, uint32_t ts, void *arg)
{
    (void)arg;
    g_status5 = status;
    g_ts5 = ts;
    g_seen5 = 1;
}
static int seen5_pred(void) { return g_seen5; }

static int g_ack_seen5;
static void ack_cb5(int status, const v_db_reply_t *reply, void *arg)
{
    (void)status; (void)reply; (void)arg;
    g_ack_seen5 = 1;
}
static int ack_seen5_pred(void) { return g_ack_seen5; }

/* plan.md §8.1 / §9.3 "Recovery Time Stamp stable across simulated
 * restarts": every pod boot must read the SAME value a prior cold
 * start wrote, not derive its own from local boot time. Simulated here
 * by tearing v_node_state down and reinitializing with the same
 * node_id, standing in for a second pod (or a restarted one) coming up
 * against the same Redis-backed value. */
void test_node_state_recovery_ts_stable_across_restarts(void)
{
    printf("-- test_node_state_recovery_ts_stable_across_restarts --\n");

    static const uint8_t node_id[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01 };

    /* Clean slate. */
    char key[160];
    snprintf(key, sizeof(key), "upf:%02x%02x%02x%02x%02x:recovery",
             node_id[0], node_id[1], node_id[2], node_id[3], node_id[4]);
    g_ack_seen5 = 0;
    v_port_db_cmd(v_port_db_shard_of(key), ack_cb5, NULL, "DEL %s", key);
    CHECK(test_wait_until(ack_seen5_pred, 2000));

    CHECK(v_node_state_init(node_id, sizeof(node_id)) == RET_CODE_OK);
    CHECK(!v_node_state_recovery_ts_ready());

    g_seen5 = 0;
    CHECK(v_node_state_load_recovery_ts(recovery_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(seen5_pred, 2000));
    CHECK(g_status5 == RET_CODE_OK);
    uint32_t first_ts = g_ts5;
    CHECK(first_ts > 0);
    CHECK(v_node_state_recovery_ts_ready());
    CHECK(v_node_state_recovery_ts() == first_ts);

    /* "Restart": a second (or the same, restarted) pod boots and reads
     * the node's state fresh. */
    v_node_state_fini();
    CHECK(v_node_state_init(node_id, sizeof(node_id)) == RET_CODE_OK);
    CHECK(!v_node_state_recovery_ts_ready());

    g_seen5 = 0;
    CHECK(v_node_state_load_recovery_ts(recovery_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(seen5_pred, 2000));
    CHECK(g_status5 == RET_CODE_OK);
    CHECK(g_ts5 == first_ts); /* stable — did NOT pick up a fresh "now" */

    /* A THIRD "pod" too, for good measure. */
    v_node_state_fini();
    CHECK(v_node_state_init(node_id, sizeof(node_id)) == RET_CODE_OK);
    g_seen5 = 0;
    CHECK(v_node_state_load_recovery_ts(recovery_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(seen5_pred, 2000));
    CHECK(g_ts5 == first_ts);

    printf("test_node_state_recovery_ts_stable_across_restarts: done (ts=%u stable across 3 boots)\n", first_ts);
}

void test_node_state_force_new_recovery_ts(void)
{
    printf("-- test_node_state_force_new_recovery_ts --\n");

    static const uint8_t node_id[] = { 0xC0, 0x1D, 0xC0, 0xFF, 0xEE };
    char key[160];
    snprintf(key, sizeof(key), "upf:%02x%02x%02x%02x%02x:recovery",
             node_id[0], node_id[1], node_id[2], node_id[3], node_id[4]);

    /* Seed a deliberately stale sentinel value, simulating a prior
     * cold start long ago. */
    g_ack_seen5 = 0;
    v_port_db_cmd(v_port_db_shard_of(key), ack_cb5, NULL, "SET %s 12345", key);
    CHECK(test_wait_until(ack_seen5_pred, 2000));

    CHECK(v_node_state_init(node_id, sizeof(node_id)) == RET_CODE_OK);

    /* A normal load must NOT disturb the existing value (rolling
     * update / ordinary boot path). */
    g_seen5 = 0;
    CHECK(v_node_state_load_recovery_ts(recovery_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(seen5_pred, 2000));
    CHECK(g_ts5 == 12345);

    /* A deliberate cold start DOES overwrite it. */
    g_seen5 = 0;
    CHECK(v_node_state_force_new_recovery_ts(recovery_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(seen5_pred, 2000));
    CHECK(g_status5 == RET_CODE_OK);
    CHECK(g_ts5 != 12345);
    CHECK(v_node_state_recovery_ts() == g_ts5);

    printf("test_node_state_force_new_recovery_ts: done\n");
}

static int g_assoc_seen;
static int g_assoc_status, g_assoc_associated;
static uint64_t g_assoc_smf;
static void assoc_query_cb(int status, int associated, uint64_t smf, void *arg)
{
    (void)arg;
    g_assoc_status = status;
    g_assoc_associated = associated;
    g_assoc_smf = smf;
    g_assoc_seen = 1;
}
static int assoc_seen_pred(void) { return g_assoc_seen; }

static int g_simple_seen5, g_simple_status5;
static void simple_cb5(int status, void *arg)
{
    (void)arg;
    g_simple_status5 = status;
    g_simple_seen5 = 1;
}
static int simple_seen5_pred(void) { return g_simple_seen5; }

void test_node_state_association(void)
{
    printf("-- test_node_state_association --\n");

    static const uint8_t node_id[] = { 0xAA, 0xBB, 0xCC };
    CHECK(v_node_state_init(node_id, sizeof(node_id)) == RET_CODE_OK);

    g_simple_seen5 = 0;
    CHECK(v_node_state_clear_associated(simple_cb5, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(simple_seen5_pred, 2000));

    g_assoc_seen = 0;
    CHECK(v_node_state_query_associated(assoc_query_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(assoc_seen_pred, 2000));
    CHECK(g_assoc_status == RET_CODE_OK);
    CHECK(g_assoc_associated == 0);

    g_simple_seen5 = 0;
    CHECK(v_node_state_set_associated(0x5544332211ull, simple_cb5, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(simple_seen5_pred, 2000));

    /* Simulate a different pod (or the same one, freshly booted mid-
     * association) learning the state purely from Redis. */
    v_node_state_fini();
    CHECK(v_node_state_init(node_id, sizeof(node_id)) == RET_CODE_OK);

    g_assoc_seen = 0;
    CHECK(v_node_state_query_associated(assoc_query_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(assoc_seen_pred, 2000));
    CHECK(g_assoc_associated == 1);
    CHECK(g_assoc_smf == 0x5544332211ull);

    printf("test_node_state_association: done\n");
}
