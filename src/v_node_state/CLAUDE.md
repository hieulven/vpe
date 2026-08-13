# v_node_state

Node ID, Recovery Time Stamp, association state. plan.md §6.7.

## Public API (`inc/v_node_state.h`)

- `v_node_state_init(node_id, len)` — process-wide config in, hex-encodes
  it into the Redis key namespace. Call once at startup (Node ID is
  out of scope for Stage 1's config wiring per plan.md §0 — currently a
  placeholder constant in `main.c`).
- `v_node_state_load_recovery_ts(cb, arg)` — one round trip:
  `SET upf:<id>:recovery <now> NX GET`. Caches the result. **Call once
  at startup and use `v_node_state_recovery_ts()` afterward — never
  re-call this on the heartbeat hot path.**
- `v_node_state_force_new_recovery_ts(cb, arg)` — deliberate-cold-start
  ONLY. Unconditional `SET`, always wins. Not wired to anything
  automatic — plan.md §6.7 is explicit that a rolling update must
  never trigger this.
- `v_node_state_set_associated` / `_clear_associated` / `_query_associated`
  — association state as a Redis hash, so a pod that boots mid-
  association still answers correctly.

## Dependencies

`v_port_db.h` only.

## Invariants an agent must not break

- **Never derive Node ID or Recovery Time Stamp from pod identity**
  (hostname, IP, boot time). That's the exact bug this module exists to
  prevent — plan.md §6.7: a per-pod Recovery Time Stamp means every
  heartbeat that ECMP-lands on a different pod tells the SMF the UPF
  restarted, purging all sessions.
- The `SET ... NX GET` idiom is one round trip specifically *because*
  it's atomic: a NIL reply means **this call's** SET actually landed
  (first cold start wins), a string reply means an earlier cold start's
  value wins, unconditionally. Don't split this into a separate
  GET-then-maybe-SET — that reintroduces a race between pods.
- `v_node_state_force_new_recovery_ts` must stay a distinct, clearly-
  separate entry point from the normal load path. If you're tempted to
  add a parameter to `v_node_state_load_recovery_ts` that sometimes
  forces a new value, don't — the separation is what makes it obvious
  at every call site which behavior is happening.

## Tests

`test/test_node_state.c` — simulates repeated pod boots by tearing the
module down and reinitializing against the same `node_id`, and asserts
the recovery timestamp is bit-for-bit stable across three of them (the
S11 acceptance criterion). Also verifies `force_new_recovery_ts` is the
only path that actually changes it, and that an association set on one
simulated pod is visible to a freshly booted one.
