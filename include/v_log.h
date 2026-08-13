#ifndef VPE_LOG_H
#define VPE_LOG_H

/*
 * Logging facility for the vPE stateless build.
 *
 * Levels: DEV, DEBUG, INFO, WARNING, ERR, CRIT.
 * Modules (per plan.md environment facts): "PFCP", "DPDB", "MEM".
 *   - PFCP  : session/txn/flow/dispatch control logic
 *   - DPDB  : Redis / DB / id-allocation / retransmit-cache logic
 *   - MEM   : mempool / ctx allocation logic
 *
 * Usage: V_LOG(INFO, "PFCP", "seid=%lu accepted", seid);
 */

typedef enum {
    V_LOG_DEV = 0,
    V_LOG_DEBUG,
    V_LOG_INFO,
    V_LOG_WARNING,
    V_LOG_ERR,
    V_LOG_CRIT,
} v_log_level_t;

void v_log_write(v_log_level_t level, const char *module,
                  const char *file, int line, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 5, 6)))
#endif
    ;

/* Minimum level actually emitted. Defaults to V_LOG_INFO; tests may lower
 * it to V_LOG_DEV to see everything. */
void v_log_set_min_level(v_log_level_t level);

#define V_LOG(level, module, ...) \
    v_log_write(V_LOG_##level, (module), __FILE__, __LINE__, __VA_ARGS__)

#endif /* VPE_LOG_H */
