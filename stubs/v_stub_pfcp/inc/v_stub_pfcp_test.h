#ifndef VPE_STUB_PFCP_TEST_H
#define VPE_STUB_PFCP_TEST_H

/*
 * Test-only surface of the PFCP stub. Not part of v_port_pfcp.h — the
 * standalone test suite uses this to hand-build synthetic requests and
 * inspect what the code under test sent back. Absent at Stage 2.
 */

#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>

typedef struct {
    uint8_t  type;
    uint32_t seq;
    uint64_t seid;
    uint64_t smf_fseid;
    int      has_ue_ip;
    uint32_t v4_ue_ip;
    uint8_t  v6_ue_ip[16];
} v_stub_pfcp_desc_t;

/* Encodes desc into buf using the stub wire format. Returns bytes
 * written, or 0 if buf_cap is too small. */
size_t v_stub_pfcp_encode(const v_stub_pfcp_desc_t *desc,
                           uint8_t *buf, size_t buf_cap);

/* Synchronously invokes the rx callback registered via
 * v_port_pfcp_io_init(), as if a UDP datagram had just arrived. */
void v_stub_pfcp_io_inject_rx(const uint8_t *buf, size_t len,
                               const struct sockaddr *peer);

/* Captures the most recent v_port_pfcp_io_send() call. Returns 0 if
 * nothing has been sent yet since the last reset. */
size_t v_stub_pfcp_io_last_tx(uint8_t *out, size_t out_cap);
void v_stub_pfcp_io_reset(void);

#endif /* VPE_STUB_PFCP_TEST_H */
