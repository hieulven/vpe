#include "v_seid.h"
#include "v_common.h"
#include "v_log.h"

int v_seid_encode(uint16_t part_id, uint64_t local, uint64_t *out)
{
    if (part_id >= V_NUM_PARTS) {
        V_LOG(ERR, "PFCP", "v_seid_encode: part_id %u out of range", part_id);
        return RET_CODE_ERR;
    }
    if (local > V_SEID_LOCAL_MASK) {
        V_LOG(ERR, "PFCP", "v_seid_encode: local 0x%lx overflows %d bits", local, V_SEID_LOCAL_BITS);
        return RET_CODE_ERR;
    }
    *out = ((uint64_t)V_SEID_FMT_VER << V_SEID_FMT_SHIFT) |
           ((uint64_t)part_id << V_SEID_PART_SHIFT) |
           (local & V_SEID_LOCAL_MASK);
    return RET_CODE_OK;
}

uint16_t v_seid_part(uint64_t seid)
{
    return (uint16_t)((seid >> V_SEID_PART_SHIFT) & V_SEID_PART_MASK);
}

uint64_t v_seid_local(uint64_t seid)
{
    return seid & V_SEID_LOCAL_MASK;
}

int v_teid_encode(uint16_t part_id, uint32_t local, uint32_t *out)
{
    if (part_id >= V_NUM_PARTS) {
        V_LOG(ERR, "PFCP", "v_teid_encode: part_id %u out of range", part_id);
        return RET_CODE_ERR;
    }
    if (local == 0 || local > V_TEID_LOCAL_MASK) {
        V_LOG(ERR, "PFCP", "v_teid_encode: local %u out of range (0 reserved, max %u)",
              local, V_TEID_LOCAL_MASK);
        return RET_CODE_ERR;
    }
    *out = ((uint32_t)part_id << V_TEID_PART_SHIFT) | (local & V_TEID_LOCAL_MASK);
    return RET_CODE_OK;
}

uint16_t v_teid_part(uint32_t teid)
{
    return (uint16_t)((teid >> V_TEID_PART_SHIFT) & V_TEID_PART_MASK);
}

uint32_t v_teid_local(uint32_t teid)
{
    return teid & V_TEID_LOCAL_MASK;
}

int v_seid_validate(uint64_t seid, uint16_t *part_out)
{
    uint16_t part;

    if ((seid >> V_SEID_FMT_SHIFT) != V_SEID_FMT_VER) {
        V_LOG(WARNING, "PFCP", "bad seid fmt: 0x%016lx", seid);
        return RET_CODE_ERR;
    }
    if (((seid >> V_SEID_RSV_SHIFT) & V_SEID_RSV_MASK) != 0) {
        V_LOG(WARNING, "PFCP", "bad seid reserved bits: 0x%016lx", seid);
        return RET_CODE_ERR;
    }
    part = v_seid_part(seid);
    if (part >= V_NUM_PARTS) {
        V_LOG(WARNING, "PFCP", "bad seid part=%u: 0x%016lx", part, seid);
        return RET_CODE_ERR;
    }
    *part_out = part;
    return RET_CODE_OK;
}

int v_teid_validate(uint32_t teid, uint16_t *part_out)
{
    uint16_t part;

    if (teid == 0) {
        V_LOG(WARNING, "PFCP", "bad teid: 0 is reserved");
        return RET_CODE_ERR;
    }
    if ((teid >> V_TEID_RSV_SHIFT) != 0) {
        V_LOG(WARNING, "PFCP", "bad teid reserved bit: 0x%08x", teid);
        return RET_CODE_ERR;
    }
    part = v_teid_part(teid);
    if (part >= V_NUM_PARTS) {
        V_LOG(WARNING, "PFCP", "bad teid part=%u: 0x%08x", part, teid);
        return RET_CODE_ERR;
    }
    *part_out = part;
    return RET_CODE_OK;
}
