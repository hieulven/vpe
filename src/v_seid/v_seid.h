#ifndef VPE_V_SEID_H
#define VPE_V_SEID_H

#include <stdint.h>

/*
 * v_seid — SEID/TEID bit layout, encode, decode, validate. plan.md §7,
 * §6.3, environment facts row "part_id".
 *
 * SEID (64 bits):
 *   [63:60] format version   (V_SEID_FMT_VER)
 *   [59:56] reserved, MUST BE ZERO — the old layout's 4 VPE-ID bits.
 *           Never set, never reused (plan.md §7, §11 "Encoding VPE
 *           worker or pod identity in the SEID").
 *   [55:46] part_id, 10 bits (V_NUM_PARTS = 1024)
 *   [45:0]  local index, 46 bits. Never reclaimed (plan.md §6.3) — the
 *           block-lease counter in v_id_alloc only ever increases.
 *
 * TEID (32 bits):
 *   [31]    reserved, MUST BE ZERO
 *   [30:21] part_id, 10 bits (same partition space as SEID)
 *   [20:0]  local index, 21 bits (~2M per partition). 0 is reserved —
 *           local indices start at 1 (plan.md §7 "TEID 0 is reserved").
 *           Reclaimed via a free list (plan.md §6.2, §6.3, §6.5).
 */

#define V_NUM_PARTS        1024u   /* 10 bits */
#define V_SEID_FMT_VER     0x1u

#define V_SEID_FMT_SHIFT   60
#define V_SEID_RSV_SHIFT   56
#define V_SEID_RSV_MASK    0xFull
#define V_SEID_PART_SHIFT  46
#define V_SEID_PART_MASK   0x3FFull
#define V_SEID_LOCAL_BITS  46
#define V_SEID_LOCAL_MASK  ((1ull << V_SEID_LOCAL_BITS) - 1)

#define V_TEID_RSV_SHIFT   31
#define V_TEID_PART_SHIFT  21
#define V_TEID_PART_MASK   0x3FFu
#define V_TEID_LOCAL_BITS  21
#define V_TEID_LOCAL_MASK  ((1u << V_TEID_LOCAL_BITS) - 1)

/* Encode/decode. Encode fails (RET_CODE_ERR) if part_id or local overflow
 * their field width — a bug in the caller (v_id_alloc), not attacker
 * input, but checked anyway per the project's error-handling convention. */
int v_seid_encode(uint16_t part_id, uint64_t local, uint64_t *out);
uint16_t v_seid_part(uint64_t seid);
uint64_t v_seid_local(uint64_t seid);

int v_teid_encode(uint16_t part_id, uint32_t local, uint32_t *out);
uint16_t v_teid_part(uint32_t teid);
uint32_t v_teid_local(uint32_t teid);

/* Validates attacker-controlled input per plan.md §7 (free5gc #730/#731:
 * SEID = 0xFFFFFFFFFFFFFFFF underflow / negative array index). Call in
 * EVERY handler that dereferences an inbound SEID to reach a session —
 * modification, deletion, report. Do NOT call on an Establishment
 * request's header SEID, which is legitimately 0 pre-session. */
int v_seid_validate(uint64_t seid, uint16_t *part_out);
int v_teid_validate(uint32_t teid, uint16_t *part_out);

#endif /* VPE_V_SEID_H */
