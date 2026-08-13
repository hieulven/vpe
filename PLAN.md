# VPE Stateless — Build & Integration Plan

This plan covers a new VPE service built in **two stages by two different
agents**, neither of which sees both halves of the system at once.

- **Stage 1 — build agent (Claude Code, cloud).** No access to the existing
  codebase or the internet. Writes all new code against the adapter interfaces
  declared in §3, plus stubs, plus a standalone test suite. Everything must
  compile, run, and pass tests **without the real modules present.**
- **Stage 2 — integration agent (private, on-prem).** Has both codebases.
  Replaces the stubs with bindings to the real reused modules, resolves types,
  builds, and runs the integrated test suite.

The plan is written so Stage 1 never needs to ask a question about code it
cannot see. Everything Stage 1 must assume is stated as a contract in §3; if
reality differs, Stage 2 fixes it at the adapter, not in the new code.

---

## 0. Environment facts

| Item | Value |
|---|---|
| Session struct | `struct pdu_ses_ctx` — nested lists of structs, **no pointers**. **Treated as opaque by all new code** (§3.0). |
| Memory | No `malloc`. All objects from **DPDK mempool**. |
| Redis client | **hiredis**, event-loop driven. |
| Redis topology | **Sentinel, 5 master/slave pairs** (client-side sharding, *not* Redis Cluster). |
| Session key | `pdu_<partid>:<seid>` — a Redis **hash**. **MUST NOT CHANGE.** |
| SEID / TEID | May change. |
| part_id | 10 bits, in **both** SEID and TEID. Derived from the UE IP (allocated by the SMF, arrives in Create PDR). Used by VDP for LB/HA. part_id → VDP mapping is system-persistent. |
| Sessions without UE IP | Exist. part_id assigned round-robin. **The UE IP is never supplied later** — a session either has it at establishment or never. |
| Logging | `V_LOG(level, module, ...)`. Levels: `DEV`, `DEBUG`, `INFO`, `ERR`, `WARNING`, `CRIT`. Modules: `PFCP`, `DPDB`, `MEM`. |
| Return codes | `RET_CODE_OK` / `RET_CODE_ERR`. Test with `ret < 0`. |
| File naming | `v_` prefix. Indentation: **spaces**. |
| Threads | 1 IO thread, 4 workers, per pod. |
| Build | `make`. DPDK **22.11.1**. |
| Deployment | Kubernetes. Northbound: OSPF ECMP on the switch. |

**Out of scope for this plan:** metrics, liveness/readiness probes, config
wiring from the netconf server. Assume config values arrive as constants or
globals; Stage 2 binds them.

---

## 1. Component inventory

### Reused, unchanged (guaranteed independent of each other)

| Component | Role |
|---|---|
| **PFCP business logic** | encode, decode, build / modify / delete session |
| **PFCP IO** | UDP socket, receive/send PFCP datagrams |
| **VDP IO** | TCP connection to VDP, internal message header |
| **VDP connection handling** | connection lifecycle, reconnect |
| **DB connection handling + command layer** | connections, Sentinel failover, shard routing. **Lacks `EVAL`/Lua — see §4.** |
| **pdu_ses_ctx serialize / deserialize** | blob ↔ struct |
| **Transaction table** | hash keyed on SMF and VDP side; holds mempool pointers and metadata |

### Built new (Stage 1)

- `v_seid` — SEID/TEID bit layout, encode, decode, validate
- `v_sess_store` — session persistence with version CAS
- `v_id_alloc` — SEID/TEID rings with Redis block leasing
- `v_retrans_cache` — cross-pod retransmission dedup
- `v_node_state` — Node ID, Recovery Time Stamp, association state
- `v_dispatch` — rings between PFCP IO and the 4 workers; worker loop
- `v_flow` — the establishment / modification / deletion state flows
- `v_db_script` — EVAL/EVALSHA layered on the reused DB command layer
- `v_port_*.h` adapter headers + `v_stub_*.c` stub implementations
- Standalone test suite

---

## 2. The design in two changes

The old VPE pinned each session to one worker in one pod so two messages could
not mutate it concurrently. That pinning is what prevents scaling pods down,
prevents clean rolling upgrades, and forces an internal relay path.

### Change 1 — reply to the SMF only after the Redis write completes

The old service replied first and let the write finish later. A pod dying in
that window loses a session the SMF believes is up, and no other pod can find
it. Statelessness is impossible in that state, because the state is not
anywhere recoverable.

> **Invariant: every session the SMF believes exists is in Redis.**

This is *not* synchronous Redis. The write stays async; only the *reply* moves
into the completion callback. The worker never blocks and keeps processing
other sessions while the write is in flight. Cost is one Redis RTT on that
session's establishment, against an SMF timer measured in seconds.

### Change 2 — version CAS instead of ownership

A distributed lock would work but reintroduces TTLs, expiry-while-held, fencing
tokens for the paused-pod case, and release-on-crash — the ownership system
under a different name. Optimistic concurrency has nothing to release: read
returns `(state, ver)`, write asserts `ver` unchanged, mismatch means re-read
and re-apply.

**Why the version field is nearly free:** the session record is a Redis *hash*,
so `ver` is a new **field inside the existing key**. `struct pdu_ses_ctx` is
unchanged, the serializer is unchanged, the VDP wire format is unchanged, and
`pdu_<partid>:<seid>` is unchanged.

