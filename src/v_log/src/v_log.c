#include "v_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static v_log_level_t g_min_level = V_LOG_INFO;

/* Renders a level enum as the fixed-width string printed in every
 * log line. */
static const char *level_str(v_log_level_t level)
{
    switch (level) {
    case V_LOG_DEV:     return "DEV";
    case V_LOG_DEBUG:   return "DEBUG";
    case V_LOG_INFO:    return "INFO";
    case V_LOG_WARNING: return "WARNING";
    case V_LOG_ERR:     return "ERR";
    case V_LOG_CRIT:    return "CRIT";
    default:            return "?";
    }
}

/* Public: sets the minimum level actually printed. */
void v_log_set_min_level(v_log_level_t level)
{
    g_min_level = level;
}

/* Public, invoked via the V_LOG() macro — never called directly.
 * Prints "[epoch.ms] LEVEL   MODULE file:line: message". */
void v_log_write(v_log_level_t level, const char *module,
                  const char *file, int line, const char *fmt, ...)
{
    if (level < g_min_level)
        return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    fprintf(stderr, "[%ld.%03ld] %-7s %-5s %s:%d: ",
            (long)ts.tv_sec, ts.tv_nsec / 1000000L,
            level_str(level), module, file, line);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
}
