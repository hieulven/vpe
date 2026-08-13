# INTEGRATION.md — Stage 1 → Stage 2 handoff

This document is for the integration agent replacing `stubs/` with real
bindings to the reused modules (`plan.md` §9). It records every
assumption Stage 1 made about those modules, every place the actual
port contract ended up differing from `plan.md` §3's draft, and what's
still open.

Stage 1 had no access to the existing codebase or the internet. Where
`plan.md`'s draft contract turned out to be unusable as written, the
fix was made in the port header + stub only, never in the new modules
(`src/`) — that's the rule this document exists to make auditable.

---

## 1. How to build and test

```
make            # builds ./vpe (USE_STUBS=1 — links stubs/, for local dev)
make test       # builds and runs the standalone suite against a real
                # local redis-server (127.0.0.1:6379) and real DPDK
                # mempools/rings (--no-huge, no hugepages required)
make vpe USE_STUBS=0   # the production target: compiles src/ + main.c
                        # only. Fails to link until Stage 2 supplies real
                        # v_port_*.c implementations somewhere under
                        # src/ (any subdirectory — the Makefile globs
                        # recursively) — that failure is expected and is
                        # the whole point of this target.
```

Requires `libhiredis-dev`, `libevent-dev`, `libdpdk-dev` (built and
tested against DPDK **23.11.4**, not the plan's stated 22.11.1 — no
API incompatibility was hit, but Stage 2 should confirm against the
real 22.11.1 headers rather than assume). `make test` needs a
redis-server reachable at `127.0.0.1:6379` by default (override with
`VPE_TEST_REDIS_HOST` / `VPE_TEST_REDIS_PORT` env vars — see
`stubs/v_stub_db/v_stub_db.c`).

The suite is idempotent against a persistent redis-server — rerun
`make test` freely without flushing. Full run is ~4150 checks,
sub-second once Redis is warm.

---

## 2. Adaptations vs. `plan.md` §3's draft contract

Every one of these was a case where the draft signature couldn't
actually support what a later section of the plan required of it. Each
is marked `ADAPTATION` in the corresponding header's comments too —
this is just the consolidated list.

### `v_port_mem.h`
- Added `v_port_mem_init(unsigned capacity)` / `v_port_mem_fini()`.
  The draft had no lifecycle hook, but something has to create the
  pool. Stage 2: if the real ctx pool already exists elsewhere at
  startup, these can become no-ops that just record the pool handle.

### `v_port_pfcp.h`
- Added `v_port_pfcp_ctx_teid(const struct pdu_ses_ctx *ctx)`. Needed
  by `v_flow`'s deletion path (§5.3 step 8, `v_teid_free`) — a
  session's TEID is fixed at establishment, and Modification/Deletion
  requests don't carry it, so it has to come from an accessor on the
  stored ctx. The module that built ctx in the first place is the
  natural owner of this.
- Message-type and cause-code `#define`s (`V_PFCP_MSG_*`,
  `V_PFCP_CAUSE_*`) are Stage-1 placeholders with self-consistent
  values, not the real wire values. Stage 2 replaces them with
  whatever the reused decoder actually defines.

### `v_port_db.h` — the biggest set of changes
- **`v_db_cb_t` carries a parsed `v_db_reply_t` tree, not the draft's
  flat `(const char *, len)`.** A flat buffer cannot represent
  `v_teid_refill`'s array reply (`{'range', ...}` / `{'list', ...}`,
  plan.md §6.3) — the flat signature is incompatible with a script the
  plan itself specifies. `v_db_reply_t` is a small tagged union
  (nil/status/integer/string/array); see the header for the full
  shape. **Lifetime rule:** a `v_db_reply_t` and everything it points
  to is only valid for the duration of the callback that receives it
  — the stub frees the underlying hiredis reply immediately after.
  Every module in `src/` follows this (copy out what you need before
  returning); `test/test_id_alloc.c`'s comment block explains the
  failure mode if you don't.
