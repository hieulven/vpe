#ifndef VPE_PORT_MEM_H
#define VPE_PORT_MEM_H

#include <stddef.h>

/*
 * Adapter contract — session-context allocation (plan.md §3.4).
 *
 * Stage 1 stub: a real rte_mempool of fixed-size opaque blocks
 * (stubs/v_stub_mem), so pool_in_use() / leak assertions are meaningful.
 * Stage 2: bind to whatever pool the reused modules already allocate
 * struct pdu_ses_ctx from.
 */

struct pdu_ses_ctx; /* opaque — never completed in new code, plan.md §3.0 */

/* Must be called once at startup before any v_port_ctx_alloc(). Returns
 * RET_CODE_OK/ERR. capacity is the number of ctx objects the pool holds. */
int v_port_mem_init(unsigned capacity);
void v_port_mem_fini(void);

struct pdu_ses_ctx *v_port_ctx_alloc(void);
void v_port_ctx_free(struct pdu_ses_ctx *ctx);
void v_port_ctx_clear(struct pdu_ses_ctx *ctx);   /* reuse across CAS retry */
size_t v_port_ctx_pool_in_use(void);               /* for leak assertions */

#endif /* VPE_PORT_MEM_H */
