#include "v_id_alloc.h"
#include "v_seid.h"
#include "v_db_script.h"
#include "v_port_db.h"
#include "v_common.h"
#include "v_log.h"

#include <rte_ring.h>
#include <rte_malloc.h>
#include <rte_errno.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define V_ID_RING_SIZE 1024 /* power of 2, several blocks of headroom */

static struct rte_ring **g_seid_ring;
static struct rte_ring **g_teid_ring;
static uint8_t *g_seid_refill_inflight;
static uint8_t *g_teid_refill_inflight;
static uint8_t *g_teid_next_seeded;
static int g_initialized;

/* Hashes just the decimal part_id, not a full key — guarantees
 * co-location of vpe:teid:<part>:free and vpe:teid:<part>:next on the
 * same shard regardless of whether v_port_db_shard_of() is itself
 * partid-derived (see CLAUDE.md and INTEGRATION.md). */
static uint8_t shard_for_part(uint16_t part_id)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", part_id);
    return v_port_db_shard_of(buf);
}

/* --- SEID: plain INCRBY block lease, no Lua needed (single key). --- */

/* INCRBY reply handler: the returned counter value is the END of the
 * newly leased block, so the block's start is value - V_ID_BLK_SIZE.
 * Encodes and enqueues every id in the block. */
static void seid_refill_reply_cb(int status, const v_db_reply_t *reply, void *arg)
{
    uint16_t part_id = (uint16_t)(uintptr_t)arg;
    g_seid_refill_inflight[part_id] = 0;

    if (status != RET_CODE_OK || !reply || reply->type != V_DB_REPLY_INTEGER) {
        V_LOG(ERR, "DPDB", "seid refill failed part=%u", part_id);
        return;
    }

    uint64_t base = (uint64_t)reply->integer - V_ID_BLK_SIZE;
    for (uint32_t i = 0; i < V_ID_BLK_SIZE; i++) {
        uint64_t seid;
        if (v_seid_encode(part_id, base + i, &seid) != RET_CODE_OK) {
            V_LOG(ERR, "DPDB", "seid encode failed part=%u local=%lu", part_id, base + i);
            continue;
        }
        if (rte_ring_enqueue(g_seid_ring[part_id], (void *)(uintptr_t)seid) != 0) {
            V_LOG(WARNING, "DPDB", "seid ring full during refill part=%u", part_id);
            break;
        }
    }
    V_LOG(DEBUG, "DPDB", "seid refill done part=%u base=%lu count=%u", part_id, base, V_ID_BLK_SIZE);
}

/* Triggers an async block lease when a partition's SEID ring drops
 * below the watermark. No-op if a refill for this partition is already
 * in flight (at most one outstanding INCRBY per ring). */
static void seid_maybe_refill(uint16_t part_id)
{
    if (rte_ring_count(g_seid_ring[part_id]) >= V_ID_WATERMARK)
        return;
    if (g_seid_refill_inflight[part_id])
        return;
    g_seid_refill_inflight[part_id] = 1;

    char key[32];
    snprintf(key, sizeof(key), "vpe:seid:%u:next", part_id);
    uint8_t shard = shard_for_part(part_id);

    if (v_port_db_cmd(shard, seid_refill_reply_cb, (void *)(uintptr_t)part_id,
                       "INCRBY %s %d", key, V_ID_BLK_SIZE) != RET_CODE_OK) {
        g_seid_refill_inflight[part_id] = 0;
        V_LOG(ERR, "DPDB", "seid refill dispatch failed part=%u", part_id);
    }
}

/* Public: dequeue one SEID for part_id, kicking off a background
 * refill first if the ring is running low. Returns 0 (never a valid
 * SEID) if the ring is dry — caller maps that to NO_RESOURCES_AVAILABLE. */
uint64_t v_seid_alloc(uint16_t part_id)
{
    seid_maybe_refill(part_id);

    void *obj;
    if (rte_ring_dequeue(g_seid_ring[part_id], &obj) != 0) {
        V_LOG(WARNING, "DPDB", "seid ring dry, part=%u", part_id);
        return 0;
    }
    return (uint64_t)(uintptr_t)obj;
}

