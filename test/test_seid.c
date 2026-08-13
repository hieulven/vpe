#include "test_util.h"
#include "v_seid.h"
#include "v_common.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

void test_seid(void)
{
    printf("-- test_seid --\n");

    /* Round trip across the interesting part_id / local corners. */
    uint16_t parts[] = { 0, 1, 511, 1022, 1023 };
    uint64_t locals[] = { 0, 1, 12345, V_SEID_LOCAL_MASK - 1, V_SEID_LOCAL_MASK };

    for (size_t pi = 0; pi < sizeof(parts) / sizeof(parts[0]); pi++) {
        for (size_t li = 0; li < sizeof(locals) / sizeof(locals[0]); li++) {
            uint64_t seid;
            CHECK(v_seid_encode(parts[pi], locals[li], &seid) == RET_CODE_OK);
            CHECK(v_seid_part(seid) == parts[pi]);
            CHECK(v_seid_local(seid) == locals[li]);

            uint16_t part_out;
            CHECK(v_seid_validate(seid, &part_out) == RET_CODE_OK);
            CHECK(part_out == parts[pi]);
        }
    }

    /* Encode range checks. */
    uint64_t dummy;
    CHECK(v_seid_encode(V_NUM_PARTS, 0, &dummy) == RET_CODE_ERR);          /* part_id out of range */
    CHECK(v_seid_encode(0, V_SEID_LOCAL_MASK + 1, &dummy) == RET_CODE_ERR); /* local overflow */

    /* free5gc #730/#731-class fuzz: all-ones SEID must be rejected, not
     * underflow into a negative/huge part index. */
    uint16_t part_out;
    CHECK(v_seid_validate(0xFFFFFFFFFFFFFFFFull, &part_out) == RET_CODE_ERR);
    CHECK(v_seid_validate(0x0000000000000000ull, &part_out) == RET_CODE_ERR); /* fmt version 0 */

    /* Wrong format version (valid-looking otherwise). */
    uint64_t good_seid;
    CHECK(v_seid_encode(3, 42, &good_seid) == RET_CODE_OK);
    uint64_t wrong_fmt = (good_seid & ~((uint64_t)0xF << V_SEID_FMT_SHIFT)) |
                         ((uint64_t)0x2 << V_SEID_FMT_SHIFT);
    CHECK(v_seid_validate(wrong_fmt, &part_out) == RET_CODE_ERR);

    /* Non-zero reserved (old VPE-ID) bits — must reject even with a
     * correct format version and a part_id that would otherwise be
     * in-range. */
    uint64_t bad_reserved = good_seid | ((uint64_t)0x1 << V_SEID_RSV_SHIFT);
    CHECK(v_seid_validate(bad_reserved, &part_out) == RET_CODE_ERR);

    /* --- TEID --- */
    uint32_t teid_parts[] = { 0, 1, 511, 1023 };
    uint32_t teid_locals[] = { 1, 2, V_TEID_LOCAL_MASK - 1, V_TEID_LOCAL_MASK };
    for (size_t pi = 0; pi < sizeof(teid_parts) / sizeof(teid_parts[0]); pi++) {
        for (size_t li = 0; li < sizeof(teid_locals) / sizeof(teid_locals[0]); li++) {
            uint32_t teid;
            CHECK(v_teid_encode((uint16_t)teid_parts[pi], teid_locals[li], &teid) == RET_CODE_OK);
            CHECK(v_teid_part(teid) == teid_parts[pi]);
            CHECK(v_teid_local(teid) == teid_locals[li]);

            uint16_t tpart;
            CHECK(v_teid_validate(teid, &tpart) == RET_CODE_OK);
            CHECK(tpart == (uint16_t)teid_parts[pi]);
        }
    }

    uint32_t tdummy;
    CHECK(v_teid_encode(V_NUM_PARTS, 1, &tdummy) == RET_CODE_ERR);
    CHECK(v_teid_encode(0, 0, &tdummy) == RET_CODE_ERR);                    /* local 0 reserved */
    CHECK(v_teid_encode(0, V_TEID_LOCAL_MASK + 1, &tdummy) == RET_CODE_ERR); /* local overflow */

    uint16_t tpart;
    CHECK(v_teid_validate(0, &tpart) == RET_CODE_ERR);            /* 0 reserved */
    CHECK(v_teid_validate(0xFFFFFFFFu, &tpart) == RET_CODE_ERR);  /* reserved bit + fuzz */

    uint32_t good_teid;
    CHECK(v_teid_encode(7, 99, &good_teid) == RET_CODE_OK);
    uint32_t bad_teid_rsv = good_teid | (1u << V_TEID_RSV_SHIFT);
    CHECK(v_teid_validate(bad_teid_rsv, &tpart) == RET_CODE_ERR);

    printf("test_seid: done\n");
}
