# v_txn

Timeout sweeper layered on `v_port_txn`'s single hard timeout.
plan.md §3.5.

## Public API (`inc/v_txn.h`)

- `v_txn_sm_init()` — registers `timeout_handler` via
  `v_port_txn_on_timeout()`. Call once at startup, after
  `v_port_txn_init()`.

Everything else is internal — `v_flow.c` talks to `v_port_txn.h`
directly (create/find/set_state), not to this module. This module
*only* decides what a timeout means, per state.

## Dependencies

`v_port_txn.h`, `v_port_mem.h` (frees `t->ctx`), `v_port_pfcp.h` (frees
`t->req`), `v_sess_store.h` (the VDP_WAIT/establishment and
DB_CONFIRM_WAIT recovery actions are session-store calls).

## Per-state timeout behavior (do not change without re-reading plan.md §3.5)

- `TXN_ST_RECV` / `TXN_ST_DB_READ_WAIT`: nothing was ever written —
  free and destroy, no Redis action.
- `TXN_ST_DB_WRITE_WAIT`: a CAS write was issued but never confirmed.
  No explicit cleanup — establishment's pending TTL self-cleans if the
  write actually landed; otherwise the SMF's retransmit (or the
  original caller giving up) recovers cleanly either way.
- `TXN_ST_VDP_WAIT`: **`t->db_ver == 0` is the establishment/
  modification signal** (a fresh session has no prior read, so its
  `db_ver` is 0 going into the CAS write; an existing session's read
  always returns `ver >= 1`). Establishment (`db_ver==0`): delete the
  now-orphaned pending record instead of waiting out the 30s TTL.
  Modification (`db_ver!=0`): **KNOWN GAP** — cannot compensate the
  write-back here, only log and free. See the header's `KNOWN GAP`
  comment and `INTEGRATION.md` §3 for the full reasoning (the
  compensating write needs the pre-modification ctx, which has nowhere
  to live in `struct v_txn` as given by the port).
- `TXN_ST_DB_CONFIRM_WAIT`: VDP already accepted, data's already
  durable — retry the `PERSIST` once, best-effort, rather than risk the
  pending TTL expiring under a live session.
- `TXN_ST_REPLIED`: should be unreachable (the txn should already be
  destroyed by the time it reaches this state) — logs CRIT if it ever
  fires, treat that as a real bug report, not routine noise.

## Invariants an agent must not break

- Every path must free `t->ctx` and `t->req` before `v_port_txn_destroy(t)`
  — this is what the S6 test suite actually asserts (`v_port_ctx_pool_in_use()`
  returns to baseline after every timeout path).
- Do not add a second timer field to `struct v_txn` — plan.md §3.5/§9.4
  is explicit that the reused table enforces exactly one hard timeout
  per transaction, and the state machine's job is to interpret that one
  timeout correctly per state, not to multiply timers.

## Tests

`test/test_txn_sm.c` — drives every state through the port's simulated
clock (`v_port_txn_test_advance_ms`), and specifically verifies the
VDP_WAIT/establishment path actually issues the Redis delete (not just
frees local memory).
