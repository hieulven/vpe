#ifndef VPE_TEST_REGISTRY_H
#define VPE_TEST_REGISTRY_H

/* One entry point per test_*.c file. main_test.c calls these in order
 * after global setup. Declared here so adding a test file only means
 * adding one line here + one line in main_test.c. */

void test_db_script(void);
void test_db_script_noscript(void);
void test_seid(void);
void test_id_alloc_uniqueness(void);
void test_id_alloc_teid_floor(void);
void test_sess_store_cas(void);
void test_sess_store_pending_ttl(void);
void test_txn_sm_timeout_paths(void);
void test_dispatch_echo(void);
void test_dispatch_any_worker(void);
void test_retrans_cache_basic(void);
void test_retrans_cache_ttl(void);
void test_flow_establishment_accept(void);
void test_flow_establishment_reject(void);
void test_flow_establishment_timeout(void);
void test_flow_establishment_retransmit_dedup(void);
void test_flow_modification_accept(void);
void test_flow_modification_vdp_reject_compensates(void);
void test_flow_modification_not_found(void);
void test_flow_modification_bad_seid_fuzz(void);
void test_flow_deletion_accept_frees_teid(void);
void test_flow_deletion_vdp_reject_keeps_session(void);
void test_flow_concurrent_modification_storm(void);
void test_flow_modification_racing_deletion(void);
void test_node_state_recovery_ts_stable_across_restarts(void);
void test_node_state_force_new_recovery_ts(void);
void test_node_state_association(void);

#endif /* VPE_TEST_REGISTRY_H */