Without it, two pods doing read-modify-write on one session silently lose an
update — exactly the race ownership prevents today.

---

## 3. Adapter contract

### 3.0 The rule that makes Stage 1 possible

> **New code never dereferences `struct pdu_ses_ctx`.**

It is passed by pointer, handed to the serializer, handed to the business
logic, and stored. Nothing in the new code reads a field from it. Every value
the new code needs — UE IP, SEID, sequence number — is obtained from the
decoded PFCP message through a port function.

Stage 1 therefore declares it as an incomplete type:

```c
struct pdu_ses_ctx;     /* opaque — never completed in new code */
```

Stage 2 includes the real header. Nothing else changes.

### 3.1 `v_port_pfcp.h`

```c
struct pfcp_msg;        /* opaque */
struct pdu_ses_ctx;     /* opaque */

/* --- PFCP IO (reused) --- */
typedef void (*v_pfcp_rx_cb_t)(const uint8_t *buf, size_t len,
                               const struct sockaddr *peer, void *arg);
int v_port_pfcp_io_init(v_pfcp_rx_cb_t cb, void *arg);
int v_port_pfcp_io_send(const struct sockaddr *peer,
                        const uint8_t *buf, size_t len);

/* --- PFCP business logic (reused) --- */
int v_port_pfcp_decode(const uint8_t *buf, size_t len, struct pfcp_msg **out);
void v_port_pfcp_msg_free(struct pfcp_msg *msg);

uint8_t  v_port_pfcp_msg_type(const struct pfcp_msg *m);
uint64_t v_port_pfcp_hdr_seid(const struct pfcp_msg *m);
uint32_t v_port_pfcp_seq(const struct pfcp_msg *m);
uint64_t v_port_pfcp_smf_fseid(const struct pfcp_msg *m);

/* returns RET_CODE_ERR if the session carries no UE IP */
int v_port_pfcp_ue_ip(const struct pfcp_msg *m, uint32_t *v4, uint8_t v6[16]);

int v_port_pfcp_build_session(const struct pfcp_msg *req,
                              uint64_t seid, uint32_t teid,
                              struct pdu_ses_ctx *ctx);
int v_port_pfcp_modify_session(const struct pfcp_msg *req,
                               struct pdu_ses_ctx *ctx);
int v_port_pfcp_delete_session(const struct pfcp_msg *req,
                               struct pdu_ses_ctx *ctx);

int v_port_pfcp_encode_rsp(const struct pfcp_msg *req,
                           const struct pdu_ses_ctx *ctx, uint8_t cause,
                           uint8_t *buf, size_t *len);

/* --- serializer (reused) --- */
int v_port_pdu_serialize(const struct pdu_ses_ctx *ctx,
                         uint8_t *buf, size_t *len);
int v_port_pdu_deserialize(const uint8_t *buf, size_t len,
                           struct pdu_ses_ctx *ctx);
```

### 3.2 `v_port_db.h`

```c
typedef void (*v_db_cb_t)(int status, const char *reply, size_t len, void *arg);

/* reused command layer */
int v_port_db_cmd(uint8_t shard, v_db_cb_t cb, void *arg,
                  const char *fmt, ...);
uint8_t v_port_db_shard_of(const char *key);
int v_port_db_shard_count(void);          /* expected: 5 */

/* CONFIRMED PRESENT in the reused DB layer. Fired on (re)connect and on
 * Sentinel promotion. v_db_script binds to it to reload the Lua cache — §4.
 * Stage 2 wires this to the existing hook; no change to the DB layer needed. */
typedef void (*v_db_reconnect_cb_t)(uint8_t shard, void *arg);
int v_port_db_on_reconnect(v_db_reconnect_cb_t cb, void *arg);
```

### 3.3 `v_port_vdp.h`

```c
typedef enum { V_VDP_ACCEPT, V_VDP_REJECT, V_VDP_TIMEOUT } v_vdp_result_t;
typedef void (*v_vdp_cb_t)(v_vdp_result_t res, void *arg);

int v_port_vdp_io_init(void);
int v_port_vdp_session_create(uint16_t part_id, const struct pdu_ses_ctx *ctx,
                              v_vdp_cb_t cb, void *arg);
int v_port_vdp_session_modify(uint16_t part_id, const struct pdu_ses_ctx *ctx,
                              v_vdp_cb_t cb, void *arg);
int v_port_vdp_session_delete(uint16_t part_id, uint64_t seid,
                              v_vdp_cb_t cb, void *arg);
```

### 3.4 `v_port_mem.h`

```c
struct pdu_ses_ctx *v_port_ctx_alloc(void);
void v_port_ctx_free(struct pdu_ses_ctx *ctx);
void v_port_ctx_clear(struct pdu_ses_ctx *ctx);   /* reuse across CAS retry */
size_t v_port_ctx_pool_in_use(void);              /* for leak assertions */
```

### 3.5 `v_port_txn.h`

The transaction table is reused, but the new code needs states and fields it
does not currently have. Expose it through a port so Stage 1 can stub it and
Stage 2 can decide whether to extend the real table or wrap it.

