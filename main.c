#include "v_log.h"

#include <rte_eal.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        fprintf(stderr, "rte_eal_init failed\n");
        return 1;
    }

    V_LOG(INFO, "PFCP", "vpe starting (Stage 1 skeleton)");
    return 0;
}
