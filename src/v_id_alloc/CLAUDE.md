# v_id_alloc

SEID/TEID rings with Redis block leasing. plan.md §6.2, §6.3, §6.5.

## Public API (`inc/v_id_alloc.h`)

- `v_id_alloc_init()` — creates 1024 SEID ring + 1024 TEID ring pairs
  and immediately kicks off an async refill for every one of them.
  **Must be called after `v_db_script_ready()` is true** (TEID refill
  goes through `v_db_evalsha`, which needs the Lua SHA cached first —
  see `main.c`'s startup sequence and the bug it fixes, described
  there, if this ordering is ever "simplified" away).
- `v_id_alloc_ready()` — true once every partition's SEID and TEID ring
  has its first non-empty refill. Gate PFCP traffic on this.
- `v_seid_alloc(part_id)` / `v_teid_alloc(part_id)` — ring dequeue,
  returns 0 (never a valid id — see `v_seid.h`) on a dry ring; caller
  maps that to `PFCP_CAUSE_NO_RESOURCES_AVAILABLE`.
- `v_teid_free(part_id, teid)` — RPUSH the local index onto the Redis
  free list. No SEID equivalent exists on purpose.

## Dependencies

`v_seid.h` (encode/local-index math), `v_db_script.h` (TEID refill is a
Lua script — free-list-preferring, counter-fallback), `v_port_db.h`
(SEID refill is plain INCRBY, no Lua needed for a single key).

## Invariants an agent must not break

- **Ring flags are MP/MC (flags=0), not SP/SC.** Any worker may
  allocate in any partition — that's deliberate (plan.md §6.2: splitting
  into per-worker rings would quadruple the leak-on-pod-death exposure
  for no benefit, since establishment isn't hot enough to justify it).
- **Co-location trick:** `shard_for_part()` hashes just the decimal
  `<part_id>` string, not the full Redis key, specifically so
  `vpe:teid:<part>:free` and `vpe:teid:<part>:next` always land on the
  same shard (the Lua script touches both in one call — Redis requires
  that). Don't replace this with `v_port_db_shard_of()` on the full key
  without re-verifying co-location; see `INTEGRATION.md` §2.
- **The Redis TEID counter is seeded to 1, not 0**, via a one-time
  `SETNX ... 1` before each partition's first refill, relying on
  same-connection command ordering rather than waiting for the SETNX's
  reply. This exists because local index 0 is reserved (see
  `v_seid/CLAUDE.md`) — a fresh `INCRBY` counter starts from 0 and would
  otherwise hand out local=0 on a partition's very first block. This was
  a real bug the test suite caught; don't remove the seed step.
- Watermark refill (`V_ID_WATERMARK`) must stay async — never block a
  worker on Redis to refill. `seid_maybe_refill`/`teid_maybe_refill`
  track one in-flight refill per ring; don't stack a second.

## Tests

`test/test_id_alloc.c` — 600-allocation uniqueness check across forced
mid-stream refills (both SEID and TEID), and the free-list-floor Lua
test (floor is a script ARGV, not `V_TEID_FREE_FLOOR` itself, so the
test doesn't need 1024+ real pushes to exercise the branch).
