#include "test_util.h"
#include "test_common.h"
#include "v_id_alloc.h"
#include "v_seid.h"
#include "v_db_script.h"
#include "v_port_db.h"
#include "v_common.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

static uint64_t alloc_seid_blocking(uint16_t part)
{
    for (int i = 0; i < 5000; i++) {
        uint64_t id = v_seid_alloc(part);
        if (id != 0)
            return id;
        v_port_db_poll(1);
        struct timespec ts = { 0, 200000L };
        nanosleep(&ts, NULL);
    }
    return 0;
}

static uint32_t alloc_teid_blocking(uint16_t part)
{
    for (int i = 0; i < 5000; i++) {
        uint32_t id = v_teid_alloc(part);
        if (id != 0)
            return id;
        v_port_db_poll(1);
        struct timespec ts = { 0, 200000L };
        nanosleep(&ts, NULL);
    }
    return 0;
}

#define UNIQ_N 600

void test_id_alloc_uniqueness(void)
{
    printf("-- test_id_alloc_uniqueness --\n");
    CHECK(v_id_alloc_ready());

    uint16_t part = 13;

    static uint64_t seids[UNIQ_N];
    for (int i = 0; i < UNIQ_N; i++) {
        seids[i] = alloc_seid_blocking(part);
        CHECK(seids[i] != 0);
        uint16_t p;
        CHECK(v_seid_validate(seids[i], &p) == RET_CODE_OK);
        CHECK(p == part);
    }
    int dup = 0;
    for (int i = 0; i < UNIQ_N && !dup; i++)
        for (int j = i + 1; j < UNIQ_N; j++)
            if (seids[i] == seids[j]) { dup = 1; break; }
    CHECK(!dup);

    static uint32_t teids[UNIQ_N];
    for (int i = 0; i < UNIQ_N; i++) {
        teids[i] = alloc_teid_blocking(part);
        CHECK(teids[i] != 0);
        uint16_t p;
        CHECK(v_teid_validate(teids[i], &p) == RET_CODE_OK);
        CHECK(p == part);
    }
    dup = 0;
    for (int i = 0; i < UNIQ_N && !dup; i++)
        for (int j = i + 1; j < UNIQ_N; j++)
            if (teids[i] == teids[j]) { dup = 1; break; }
    CHECK(!dup);

    printf("test_id_alloc_uniqueness: done (%d seids, %d teids, no dup, refill exercised)\n",
           UNIQ_N, UNIQ_N);
}

/* --- free-list floor, exercised directly against the Lua script so the
 * test doesn't need V_TEID_FREE_FLOOR (1024) real pushes: the floor is
 * a script ARGV, not a compiled-in constant. --- */

/*
 * NOTE ON REPLY LIFETIME: a v_db_reply_t (and everything it points to)
 * is only valid for the duration of the callback that receives it — the
 * stub frees/reuses the underlying hiredis reply and its own scratch
 * arena as soon as the callback returns (see v_stub_db.c). So all
 * assertions below run INSIDE the callback, not after test_wait_until()
 * returns; only a plain "done" flag crosses that boundary.
 */

static int g_reply_seen;
static void done_cb(int status, const v_db_reply_t *reply, void *arg)
{
    (void)status; (void)reply; (void)arg;
    g_reply_seen = 1;
}
static int reply_seen_pred(void) { return g_reply_seen; }

/* v_port_db_cmd is printf-style like hiredis's redisCommand: literal
 * whitespace in fmt delimits argv tokens, and each %s/%d substitutes
 * exactly one token — it does NOT re-tokenize a pre-rendered string
 * handed in through a single bare "%s". del_sync/rpush_sync below call
 * it directly with a real multi-token format string for that reason. */
static void del_sync(uint8_t shard, const char *key)
{
    g_reply_seen = 0;
    v_port_db_cmd(shard, done_cb, NULL, "DEL %s", key);
    test_wait_until(reply_seen_pred, 2000);
}

static void rpush_sync(uint8_t shard, const char *key, int value)
{
    g_reply_seen = 0;
    v_port_db_cmd(shard, done_cb, NULL, "RPUSH %s %d", key, value);
    test_wait_until(reply_seen_pred, 2000);
}

