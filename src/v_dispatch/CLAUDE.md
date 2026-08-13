# v_dispatch

Rings between PFCP IO and the workers, plus the worker-side poll
helper. plan.md §1, §8 S7.

## Public API (`inc/v_dispatch.h`)

- `v_dispatch_rx(buf, len, peer, arg)` — matches `v_pfcp_rx_cb_t`
  exactly; hand it straight to `v_port_pfcp_io_init()`. Pure byte copy
  onto the shared rx ring, no parsing.
- `v_dispatch_worker_poll(worker_id, max, handler, arg)` — any worker
  can call this and get any queued message; `worker_id` is accepted for
  logging/symmetry only, not used to filter.
- `v_dispatch_tx_enqueue(buf, len, peer)` — the only way a worker
  reaches the network. `v_dispatch_io_drain_tx(max)` (I/O-core side)
  actually calls `v_port_pfcp_io_send()`.

## Dependencies

`v_port_pfcp.h` (only for `v_port_pfcp_io_send()`, called from the I/O
side, never from a worker).

## Design decision an agent needs to understand before "fixing" this

This is **one shared work-queue ring pair**, not 4 SEID-routed rings.
That's not a missing feature — it's the direct consequence of plan.md
§2 Change 2 (version CAS removes the correctness need for a worker to
own a session) and §6.7 (node-level state lives in Redis too, so there's
no need to special-case a "worker 0 handles heartbeats" rule either).
See the full design note in the header before reintroducing SEID-based
routing — that would be reverting exactly the coupling this redesign
exists to remove.

## Invariants an agent must not break

- **Architecture rule 1**: `v_dispatch_rx` must never decode/inspect
  the PFCP payload — not even to peek at a SEID for routing (see design
  note above for why that's not needed anyway).
- **Architecture rule 2**: no code outside this module's
  `v_dispatch_io_drain_tx()` may call `v_port_pfcp_io_send()`. If you're
  adding a new outbound path, it goes through `v_dispatch_tx_enqueue()`.
- Ring/mempool capacities (`V_DISPATCH_RING_SIZE`, `V_DISPATCH_POOL_CAP`)
  are Stage-1 sizing guesses — fine for the test suite's traffic
  volume, not validated against real production load.

## Tests

`test/test_dispatch.c` — an echo test through the real stub PFCP IO
(inject → ring → byte-forward → ring → send, byte comparison, no PFCP
semantics per the S7 acceptance criterion), and a drain test using
different `worker_id` values to demonstrate there's no affinity to
break.
