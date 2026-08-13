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

#endif /* VPE_TEST_REGISTRY_H */