/* --- TEID: prefer free list, fall back to counter (v_db_script Lua). --- */

/* Parses one Redis bulk-string array element as a decimal integer —
 * every element of the teid_refill Lua script's reply arrives as a
 * string, regardless of the tag ("range" or "list"). */
static int parse_u64(const v_db_reply_t *r, uint64_t *out)
{
    if (!r || r->type != V_DB_REPLY_STRING || !r->str)
        return RET_CODE_ERR;
    char buf[32];
    size_t n = r->len < sizeof(buf) - 1 ? r->len : sizeof(buf) - 1;
    memcpy(buf, r->str, n);
    buf[n] = '\0';
    *out = strtoull(buf, NULL, 10);
    return RET_CODE_OK;
}

/* Encodes a local index into a full TEID and enqueues it — shared by
 * both the "range" (fresh from the counter) and "list" (reclaimed from
 * the free list) branches of teid_refill_reply_cb(). */
static void enqueue_teid(uint16_t part_id, uint64_t local)
{
    uint32_t teid;
    if (v_teid_encode(part_id, (uint32_t)local, &teid) != RET_CODE_OK) {
        V_LOG(ERR, "DPDB", "teid encode failed part=%u local=%lu", part_id, local);
        return;
    }
    if (rte_ring_enqueue(g_teid_ring[part_id], (void *)(uintptr_t)teid) != 0)
        V_LOG(WARNING, "DPDB", "teid ring full during refill part=%u", part_id);
}

/* teid_refill Lua script reply handler. Reply is a tagged array:
 * {'range', start, count} when pulled from the counter, or
 * {'list', id, id, ...} when pulled from the free list (plan.md §6.3). */
static void teid_refill_reply_cb(int status, const v_db_reply_t *reply, void *arg)
{
    uint16_t part_id = (uint16_t)(uintptr_t)arg;
    g_teid_refill_inflight[part_id] = 0;

    if (status != RET_CODE_OK || !reply || reply->type != V_DB_REPLY_ARRAY || reply->nelements < 1) {
        V_LOG(ERR, "DPDB", "teid refill failed part=%u", part_id);
        return;
    }

    const v_db_reply_t *tag = &reply->element[0];
    if (tag->type != V_DB_REPLY_STRING) {
        V_LOG(ERR, "DPDB", "teid refill: bad tag type part=%u", part_id);
        return;
    }

    if (tag->len == 5 && strncmp(tag->str, "range", 5) == 0) {
        if (reply->nelements != 3) {
            V_LOG(ERR, "DPDB", "teid refill: malformed range reply part=%u", part_id);
            return;
        }
        uint64_t base, count;
        if (parse_u64(&reply->element[1], &base) != RET_CODE_OK ||
            parse_u64(&reply->element[2], &count) != RET_CODE_OK) {
            V_LOG(ERR, "DPDB", "teid refill: unparsable range part=%u", part_id);
            return;
        }
        for (uint64_t i = 0; i < count; i++)
            enqueue_teid(part_id, base + i);
        V_LOG(DEBUG, "DPDB", "teid refill (range) part=%u base=%lu count=%lu", part_id, base, count);
    } else if (tag->len == 4 && strncmp(tag->str, "list", 4) == 0) {
        for (size_t i = 1; i < reply->nelements; i++) {
            uint64_t local;
            if (parse_u64(&reply->element[i], &local) == RET_CODE_OK)
                enqueue_teid(part_id, local);
        }
        V_LOG(DEBUG, "DPDB", "teid refill (list) part=%u count=%zu", part_id, reply->nelements - 1);
    } else {
        V_LOG(ERR, "DPDB", "teid refill: unknown tag part=%u", part_id);
    }
}

/* Ack-only callback for the one-time SETNX that seeds a partition's
 * TEID counter to 1 (see the ordering comment in teid_maybe_refill()). */
