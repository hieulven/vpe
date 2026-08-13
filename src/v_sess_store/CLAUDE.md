# v_sess_store

Session persistence with version CAS. plan.md §6.1. This is the module
that makes plan.md §2 Change 2 (version CAS instead of ownership) real.

## Public API (`inc/v_sess_store.h`)

- `v_sess_read(part_id, seid, ctx, cb, arg)` — fills caller-owned `ctx`
  in place, hands back `(found, ver)`.
- `v_sess_cas_write(part_id, seid, exp_ver, ctx, state, cb, arg)` —
  `exp_ver=0` means "must not exist" (establishment). `state` controls
  the TTL: `V_SESS_PENDING` sets the self-cleaning 30s TTL, `V_SESS_CONFIRMED`
  clears it.
- `v_sess_confirm(part_id, seid, cb, arg)` — `PERSIST`, clears the
  pending TTL without touching `ver`.
- `v_sess_delete(part_id, seid, exp_ver, cb, arg)` — CAS-guarded `DEL`.

**Read the header's comment on `v_sess_read_cb_t`/`v_sess_cas_cb_t`
before wiring a new caller**: the async ISSUE result
(`RET_CODE_OK`/`RET_CODE_ERR`, returned synchronously) and the CAS
OUTCOME (`v_cas_result_t`, arriving in the callback) are different
things. Conflating them is the single easiest way to introduce a
correctness bug against this module.

## Dependencies

`v_db_script.h` (both writes and the delete are Lua CAS scripts),
`v_port_pfcp.h` (serialize/deserialize — this module never dereferences
`ctx`, only hands it to the reused serializer).

## Invariants an agent must not break

- **Session key format `pdu_%u:%lu` (partid, seid) is fixed.** plan.md
  environment facts: "MUST NOT CHANGE." The version lives in a new Redis
  hash *field* (`ver`), never in the key.
- Every retry on `V_CAS_CONFLICT` must `v_port_ctx_clear(ctx)` before
  re-reading into the same object — reusing a dirty ctx across a retry
  is the most likely source of a subtle mempool/data bug in the whole
  build (plan.md §6.1 calls this out explicitly). This module's own
  functions don't loop (callers like `v_flow` do), but don't add a
  retry path here without the same discipline.
- The serialize buffer (`V_SESS_BLOB_MAX_BYTES`, currently 2048) is a
  Stage-1 guess at the real serializer's max size — flagged in
  `INTEGRATION.md` as something Stage 2 must confirm, not something to
  quietly resize without re-checking why it was sized that way.

## Tests

`test/test_sess_store.c` — full establish→modify→confirm→delete CAS
lifecycle, byte-for-byte round-trip verification through the opaque
serializer, and a **real** pending-TTL expiry (shrunk via a raw
`PEXPIRE`, not a 30-second sleep or an invented test-only constant).
