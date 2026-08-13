#include "test_util.h"
#include "v_dispatch.h"
#include "v_port_pfcp.h"
#include "v_stub_pfcp_test.h"
#include "v_common.h"

#include <stdio.h>
#include <string.h>

static void echo_handler(const v_dispatch_msg_t *msg, void *arg)
{
    int *count = (int *)arg;
    /* No PFCP semantics — pure byte forward, proving the ring plumbing
     * works without decoding anything (plan.md §8 S7). */
    CHECK(v_dispatch_tx_enqueue(msg->buf, msg->len, &msg->peer) == RET_CODE_OK);
    if (count)
        (*count)++;
}

void test_dispatch_echo(void)
{
    printf("-- test_dispatch_echo --\n");

    v_stub_pfcp_desc_t desc = {
        .type = V_PFCP_MSG_HEARTBEAT_REQ,
        .seq = 4242,
        .seid = 0,
        .smf_fseid = 0,
    };
    uint8_t buf[256];
    size_t len = v_stub_pfcp_encode(&desc, buf, sizeof(buf));
    CHECK(len > 0);

    struct sockaddr peer;
    memset(&peer, 0, sizeof(peer));

    v_stub_pfcp_io_reset();
    v_stub_pfcp_io_inject_rx(buf, len, &peer); /* -> registered v_dispatch_rx -> rx_ring */

    int processed = 0;
    unsigned n = v_dispatch_worker_poll(0, 10, echo_handler, &processed);
    CHECK(n == 1);
    CHECK(processed == 1);

    unsigned sent = v_dispatch_io_drain_tx(10);
    CHECK(sent == 1);

    uint8_t out[256];
    size_t out_len = v_stub_pfcp_io_last_tx(out, sizeof(out));
    CHECK(out_len == len);
    CHECK(memcmp(out, buf, len) == 0);

    /* Nothing left queued either direction. */
    CHECK(v_dispatch_worker_poll(1, 10, echo_handler, NULL) == 0);
    CHECK(v_dispatch_io_drain_tx(10) == 0);

    printf("test_dispatch_echo: done\n");
}

void test_dispatch_any_worker(void)
{
    printf("-- test_dispatch_any_worker --\n");

    /* Inject several messages, then drain them via poll calls tagged
     * with DIFFERENT worker_id values — demonstrating there is no SEID
     * affinity: any worker can pick up any message (plan.md §2 Change
     * 2 removes the correctness need for per-worker session pinning). */
    const int N = 9;
    for (int i = 0; i < N; i++) {
        v_stub_pfcp_desc_t desc = { .type = V_PFCP_MSG_SESSION_MOD_REQ, .seq = (uint32_t)(9000 + i) };
        uint8_t buf[256];
        size_t len = v_stub_pfcp_encode(&desc, buf, sizeof(buf));
        struct sockaddr peer;
        memset(&peer, 0, sizeof(peer));
        v_stub_pfcp_io_inject_rx(buf, len, &peer);
    }

    unsigned p1 = v_dispatch_worker_poll(2, 3, echo_handler, NULL);
    unsigned p2 = v_dispatch_worker_poll(0, 3, echo_handler, NULL);
    unsigned p3 = v_dispatch_worker_poll(3, 100, echo_handler, NULL);
    CHECK((int)(p1 + p2 + p3) == N);

    unsigned sent = v_dispatch_io_drain_tx(100);
    CHECK(sent == N);

    printf("test_dispatch_any_worker: done\n");
}
