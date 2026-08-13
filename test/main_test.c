#include "test_util.h"
#include "test_common.h"
#include "test_registry.h"

#include "v_log.h"
#include "v_port_mem.h"
#include "v_port_db.h"
#include "v_port_pfcp.h"
#include "v_port_vdp.h"
#include "v_port_txn.h"
#include "v_db_script.h"
#include "v_id_alloc.h"
#include "v_txn.h"
#include "v_dispatch.h"

#include <rte_eal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

int g_test_failures;
int g_test_count;

#define TEST_CTX_POOL_CAP 8192

static int scripts_ready_pred(void) { return v_db_script_ready(); }

int main(void)
{
    static char *eal_argv[] = {
        "vpe_test", "--no-huge", "--iova-mode=va", "-l", "0",
        "--log-level", "6", NULL
    };
    int eal_argc = 7;

    if (rte_eal_init(eal_argc, eal_argv) < 0) {
        fprintf(stderr, "rte_eal_init failed in test harness\n");
        return 1;
    }

    const char *lvl = getenv("VPE_TEST_LOG_LEVEL");
    if (lvl && strcmp(lvl, "DEV") == 0)
        v_log_set_min_level(V_LOG_DEV);
    else if (lvl && strcmp(lvl, "DEBUG") == 0)
        v_log_set_min_level(V_LOG_DEBUG);
    else if (lvl && strcmp(lvl, "INFO") == 0)
        v_log_set_min_level(V_LOG_INFO);
    else
        v_log_set_min_level(V_LOG_WARNING);

    if (v_port_mem_init(TEST_CTX_POOL_CAP) != 0) { fprintf(stderr, "mem init failed\n"); return 1; }
    if (v_port_txn_init() != 0) { fprintf(stderr, "txn init failed\n"); return 1; }
    if (v_txn_sm_init() != 0) { fprintf(stderr, "txn_sm init failed\n"); return 1; }
    if (v_db_script_init() != 0) { fprintf(stderr, "db_script init failed\n"); return 1; }
    if (v_port_db_init() != 0) { fprintf(stderr, "db init failed\n"); return 1; }
    if (v_port_vdp_io_init() != 0) { fprintf(stderr, "vdp init failed\n"); return 1; }
    if (v_dispatch_init() != 0) { fprintf(stderr, "dispatch init failed\n"); return 1; }
    if (v_port_pfcp_io_init(v_dispatch_rx, NULL) != 0) { fprintf(stderr, "pfcp io init failed\n"); return 1; }

    if (!test_wait_until(scripts_ready_pred, 5000)) {
        fprintf(stderr, "scripts never became ready — is redis-server running on 127.0.0.1:6379?\n");
        return 1;
    }

    if (v_id_alloc_init() != 0) { fprintf(stderr, "id_alloc init failed\n"); return 1; }
    if (!test_wait_until(v_id_alloc_ready, 60000)) {
        fprintf(stderr, "id_alloc never became ready (initial 2048-ring refill)\n");
        return 1;
    }

    printf("=== vpe standalone test suite ===\n");

    test_db_script();
    test_db_script_noscript();
    test_seid();
    test_id_alloc_uniqueness();
    test_id_alloc_teid_floor();
    test_sess_store_cas();
    test_sess_store_pending_ttl();
    test_txn_sm_timeout_paths();
    test_dispatch_echo();
    test_dispatch_any_worker();
    test_retrans_cache_basic();
    test_retrans_cache_ttl();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);

    v_dispatch_fini();
    v_id_alloc_fini();
    v_port_db_fini();
    v_port_txn_fini();
    v_port_mem_fini();

    return g_test_failures ? 1 : 0;
}
