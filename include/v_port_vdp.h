#ifndef VPE_PORT_VDP_H
#define VPE_PORT_VDP_H

#include <stdint.h>

struct pdu_ses_ctx; /* opaque, plan.md §3.0 */

typedef enum { V_VDP_ACCEPT, V_VDP_REJECT, V_VDP_TIMEOUT } v_vdp_result_t;
typedef void (*v_vdp_cb_t)(v_vdp_result_t res, void *arg);

int v_port_vdp_io_init(void);
int v_port_vdp_session_create(uint16_t part_id, const struct pdu_ses_ctx *ctx,
                               v_vdp_cb_t cb, void *arg);
int v_port_vdp_session_modify(uint16_t part_id, const struct pdu_ses_ctx *ctx,
                               v_vdp_cb_t cb, void *arg);
int v_port_vdp_session_delete(uint16_t part_id, uint64_t seid,
                               v_vdp_cb_t cb, void *arg);

/* Stage 1 test hooks only — not part of the reused module's real API.
 * Let the standalone suite force REJECT/TIMEOUT deterministically
 * (plan.md §3.6, "Mem stub"/"VDP stub" bullet). Absent at Stage 2. */
void v_port_vdp_test_force_result(v_vdp_result_t res);
void v_port_vdp_test_reset(void);

#endif /* VPE_PORT_VDP_H */