static void seed_reply_cb(int status, const v_db_reply_t *reply, void *arg)
{
    (void)reply; (void)arg;
    if (status != RET_CODE_OK)
        V_LOG(ERR, "DPDB", "teid next-counter seed failed");
}

/* Triggers an async teid_refill Lua call when a partition's TEID ring
 * drops below the watermark. Same in-flight guard as seid_maybe_refill(). */
static void teid_maybe_refill(uint16_t part_id)
{
    if (rte_ring_count(g_teid_ring[part_id]) >= V_ID_WATERMARK)
        return;
    if (g_teid_refill_inflight[part_id])
        return;
    g_teid_refill_inflight[part_id] = 1;

    char free_key[32], next_key[32];
    snprintf(free_key, sizeof(free_key), "vpe:teid:%u:free", part_id);
    snprintf(next_key, sizeof(next_key), "vpe:teid:%u:next", part_id);
    char blk_str[8], floor_str[8];
    snprintf(blk_str, sizeof(blk_str), "%u", V_ID_BLK_SIZE);
    snprintf(floor_str, sizeof(floor_str), "%u", V_TEID_FREE_FLOOR);

    const char *keys[2] = { free_key, next_key };
    const char *argv[2] = { blk_str, floor_str };
    size_t argvlen[2] = { strlen(blk_str), strlen(floor_str) };
    uint8_t shard = shard_for_part(part_id);

    /* Local index 0 is reserved (plan.md §7 "TEID 0 is reserved. Start
     * local indices at 1"), but a fresh vpe:teid:<part>:next counter
     * INCRBYs from 0, so the very first block ever leased for a
     * partition would otherwise start at local=0. Seed the counter to 1
     * once, before that partition's first refill. Both commands go out
     * on the same shard's single connection, and Redis executes a
     * client's commands on one connection in the order sent, so this
     * SETNX is guaranteed to land before the EVALSHA below reads/bumps
     * the same key — no need to wait for its reply. */
    if (!g_teid_next_seeded[part_id]) {
        g_teid_next_seeded[part_id] = 1;
        v_port_db_cmd(shard, seed_reply_cb, NULL, "SETNX %s 1", next_key);
    }

    if (v_db_evalsha(shard, V_SCRIPT_TEID_REFILL, 2, keys, argv, argvlen, 2,
                      teid_refill_reply_cb, (void *)(uintptr_t)part_id) != RET_CODE_OK) {
        g_teid_refill_inflight[part_id] = 0;
        V_LOG(ERR, "DPDB", "teid refill dispatch failed part=%u", part_id);
    }
}

/* Public: dequeue one TEID for part_id. Same dry-ring/refill shape as
 * v_seid_alloc(); 0 is the "none available" sentinel here too. */
uint32_t v_teid_alloc(uint16_t part_id)
{
    teid_maybe_refill(part_id);

    void *obj;
    if (rte_ring_dequeue(g_teid_ring[part_id], &obj) != 0) {
        V_LOG(WARNING, "DPDB", "teid ring dry, part=%u", part_id);
        return 0;
    }
    return (uint32_t)(uintptr_t)obj;
}

/* Ack-only callback for the free-list RPUSH — a lost ack just means the
 * id sits unreclaimed a little longer, never a correctness issue. */
static void teid_free_reply_cb(int status, const v_db_reply_t *reply, void *arg)
{
    (void)reply; (void)arg;
    if (status != RET_CODE_OK)
        V_LOG(ERR, "DPDB", "teid free RPUSH failed");
}

/* Public: return a TEID's local index to the partition's Redis free
 * list. Deliberately no v_seid_free() counterpart — see plan.md §6.3. */
void v_teid_free(uint16_t part_id, uint32_t teid)
{
    char key[32];
    snprintf(key, sizeof(key), "vpe:teid:%u:free", part_id);
    uint8_t shard = shard_for_part(part_id);
    uint32_t local = v_teid_local(teid);

    if (v_port_db_cmd(shard, teid_free_reply_cb, NULL, "RPUSH %s %u", key, local) != RET_CODE_OK)
        V_LOG(ERR, "DPDB", "teid free RPUSH dispatch failed part=%u teid=%u", part_id, teid);
}

