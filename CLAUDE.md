# CLAUDE.md — vpe Project Guide

This file is the first thing you read before touching any code. Follow
every rule here without exception. If something in a task conflicts
with this file, this file wins — flag the conflict before proceeding.

---

## What This Project Is

`vpe` is a stateless redesign of a PFCP (control-plane) endpoint for a
5G UPF, built in C against DPDK. It sits between the SMF and a VDP
(data-plane) node, handling PFCP session establishment/modification/
deletion over UDP and pushing forwarding state to VDP over TCP.

Full design rationale lives in `PLAN.md` — read it before implementing
anything non-trivial. `ARCHITECTURE.md` is the shorter map of how the
pieces fit together once you already know the rationale.
`INTEGRATION.md` documents every place Stage 1's code deviates from
`PLAN.md`'s draft port contracts, and what's still open for the
integration stage that replaces `stubs/` with real bindings.

The design's two load-bearing decisions (see `PLAN.md` §2):

1. **Reply to the SMF only after the Redis write completes.** The
   write itself stays async — only the *reply* moves into the
   completion callback.
2. **Version CAS instead of session ownership.** No worker or pod owns
   a session; a conflicting concurrent write is detected (`ver`
   mismatch) and retried, not prevented by a lock.

Everything else in this codebase is a consequence of those two rules.
If a change seems to need bringing back per-worker session pinning or
a distributed lock, it's very likely fighting the design rather than
extending it — read `PLAN.md` §11 ("Deliberately NOT doing") first.

---

## Build

```bash
make            # build ./vpe (production binary, USE_STUBS=1 by default)
make test       # build and run the standalone test suite
make clean      # remove build artifacts
```

`make test` needs a real `redis-server` reachable at `127.0.0.1:6379`
(override with `VPE_TEST_REDIS_HOST`/`VPE_TEST_REDIS_PORT`) and DPDK's
`libdpdk-dev` + `libhiredis-dev` + `libevent-dev`. It runs against a
**real** local Redis and **real** DPDK mempools/rings — nothing here
is mocked in-process; that's deliberate (see `PLAN.md` §3.6).

`make vpe USE_STUBS=0` is the production target — it compiles `src/`
and `main.c` only, no stub. It will fail to link until real
`v_port_*.c` bindings exist somewhere under `src/`; that's expected
until the integration stage happens (see `INTEGRATION.md`).

**When adding a new source file:** just create it in the right
module's `src/` subdirectory — the Makefile's `rwildcard` picks it up
automatically, and its `inc/` is added to the include path
automatically too. Do not edit the Makefile unless the task explicitly
requires it (a new module directory, a new dependency).

---

## Repository Layout

```
vpe/
  CLAUDE.md               ← this file
  PLAN.md                 ← the original design document — read first
  ARCHITECTURE.md          ← map of modules + how a request flows through them
  README.md                ← human-facing overview
  INTEGRATION.md            ← Stage 1 → Stage 2 handoff notes
  Makefile
  main.c                    ← production entry point (illustrative — see its own header comment)
  include/                  ← shared headers: v_common.h + every v_port_*.h (the adapter contracts)
  src/
    v_seid/{inc,src}/         ← SEID/TEID bit layout, encode/decode, validation
    v_id_alloc/{inc,src}/     ← SEID/TEID rings, Redis block leasing
    v_db_script/{inc,src}/    ← EVALSHA layer (the reused DB layer has no Lua support)
    v_sess_store/{inc,src}/   ← session persistence with version CAS
    v_retrans_cache/{inc,src}/← cross-pod retransmit dedup
    v_txn/{inc,src}/          ← transaction hard-timeout sweeper
    v_dispatch/{inc,src}/     ← rings between PFCP IO and the workers
    v_flow/{inc,src}/         ← establishment / modification / deletion flows
    v_node_state/{inc,src}/   ← Node ID, Recovery Time Stamp, association state
    v_log/{inc,src}/          ← V_LOG() implementation
  stubs/
    v_stub_common.h            ← struct/wire-format layout shared by the stubs below
    v_stub_{db,mem,pfcp,txn,vdp}/{inc,src}/  ← Stage 1 stand-ins for the reused modules
  test/                       ← standalone test suite (test_*.c), run via `make test`
```

Every module directory under `src/` and `stubs/` has its own
`CLAUDE.md` — **read a module's `CLAUDE.md` before changing anything in
it.** Each one covers that module's public API, its dependencies, the
invariants an agent must not break, and where its test coverage lives.
This file covers only what's true project-wide.

---

## Module Ownership

### `stubs/v_stub_*/` — Stage 1 stand-ins for reused modules

