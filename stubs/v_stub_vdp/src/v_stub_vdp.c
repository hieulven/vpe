#include "v_port_vdp.h"
#include "v_common.h"
#include "v_log.h"

static int g_forced_set;
static v_vdp_result_t g_forced;

/* Port impl: no real connection to establish — just resets the test
 * hook so a previous test's forced result can't leak into the next. */
int v_port_vdp_io_init(void)
{
    g_forced_set = 0;
    V_LOG(INFO, "PFCP", "stub vdp io initialized");
    return RET_CODE_OK;
}

/* Returns the test-forced result if one is set, else the default
 * ACCEPT (plan.md §3.6). */
static v_vdp_result_t next_result(void)
{
    return g_forced_set ? g_forced : V_VDP_ACCEPT;
}

/* Port impl: fires cb SYNCHRONOUSLY, before returning — a real
 * deliberate difference from production (a real VDP push is a TCP
 * round trip). Every caller in src/v_flow.c is written to be correct
 * either way. */
int v_port_vdp_session_create(uint16_t part_id, const struct pdu_ses_ctx *ctx,
                               v_vdp_cb_t cb, void *arg)
{
    (void)part_id;
    (void)ctx;
    cb(next_result(), arg);
    return RET_CODE_OK;
}

/* Port impl: same synchronous-callback shape as session_create(). */
int v_port_vdp_session_modify(uint16_t part_id, const struct pdu_ses_ctx *ctx,
                               v_vdp_cb_t cb, void *arg)
{
    (void)part_id;
    (void)ctx;
    cb(next_result(), arg);
    return RET_CODE_OK;
}

/* Port impl: same synchronous-callback shape as session_create(). */
int v_port_vdp_session_delete(uint16_t part_id, uint64_t seid,
                               v_vdp_cb_t cb, void *arg)
{
    (void)part_id;
    (void)seid;
    cb(next_result(), arg);
    return RET_CODE_OK;
}

/* Test-only: force every subsequent call (until reset) to report res
 * instead of the default ACCEPT. */
void v_port_vdp_test_force_result(v_vdp_result_t res)
{
    g_forced_set = 1;
    g_forced = res;
}

/* Test-only: clear a forced result — call this at the end of any test
 * that used v_port_vdp_test_force_result(), or the next test's default-
 * accept assumption will silently fail. */
void v_port_vdp_test_reset(void)
{
    g_forced_set = 0;
}
