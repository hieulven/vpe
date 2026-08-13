#ifndef VPE_PORT_TXN_H
#define VPE_PORT_TXN_H

#include <stdint.h>
#include <sys/socket.h>

struct pfcp_msg;    /* opaque */
struct pdu_ses_ctx; /* opaque, plan.md §3.0 */

/*
 * Adapter contract — transaction table (plan.md §3.5). Reused, but the
 * new code needs states/fields it does not currently have. Stage 1
 * stubs this as a plain hash table; Stage 2 decides whether to extend
 * the real table or wrap it (impl carries the real table's own handle).
 */

typedef enum {
    TXN_ST_RECV,
    TXN_ST_DB_READ_WAIT,
    TXN_ST_DB_WRITE_WAIT,
    TXN_ST_VDP_WAIT,
    TXN_ST_DB_CONFIRM_WAIT,
    TXN_ST_REPLIED,
} v_txn_state_t;

struct v_txn {
    v_txn_state_t       state;
    uint64_t             db_ver;      /* version read, for the CAS */
    uint8_t              cas_retry;   /* max V_CAS_MAX_RETRY */
    uint16_t             part_id;
    uint64_t             seid;
    uint32_t             teid;
    struct pfcp_msg      *req;
    struct pdu_ses_ctx   *ctx;
    struct sockaddr       peer;
    uint64_t              deadline_ms;
    void                 *impl;        /* reused table's own handle */
};

int v_port_txn_init(void);
void v_port_txn_fini(void);

struct v_txn *v_port_txn_create(void);
void          v_port_txn_destroy(struct v_txn *t);
struct v_txn *v_port_txn_find_by_seq(uint64_t smf_fseid, uint32_t seq);
void          v_port_txn_set_state(struct v_txn *t, v_txn_state_t st);

/* Stage 1 addition: the reused table indexes by (smf_fseid, seq) for
 * dedup lookup, but v_flow also needs to reach a live txn by the
 * SEID/TEID it allocated (e.g. the VDP accept/reject callback only
 * carries what v_flow closed over — this is just an index, not new
 * state). Trivial to provide from the real table's key space at
 * Stage 2. */
struct v_txn *v_port_txn_find_by_seid(uint64_t seid);

/* CONFIRMED PRESENT: the reused table enforces a single HARD timeout for
 * the whole transaction — not per-state. Bind this callback to it. */
typedef void (*v_txn_timeout_cb_t)(struct v_txn *t, void *arg);
int v_port_txn_on_timeout(v_txn_timeout_cb_t cb, void *arg);

/* Stage 1 only: advance simulated time / poll timers, since there is no
 * real event loop driving the stub table. Production timers come from
 * the reused table itself. */
void v_port_txn_test_advance_ms(uint64_t ms);

/* Stage 1 only: the hard timeout the stub table arms each txn with, in
 * simulated ms. Stage 2: read the real configured value instead (see
 * plan.md §3.5's worst-case-budget check). */
uint64_t v_port_txn_test_hard_timeout_ms(void);

#endif /* VPE_PORT_TXN_H */