```c
typedef enum {
    TXN_ST_RECV,
    TXN_ST_DB_READ_WAIT,
    TXN_ST_DB_WRITE_WAIT,
    TXN_ST_VDP_WAIT,
    TXN_ST_DB_CONFIRM_WAIT,
    TXN_ST_REPLIED,
} v_txn_state_t;

struct v_txn {
    v_txn_state_t       state;
    uint64_t            db_ver;      /* version read, for the CAS */
    uint8_t             cas_retry;   /* max V_CAS_MAX_RETRY */
    uint16_t            part_id;
    uint64_t            seid;
    uint32_t            teid;
    struct pfcp_msg    *req;
    struct pdu_ses_ctx *ctx;
    struct sockaddr     peer;
    uint64_t            deadline_ms;
    void               *impl;        /* reused table's own handle */
};

struct v_txn *v_port_txn_create(void);
void          v_port_txn_destroy(struct v_txn *t);
struct v_txn *v_port_txn_find_by_seq(uint64_t smf_fseid, uint32_t seq);
void          v_port_txn_set_state(struct v_txn *t, v_txn_state_t st);

/* CONFIRMED PRESENT: the reused table enforces a single HARD timeout for the
 * whole transaction — not per-state. Bind this callback to it. */
typedef void (*v_txn_timeout_cb_t)(struct v_txn *t, void *arg);
int v_port_txn_on_timeout(v_txn_timeout_cb_t cb, void *arg);
```

**Consequence of a single hard timeout.** The new flows spend time in several
sequential async waits, and they all share one budget:

```
read (1 RTT)
  + up to V_CAS_MAX_RETRY × (read + write)   /* CAS conflict retries */
  + VDP round trip
  + confirm write (1 RTT)
```

Worst case is roughly `(2 + 2 × V_CAS_MAX_RETRY) × redis_rtt + vdp_rtt`. Two
things follow:

- **The hard timeout must exceed that worst case**, or a session under
  contention gets killed mid-flight rather than retried. Check the configured
  value at Stage 2 against measured Redis RTT; if it is tight, lower
  `V_CAS_MAX_RETRY` rather than raising the timeout.
- **The hard timeout must stay below the SMF's retransmission timer**, or the
  SMF retransmits while the original is still in flight, and both compete for
  the same session. The retransmit cache handles the result correctly, but it
  is wasted work.

`v_txn.state` is still tracked, because the timeout handler must know which
wait it fired in — a timeout in `TXN_ST_VDP_WAIT` needs the Redis record
cleaned up, while one in `TXN_ST_DB_WRITE_WAIT` does not.

### 3.6 Stubs (Stage 1 deliverable)

One `v_stub_*.c` per port, sufficient for the standalone suite:

- **PFCP stub** — synthetic `struct pfcp_msg` with settable type, SEID, seq,
  F-SEID, UE IP. Build/modify/delete are no-ops returning `RET_CODE_OK`.
  Serializer emits a fixed-size blob echoing an injected pattern.
- **DB stub** — either a real local Redis via hiredis (**preferred**, so the
  Lua is genuinely exercised) or an in-memory map with scripted failures.
- **VDP stub** — returns `ACCEPT` by default; test hooks force `REJECT` and
  `TIMEOUT`.
- **Mem stub** — a real `rte_mempool` of fixed-size opaque blocks, so leak
  assertions are meaningful.
- **Txn stub** — plain hash table implementing the port.

Stubs live in a separate directory and are excluded from the production build
by a Makefile target. Stage 2 must never ship them.

---

## 4. Lua support — first module built

**The CAS design requires server-side Lua, and the reused DB command layer does
not support it.** `v_db_script` is the first thing written, since `v_sess_store`
and `v_id_alloc` both depend on it.

```c
int v_db_script_init(void);       /* SCRIPT LOAD all scripts to all shards */
int v_db_evalsha(uint8_t shard, int script_id,
                 int nkeys, const char **keys,
                 const char **argv, int nargv,
                 v_db_cb_t cb, void *arg);
```

Four things that will bite:

**The script cache is per-master and is not replicated.** `SCRIPT LOAD` to all
5 masters at startup, and again on every reconnect via
`v_port_db_on_reconnect`.

**Sentinel failover produces `NOSCRIPT`.** The promoted replica has an empty
script cache. Detect the error string, `SCRIPT LOAD` to that shard, retry the
`EVALSHA` **once**, then fail. Without this, one failover breaks all writes
until a process restart.

**All keys in one script must be on one shard.** The session script touches
only `pdu_<partid>:<seid>`, so it is safe by construction. Route the ID
allocation and retransmit keys through `v_port_db_shard_of()` on a
`<partid>`-derived key so they stay co-located per partition.

**Scripts must be deterministic.** No `TIME`, no `RANDOMKEY`. All the scripts
here already satisfy this.

---

## 5. Target flows

### 5.1 Session Establishment

