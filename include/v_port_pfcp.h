#ifndef VPE_PORT_PFCP_H
#define VPE_PORT_PFCP_H

#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>

#include "v_common.h"

/*
 * Adapter contract — PFCP IO + PFCP business logic + serializer
 * (plan.md §3.1). Reused verbatim at Stage 2; Stage 1 talks to the stub
 * in stubs/v_stub_pfcp only through this header.
 */

struct pfcp_msg;    /* opaque */
struct pdu_ses_ctx; /* opaque, plan.md §3.0 */

/* --- PFCP IO (reused) --- */
typedef void (*v_pfcp_rx_cb_t)(const uint8_t *buf, size_t len,
                                const struct sockaddr *peer, void *arg);
int v_port_pfcp_io_init(v_pfcp_rx_cb_t cb, void *arg);
int v_port_pfcp_io_send(const struct sockaddr *peer,
                         const uint8_t *buf, size_t len);

/* --- PFCP business logic (reused) --- */
int v_port_pfcp_decode(const uint8_t *buf, size_t len, struct pfcp_msg **out);
void v_port_pfcp_msg_free(struct pfcp_msg *msg);

uint8_t  v_port_pfcp_msg_type(const struct pfcp_msg *m);
uint64_t v_port_pfcp_hdr_seid(const struct pfcp_msg *m);
uint32_t v_port_pfcp_seq(const struct pfcp_msg *m);
uint64_t v_port_pfcp_smf_fseid(const struct pfcp_msg *m);

/* returns RET_CODE_ERR if the session carries no UE IP */
int v_port_pfcp_ue_ip(const struct pfcp_msg *m, uint32_t *v4, uint8_t v6[16]);

int v_port_pfcp_build_session(const struct pfcp_msg *req,
                               uint64_t seid, uint32_t teid,
                               struct pdu_ses_ctx *ctx);
int v_port_pfcp_modify_session(const struct pfcp_msg *req,
                                struct pdu_ses_ctx *ctx);
int v_port_pfcp_delete_session(const struct pfcp_msg *req,
                                struct pdu_ses_ctx *ctx);

int v_port_pfcp_encode_rsp(const struct pfcp_msg *req,
                            const struct pdu_ses_ctx *ctx, uint8_t cause,
                            uint8_t *buf, size_t *len);

/* ADAPTATION vs plan.md §3.1: a session's TEID is fixed at
 * establishment and Modification/Deletion requests don't carry it, but
 * v_flow's deletion path (§5.3 step 8, v_teid_free) needs it to know
 * what to release. The module that built ctx in the first place is the
 * natural owner of an accessor for what it embedded. */
uint32_t v_port_pfcp_ctx_teid(const struct pdu_ses_ctx *ctx);

/* --- serializer (reused) --- */
int v_port_pdu_serialize(const struct pdu_ses_ctx *ctx,
                          uint8_t *buf, size_t *len);
int v_port_pdu_deserialize(const uint8_t *buf, size_t len,
                            struct pdu_ses_ctx *ctx);

/* PFCP message types the new code branches on. Stage 2: replace with
 * whatever the reused decoder actually defines; values here only need to
 * be self-consistent within Stage 1. */
#define V_PFCP_MSG_HEARTBEAT_REQ        1
#define V_PFCP_MSG_HEARTBEAT_RSP        2
#define V_PFCP_MSG_ASSOC_SETUP_REQ      5
#define V_PFCP_MSG_ASSOC_SETUP_RSP      6
#define V_PFCP_MSG_SESSION_EST_REQ      50
#define V_PFCP_MSG_SESSION_EST_RSP      51
#define V_PFCP_MSG_SESSION_MOD_REQ      52
#define V_PFCP_MSG_SESSION_MOD_RSP      53
#define V_PFCP_MSG_SESSION_DEL_REQ      54
#define V_PFCP_MSG_SESSION_DEL_RSP      55

/* Cause values used by v_port_pfcp_encode_rsp(). TS 29.244 §8.2.1. */
#define V_PFCP_CAUSE_REQUEST_ACCEPTED       1
#define V_PFCP_CAUSE_REQUEST_REJECTED       64
#define V_PFCP_CAUSE_SESSION_CTX_NOT_FOUND  65
#define V_PFCP_CAUSE_SYSTEM_FAILURE         69
#define V_PFCP_CAUSE_NO_RESOURCES_AVAILABLE 71

#endif /* VPE_PORT_PFCP_H */
