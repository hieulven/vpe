#ifndef VPE_STUB_DB_TEST_H
#define VPE_STUB_DB_TEST_H

#include <stdint.h>

/* Test-only: tear down and reconnect one shard, to simulate a Sentinel
 * promotion (plan.md §4, §9.3). Fires the registered reconnect
 * callback again once reconnected. Absent at Stage 2. */
int v_stub_db_test_force_reconnect(uint8_t shard);

#endif /* VPE_STUB_DB_TEST_H */