These implement the adapter contracts declared in `include/v_port_*.h`
using synthetic data (a real local Redis and real DPDK mempools/rings,
but not real PFCP/VDP wire formats — see each stub's own `CLAUDE.md`).
**They are deleted at the integration stage**, replaced with bindings
to the real reused modules. Do not add production logic here beyond
what's needed to exercise the port contract in tests. Do not change a
port's signature to make a stub easier to write — if the contract in
`include/v_port_*.h` genuinely can't support what a new module needs
(this has happened — see `INTEGRATION.md` §2), fix the port header and
document the adaptation there, in the port header's own comment, and
in the affected module's `CLAUDE.md`. Never quietly work around it in
`src/`.

### `src/v_*/` — the new codebase

All real implementation work happens here, against the ports only.
Cross-module calls go through a module's public `inc/*.h` — never
`#include` another module's `.c` file, and never reach into
`stubs/` from `src/` (the only exception is `stubs/v_stub_common.h`'s
struct layouts, which `stubs/` files share with each other, not
something `src/` should ever include).

---

## Naming Conventions

| Scope | Rule | Example |
|-------|------|---------|
| Module directory | `v_` prefix, split into `inc/` + `src/` | `v_seid/inc/v_seid.h`, `v_seid/src/v_seid.c` |
| Function name | no `v_` prefix beyond the module's own — short and direct | `v_seid_alloc()`, `hash_ue_ip()` (static) |
| Variable name | short and direct, no decorative prefix | `part_id`, `seid`, `ctx` |
| Struct/enum | snake_case with `_t` suffix for typedef'd ones | `v_cas_result_t`, `v_txn_state_t` |
| Enum values | UPPER_SNAKE_CASE | `V_CAS_OK`, `TXN_ST_VDP_WAIT` |
| Constants / macros | UPPER_SNAKE_CASE | `V_SESS_PENDING_TTL`, `V_NUM_PARTS` |

## Logging

Use only `V_LOG(level, module, ...)` from `include/v_log.h` (see
`src/v_log/CLAUDE.md`). Never use `printf`/`fprintf` for anything but
the standalone test suite's own summary output. Never define a second
logging macro.

## Coding Rules

- **No `malloc`/`calloc`/`free` anywhere.** Startup-time allocation
  uses `rte_zmalloc`; runtime/hot-path allocation uses an
  `rte_mempool`. This applies inside `stubs/` too, not just `src/`.
- **No new OS threads.** DPDK lcores (`rte_eal_remote_launch`) are the
  only concurrency mechanism — see `main.c`.
- **No blocking on the critical path** — no `sleep`/`usleep`, no
  blocking socket I/O, no synchronous Redis calls. Every DB/VDP call in
  `src/` is async: dispatch returns immediately, the outcome arrives in
  a callback. The one sanctioned exception is startup-phase
  pump-and-wait (see `main.c`'s `wait_until()` and `test/test_common.c`'s
  `test_wait_until()`) — legitimate only because nothing is accepting
  traffic yet at that point.
- **Every function that can fail returns `int`** (`RET_CODE_OK`/
  `RET_CODE_ERR` from `include/v_common.h`) or a pointer (`NULL` =
  failure). Async functions have two separate outcomes that must not
  be conflated: the synchronous **dispatch** result (was the call even
  issued) and the **outcome** that arrives later in the callback (e.g.
  `v_cas_result_t`). See any module's `CLAUDE.md` for the specific
  callback shapes.
- Do not silently ignore a `rte_ring_enqueue`/`rte_mempool_get` failure
  — log it with context (partition, SEID, seq where available) and
  handle it (drop with a log, or fail the request), never assume it
  can't happen.
- Header include guards: `#ifndef VPE_<MODULE>_H`, `#define
  VPE_<MODULE>_H`, `#endif`.

## Architecture Rules — Do Not Violate

See `ARCHITECTURE.md` for the full picture; the two hard rules are:

1. **The I/O core does no PFCP parsing.** `v_dispatch_rx()` is a pure
   byte copy onto a ring. Routing decisions never need to inspect the
   payload — see `src/v_dispatch/CLAUDE.md`'s design note on why that's
   actually true in this design (it wasn't in the old, ownership-based
   VPE).
2. **Workers never touch the network directly.** Outbound always goes
   through `v_dispatch_tx_enqueue()`; only `v_dispatch_io_drain_tx()`
   calls `v_port_pfcp_io_send()`.

## Working Through the Codebase

1. Read `PLAN.md`'s relevant section and the target module's
   `CLAUDE.md` before writing code.
2. Follow an existing module's shape for a new one: `inc/` header with
   the public API documented, `src/` implementation, a matching
   `test/test_*.c`, and a `CLAUDE.md` once the module is more than a
   few functions.
3. Verify `make clean && make test` is clean (zero compiler warnings
   under `-Wall -Wextra`, all checks passing) before considering work
   done. Run it more than once in a row — some bugs only show up
   against a persistent Redis instance's leftover state from a prior
   run (this has happened; see `test/test_retrans_cache.c`'s git
   history for a real example).
4. If a task references a reused module's real behavior that Stage 1
   can't observe (no access to the real codebase), state the
   assumption you're making, implement against it, and record it in
   `INTEGRATION.md` — don't guess silently.
