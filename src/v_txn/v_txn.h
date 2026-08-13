#ifndef VPE_V_TXN_H
#define VPE_V_TXN_H

/*
 * v_txn — timeout sweeper layered on the v_port_txn table (plan.md
 * §3.5). The reused table enforces one hard timeout per transaction,
 * not per-state; this module is what actually decides what "timed out"
 * means for each v_txn_state_t and cleans up accordingly (frees ctx,
 * frees the held pfcp_msg, and where the state demands it, compensates
 * in Redis) before destroying the txn.
 *
 * KNOWN GAP (see INTEGRATION.md): a hard timeout in TXN_ST_VDP_WAIT
 * during Modification/Deletion (t->db_ver != 0) cannot compensate the
 * session record back to its pre-modification value here, because
 * v_port_txn's struct v_txn has no field for the prior ctx — only
 * v_flow's own (non-timeout) REJECT handling path has that value in
 * scope. This sweeper logs the anomaly and frees resources without
 * attempting the compensating write.
 */

int v_txn_sm_init(void);
void v_txn_sm_fini(void);

#endif /* VPE_V_TXN_H */
