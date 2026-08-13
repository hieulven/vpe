# ARCHITECTURE.md

A map of how `vpe`'s pieces fit together, once you already know *why*
(that's `PLAN.md`). Read this to find where a change belongs; read a
module's own `CLAUDE.md` for the details of that module.

---

## 1. Process shape

One process per pod: 1 main/IO lcore + N worker lcores (production
target: 4 workers, per `PLAN.md`'s environment facts). No OS threads —
DPDK lcores (`rte_eal_remote_launch`) are the only concurrency
mechanism. See `main.c`.

```
                         ┌─────────────────────────────┐
   UDP datagram  ──────► │  I/O / main lcore            │
   (PFCP, stub in        │  v_port_pfcp_io (rx callback) │
   Stage 1)               │        │                     │
                          │        ▼                     │
                          │  v_dispatch_rx()              │──┐
                          │  (byte copy only, no parsing) │  │ rx_ring
                          └─────────────────────────────┘  │ (shared,
                                                              │  MC/MP)
        ┌─────────────────────────────────────────────────┘
        ▼
┌───────────────┐  ┌───────────────┐  ┌───────────────┐  ┌───────────────┐
│ worker lcore 0 │  │ worker lcore 1 │  │ worker lcore 2 │  │ worker lcore 3 │
│ v_dispatch_    │  │ v_dispatch_    │  │ v_dispatch_    │  │ v_dispatch_    │
│ worker_poll()  │  │ worker_poll()  │  │ worker_poll()  │  │ worker_poll()  │
│      │         │  │      │         │  │      │         │  │      │         │
│      ▼         │  │      ▼         │  │      ▼         │  │      ▼         │
│ v_flow_        │  │ v_flow_        │  │ v_flow_        │  │ v_flow_        │
│ handle_msg()   │  │ handle_msg()   │  │ handle_msg()   │  │ handle_msg()   │
└───────┬────────┘  └───────┬────────┘  └───────┬────────┘  └───────┬────────┘
        │                    │                    │                    │
        └────────────────────┴─────────┬──────────┴────────────────────┘
                                         │  tx_ring (shared, MC/MP)
                                         ▼
                          ┌─────────────────────────────┐
                          │  I/O / main lcore             │
                          │  v_dispatch_io_drain_tx()     │
                          │  → v_port_pfcp_io_send()       │
                          └─────────────────────────────┘  ──► UDP datagram out
```

**Any worker can process any message** — there is no per-SEID routing
and no "worker 0 handles node-level messages" special case. That's not
a missing optimization; it's the direct consequence of the CAS design
(§3 below) removing the correctness need for a worker to own a
session. See `src/v_dispatch/CLAUDE.md`'s design note.

---

## 2. Module dependency graph

```
                          v_flow  (orchestrates every flow)
                        /   |   |   \        \
                       /    |   |    \        \
              v_seid  v_id_alloc v_sess_store v_retrans_cache
                 |         |          |             |
                 |         +----------+-------------+
                 |                    |
                 |               v_db_script
                 |                    |
                 +--------------------+---- v_port_db.h (port)
                                       |
        v_dispatch ─── v_port_pfcp.h (port)
                                       |
        v_txn (timeout sweeper) ── v_port_txn.h, v_sess_store
                                       |
        v_node_state ── v_port_db.h (port, independent of the above)

        v_port_mem.h (port) — used by v_flow, v_txn
        v_port_vdp.h (port) — used by v_flow only
        v_log — used by everything
```

`v_flow` is the only module that touches every port and every other
new module. Every other module is a narrow, independently-testable
wrapper around one port (or, for `v_db_script`, a layer that adds
capability — Lua — the port doesn't otherwise have).

---

## 3. The CAS design, concretely

`PLAN.md` §2 states the two changes; this is what they look like in
code.

**Every session mutation is read-version-write, not lock-mutate-unlock:**

```
v_sess_read(part, seid)  →  (ctx, ver)
   ... build the new ctx from ctx + the PFCP request ...
v_sess_cas_write(part, seid, exp_ver=ver, new_ctx, ...)
   V_CAS_OK       → the write landed, ver is now ver+1
   V_CAS_CONFLICT → someone else wrote first; re-read and retry
   V_CAS_GONE     → someone deleted it; the request fails as "not found"
```

`v_flow`'s Modification and Deletion flows both implement this retry
loop (up to `V_CAS_MAX_RETRY`, re-reading fresh state each time — see
`src/v_flow/CLAUDE.md`). Establishment doesn't need the loop: it always
writes at `exp_ver=0` ("must not exist yet"), and a freshly-allocated
SEID is never reused (`PLAN.md` §6.3), so a conflict there would
indicate a real bug, not a normal race.

**Why this makes statelessness possible:** two pods can now process
messages for the same session concurrently without coordinating first.
Worst case, one of them retries once. Nothing needs to know who else
might be touching the session, because nothing needs permission to
touch it — only agreement on which version it's touching.

---

## 4. The txn as async context

Every flow's async chain is stitched together by `struct v_txn`
(`include/v_port_txn.h`) — created at the start of a flow, threaded
through every callback as the `arg`, destroyed at the end. It carries
the state that would otherwise need to be captured in a closure: the
decoded request, the allocated ctx, the peer address, the part_id/seid
being operated on, and the CAS-retry counter.

```
handle_establishment()
  → v_port_txn_create()
  → v_retrans_lookup(..., t)
       retrans_lookup_cb(..., t)
         → v_seid_alloc / v_teid_alloc
         → v_port_pfcp_build_session(..., t->ctx)
         → v_sess_cas_write(..., t)
              cas_write_cb(..., t)
                → v_port_vdp_session_create(..., t)
                     vdp_create_cb(..., t)
                       → v_sess_confirm(..., t)
                            confirm_cb(..., t)
                              → finish(t, cause)   ← every path ends here
```

`finish()` (in `src/v_flow/src/v_flow.c`) is the single place that
builds the response, stores it in the retransmit cache, sends it, and
releases everything `t` is holding. Every terminal outcome —
success, VDP reject, allocation exhaustion, a dispatch failure at any
step — funnels through it. If you add a new terminal path to a flow,
route it through `finish()` rather than duplicating cleanup.

**The hard timeout backstop:** `v_port_txn` enforces one hard timeout
per transaction, not per async wait. `src/v_txn` is what decides, given
`t->state` at the moment of timeout, what recovery (if any) is
possible — see its own `CLAUDE.md` for the per-state behavior and the
one known gap (compensating a Modification's write-back on a
timed-out, not merely rejected, VDP call).

---

## 5. Where Redis fits

Every module that touches Redis goes through `include/v_port_db.h` —
never hiredis directly (only `stubs/v_stub_db` does that, standing in
for the reused connection layer). Two things ride on top of the raw
command layer:

- **`v_db_script`** adds EVALSHA + NOSCRIPT recovery, because the reused
  command layer has no Lua support (`PLAN.md` §4). `v_sess_store` and
  `v_id_alloc` both depend on it for their CAS operations.
- **Sharding**: `v_port_db_shard_of(key)` routes a key to one of 5
  logical shards. Its co-location guarantee (whether two different key
  *strings* sharing a partition number land on the same shard) is not
  confirmed by the port contract — `v_id_alloc` works around this by
  hashing only the decimal `part_id`, not a full key, wherever a
  multi-key Lua script needs guaranteed co-location. See
  `INTEGRATION.md` §2.

Redis key namespaces in use:

| Prefix | Owner | Purpose |
|---|---|---|
| `pdu_<part>:<seid>` | `v_sess_store` | session record (hash: `data`, `ver`) — **fixed format, never changes** |
| `vpe:seid:<part>:next` | `v_id_alloc` | SEID block-lease counter |
| `vpe:teid:<part>:next` / `:free` | `v_id_alloc` | TEID block-lease counter / reclaim free list |
| `txn_<part>:<smf_fseid>:<seq>` | `v_retrans_cache` | cached PFCP response, for retransmit dedup |
| `upf:<nodeid>:recovery` / `:assoc` | `v_node_state` | Recovery Time Stamp / association state |

---

## 6. Test strategy

The standalone suite (`test/`) exercises every module against a real
local `redis-server` and real DPDK mempools/rings — see each module's
`CLAUDE.md` for what its own tests cover, and `INTEGRATION.md` §5 for
the full milestone-by-milestone coverage map. The suite is idempotent
against a persistent Redis instance (tests that create Redis state
clean up or route around leftovers from a prior run) — if you add a
test, make it survive being run twice in a row without a `FLUSHALL` in
between.
