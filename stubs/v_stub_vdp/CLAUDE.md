# v_stub_vdp

Stage 1 stub for `v_port_vdp.h` — the VDP (data plane) push. **Deleted
at Stage 2**; see `INTEGRATION.md`.

## What it actually does

Returns `V_VDP_ACCEPT` by default, **synchronously** — the callback
fires from inside `v_port_vdp_session_create/modify/delete()` itself,
before the call returns. This is a real behavioral difference from
production (a real VDP push is a TCP round trip), and every caller in
`src/v_flow.c` is written to be correct either way — none of them do
anything after issuing a VDP call that assumes the callback *hasn't*
fired yet.

## Test-only surface (`inc/v_port_vdp.h`, not a separate test header)

`v_port_vdp_test_force_result(res)` / `v_port_vdp_test_reset()` — force
the next call(s) to return `V_VDP_REJECT` or `V_VDP_TIMEOUT` instead of
the default accept. Every `test_flow_*.c` reject/timeout test uses
this. **Remember to call `v_port_vdp_test_reset()` after a test that
forces a non-accept result**, or the next test that expects the default
accept behavior will silently fail against a stale forced result — this
has bitten test-writing in this repo before, check the end of every
`test_flow_*_reject`/`*_timeout` function for the reset call before
adding a new one.

## Things to know before touching this file

- This stub takes no `part_id`/`ctx`-derived branching — it can't
  reject based on session content, only via the test hook. If a future
  test needs conditional VDP behavior (e.g. reject only for a specific
  seid), that needs a new test hook here, not a workaround in the test
  file.