static void expect_llen_cb(int status, const v_db_reply_t *reply, void *arg)
{
    long long *expect = (long long *)arg;
    CHECK(status == RET_CODE_OK);
    CHECK(reply && reply->type == V_DB_REPLY_INTEGER);
    if (reply)
        CHECK(reply->integer == *expect);
    g_reply_seen = 1;
}

static void expect_range_cb(int status, const v_db_reply_t *reply, void *arg)
{
    (void)arg;
    CHECK(status == RET_CODE_OK);
    CHECK(reply && reply->type == V_DB_REPLY_ARRAY && reply->nelements == 3);
    if (reply && reply->nelements == 3)
        CHECK(reply->element[0].type == V_DB_REPLY_STRING && reply->element[0].len == 5 &&
              strncmp(reply->element[0].str, "range", 5) == 0);
    g_reply_seen = 1;
}

static void expect_list_cb(int status, const v_db_reply_t *reply, void *arg)
{
    long long *expect_n = (long long *)arg;
    CHECK(status == RET_CODE_OK);
    CHECK(reply && reply->type == V_DB_REPLY_ARRAY);
    if (reply) {
        CHECK((long long)reply->nelements == *expect_n + 1);
        CHECK(reply->element[0].type == V_DB_REPLY_STRING && reply->element[0].len == 4 &&
              strncmp(reply->element[0].str, "list", 4) == 0);
    }
    g_reply_seen = 1;
}

void test_id_alloc_teid_floor(void)
{
    printf("-- test_id_alloc_teid_floor --\n");

    const char *free_key = "vpe:teid:9001:free";
    const char *next_key = "vpe:teid:9001:next";
    uint8_t shard = v_port_db_shard_of("9001");

    del_sync(shard, free_key);
    del_sync(shard, next_key);

    /* floor=2: push exactly 2 (== floor, NOT > floor) -> must fall
     * through to the counter, i.e. tag "range". */
    rpush_sync(shard, free_key, 501);
    rpush_sync(shard, free_key, 502);

    long long expect2_pre = 2;
    g_reply_seen = 0;
    v_port_db_cmd(shard, expect_llen_cb, &expect2_pre, "LLEN %s", free_key);
    CHECK(test_wait_until(reply_seen_pred, 2000));

    const char *keys[2] = { free_key, next_key };
    const char *argv_range[2] = { "5", "2" }; /* blocksize=5, floor=2 */
    size_t argvlen_range[2] = { 1, 1 };

    g_reply_seen = 0;
    CHECK(v_db_evalsha(shard, V_SCRIPT_TEID_REFILL, 2, keys, argv_range, argvlen_range, 2,
                        expect_range_cb, NULL) == RET_CODE_OK);
    CHECK(test_wait_until(reply_seen_pred, 2000));

    /* Free list must be untouched (still 2 entries) — the floor path
     * never LPOPs. */
    long long expect2 = 2;
    g_reply_seen = 0;
    v_port_db_cmd(shard, expect_llen_cb, &expect2, "LLEN %s", free_key);
    CHECK(test_wait_until(reply_seen_pred, 2000));

    /* Push a 3rd entry: LLEN(3) > floor(2) -> must now pull from the
     * free list, tag "list", and drain all 3 entries. */
    rpush_sync(shard, free_key, 503);

    const char *argv_list[2] = { "5", "2" };
    size_t argvlen_list[2] = { 1, 1 };
    long long expect3 = 3;
    g_reply_seen = 0;
    CHECK(v_db_evalsha(shard, V_SCRIPT_TEID_REFILL, 2, keys, argv_list, argvlen_list, 2,
                        expect_list_cb, &expect3) == RET_CODE_OK);
    CHECK(test_wait_until(reply_seen_pred, 2000));

    /* Free list must now be empty. */
    long long expect0 = 0;
    g_reply_seen = 0;
    v_port_db_cmd(shard, expect_llen_cb, &expect0, "LLEN %s", free_key);
    CHECK(test_wait_until(reply_seen_pred, 2000));

    printf("test_id_alloc_teid_floor: done\n");
}