```
1.  PFCP IO delivers a datagram → v_dispatch enqueues work to a worker ring.
2.  Worker: validate header (§7). v_port_pfcp_decode().
3.  Retransmit check: v_retrans_lookup(smf_fseid, seq)
      hit  → reply with the cached response, done.
      miss → continue.
4.  part_id:
      v_port_pfcp_ue_ip() == RET_CODE_OK → part_id = hash(ue_ip)
      else                               → part_id = v_part_rr()      [§6.4]
5.  seid = v_seid_alloc(part_id)   /* ring dequeue; may trigger async refill */
    teid = v_teid_alloc(part_id)
6.  v_port_pfcp_build_session(req, seid, teid, ctx)
7.  v_sess_cas_write(part_id, seid, exp_ver=0, ctx, V_SESS_PENDING)
      → HSET data+ver, EXPIRE 30      /* pending: self-cleaning */
    txn → TXN_ST_DB_WRITE_WAIT.  Worker returns to its ring.
8.  DB completion → v_port_vdp_session_create().  txn → TXN_ST_VDP_WAIT.
9.  ACCEPT → v_sess_confirm() (PERSIST) → TXN_ST_DB_CONFIRM_WAIT
    REJECT → v_sess_delete(), free IDs, reply SMF failure
10. Confirm completion →
      v_retrans_store(response)        /* SETEX 10s */
      v_port_pfcp_io_send(response)
      free ctx, destroy txn
```

**Why PENDING + TTL.** VDP can reject, so establishment is a two-phase commit.
Writing Redis first and compensating on reject leaves an orphan if the pod dies
before the reject arrives — and that orphan is unfindable, because the SMF's
retransmitted Establishment allocates a *fresh* SEID and never collides with
it. A 30s TTL makes Redis clean up after itself; `PERSIST` on accept.

**Why Redis before VDP.** If the VDP push fails, the session is durable and the
rule is reinstallable. Reversed, a successful VDP push followed by a Redis
failure leaves a forwarding rule for a session nobody knows about.

### 5.2 Session Modification

```
1-3. As above (validate, decode, retransmit check).
4.   part_id = v_seid_part(seid)
5.   v_sess_read(part_id, seid) → (ctx, ver)
       not found → reply Session context not found
6.   v_port_pfcp_modify_session(req, ctx)
7.   v_sess_cas_write(part_id, seid, exp_ver=ver, ctx, V_SESS_CONFIRMED)
       V_CAS_OK       → continue
       V_CAS_CONFLICT → v_port_ctx_clear(ctx); goto 5;
                        retry max V_CAS_MAX_RETRY, then SYSTEM_FAILURE
       V_CAS_GONE     → reply Session context not found
8.   v_port_vdp_session_modify(). On REJECT → CAS-write the old ctx back,
     reply failure.
9.   v_retrans_store(response); reply SMF.
```

Race outcomes:
- Two concurrent Modifications → one wins, the other retries against fresh
  state. Both end up applied.
- Modification racing Deletion → deletion wins, the modification returns
  *Session context not found*, which is correct.

### 5.3 Session Deletion

```
1-5. As Modification (read to obtain ver).
6.   v_port_vdp_session_delete(), wait ACCEPT.
7.   v_sess_delete(part_id, seid, exp_ver=ver)     /* CAS-guarded DEL */
8.   v_teid_free(part_id, teid)                    /* RPUSH to Redis free list */
     /* SEID is deliberately NOT reclaimed — §6.3 */
9.   v_retrans_store(response); reply SMF.
```

VDP first here: a pod dying after the VDP delete but before the Redis delete
leaves a harmless orphan record — the next Modification fails at VDP and the
SMF cleans up. The reverse leaves a live forwarding rule for a session that no
longer exists, which is worse.

---

## 6. New modules in detail

### 6.1 `v_sess_store`

```c
#define V_SESS_KEY_FMT      "pdu_%u:%lu"     /* UNCHANGED — partid, seid */
#define V_SESS_FIELD_DATA   "data"
#define V_SESS_FIELD_VER    "ver"
#define V_SESS_PENDING_TTL  30
#define V_CAS_MAX_RETRY     3

typedef enum {
    V_CAS_OK       =  1,
    V_CAS_CONFLICT =  0,
    V_CAS_GONE     = -1,
} v_cas_result_t;

typedef enum { V_SESS_PENDING, V_SESS_CONFIRMED } v_sess_state_t;

/* All async. The RETURN CODE says whether the request was ISSUED
 * (RET_CODE_OK / RET_CODE_ERR, tested with ret < 0).
 * The CAS OUTCOME arrives in the callback as v_cas_result_t.
 * Do not conflate the two. */
int v_sess_read(uint16_t part_id, uint64_t seid,
                v_sess_read_cb_t cb, void *arg);
int v_sess_cas_write(uint16_t part_id, uint64_t seid, uint64_t exp_ver,
                     const struct pdu_ses_ctx *ctx, v_sess_state_t state,
                     v_sess_cas_cb_t cb, void *arg);
int v_sess_confirm(uint16_t part_id, uint64_t seid,
                   v_sess_cb_t cb, void *arg);       /* PERSIST */
int v_sess_delete(uint16_t part_id, uint64_t seid, uint64_t exp_ver,
                  v_sess_cas_cb_t cb, void *arg);
```

```lua
-- v_sess_cas_write
-- KEYS[1] = pdu_<part>:<seid>
-- ARGV[1] = expected_ver ("0" = must not exist)
-- ARGV[2] = serialized blob
-- ARGV[3] = ttl seconds, or "0" for none
local v = redis.call('HGET', KEYS[1], 'ver')
if ARGV[1] == '0' then
    if v then return 0 end                      -- exists, caller expected new
    redis.call('HSET', KEYS[1], 'ver', 1, 'data', ARGV[2])
    if ARGV[3] ~= '0' then redis.call('EXPIRE', KEYS[1], ARGV[3]) end
    return 1
end
if not v then return -1 end                     -- deleted meanwhile
if v ~= ARGV[1] then return 0 end               -- lost the race
redis.call('HSET', KEYS[1], 'ver', v + 1, 'data', ARGV[2])
if ARGV[3] ~= '0' then redis.call('EXPIRE', KEYS[1], ARGV[3]) end
return 1
```

