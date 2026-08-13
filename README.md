# vpe — stateless PFCP endpoint

A stateless redesign of a PFCP (control-plane) endpoint for a 5G UPF,
written in C against DPDK. It sits between the SMF (control plane) and
a VDP (data-plane) node, handling the PFCP session lifecycle
(Establishment / Modification / Deletion) over UDP and pushing
forwarding state to VDP over TCP.

The full design rationale is in [`PLAN.md`](PLAN.md); a shorter map of
how the pieces fit together is in [`ARCHITECTURE.md`](ARCHITECTURE.md).
If you're an AI coding agent working in this repo, start with
[`CLAUDE.md`](CLAUDE.md).

## Why "stateless"

The previous version of this service pinned each session to one
worker in one pod, so that only that worker could safely mutate it.
That pinning is what made scaling pods down, doing a clean rolling
upgrade, or losing a pod without losing sessions all hard problems.

This version removes that pinning with two changes:

1. **The SMF gets a reply only after the Redis write durably
   completes** — not before. A pod that dies mid-establishment leaves
   nothing the SMF believes exists that isn't actually recoverable.
2. **Optimistic concurrency (version CAS) replaces ownership.** Any
   worker on any pod can handle any session's message. A conflicting
   concurrent write is detected via a version mismatch and retried,
   not prevented by a lock.

The practical result: a pod can be killed, scaled down, or replaced at
any time without losing a session, without a rebalancing step, and
without a relay path to whichever pod "owns" a given session — because
nothing owns a session anymore. Session state lives in Redis; any pod
can serve any request.

## Status

This is the **Stage 1** build: a complete implementation against
adapter ports (`include/v_port_*.h`) with stub implementations
(`stubs/`) standing in for the modules that get reused unchanged from
the previous service (PFCP codec, PFCP/VDP I/O, the DB connection
layer, the session-context struct + its serializer, the transaction
table). The stubs talk to a **real** local Redis and use **real** DPDK
mempools/rings — they're not in-process mocks — so the standalone test
suite genuinely exercises the Lua scripts, the ring/mempool discipline,
and the async callback plumbing, just against synthetic PFCP messages
instead of real ones.

An integration stage (not part of this repo's history yet) replaces
`stubs/` with bindings to the real reused modules. See
[`INTEGRATION.md`](INTEGRATION.md) for exactly what that stage needs to
do and every assumption Stage 1 made along the way.

## Build & test

```bash
make            # build ./vpe (links against the stubs by default)
make test       # build and run the standalone test suite
```

`make test` needs a local `redis-server` (default `127.0.0.1:6379`,
override via `VPE_TEST_REDIS_HOST`/`VPE_TEST_REDIS_PORT`) plus
`libdpdk-dev`, `libhiredis-dev`, and `libevent-dev`. It's idempotent —
safe to rerun against the same Redis instance without flushing.

## Layout

```
include/          shared headers: v_common.h + every adapter contract (v_port_*.h)
src/v_*/           the new modules, each split into inc/ (public header) + src/ (.c)
stubs/v_stub_*/    Stage 1 stand-ins for the reused modules, same inc/+src/ split
test/              the standalone test suite
```

Every module directory has its own `CLAUDE.md` describing its API,
dependencies, and the invariants it depends on.
