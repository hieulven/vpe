#ifndef VPE_V_ID_ALLOC_H
#define VPE_V_ID_ALLOC_H

#include <stdint.h>

/*
 * v_id_alloc — SEID/TEID rings with Redis block leasing. plan.md §6.2,
 * §6.3, §6.5.
 *
 * 1024 ring pairs, statically created at v_id_alloc_init(). SEID rings
 * are fed purely by an ever-increasing Redis counter (no free list —
 * SEIDs are never reclaimed, plan.md §6.3). TEID rings are fed by
 * v_db_script's teid_refill Lua script, which prefers the free list and
 * falls through to the counter below V_TEID_FREE_FLOOR (plan.md §6.5
 * quarantine).
 */

#define V_ID_BLK_SIZE       256   /* small on purpose — plan.md §6.3 */
#define V_ID_WATERMARK      64
#define V_TEID_FREE_FLOOR   1024  /* quarantine depth — plan.md §6.5 */

int v_id_alloc_init(void);
void v_id_alloc_fini(void);

/* True once every partition's SEID and TEID ring has received its
 * initial refill. Gate PFCP traffic on this at startup (plan.md §6.2:
 * "do not accept PFCP traffic until the initial 2048 refills
 * complete"). */
int v_id_alloc_ready(void);

/* 0 is returned (and never a valid allocation — see v_seid.h) if the
 * ring is dry; caller returns PFCP_CAUSE_NO_RESOURCES_AVAILABLE. */
uint64_t v_seid_alloc(uint16_t part_id);
uint32_t v_teid_alloc(uint16_t part_id);
void     v_teid_free(uint16_t part_id, uint32_t teid);
/* deliberately no v_seid_free — plan.md §6.3 */

#endif /* VPE_V_ID_ALLOC_H */