```lua
-- v_sess_delete
-- KEYS[1] = pdu_<part>:<seid>   ARGV[1] = expected_ver
local v = redis.call('HGET', KEYS[1], 'ver')
if not v then return -1 end
if v ~= ARGV[1] then return 0 end
redis.call('DEL', KEYS[1])
return 1
```

**Mempool discipline on retry.** Each CAS retry re-reads and re-deserializes.
Use `v_port_ctx_clear()` and reuse the same object, or free before reallocating.
This is the most likely source of a slow mempool leak in the whole build, and
it only manifests under contention — the retry test must assert
`v_port_ctx_pool_in_use()` returns to baseline.

### 6.2 `v_id_alloc`

```c
#define V_NUM_PARTS       1024
#define V_ID_BLK_SIZE     256        /* small on purpose — §6.3 */
#define V_ID_WATERMARK    64
#define V_TEID_FREE_FLOOR 1024       /* quarantine depth — §6.5 */

int      v_id_alloc_init(void);      /* 2048 rings, seed, gate traffic */
uint64_t v_seid_alloc(uint16_t part_id);
uint32_t v_teid_alloc(uint16_t part_id);
void     v_teid_free(uint16_t part_id, uint32_t teid);
/* deliberately no v_seid_free — §6.3 */
```

1024 ring pairs, statically allocated at startup. Memory is not a constraint;
static allocation avoids a hot-path branch and first-touch latency spikes.

**Ring flags: MC/MP.** Any of the 4 workers may allocate in any partition. The
alternative — 4096 rings, one set per worker, SC/SP — is faster but splits each
block four ways, raising leak-on-pod-death 4×. Establishment is not hot enough
to justify that.

**Block leasing:**

```c
uint64_t base = INCRBY("vpe:seid:<part>:next", V_ID_BLK_SIZE) - V_ID_BLK_SIZE;
for (uint32_t i = 0; i < V_ID_BLK_SIZE; i++)
    rte_ring_enqueue(seid_ring[part], (void *)(uintptr_t)(base + i));
```

The counter never decreases and never resets, so two pods can never be handed
the same block. One Redis op per 256 establishments in that partition. That is
the entire coordination cost of ID allocation.

**Async watermark refill:**

```c
if (rte_ring_count(ring[part]) < V_ID_WATERMARK)
    v_id_refill_async(part);        /* one in-flight per ring; do not stack */
if (rte_ring_dequeue(ring[part], &id) != 0) {
    V_LOG(WARNING, PFCP, "id ring dry, part=%u", part);
    return PFCP_CAUSE_NO_RESOURCES_AVAILABLE;
}
```

A synchronous refill on empty would block a worker on Redis exactly when the
establishment rate is highest. Watermark ≥ Redis RTT × peak establishment rate,
with margin.

**Startup:** do not accept PFCP traffic until the initial 2048 refills complete,
or the first burst hits empty rings and returns `NO_RESOURCES` for no reason.
Jitter the start so a mass pod restart does not produce a synchronized burst.
(Probe wiring is Stage 2 / out of scope; expose a `v_id_alloc_ready()`
predicate.)

### 6.3 SEID and TEID are NOT symmetric

| | SEID | TEID |
|---|---|---|
| Width | 64 bits | 32 bits |
| Local space after 10-bit part_id | ~48 bits | 21 bits ≈ 2M per partition |
| Reuse | **Never** | **Must reclaim** |
| Free list | none | required |
| Quarantine | n/a | required |

**SEID:** at 48 bits of local space you would need a billion establishments per
second for a century to exhaust one partition. Let deleted SEIDs vanish — no
free list, no quarantine, no reclamation code. This halves the module.

**TEID:** 2M per partition. A pod dying with a half-consumed block leaks the
remainder. At 64K blocks that is ~32 blocks per partition, so a pod restarting
hourly exhausts the space in days. At 256 blocks plus a free list it is
comfortable. **That is why `V_ID_BLK_SIZE` is 256 and not 64K.**

```lua
-- v_teid_refill: prefer the free list, fall back to the counter
-- KEYS[1] = vpe:teid:<part>:free   KEYS[2] = vpe:teid:<part>:next
-- ARGV[1] = block size             ARGV[2] = free-list floor
local out = {}
if redis.call('LLEN', KEYS[1]) > tonumber(ARGV[2]) then
    for i = 1, tonumber(ARGV[1]) do
        local id = redis.call('LPOP', KEYS[1])
        if not id then break end
        table.insert(out, id)
    end
end
if #out == 0 then
    local base = redis.call('INCRBY', KEYS[2], ARGV[1])
    return {'range', tostring(base - ARGV[1]), ARGV[1]}
end
return {'list', unpack(out)}
```

### 6.4 Round-robin part_id for sessions with no UE IP

