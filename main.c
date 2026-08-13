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
#include "v_flow.h"
#include "v_node_state.h"
#include "v_common.h"

#include <rte_eal.h>
#include <rte_lcore.h>
#include <stdio.h>
#include <time.h>

/*
 * Stage 1 skeleton main — demonstrates the intended production shape
 * (1 main/IO lcore, N worker lcores per plan.md's "1 IO thread, 4
 * workers, per pod") wired against the stub ports. Not exercised by
 * the standalone test suite (test/main_test.c drives every module
 * directly instead); this is what Stage 2 has to extend once real
 * bindings replace the stubs — see INTEGRATION.md.
 *
 * Deliberately NOT addressed here (see plan.md §0 "out of scope" and
 * §6.2): config wiring from the netconf server (node_id below is a
 * placeholder constant) and graceful shutdown / signal handling.
 * Startup IS gated on v_db_script_ready() and v_id_alloc_ready() below
 * (found the hard way — without it, every partition's initial refill
 * dispatches before its shard's Lua scripts are cached and silently
 * fails). v_port_pfcp_io_init() itself is still called before that
 * gate, though, since in this skeleton nothing is listening on a real
 * socket yet regardless — Stage 2's real PFCP IO binding is what must
 * actually gate accepting traffic on both readiness checks.
 */

#define VPE_CTX_POOL_CAP 65536

/* TODO(Stage 2): bind to real config (netconf server), never to pod
 * hostname/IP — plan.md §6.7. */
static const uint8_t g_node_id[] = { 0x00, 0x01, 0x02, 0x03 };

/* Startup-phase-only pump-and-wait: legitimate here (nothing is
 * accepting PFCP traffic yet) even though the same pattern would
 * violate the no-blocking-on-the-critical-path rule anywhere past this
 * point. Mirrors test/test_common.c's test_wait_until(). */
static int wait_until(int (*pred)(void), int max_iters)
{
    for (int i = 0; i < max_iters; i++) {
        if (pred())
            return 1;
        v_port_db_poll(1);
        struct timespec ts = { 0, 1000000L }; /* 1ms */
        nanosleep(&ts, NULL);
    }
    return pred();
}

static void recovery_ts_loaded_cb(int status, uint32_t ts, void *arg)
{
    (void)arg;
    if (status != RET_CODE_OK)
        V_LOG(CRIT, "PFCP", "failed to load recovery timestamp");
    else
        V_LOG(INFO, "PFCP", "recovery_ts=%u", ts);
}

static int worker_main(void *arg)
{
    (void)arg;
    unsigned lcore = rte_lcore_id();
    V_LOG(INFO, "PFCP", "worker lcore=%u started", lcore);

    /* Runs forever — plan.md: DPDK lcores are the only concurrency
     * mechanism, no new threads, no blocking on the critical path.
     * v_dispatch_worker_poll() never blocks; an idle poll just returns
     * 0 (a real build would rte_pause() here, omitted from this
     * skeleton). */
    for (;;)
        v_dispatch_worker_poll(lcore, 32, v_flow_handle_msg, NULL);

    return 0; /* unreachable */
}

int main(int argc, char **argv)
{
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        fprintf(stderr, "rte_eal_init failed\n");
        return 1;
    }

    V_LOG(INFO, "PFCP", "vpe starting (Stage 1 skeleton, USE_STUBS build)");

    if (v_port_mem_init(VPE_CTX_POOL_CAP) != RET_CODE_OK) goto fail;
    if (v_port_txn_init() != RET_CODE_OK) goto fail;
    if (v_txn_sm_init() != RET_CODE_OK) goto fail;
    if (v_db_script_init() != RET_CODE_OK) goto fail;      /* before v_port_db_init(): must
                                                              catch the initial connect events */
    if (v_port_db_init() != RET_CODE_OK) goto fail;
    if (v_port_vdp_io_init() != RET_CODE_OK) goto fail;
    if (v_dispatch_init() != RET_CODE_OK) goto fail;
    if (v_flow_init() != RET_CODE_OK) goto fail;
    if (v_port_pfcp_io_init(v_dispatch_rx, NULL) != RET_CODE_OK) goto fail;
    if (v_node_state_init(g_node_id, sizeof(g_node_id)) != RET_CODE_OK) goto fail;

    /* v_port_db_init() only dispatches the 5 shard connections — the
     * SCRIPT LOAD replies v_db_script queued on connect haven't landed
     * yet. v_id_alloc_init()'s TEID refills need those SHAs cached
     * (they go through v_db_evalsha), so wait here or every initial
     * refill dispatch fails with "no cached sha yet". */
    if (!wait_until(v_db_script_ready, 30000)) {
        V_LOG(CRIT, "PFCP", "scripts never became ready");
        goto fail;
    }

    if (v_id_alloc_init() != RET_CODE_OK) goto fail;       /* kicks off the initial refill
                                                              of all 2048 rings */
    if (!wait_until(v_id_alloc_ready, 60000)) {
        V_LOG(CRIT, "PFCP", "id_alloc never became ready (initial 2048-ring refill)");
        goto fail;
    }
    v_node_state_load_recovery_ts(recovery_ts_loaded_cb, NULL);
    if (!wait_until(v_node_state_recovery_ts_ready, 5000))
        V_LOG(WARNING, "PFCP", "recovery_ts not yet loaded, proceeding anyway");

    unsigned lcore;
    RTE_LCORE_FOREACH_WORKER(lcore) {
        if (rte_eal_remote_launch(worker_main, NULL, lcore) != 0)
            V_LOG(ERR, "PFCP", "failed to launch worker on lcore=%u", lcore);
    }

    /* Main lcore doubles as the I/O core: drives the DB layer's event
     * loop and drains outbound datagrams. It never touches PFCP
     * semantics (architecture rule 1) — only v_port_db_poll() and
     * v_dispatch_io_drain_tx() -> v_port_pfcp_io_send(). */
    for (;;) {
        v_port_db_poll(1);
        v_dispatch_io_drain_tx(64);
    }

fail:
    V_LOG(CRIT, "PFCP", "vpe startup failed");
    return 1;
}
