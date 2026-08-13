#ifndef VPE_V_RETRANS_CACHE_H
#define VPE_V_RETRANS_CACHE_H

#include <stdint.h>
#include <stddef.h>

/*
 * v_retrans_cache — cross-pod retransmission dedup. plan.md §6.6.
 * MUST be in Redis, not in-process: the retransmit being deduplicated
 * is precisely the one that lands on a different pod after an SMF
 * timeout. Load-bearing, not an optimization (plan.md §6.6) — without
 * it, an SMF retransmit of an Establishment allocates a second
 * SEID/TEID pair and leaks the first.
 */

#define V_RETRANS_KEY_FMT "txn_%u:%lu:%u"   /* partid, smf_fseid, seq */
#define V_RETRANS_TTL     10

/* Establishment has no SEID yet, so callers route by hash(smf_fseid),
 * not a session part_id — it only needs to be consistent between the
 * lookup and the store for the same (smf_fseid, seq) (plan.md §6.6). */
uint16_t v_retrans_part(uint64_t smf_fseid);

typedef void (*v_retrans_cb_t)(int status, int hit, const uint8_t *resp, size_t len, void *arg);

int v_retrans_lookup(uint16_t part_id, uint64_t smf_fseid, uint32_t seq,
                      v_retrans_cb_t cb, void *arg);

/* Fire-and-forget (SETEX), matching plan.md's signature — no callback.
 * Failures are logged, not surfaced: a lost cache write only risks a
 * duplicate allocation on a subsequent cross-pod retransmit, which is
 * the same failure mode as not having this module at all for that one
 * request, not a correctness break. */
int v_retrans_store(uint16_t part_id, uint64_t smf_fseid, uint32_t seq,
                     const uint8_t *resp, size_t len);

#endif /* VPE_V_RETRANS_CACHE_H */
