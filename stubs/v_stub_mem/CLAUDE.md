# v_stub_mem

Stage 1 stub for `v_port_mem.h` — session-context allocation.
**Deleted at Stage 2**; see `INTEGRATION.md`.

## What it actually does

A **real `rte_mempool`** of fixed-size opaque blocks (`struct
v_stub_ses_ctx`, from `../v_stub_common.h`), not a malloc wrapper — so
`v_port_ctx_pool_in_use()` is a real, meaningful leak assertion, which
is exactly what `test/test_txn_sm.c` and `test/test_sess_store.c` rely
on. No per-lcore cache (`rte_mempool_create(..., 0 /* cache */, ...)`)
— deliberate, so `pool_in_use()` is exact rather than approximate
across lcores.

## Things to know before touching this file

- `V_STUB_CTX_MAGIC` guards every free/clear against a double-free or a
  foreign pointer — keep this if you change the internal struct.
- `v_port_ctx_clear()` preserves the magic and zeroes everything else —
  this is what CAS-retry loops in `v_sess_store`/`v_flow` callers use to
  reuse a ctx object across a retry without a free+realloc round trip.
- The stub's `struct v_stub_ses_ctx` layout is shared with
  `v_stub_pfcp` (via `../v_stub_common.h`) — both stubs stand in for
  what would, at Stage 2, be two separate reused modules that happen to
  agree on a real struct's layout. Don't let this stub and
  `v_stub_pfcp` disagree on the struct without updating both.
