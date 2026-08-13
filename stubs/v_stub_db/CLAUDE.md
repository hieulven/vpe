# v_stub_db

Stage 1 stub for `v_port_db.h` — the DB connection + command layer.
**Deleted at Stage 2**, replaced with real bindings; see `INTEGRATION.md`.

## What it actually does

Talks to a **real local `redis-server`** via hiredis (not an in-memory
fake) — plan.md §3.6 prefers this "so the Lua is genuinely exercised."
Five logical shards are five separate hiredis async connections, all to
the same physical server (this sandbox has no 5-master Sentinel
topology) — see the file's top comment for exactly what that does and
doesn't prove.

## Test-only surface (`inc/v_stub_db_test.h`)

`v_stub_db_test_force_reconnect(shard)` — tears down and reconnects one
shard's connection, simulating a Sentinel promotion. Fires the
registered `v_db_reconnect_cb_t` again. **Does not exist in the real DB
layer** — Stage 2 doesn't need an equivalent, it's purely how this stub
lets the test suite exercise `v_db_script`'s reload-on-reconnect path
without a real failover available.

## Things to know before touching this file

- `V_STUB_DB_SHARDS` (5) hard-codes the shard count `v_port_db_shard_count()`
  reports — matches plan.md's "5 master/slave pairs."
- Env vars `VPE_TEST_REDIS_HOST` / `VPE_TEST_REDIS_PORT` override the
  target (default `127.0.0.1:6379`) — used by the test suite, not by
  anything in `src/`.
- `v_port_db_shard_of()` here is a plain FNV-1a hash of the whole key
  string — **this is exactly the behavior `v_id_alloc.c`'s
  `shard_for_part()` works around** (see that module's `CLAUDE.md`)
  because it does *not* guarantee co-location for two different keys
  sharing a partition number. Don't "fix" this stub to be
  partid-derived without also re-checking whether `shard_for_part()`
  becomes redundant — that's exactly the open question `INTEGRATION.md`
  §2 flags for Stage 2 to resolve against the *real* function.
- `g_scratch`/reply lifetime: a `v_db_reply_t` handed to a callback is
  only valid inside that callback — the scratch arena is reset on the
  next dispatched reply. This is not a stub-only quirk to "fix"; every
  caller in `src/` is written to respect it, and the real DB layer will
  have the same kind of constraint (hiredis frees its own reply objects
  right after the callback returns).
