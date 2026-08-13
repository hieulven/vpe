#ifndef VPE_TEST_COMMON_H
#define VPE_TEST_COMMON_H

/* Pumps v_port_db_poll(1) (and the txn stub's simulated clock stays
 * untouched) until pred() is true or max_iters is reached. Standalone
 * tests are the only place a "block until async completes" pattern is
 * legitimate — production code never does this (plan.md: no blocking on
 * the critical path). Returns 1 if pred() became true, 0 on timeout. */
int test_wait_until(int (*pred)(void), int max_iters);

#endif /* VPE_TEST_COMMON_H */
