# v_db_script

EVAL/EVALSHA layered on the reused DB command layer, which lacks Lua
support. plan.md §4. First module built in Stage 1 — `v_id_alloc` and
`v_sess_store` both depend on it.

## Public API (`inc/v_db_script.h`)

- `v_db_script_init()` — registers the `SCRIPT LOAD`-on-(re)connect
  hook via `v_port_db_on_reconnect()`. **Call before `v_port_db_init()`**
  so the initial connect events (not just later Sentinel-promotion
  reconnects) are caught by the same hook.
- `v_db_script_ready()` — true once every shard has a cached SHA for
  every script.
- `v_db_evalsha(shard, script_id, keys, argv, argvlen, nargv, cb, arg)`
  — EVALSHA with automatic NOSCRIPT recovery: on NOSCRIPT, reloads the
  script to that shard and retries exactly once, then fails.

## Scripts (`inc/v_db_lua_scripts.h`)

Three scripts, transcribed verbatim from plan.md §6.1/§6.3 — this file
is meant to stay a byte-for-byte copy of the plan's Lua, not a
paraphrase. If the Lua ever needs to change, change it here and update
plan.md's copy too so they don't drift.

## Dependencies

`v_port_db.h` only.

## Invariants an agent must not break

- **`v_db_evalsha`'s `argv`/`argvlen` are binary-safe on purpose** — the
  session CAS-write script's `ARGV[2]` is a serialized blob that may
  contain embedded NULs. Do not "simplify" this back to NUL-terminated
  strings.
- **The script cache is per-shard, not global.** `g_sha[shard][id]`
  must be checked/set per shard; don't cache a single SHA and assume
  it's valid everywhere (Redis's own script cache isn't replicated
  either, which is the whole reason this reload-on-reconnect exists).
- **NOSCRIPT retries exactly once.** If the reload also fails, the
  caller must see a real failure, not an infinite retry loop.
- `v_evalsha_pending`/`v_script_load_ctx` objects come from
  `rte_mempool`s sized generously — if you add a caller with much
  larger request volume, check `V_DB_EVALSHA_POOL_CAP` /
  `V_DB_SCRIPT_LOAD_POOL_CAP` still cover the concurrent-in-flight count.

## Tests

`test/test_db_script.c` — CAS script outcomes end to end, plus a
NOSCRIPT recovery test that issues a **real** `SCRIPT FLUSH` against
local Redis (not a simulated error) and confirms the retry transparently
succeeds.
