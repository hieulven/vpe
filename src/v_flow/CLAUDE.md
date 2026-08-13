# v_flow

Session establishment / modification / deletion flows. plan.md §5. The
module where every other module gets wired together.

## Public API (`inc/v_flow.h`)

- `v_flow_init()` — seeds the round-robin partition counter (plan.md
  §6.4). Call once at startup.
- `v_flow_handle_msg(msg, arg)` — matches `v_dispatch_handler_t`; pass
  directly to `v_dispatch_worker_poll()`. Decodes, dispatches on PFCP
  message type, drives the rest of the flow entirely through async
  ports. Never blocks.

Everything else in `src/v_flow.c` is `static` — this module owns its
whole call graph per flow and doesn't expose intermediate steps.

## Dependencies

Everything: `v_port_pfcp.h`, `v_port_mem.h`, `v_port_txn.h`,
`v_port_vdp.h`, `v_seid.h`, `v_id_alloc.h`, `v_sess_store.h`,
`v_retrans_cache.h`, `v_dispatch.h`.

## The pattern every flow follows

`struct v_txn` (from `v_port_txn.h`) is the async context carried
through every callback in a flow — that's exactly what it's for. Read
one flow's callback chain top-to-bottom before touching any of them;
each function only makes sense relative to which txn state it's
handling and what the *next* async call is.

**Establishment** (`handle_establishment` → `retrans_lookup_cb` →
`cas_write_cb` → `vdp_create_cb` → `confirm_cb` → `finish`): retrans
check, part_id (UE-IP hash or round-robin), SEID/TEID alloc,
build_session, CAS-write PENDING, VDP create, confirm (PERSIST) on
accept. Every terminal path — success, VDP reject, VDP timeout, alloc
exhaustion, any dispatch failure — funnels through `finish()`, which
also stores the reply in the retransmit cache. That's deliberate: a
same-seq retransmit must get the identical cached answer instead of
redoing the whole allocation, on *every* terminal outcome including
failures (a NO_RESOURCES reply retransmitted 3 times shouldn't burn 3
SEIDs).

**Modification** (`handle_modification` → `mod_retrans_lookup_cb` →
`mod_read_cb` → `mod_cas_cb` → `mod_vdp_cb` → `finish`): validates the
header SEID with `v_seid_validate()` first — unlike Establishment, this
is a session-referencing message and skipping validation here is
exactly the free5gc #730/#731 class of bug plan.md §7 exists to
prevent. Reads, calls `v_port_pfcp_modify_session()`, CAS-writes.
`V_CAS_CONFLICT` retries (re-read fresh state) up to
`V_CAS_MAX_RETRY`. On VDP reject, CAS-writes a **snapshot** of the
pre-modification ctx back — see "the t->impl trick" below.

**Deletion** (`handle_deletion` → `del_retrans_lookup_cb` →
`del_read_cb` → `del_vdp_cb` → `del_delete_cb` → `finish`): same shape,
but VDP is called *before* any Redis mutation, so a VDP reject just
fails the request — nothing to compensate. Accept path frees the TEID
via `v_port_pfcp_ctx_teid()` (an ADAPTATION — see
`include/v_port_pfcp.h`'s comment on it) and never reclaims the SEID.

## The `t->impl` trick — read before touching Modification's reject path

`struct v_txn` has no field for "the ctx before this modification was
applied," but the VDP-reject compensating write (plan.md §5.2 step 8)
needs exactly that. `mod_read_cb` clones the freshly-read ctx into a
second `pdu_ses_ctx` (via serialize+deserialize, since ctx is opaque —
plan.md §3.0) and stashes the pointer in `t->impl`, which `v_port_txn.h`
documents as "reused table's own handle." This is a Stage-1-only
repurposing, not a real use of that field. **If you're extending this
flow and see `t->impl` being read or written, that's what it's holding
— don't assume it means what the port header says it means, and don't
add a second unrelated use of `t->impl` in this file.** `mod_vdp_cb`
frees it on every path (accept and reject both) — if you add a new
terminal path out of the modification chain, make sure it frees
`t->impl` too, or it leaks a ctx pool object.

## Invariants an agent must not break

- Every terminal path calls `finish()` — don't hand-roll a reply +
  cleanup somewhere else in a new code path. `finish()` is what
  guarantees the retransmit-cache store and the ctx/req free happen
  together.
- `v_seid_validate()` on every session-referencing message, before
  anything else touches the SEID. Establishment's header SEID (which is
  legitimately 0) is the one call site that must **not** validate it.
- `v_teid_free()` only, never anything that frees a SEID — see
  `v_seid/CLAUDE.md`.

## Tests

`test/test_flow_establishment.c` and `test/test_flow_mod_del.c` — full
chains through the real stub PFCP IO + `v_dispatch`, including VDP
reject/timeout, the retransmit-dedup single-allocation scenario, a
concurrent-modification-storm check (via the version counter — can only
reach `+2` if neither concurrent write was lost), and
modification-racing-deletion (hard invariant: the session always ends
up deleted).
