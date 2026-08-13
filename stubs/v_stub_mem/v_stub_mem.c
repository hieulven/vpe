#include "v_port_mem.h"
#include "v_common.h"
#include "v_log.h"
#include "v_stub_common.h"

#include <rte_mempool.h>
#include <rte_errno.h>
#include <string.h>

static struct rte_mempool *g_pool;
static unsigned g_capacity;

int v_port_mem_init(unsigned capacity)
{
    if (g_pool) {
        V_LOG(WARNING, "MEM", "v_port_mem_init called twice");
        return RET_CODE_ERR;
    }

    g_pool = rte_mempool_create("v_ctx_pool", capacity,
                                 sizeof(struct v_stub_ses_ctx),
                                 0 /* no per-lcore cache: deterministic
                                      pool_in_use() for leak tests */,
                                 0, NULL, NULL, NULL, NULL,
                                 SOCKET_ID_ANY, 0);
    if (!g_pool) {
        V_LOG(ERR, "MEM", "rte_mempool_create failed: %s",
              rte_strerror(rte_errno));
        return RET_CODE_ERR;
    }

    g_capacity = capacity;
    return RET_CODE_OK;
}

void v_port_mem_fini(void)
{
    if (g_pool) {
        rte_mempool_free(g_pool);
        g_pool = NULL;
        g_capacity = 0;
    }
}

struct pdu_ses_ctx *v_port_ctx_alloc(void)
{
    struct v_stub_ses_ctx *c;

    if (!g_pool) {
        V_LOG(ERR, "MEM", "ctx pool not initialized");
        return NULL;
    }

    if (rte_mempool_get(g_pool, (void **)&c) != 0) {
        V_LOG(WARNING, "MEM", "ctx pool exhausted (capacity=%u)", g_capacity);
        return NULL;
    }

    memset(c, 0, sizeof(*c));
    c->magic = V_STUB_CTX_MAGIC;
    return (struct pdu_ses_ctx *)c;
}

void v_port_ctx_free(struct pdu_ses_ctx *ctx)
{
    if (!ctx)
        return;
    struct v_stub_ses_ctx *c = (struct v_stub_ses_ctx *)ctx;
    if (c->magic != V_STUB_CTX_MAGIC) {
        V_LOG(CRIT, "MEM", "double-free or foreign ctx pointer %p", (void *)ctx);
        return;
    }
    memset(c, 0, sizeof(*c));
    rte_mempool_put(g_pool, c);
}

void v_port_ctx_clear(struct pdu_ses_ctx *ctx)
{
    if (!ctx)
        return;
    struct v_stub_ses_ctx *c = (struct v_stub_ses_ctx *)ctx;
    uint32_t magic = c->magic;
    memset(c, 0, sizeof(*c));
    c->magic = magic;
}

size_t v_port_ctx_pool_in_use(void)
{
    if (!g_pool)
        return 0;
    return g_capacity - rte_mempool_avail_count(g_pool);
}