/* --- lifecycle --- */

/* Public: allocates the 1024 SEID/TEID ring pairs and their
 * bookkeeping arrays, then immediately fires an initial refill for
 * every partition. Callers must wait for v_id_alloc_ready() before
 * accepting traffic. */
int v_id_alloc_init(void)
{
    if (g_initialized) {
        V_LOG(WARNING, "MEM", "v_id_alloc_init called twice");
        return RET_CODE_ERR;
    }

    g_seid_ring = rte_zmalloc("v_seid_ring_tbl", V_NUM_PARTS * sizeof(*g_seid_ring), 0);
    g_teid_ring = rte_zmalloc("v_teid_ring_tbl", V_NUM_PARTS * sizeof(*g_teid_ring), 0);
    g_seid_refill_inflight = rte_zmalloc("v_seid_inflight", V_NUM_PARTS, 0);
    g_teid_refill_inflight = rte_zmalloc("v_teid_inflight", V_NUM_PARTS, 0);
    g_teid_next_seeded = rte_zmalloc("v_teid_next_seeded", V_NUM_PARTS, 0);
    if (!g_seid_ring || !g_teid_ring || !g_seid_refill_inflight || !g_teid_refill_inflight ||
        !g_teid_next_seeded) {
        V_LOG(ERR, "MEM", "v_id_alloc_init: rte_zmalloc failed");
        return RET_CODE_ERR;
    }

    for (uint16_t part = 0; part < V_NUM_PARTS; part++) {
        char name[32];
        snprintf(name, sizeof(name), "v_seid_ring_%u", part);
        /* flags=0: multi-producer/multi-consumer — any of the 4 workers
         * may allocate in any partition (plan.md §6.2 "Ring flags: MC/MP"). */
        g_seid_ring[part] = rte_ring_create(name, V_ID_RING_SIZE, SOCKET_ID_ANY, 0);
        snprintf(name, sizeof(name), "v_teid_ring_%u", part);
        g_teid_ring[part] = rte_ring_create(name, V_ID_RING_SIZE, SOCKET_ID_ANY, 0);
        if (!g_seid_ring[part] || !g_teid_ring[part]) {
            V_LOG(ERR, "MEM", "ring create failed part=%u: %s", part, rte_strerror(rte_errno));
            return RET_CODE_ERR;
        }
    }

    g_initialized = 1;

    for (uint16_t part = 0; part < V_NUM_PARTS; part++) {
        seid_maybe_refill(part);
        teid_maybe_refill(part);
    }

    return RET_CODE_OK;
}

/* Public: frees every ring and bookkeeping array. */
void v_id_alloc_fini(void)
{
    if (!g_initialized)
        return;
    for (uint16_t part = 0; part < V_NUM_PARTS; part++) {
        rte_ring_free(g_seid_ring[part]);
        rte_ring_free(g_teid_ring[part]);
    }
    rte_free(g_seid_ring);
    rte_free(g_teid_ring);
    rte_free(g_seid_refill_inflight);
    rte_free(g_teid_refill_inflight);
    rte_free(g_teid_next_seeded);
    g_seid_ring = NULL;
    g_teid_ring = NULL;
    g_initialized = 0;
}

/* Public: true once every partition's SEID and TEID ring has received
 * at least its first refill. O(V_NUM_PARTS) — fine for a startup-time
 * poll, not meant to be called on the hot path. */
int v_id_alloc_ready(void)
{
    if (!g_initialized)
        return 0;
    for (uint16_t part = 0; part < V_NUM_PARTS; part++) {
        if (rte_ring_count(g_seid_ring[part]) == 0)
            return 0;
        if (rte_ring_count(g_teid_ring[part]) == 0)
            return 0;
    }
    return 1;
}