```c
static uint16_t rr_base;    /* pod UID hash at boot — no coordination needed */
static uint16_t rr_ctr;

#define V_PART_RR_SPAN 16

static inline uint16_t v_part_rr(void)
{
    return (rr_base + (rr_ctr++ & (V_PART_RR_SPAN - 1))) & (V_NUM_PARTS - 1);
}
```

Round-robin across all 1024 would have every pod seeding every ring for a
handful of sessions. A 16-partition span still spreads across VDP instances
(part → VDP is many-to-one).

**Assumption to validate at Stage 2:** these sessions have no UE IP ever, so
VDP has no key to hash for downlink. Uplink is fine — the TEID carries part_id.
This is only safe if such sessions never carry downlink traffic. Confirm with
the VDP team rather than assuming.

### 6.5 TEID quarantine — do not skip

A TEID freed and immediately reissued can receive in-flight GTP-U from the old
session's gNB. That is **user data delivered to the wrong UE.**

`RPUSH` on free, `LPOP` on refill gives a delay proportional to free-list depth.
`V_TEID_FREE_FLOOR` enforces a minimum: below that depth, fall through to
`INCRBY` rather than popping. Size the floor so the delay comfortably exceeds
the worst-case gNB in-flight window.

### 6.6 `v_retrans_cache`

```c
#define V_RETRANS_KEY_FMT "txn_%u:%lu:%u"   /* partid, smf_fseid, seq */
#define V_RETRANS_TTL     10

int v_retrans_lookup(uint16_t part_id, uint64_t smf_fseid, uint32_t seq,
                     v_retrans_cb_t cb, void *arg);
int v_retrans_store(uint16_t part_id, uint64_t smf_fseid, uint32_t seq,
                    const uint8_t *resp, size_t len);
```

**Must be in Redis, not in-process** — the retransmit being deduplicated is
precisely the one that lands on a *different* pod after an SMF timeout.

**This is load-bearing, not an optimization.** The old service relayed
retransmits to the owner pod, which made the problem invisible. Without this
module, an SMF retransmit of an Establishment allocates a second SEID/TEID pair
and leaks the first.

Establishment has no SEID yet, so key on SMF F-SEID + sequence number. Derive
the routing `part_id` from `hash(smf_fseid)` — it only needs to be consistent.

### 6.7 `v_node_state`

The SMF associates with a *UPF node*, not a pod. These must be identical across
every replica:

- **Node ID** — one value for the whole Deployment. Never derived from pod
  hostname or IP.
- **Recovery Time Stamp** — the dangerous one. If each pod uses its own boot
  time, every heartbeat that ECMP lands on a different pod tells the SMF the
  UPF restarted, and it purges all sessions. Store in Redis
  (`upf:<nodeid>:recovery`), `SETNX` at first cold start, every pod reads it at
  boot. **A rolling update must not change it.** Bump deliberately, only on a
  real cold start.
- **Association state** — Association Setup arrives at one pod; all others must
  learn it from Redis. A pod booting mid-association must answer heartbeats
  correctly rather than replying "no association."

### 6.8 No read cache in v1

The old service had a large in-process cache serving three roles at once. Two
are gone by construction: the write buffer is the durability hole Change 1
removes, and transaction state is the transaction table.

That leaves a pure read cache. **Do not build it.** Ship without, measure Redis
read latency under real load, add it later only if the numbers justify it. CAS
makes such a cache *safe* — a stale read produces a version mismatch and
retries — but every cache is a class of bug.

---

## 7. Input validation

