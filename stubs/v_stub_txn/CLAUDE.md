# v_stub_txn

Stage 1 stub for `v_port_txn.h` — the transaction table. **Deleted at
Stage 2**; see `INTEGRATION.md`.

## What it actually does

A plain fixed-capacity slot array (`g_slots[V_STUB_TXN_CAP]`) plus an
`rte_mempool` for the `struct v_txn` objects themselves —
`v_port_txn_find_by_seq`/`find_by_seid` are linear scans, which is fine
at the test suite's scale and is explicitly not something Stage 2
should copy (the real reused table presumably indexes properly).

Timeouts are driven by a **simulated clock** (`g_now_ms`), advanced only
by the test-only `v_port_txn_test_advance_ms()` — there is no real
timer here. Each `v_port_txn_create()` arms a deadline
`V_STUB_TXN_HARD_TIMEOUT_MS` (2000ms, a Stage-1-only constant) ahead of
the current simulated time; `v_port_txn_on_timeout()`'s callback fires
once per txn, tracked via a parallel `g_fired[]` array so it can't
double-fire.

## Things to know before touching this file

- `find_by_seq` reads `t->req` (via `v_port_pfcp_seq`/`smf_fseid`)
  rather than storing its own copy of those fields — this only works
  because callers set `t->req` immediately after `v_port_txn_create()`,
  before any lookup could plausibly race it (this is single-threaded
  Stage 1; a real concurrent table would need to handle this
  differently, which is exactly why plan.md leaves the real table's
  internals to Stage 2).
- `V_STUB_TXN_HARD_TIMEOUT_MS` is read by tests via
  `v_port_txn_test_hard_timeout_ms()` rather than hard-coded at call
  sites — if you change the constant, nothing else needs to change.
- This stub's `struct v_txn` fields are the *real* port contract (see
  `include/v_port_txn.h`), not stub-only scaffolding — `t->impl` here is
  always NULL (this stub never uses it), but `v_flow.c`'s modification
  path repurposes it as scratch storage for a ctx snapshot. That's a
  `v_flow`-side decision this stub doesn't need to know about; don't add
  stub-side logic that assumes anything about what `t->impl` holds.