- **Added `v_port_db_cmd_argv()`** (binary-safe, explicit
  `argc`/`argv`/`argvlen`). `v_sess_cas_write`'s `ARGV[2]` is a
  serialized session blob that may contain embedded NULs or spaces —
  a `printf`-style `v_port_db_cmd(fmt, ...)` call cannot carry that
  safely (hiredis's `%s` substitution treats the whole format string's
  literal whitespace as token boundaries, not the substituted value's
  content, so this specifically means "the value can contain anything
  binary," not "you can pass a pre-rendered command string through a
  bare `%s`" — the test suite hit exactly that second, wrong pattern
  once, see `test/test_id_alloc.c`'s `del_sync`/`rpush_sync` comment).
  `v_db_script.c`'s EVALSHA path and `v_retrans_cache.c`'s store path
  both use this.
- **Added `v_port_db_init()` / `v_port_db_fini()` / `v_port_db_poll()`.**
  The draft assumed the DB layer's event loop is driven externally
  (correct — production binds it to a worker's libevent loop, same
  pattern as `v_pdu_cacher`), but Stage 1 has no such loop to bind to,
  so the stub exposes its own for `main.c` and the test suite to pump.
- **Confirmed assumption, not a change:** `v_port_db_shard_of()` is
  **not** guaranteed `<partid>`-derived by the given contract (plan.md
  flags this explicitly at §9.4 point 5 / I4). `v_id_alloc.c` works
  around it by never calling `v_port_db_shard_of()` on a full
  multi-key script's key directly — it hashes just the decimal
  `<part_id>` string (`shard_for_part()` in `v_id_alloc.c`) so that
  `vpe:teid:<part>:free` and `vpe:teid:<part>:next` are *guaranteed*
  co-located regardless of what the real function does with the rest
  of a key string. **Stage 2 must confirm `v_port_db_shard_of()` is
  actually `<partid>`-derived; if it is, `shard_for_part()` becomes
  redundant (harmless) but can be simplified away. If it isn't,
  `shard_for_part()` is required, not optional — keep it.**
- **Environment limitation, not a code change:** this sandbox has one
  physical `redis-server`, not a 5-master Sentinel topology. The stub
  simulates 5 shards as 5 separate hiredis connections to that one
  server. Shard *routing* logic is exercised for real; true
  cross-shard independence under a Sentinel failover (one shard's
  `SCRIPT FLUSH` not affecting another) is **not** — see open items.

### `v_port_txn.h`
- Added `v_port_txn_init()` / `v_port_txn_fini()`.
- Added `v_port_txn_find_by_seid()` (the draft only gave
  `find_by_seq`). Needed wherever a flow has to reach a live txn by the
  SEID/TEID it allocated rather than by (smf_fseid, seq).
- Added `v_port_txn_test_advance_ms()` / `v_port_txn_test_hard_timeout_ms()`
  — test-only, simulate the single hard timeout without a real clock.

### `v_port_vdp.h`
- Added `v_port_vdp_test_force_result()` / `v_port_vdp_test_reset()` —
  test-only hooks the plan already anticipated needing (§3.6) but
  didn't give signatures for.

---

## 3. Known gap: `v_txn`'s VDP_WAIT timeout during Modification

`src/v_txn/v_txn.c` (the hard-timeout sweeper, milestone S6) cannot
fully implement plan.md §3.5's requirement for a timeout in
`TXN_ST_VDP_WAIT` **during Modification** (as opposed to
Establishment). The reason: compensating a modification that timed out
mid-VDP-call requires the *pre-modification* ctx, but `struct v_txn`
(as given in plan.md §3.5) has no field for it.

`v_flow.c`'s own **non-timeout** REJECT handling for Modification
(§5.2 step 8) does have this — it snapshots ctx into `t->impl` right
after the read, before calling `v_port_pfcp_modify_session()`, and
uses it in `mod_vdp_cb()`. The hard-timeout sweeper in `v_txn.c` can't
reuse this because `t->impl` is only populated while a modification is
actively in flight and torn down before the txn would ever be visible
to the sweeper's generic handler in a state where recovery is possible
— by the time a *timeout* fires, there's no guarantee the snapshot is
still there or valid.

**Stage 2 needs to decide:** either extend the real transaction
table's struct with a dedicated "prior ctx" slot for this, or accept
that a VDP_WAIT hard-timeout during Modification currently only logs a
CRIT and frees resources without compensating (see the `KNOWN GAP`
comment in `src/v_txn/v_txn.h` and `src/v_txn/v_txn.c`). Given the hard
timeout is a last-resort backstop (VDP itself has its own timeout that
normally resolves through the ordinary `v_vdp_cb_t` callback path,
which *is* compensated correctly), this is a low-probability edge, but
it is a real one.

---

## 4. Other open items for Stage 2

1. **§3.5 worst-case-timeout budget.** `v_port_txn_test_hard_timeout_ms()`
   is a Stage-1-only constant (2000ms). Stage 2 must check the real
   configured hard timeout against the worst-case budget in plan.md
   §3.5 (`(2 + 2 × V_CAS_MAX_RETRY) × redis_rtt + vdp_rtt`) using
   measured Redis RTT, and against the SMF's retransmission timer.
2. **Retransmit cache key assumption.** `v_retrans_cache` keys on
   `hash(smf_fseid) + seq` for *all* message types (plan.md §5.2/§5.3
   say "1-3. As above" for Modification/Deletion, reusing
   Establishment's retransmit-check step verbatim). Real PFCP may not
   carry a usable F-SEID on Modification/Deletion the way Establishment
   does — confirm against the real decoder, and if it doesn't, key
   Modification/Deletion retransmit dedup on `(seid, seq)` instead
   (straightforward change, contained to `v_flow.c`'s
   `handle_modification`/`handle_deletion`).
3. **Sentinel failover independence is unverified** (see §2 above,
   `v_port_db.h`). The NOSCRIPT recovery *mechanism* is tested for real
   (`test/test_db_script.c` forces an actual `SCRIPT FLUSH`), but
   whether flushing one real master leaves the other 4 untouched can
   only be verified against the real 5-master topology.
4. **`main.c` is illustrative, not complete.** No signal handling, no
   graceful shutdown, no config loader (Node ID is a placeholder
   constant — plan.md §0 puts config wiring out of scope for this
   plan). It demonstrates the intended lcore shape and startup
   ordering (see the fix noted in its own header comment: startup must
   gate on `v_db_script_ready()` before `v_id_alloc_init()`, and on
   `v_id_alloc_ready()` before launching workers — found by actually
   running it, not just reasoning about it).
5. **TEID quarantine depth is asserted at the Lua-script level, not at
   production scale.** `test/test_id_alloc.c`'s floor test passes a
   small `ARGV[2]` floor directly to the script rather than pushing
   `V_TEID_FREE_FLOOR` (1024) real entries, so the *logic* is verified
   but the real floor's effectiveness against the "worst-case gNB
   in-flight window" (plan.md §6.5) is a sizing question for Stage 2,
   not something Stage 1 could validate without a real gNB.
6. **DPDK version:** built and tested against 23.11.4 (the sandbox's
   available `libdpdk-dev`), not the plan's 22.11.1. No incompatibility
   surfaced, but this needs re-verification against the real version.

---

## 5. What each milestone's test coverage actually exercises

All of S1–S11 run in `test/main_test.c` in order; see individual
`test/test_*.c` files for what each one checks. Everything below ran
against a real local Redis and real DPDK mempools/rings — nothing here
is mocked-in-process.

| Milestone | Module | Test file |
|---|---|---|
| S1 | ports + stubs + Makefile | (build itself; `make` and `make test` both green) |
| S2 | `v_db_script` | `test_db_script.c` — CAS scripts, NOSCRIPT recovery against a real `SCRIPT FLUSH` |
| S3 | `v_seid` | `test_seid.c` — round trip + free5gc #730/#731-class fuzz |
| S4 | `v_id_alloc` | `test_id_alloc.c` — 600-alloc uniqueness across forced refills, floor logic |
| S5 | `v_sess_store` | `test_sess_store.c` — CAS ok/conflict/gone, real TTL expiry |
| S6 | txn sweeper | `test_txn_sm.c` — every state's timeout path, ctx pool returns to baseline |
| S7 | `v_dispatch` | `test_dispatch.c` — echo test, no-affinity drain |
| S8/S9 | `v_flow` establishment + `v_retrans_cache` | `test_flow_establishment.c` — accept/reject/timeout, retransmit dedup with no second allocation |
| S10 | `v_flow` mod/del | `test_flow_mod_del.c` — accept, VDP-reject compensation, not-found, SEID fuzz, concurrent-modification storm (version-counter invariant), modification-racing-deletion |
| S11 | `v_node_state` | `test_node_state.c` — recovery timestamp stable across 3 simulated pod boots |

Total: ~4150 checks, `make test` from a clean `make clean` produces
zero compiler warnings under `-Wall -Wextra`.