free5gc has open CVEs from exactly this code path (issues #730, #731): a
Session Modification or Deletion carrying `SEID = 0xFFFFFFFFFFFFFFFF` produces
a signed-conversion underflow and a negative array index. **The two entry
points are distinct — fixing one does not fix the other.**

`v_seid_part()` has the same exposure: an attacker-supplied SEID decodes to a
part_id used to index a 1024-entry array.

```c
static inline int v_seid_validate(uint64_t seid, uint16_t *part_out)
{
    uint16_t part;

    if ((seid >> 60) != V_SEID_FMT_VER) {
        V_LOG(WARNING, PFCP, "bad seid fmt: 0x%016lx", seid);
        return RET_CODE_ERR;
    }
    part = v_seid_part(seid);
    if (part >= V_NUM_PARTS) {
        V_LOG(WARNING, PFCP, "bad seid part=%u: 0x%016lx", part, seid);
        return RET_CODE_ERR;
    }
    *part_out = part;
    return RET_CODE_OK;
}
```

Call it in **every** handler — establishment, modification, deletion, report —
not only in the shared dispatcher. Per TS 29.244, the correct reply is *Session
context not found* with the SEID in the **response header set to 0**.

**TEID 0 is reserved.** Start local indices at 1.

The 4 VPE-ID bits from the old SEID layout are **reserved-must-be-zero** —
validated, never set, never reused. Live SEIDs sit in SMF memory for the life
of a session, so reclaiming those bits needs a format version bump and a
parallel-decoder migration.

---

## 8. Stage 1 — build order (Claude Code)

Everything below compiles and passes tests with stubs only. No access to the
existing codebase is required at any step.

| # | Milestone | Done when |
|---|---|---|
| **S1** | Ports + stubs + Makefile (prod target excludes stubs) | Empty binary links; stub suite runs |
| **S2** | `v_db_script` — EVAL/EVALSHA, script load, NOSCRIPT retry | Against local Redis: scripts load to all shards; simulated `SCRIPT FLUSH` recovers |
| **S3** | `v_seid` — layout, encode, decode, validate | Fuzz suite passes (§7) |
| **S4** | `v_id_alloc` | Two simulated pods never receive the same ID; watermark refill under load; free-list floor honoured |
| **S5** | `v_sess_store` | CAS ok / conflict / gone; pending TTL expiry; NOSCRIPT retry; mempool baseline restored after retries |
| **S6** | Txn state machine + timeout sweeper | Every timeout path frees ctx; pool returns to baseline |
| **S7** | `v_dispatch` — rings, worker loop | Echo test through the stub PFCP IO; no PFCP semantics |
| **S8** | `v_flow` establishment | Full flow against stubs; ordering asserted; REJECT and TIMEOUT paths correct |
| **S9** | `v_retrans_cache` + wiring | Two simulated pods, forced mislanded retransmit, single allocation |
| **S10** | `v_flow` modification / deletion / report | Concurrent modification; modification-racing-deletion |
| **S11** | `v_node_state` | Recovery Time Stamp stable across simulated restarts |
| **S12** | Handoff package (§9.1) | Builds clean; suite green; open items listed |

### 8.1 Standalone test suite

- Lua against local Redis: CAS success / conflict / gone, NOSCRIPT recovery,
  pending TTL expiry.
- `v_id_alloc`: block uniqueness across simulated pods, watermark refill,
  free-list floor, ring exhaustion → `NO_RESOURCES_AVAILABLE`.
- `v_seid_validate`: fuzz `0xFFFFFFFFFFFFFFFF`, part_id ≥ 1024, wrong format
  version, non-zero reserved bits, SEID 0 in non-establishment messages.
- Txn: every timeout path frees mempool objects; assert baseline.
- Flows: establishment ACCEPT / REJECT / TIMEOUT; modification CAS conflict
  retry; deletion; retransmit dedup across two simulated pods.
- **Fault injection at the ports:** DB error, DB timeout, VDP reject, VDP
  timeout, ring exhaustion, NOSCRIPT. Every one must free its mempool object.

---

## 9. Stage 2 — integration (private agent)

### 9.1 What Stage 1 hands over

- New source tree, `v_`-prefixed, spaces, `V_LOG`, `RET_CODE_*`.
- `v_port_*.h` — the complete contract.
- `v_stub_*.c` — reference behaviour, excluded from the production build.
- Standalone test suite plus a Makefile target that runs it.
- **`INTEGRATION.md`**: every assumption made about the reused modules, every
  port function, and what Stage 2 must supply for each.

### 9.2 Integration order

| # | Task | Verify |
|---|---|---|
| **I1** | Replace the opaque `struct pdu_ses_ctx` forward declaration with the real header. Confirm no new code dereferences it (grep for `->` on a ctx pointer). | Compiles; no new field access |
| **I2** | Bind `v_port_mem` to the real DPDK mempool | Existing pool sizing honoured; `pool_in_use()` accurate |
| **I3** | Bind `v_port_pfcp` serializer + business logic | Round-trip test: build → serialize → deserialize → compare |
| **I4** | Bind `v_port_db`. **Confirm `v_port_db_shard_of()` is `<partid>`-derived**; if not, adjust key construction so per-partition keys co-locate | All Lua scripts execute; no CROSSSLOT-equivalent errors |
| **I5** | Bind `v_port_db_on_reconnect` to the existing reconnect hook | Kill a master; verify script reload and write recovery |
| **I6** | Bind `v_port_pfcp_io` | Real datagrams reach `v_dispatch` |
| **I7** | Bind `v_port_vdp` | Real accept/reject drive the txn correctly |
| **I8** | Bind `v_port_txn` — extend the real table with the new states/fields, or wrap it. Bind the callback to the existing hard timeout. **Check the configured timeout against the §3.5 worst-case budget** | Timeout fires and frees ctx from every wait state; no stranded transactions |
| **I9** | Delete stubs from the production build | Stub symbols absent from the binary |
| **I10** | Run the full standalone suite against real bindings | All green |
| **I11** | Integrated tests (§9.3) | All green |

### 9.3 Integrated tests (need an SMF simulator / load generator)

- **Kill a pod mid-establishment** (SIGKILL between allocate and reply). SMF
  times out, retransmits, a different pod serves it; no duplicate SEID, no
  leaked TEID, pending record expired.
- **Concurrent Modification storm** on one session from two pods. All applied,
  none lost, no corruption.
- **Modification racing Deletion.** Deletion wins; modification returns
  *Session context not found*.
- **Retransmit landing on a different pod.** Cached response returned, no
  second allocation.
- **Scale 8 → 2 → 8 pods under traffic.** Zero session loss, zero SMF-visible
  errors. **This is the acceptance test for the whole design.**
- **Redis unreachable.** Existing sessions keep forwarding (VDP holds the
  rules); new establishments rejected with `NO_RESOURCES_AVAILABLE`; no crash;
  clean recovery.
- **Sentinel failover under load.** Verify NOSCRIPT recovery; quantify the
  lost-write window (§10.1).
- **TEID reuse timing.** Delete a session, force reallocation of the same TEID,
  replay in-flight GTP-U from the old tunnel. Expect dropped, not delivered.
- **Rolling restart.** Recovery Time Stamp unchanged; SMF does not purge.

### 9.4 Likely friction points

The two highest-risk items — a missing DB reconnect hook and a missing
transaction timeout — are **resolved**: both exist in the reused modules. No
change to existing code is required for either. What remains is adapter work,
none of which touches the new modules:

1. **The transaction timeout is a single hard budget, not per-state.** Verify
   the configured value exceeds the §3.5 worst case, and sits below the SMF's
   retransmission timer. If it is tight, lower `V_CAS_MAX_RETRY` rather than
   raising the timeout.
2. **Business logic signatures will not match §3.1 exactly.** Expected and
   fine — adapt in the port, never in the new code.
3. **The serializer may allocate its own buffer** rather than filling a caller
   buffer. Adapt in the port.
4. **PFCP IO may deliver parsed messages rather than raw datagrams.** Adapt in
   the port; `v_dispatch` does not care.
5. **`v_port_db_shard_of()` may not be `<partid>`-derived.** If not, adjust key
   construction so per-partition keys co-locate (I4). Correctness is unaffected
   either way — only locality.

---

## 10. Deployment

### 10.1 Sentinel lost writes — decide explicitly

Redis replication is asynchronous. A master failover can drop writes the client
already saw acknowledged, weakening the §2 invariant during the failover
window.

- **Accept it.** Rare, small window, and the SMF eventually detects via audit
  or a failed Modification. **Recommended.**
- Or issue `WAIT 1 <ms>` after each session write. Better correctness, worse
  latency and worse failure modes under a degraded replica. Not recommended at
  establishment rates.

### 10.2 Cutover

The new service reads and writes the same Redis keys as the old one, which
makes a mixed fleet tempting and dangerous: an old pod may hold session state
in its write-buffer cache that was never made durable, and a new pod cannot see
it. **Do not run old and new pods against the same Node ID simultaneously.**

Recommended: **separate Node ID and separate PFCP IP for the new deployment.**
The SMF treats it as a distinct UPF. Steer new PDU sessions to the new node and
let the old node drain naturally as sessions end. No shared state, no mixed
fleet, no session loss.

Fallback if the SMF cannot be steered per-node: a maintenance window — drain
old, deploy new, accept re-establishment. Simpler, but visible to subscribers.

**Do not attempt a rolling in-place replacement.** The two services disagree
about who owns a session, and there is no protocol between them to resolve it.

### 10.3 Kubernetes

- **Deployment**, not StatefulSet — nothing needs stable pod identity.
- `preStop`: withdraw the BGP route, sleep ~5s for switch convergence, exit.
  No flush (writes are already durable), no lease release (there are no
  leases).
- HPA on session count or PFCP req/s, not CPU.
  `scaleDown.stabilizationWindowSeconds: 300`.

---

## 11. Deliberately NOT doing

Recorded so these are not re-litigated from scratch.

- **wslot / partition leasing / epoch fencing.** An earlier design gave VPE its
  own ownership partition space (separate from VDP's part_id), leased to pods
  with an epoch fence. It works, and it is the right answer *if* the SMF reply
  cannot be deferred until the write completes. Since it can, all of that
  machinery is unnecessary. **Do not rebuild it.**
- **Distributed locks.** Equivalent to leases — same TTL, same fencing, same
  release-on-crash. CAS is strictly simpler.
- **Changing the session key.** `pdu_<partid>:<seid>` is fixed. The version
  lives in a new *field*, not a new key.
- **Touching the PFCP business logic.** Encode, decode, and build/modify/delete
  are reused verbatim. If a change looks necessary, adapt at the port instead.
- **A read cache in v1.** See §6.8.
- **Splitting VPE into further microservices.** The CUPS split (VPE/VDP) is
  already the correct decomposition. Splitting further along the same axis
  inserts network hops into a latency budget that cannot absorb them.
  Components that *do* separate cleanly — usage report aggregation, charging
  export, config distribution — tolerate hundreds of ms and have no SMF timer
  waiting. The PFCP session lifecycle and GTP-U forwarding are one latency
  domain.
- **Per-pod Node ID / Recovery Time Stamp** as a steady state. That is the
  architecture where each pod is an independent small UPF and sessions die with
  their pod — simpler in places, but it accepts session loss on pod restart.
  (§10.2 uses a separate Node ID as a one-time migration boundary, which is a
  different thing.)
- **Encoding VPE worker or pod identity in the SEID.** Makes the SEID a
  physical address; breaks on pod death, since adopted sessions name workers
  that no longer exist. The 4 VPE-ID bits in the old layout are the vestige of
  this and stay reserved-must-be-zero.

---

## 12. Reference

- **free5gc issues #730, #731** — the SEID validation bugs behind §7.
- **omec-project/upf** — Go PFCP agent with datapath plugins; useful for the
  control→datapath interface boundary.
- **5GOpenUPF/openupf** — R16-compliant, C, DPDK. LBU/SMU/FPU split is close to
  our VDP/VPE decomposition.
- **TS 29.244** — SEID 0 semantics, Recovery Time Stamp behaviour, *Session
  context not found* handling.
- **Hazelcast partition table / Kafka partition assignment / Vitess shard
  leases** — prior art for the ownership design in §11, should it ever become
  necessary.
