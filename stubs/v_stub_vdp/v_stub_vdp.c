#include "v_port_vdp.h"
#include "v_common.h"
#include "v_log.h"

static int g_forced_set;
static v_vdp_result_t g_forced;

int v_port_vdp_io_init(void)
{
    g_forced_set = 0;
    V_LOG(INFO, "PFCP", "stub vdp io initialized");
    return RET_CODE_OK;
}

static v_vdp_result_t next_result(void)
{
    return g_forced_set ? g_forced : V_VDP_ACCEPT;
}

int v_port_vdp_session_create(uint16_t part_id, const struct pdu_ses_ctx *ctx,
                               v_vdp_cb_t cb, void *arg)
{
    (void)part_id;
    (void)ctx;
    cb(next_result(), arg);
    return RET_CODE_OK;
}

int v_port_vdp_session_modify(uint16_t part_id, const struct pdu_ses_ctx *ctx,
                               v_vdp_cb_t cb, void *arg)
{
    (void)part_id;
    (void)ctx;
    cb(next_result(), arg);
    return RET_CODE_OK;
}

int v_port_vdp_session_delete(uint16_t part_id, uint64_t seid,
                               v_vdp_cb_t cb, void *arg)
{
    (void)part_id;
    (void)seid;
    cb(next_result(), arg);
    return RET_CODE_OK;
}

void v_port_vdp_test_force_result(v_vdp_result_t res)
{
    g_forced_set = 1;
    g_forced = res;
}

void v_port_vdp_test_reset(void)
{
    g_forced_set = 0;
}
