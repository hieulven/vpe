#ifndef VPE_V_NODE_STATE_H
#define VPE_V_NODE_STATE_H

#include <stdint.h>
#include <stddef.h>

/*
 * v_node_state — Node ID, Recovery Time Stamp, association state.
 * plan.md §6.7. The SMF associates with a UPF *node*, not a pod: these
 * three must be identical across every replica in the Deployment.
 *
 * Node ID: process-wide config (out of scope per plan.md §0 — "Assume
 * config values arrive as constants or globals; Stage 2 binds them"),
 * never derived from pod hostname/IP. Passed in at v_node_state_init().
 *
 * Recovery Time Stamp: the dangerous one. Per-pod boot time would tell
 * the SMF the UPF restarted on every ECMP-landed heartbeat, purging all
 * sessions. Stored in Redis (upf:<nodeid>:recovery), created with
 * SET...NX on first cold start; every pod reads whatever's already
 * there. Load it ONCE at startup and cache it — v_node_state_recovery_ts()
 * must never re-touch Redis on the heartbeat hot path.
 *
 * Association state: Association Setup can land on any one pod; every
 * other pod (including ones that boot mid-association) must be able to
 * answer heartbeats correctly, so this lives in Redis too, not in a
 * particular worker's memory (this is also why v_dispatch has no
 * "worker 0 handles node-level messages" special case — see its design
 * note).
 */

#define V_NODE_ID_MAX_BYTES 64

int v_node_state_init(const uint8_t *node_id, size_t node_id_len);
void v_node_state_fini(void);

typedef void (*v_node_state_recovery_cb_t)(int status, uint32_t recovery_ts, void *arg);

/* One round trip: SET upf:<id>:recovery <now> NX GET. NIL reply means
 * this pod won the race and <now> is authoritative; a string reply
 * means an earlier cold start already set it — use that value instead.
 * Caches the result; call once at startup. */
int v_node_state_load_recovery_ts(v_node_state_recovery_cb_t cb, void *arg);

/* Cached accessor for the heartbeat hot path — never touches Redis. */
uint32_t v_node_state_recovery_ts(void);
int v_node_state_recovery_ts_ready(void);

/* Deliberate cold start ONLY — unconditionally overwrites the stored
 * timestamp. Never call this from a rolling-update path (plan.md §6.7:
 * "A rolling update must not change it. Bump deliberately, only on a
 * real cold start."). Not wired to anything automatic in Stage 1 —
 * exposed for Stage 2's operator-triggered cold-start path. */
int v_node_state_force_new_recovery_ts(v_node_state_recovery_cb_t cb, void *arg);

typedef void (*v_node_state_cb_t)(int status, void *arg);

int v_node_state_set_associated(uint64_t smf_node_id_hash, v_node_state_cb_t cb, void *arg);
int v_node_state_clear_associated(v_node_state_cb_t cb, void *arg);

typedef void (*v_node_state_assoc_cb_t)(int status, int associated, uint64_t smf_node_id_hash, void *arg);
int v_node_state_query_associated(v_node_state_assoc_cb_t cb, void *arg);

#endif /* VPE_V_NODE_STATE_H */
