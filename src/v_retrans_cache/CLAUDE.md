# v_retrans_cache

Cross-pod retransmission dedup. plan.md §6.6. **Load-bearing, not an
optimization** — without this, an SMF retransmit of an Establishment
that lands on a different pod allocates a second SEID/TEID and leaks
the first.

## Public API (`inc/v_retrans_cache.h`)

- `v_retrans_part(smf_fseid)` — routing-only partition hash. This is
  **not** a session `part_id` — Establishment has no SEID yet, so
  everything routes by `hash(smf_fseid)` instead. It only needs to be
  consistent between a lookup and its matching store.
- `v_retrans_lookup(part_id, smf_fseid, seq, cb, arg)` — GET.
- `v_retrans_store(part_id, smf_fseid, seq, resp, len)` — fire-and-forget
  SETEX, binary-safe (the cached value is a raw PFCP response byte
  string).

## Dependencies

`v_port_db.h` only. Single key per operation — no Lua needed, unlike
`v_sess_store`'s multi-field CAS.

## Invariants an agent must not break

- **Must stay in Redis, never in-process.** The whole point is
  deduplicating a retransmit that lands on a *different* pod after an
  SMF timeout — an in-process cache can't see that.
- `v_retrans_store` is genuinely fire-and-forget (no callback in the
  public signature, matching plan.md's given contract) — a lost cache
  write only risks one duplicate allocation on a future retransmit, not
  a correctness break. Don't add retry logic here; if a caller needs
  stronger guarantees, that belongs in the caller.
- Key format `txn_%u:%lu:%u` (partid, smf_fseid, seq) is fixed by
  plan.md §6.6; `V_RETRANS_TTL` (10s) likewise.

## Open question (see INTEGRATION.md)

This module assumes `smf_fseid` is available on Modification/Deletion
requests too, because `v_flow` reuses the same retransmit-check step
for all three flows per plan.md §5.2/§5.3 ("1-3. As above"). Real PFCP
may not carry F-SEID reliably outside Establishment — if Stage 2 finds
that's true, the fix is keying Modification/Deletion's retransmit check
on `(seid, seq)` instead, entirely inside `v_flow.c`; this module
doesn't need to change.

## Tests

`test/test_retrans_cache.c` — miss/hit/distinct-seq, and a **real**
TTL expiry via a shrunk `PEXPIRE` (same pattern as `v_sess_store`'s TTL
test). The plan's actual acceptance scenario (a retransmit forced to
land on a different simulated pod, single allocation) is exercised
end-to-end in `src/v_flow/CLAUDE.md`'s test instead, since it only means
something in the context of a full establishment flow.
