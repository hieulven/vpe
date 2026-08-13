#ifndef VPE_V_DB_LUA_SCRIPTS_H
#define VPE_V_DB_LUA_SCRIPTS_H

/*
 * Lua script bodies, verbatim from plan.md §6.1 and §6.3. Single source
 * of truth — indices must line up with v_script_id_t in v_db_script.h.
 */

/* v_sess_cas_write — plan.md §6.1
 * KEYS[1] = pdu_<part>:<seid>
 * ARGV[1] = expected_ver ("0" = must not exist)
 * ARGV[2] = serialized blob
 * ARGV[3] = ttl seconds, or "0" for none
 */
static const char *V_LUA_SESS_CAS_WRITE =
    "local v = redis.call('HGET', KEYS[1], 'ver')\n"
    "if ARGV[1] == '0' then\n"
    "    if v then return 0 end\n"
    "    redis.call('HSET', KEYS[1], 'ver', 1, 'data', ARGV[2])\n"
    "    if ARGV[3] ~= '0' then redis.call('EXPIRE', KEYS[1], ARGV[3]) end\n"
    "    return 1\n"
    "end\n"
    "if not v then return -1 end\n"
    "if v ~= ARGV[1] then return 0 end\n"
    "redis.call('HSET', KEYS[1], 'ver', v + 1, 'data', ARGV[2])\n"
    "if ARGV[3] ~= '0' then redis.call('EXPIRE', KEYS[1], ARGV[3]) end\n"
    "return 1\n";

/* v_sess_delete — plan.md §6.1
 * KEYS[1] = pdu_<part>:<seid>   ARGV[1] = expected_ver
 */
static const char *V_LUA_SESS_DELETE =
    "local v = redis.call('HGET', KEYS[1], 'ver')\n"
    "if not v then return -1 end\n"
    "if v ~= ARGV[1] then return 0 end\n"
    "redis.call('DEL', KEYS[1])\n"
    "return 1\n";

/* v_teid_refill — plan.md §6.3
 * KEYS[1] = vpe:teid:<part>:free   KEYS[2] = vpe:teid:<part>:next
 * ARGV[1] = block size             ARGV[2] = free-list floor
 */
static const char *V_LUA_TEID_REFILL =
    "local out = {}\n"
    "if redis.call('LLEN', KEYS[1]) > tonumber(ARGV[2]) then\n"
    "    for i = 1, tonumber(ARGV[1]) do\n"
    "        local id = redis.call('LPOP', KEYS[1])\n"
    "        if not id then break end\n"
    "        table.insert(out, id)\n"
    "    end\n"
    "end\n"
    "if #out == 0 then\n"
    "    local base = redis.call('INCRBY', KEYS[2], ARGV[1])\n"
    "    return {'range', tostring(base - ARGV[1]), ARGV[1]}\n"
    "end\n"
    "return {'list', unpack(out)}\n";

#endif /* VPE_V_DB_LUA_SCRIPTS_H */
