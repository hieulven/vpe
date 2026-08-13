# v_seid

SEID/TEID bit layout, encode/decode, and attacker-input validation.
plan.md §7, §6.3.

## Public API (`inc/v_seid.h`)

- `v_seid_encode(part_id, local, *out)` / `v_teid_encode(...)` — pack a
  partition + local index into the wire value. Fail (`RET_CODE_ERR`) on
  field overflow.
- `v_seid_part(seid)` / `v_teid_part(teid)`, `v_seid_local` /
  `v_teid_local` — pure bit extraction, no validation.
- `v_seid_validate(seid, *part_out)` / `v_teid_validate(teid, *part_out)`
  — the security-relevant function in this module. Call this on every
  attacker-supplied SEID/TEID that will be used to index anything,
  **except** an Establishment request's header SEID (legitimately 0
  pre-session — do not validate that one).

## Layout

64-bit SEID: `[63:60] fmt_ver | [59:56] reserved(must be 0) | [55:46]
part_id(10b) | [45:0] local(46b, never reclaimed)`.

32-bit TEID: `[31] reserved(must be 0) | [30:21] part_id(10b) | [20:0]
local(21b, 0 reserved, reclaimed via v_id_alloc's free list)`.

Full field-by-field rationale is in the header comment — read it before
changing any shift/mask constant, they're load-bearing for
`v_id_alloc`'s block-leasing math too (`V_SEID_LOCAL_MASK`,
`V_TEID_LOCAL_MASK`).

## Dependencies

None — this module is pure bit arithmetic + logging. No Redis, no
mempool, no ports beyond `v_log.h`/`v_common.h`.

## Invariants an agent must not break

- SEID local index space is **never reclaimed** — there is deliberately
  no `v_seid_free()`. Don't add one; see plan.md §6.3 for why (billions
  of establishments needed to exhaust 46 bits, so reclamation machinery
  isn't worth the complexity).
- TEID local index 0 must never be issued — `v_teid_encode` and
  `v_teid_validate` both reject it. `v_id_alloc` separately has to seed
  its Redis counter to 1 to avoid ever generating local=0 on a fresh
  partition; that's `v_id_alloc`'s bug to watch, not this module's, but
  the 0-is-reserved invariant originates here.
- `v_seid_validate`/`v_teid_validate` must reject BEFORE any arithmetic
  that could underflow/index out of range — this is the free5gc
  #730/#731 defense (plan.md §7). Don't reorder the checks.

## Tests

`test/test_seid.c` — round trips across part_id/local corners plus the
fuzz cases (`0xFFFFFFFFFFFFFFFF`, wrong format version, non-zero
reserved bits, TEID 0, TEID reserved bit set).
