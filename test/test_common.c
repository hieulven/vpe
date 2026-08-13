#include "test_common.h"
#include "v_port_db.h"

#include <time.h>

int test_wait_until(int (*pred)(void), int max_iters)
{
    for (int i = 0; i < max_iters; i++) {
        if (pred())
            return 1;
        v_port_db_poll(1);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L }; /* 1ms */
        nanosleep(&ts, NULL);
    }
    return pred();
}
