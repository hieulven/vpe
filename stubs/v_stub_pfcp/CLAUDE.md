# v_stub_pfcp

Stage 1 stub for `v_port_pfcp.h` — PFCP IO + PFCP business logic +
serializer, all three reused-module contracts in one stub file since
plan.md groups them in one header. **Deleted at Stage 2**; see
`INTEGRATION.md`.

## What it actually does

Implements a **synthetic wire format** (`struct v_stub_wire_req` /
`struct v_stub_wire_ses_ctx`, defined in `../v_stub_common.h`) that
bears no relation to real PFCP encoding — it exists purely so
`v_port_pfcp_decode()`/`encode_rsp()` have something concrete to
round-trip. `pfcp_msg` objects come from a real `rte_mempool`, same
discipline as `v_stub_mem`.

`v_port_pfcp_build_session()` writes a deterministic pattern into the
ctx payload derived from the request's `seq`; `modify_session()` XORs
it with a different constant. This is what `test/test_sess_store.c` and
`test/test_flow_mod_del.c` check against to prove data actually
round-trips/changes, not just that a call returned OK.

## Test-only surface (`inc/v_stub_pfcp_test.h`)

`v_stub_pfcp_encode()` (builds a synthetic request), `v_stub_pfcp_io_inject_rx()`
(synchronously fires whatever callback was registered via
`v_port_pfcp_io_init()` — this is how every `test_flow_*.c` test
injects a "received datagram"), `v_stub_pfcp_io_last_tx()` /
`v_stub_pfcp_io_reset()` (captures what `v_dispatch_io_drain_tx()`
actually sent). None of this exists at Stage 2 — real traffic arrives
over a real socket.

## Things to know before touching this file

- `v_port_pfcp_ctx_teid()` is a Stage-1 ADAPTATION (see
  `include/v_port_pfcp.h`'s comment on it and `INTEGRATION.md` §2) — a
  session's TEID doesn't appear in Modification/Deletion requests, only
  in what Establishment stored, so `v_flow`'s deletion path needs an
  accessor to pull it back out of ctx.
- `V_PFCP_MSG_*` / `V_PFCP_CAUSE_*` values in `include/v_port_pfcp.h`
  are self-consistent placeholders, not real TS 29.244 wire values —
  don't treat them as authoritative outside this codebase.
- Response header SEID must be 0 when `ctx` is NULL (see
  `v_port_pfcp_encode_rsp`) — this is `v_flow`'s mechanism for the
  "Session context not found → SEID 0 in the response header" rule from
  plan.md §7; don't change the NULL-ctx behavior without checking every
  `finish()`/`reply_ctx_not_found()` call site in `src/v_flow`.
