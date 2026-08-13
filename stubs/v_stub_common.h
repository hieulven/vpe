#ifndef VPE_STUB_COMMON_H
#define VPE_STUB_COMMON_H

#include <stdint.h>

/*
 * Shared internal layout used ONLY by the stub implementations
 * (v_stub_mem, v_stub_pfcp) to stand in for the real, reused
 * `struct pdu_ses_ctx` + mempool + serializer. New (Stage 1) production
 * code never includes this header and never dereferences a ctx pointer
 * — see plan.md §3.0. Stage 2 deletes this file along with the rest of
 * stubs/.
 */

#define V_STUB_CTX_MAGIC 0x53455343u /* "SESC" */
#define V_STUB_CTX_PAYLOAD 128

struct v_stub_ses_ctx {
    uint32_t magic;
    uint64_t seid;
    uint32_t teid;
    uint32_t v4_ue_ip;
    uint8_t  v6_ue_ip[16];
    uint8_t  has_ue_ip;
    uint8_t  payload[V_STUB_CTX_PAYLOAD]; /* echoes an injected pattern */
};

/* Synthetic wire formats used only by the stub PFCP codec, so the
 * standalone test suite can hand-build request bytes and the stub's
 * v_port_pfcp_decode()/encode_rsp() can round-trip them. Bears no
 * relation to the real PFCP wire format — Stage 2 deletes it. */
struct v_stub_wire_req {
    uint8_t  type;
    uint8_t  has_ue_ip;
    uint16_t rsv;
    uint32_t seq;
    uint64_t seid;        /* header SEID, 0 pre-establishment */
    uint64_t smf_fseid;
    uint32_t v4_ue_ip;
    uint8_t  v6_ue_ip[16];
} __attribute__((packed));

struct v_stub_wire_rsp {
    uint8_t  type;
    uint8_t  cause;
    uint16_t rsv;
    uint32_t seq;
    uint64_t seid;
} __attribute__((packed));

#endif /* VPE_STUB_COMMON_H */
