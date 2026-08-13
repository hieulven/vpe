#ifndef VPE_V_FLOW_H
#define VPE_V_FLOW_H

#include "v_dispatch.h"

/*
 * v_flow — session establishment / modification / deletion flows.
 * plan.md §5. Session Establishment lands in S8/S9; Modification and
 * Deletion in S10.
 */

int v_flow_init(void);

/* Matches v_dispatch_handler_t — pass directly to
 * v_dispatch_worker_poll(). Decodes the datagram, dispatches on PFCP
 * message type, and drives the rest of the flow via the async ports.
 * Never blocks. */
void v_flow_handle_msg(const v_dispatch_msg_t *msg, void *arg);

#endif /* VPE_V_FLOW_H */
