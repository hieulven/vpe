#include "v_log.h"

#include <rte_eal.h>
#include <stdio.h>

int g_test_failures;
int g_test_count;

#define CHECK(cond) do { \
    g_test_count++; \
    if (!(cond)) { \
        g_test_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

int main(void)
{
    static char *eal_argv[] = {
        "vpe_test", "--no-huge", "--iova-mode=va", "-l", "0",
        "--log-level", "6", NULL
    };
    int eal_argc = 7;

    int ret = rte_eal_init(eal_argc, eal_argv);
    if (ret < 0) {
        fprintf(stderr, "rte_eal_init failed in test harness\n");
        return 1;
    }

    v_log_set_min_level(V_LOG_WARNING);

    printf("vpe standalone test suite (skeleton) — no test cases registered yet\n");

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures ? 1 : 0;
}
